#include "ai/Translate.h"

#include "ai/Onnx.h"

#include <sentencepiece_processor.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace movix::ai {

namespace {

// Identificadores especiales de los modelos Marian de Opus-MT (config.json).
constexpr int64_t kEos = 0;
constexpr int64_t kPad = 65000; // también es el inicio del decodificador

struct MarianModel {
    std::string folder;
    std::unique_ptr<Ort::Session> encoder;
    std::unique_ptr<Ort::Session> decoder;
    sentencepiece::SentencePieceProcessor source;
    sentencepiece::SentencePieceProcessor target;
    std::unordered_map<std::string, int64_t> vocab;
    std::vector<std::string> pieces; // id → pieza
    int64_t unk = 1;
    std::vector<std::string> encoderInputs, decoderInputs;
    std::string encoderOutput, decoderOutput;
};

MarianModel& marian()
{
    static MarianModel& m = *new MarianModel; // sin destruir al salir (ver onnx::env)
    return m;
}

bool load(const std::string& folder, std::string* error)
{
    MarianModel& m = marian();
    if (m.encoder && m.folder == folder)
        return true;
    try {
        if (!m.source.Load(folder + "/source.spm").ok() || !m.target.Load(folder + "/target.spm").ok())
            throw std::runtime_error("no se pudieron leer los tokenizadores (.spm)");
        QFile vocabFile(QString::fromStdString(folder + "/vocab.json"));
        if (!vocabFile.open(QIODevice::ReadOnly))
            throw std::runtime_error("no se encontró vocab.json");
        const QJsonObject vocab = QJsonDocument::fromJson(vocabFile.readAll()).object();
        m.vocab.clear();
        m.pieces.assign(size_t(vocab.size()) + 1, {});
        for (auto it = vocab.begin(); it != vocab.end(); ++it) {
            const int64_t id = it.value().toInteger();
            m.vocab[it.key().toStdString()] = id;
            if (id >= 0 && size_t(id) < m.pieces.size())
                m.pieces[size_t(id)] = it.key().toStdString();
        }
        if (auto it = m.vocab.find("<unk>"); it != m.vocab.end())
            m.unk = it->second;
        m.encoder = onnx::openSession(folder + "/encoder.onnx");
        m.decoder = onnx::openSession(folder + "/decoder.onnx");
        m.encoderInputs = onnx::inputNames(*m.encoder);
        m.decoderInputs = onnx::inputNames(*m.decoder);
        m.encoderOutput = onnx::outputNames(*m.encoder).at(0);
        m.decoderOutput = onnx::outputNames(*m.decoder).at(0);
        m.folder = folder;
        return true;
    } catch (const std::exception& e) {
        m.encoder.reset();
        m.decoder.reset();
        if (error)
            *error = std::string("No se pudo cargar el modelo de traducción: ") + e.what();
        return false;
    }
}

// Texto → piezas de SentencePiece → ids del vocabulario compartido.
std::vector<int64_t> toIds(MarianModel& m, sentencepiece::SentencePieceProcessor& spm, const std::string& text)
{
    std::vector<std::string> pieces;
    spm.Encode(text, &pieces);
    std::vector<int64_t> ids;
    for (const std::string& p : pieces) {
        auto it = m.vocab.find(p);
        ids.push_back(it != m.vocab.end() ? it->second : m.unk);
    }
    return ids;
}

// El texto codificado una vez: lo que lee el decodificador en cada paso.
struct Encoded {
    std::vector<int64_t> mask;
    std::vector<float> hidden;
    std::vector<int64_t> hiddenShape; // [1, n, d]
};

Encoded encode(MarianModel& m, const std::string& text)
{
    std::vector<int64_t> ids = toIds(m, m.source, text);
    ids.push_back(kEos);
    const int64_t n = int64_t(ids.size());
    Encoded e;
    e.mask.assign(size_t(n), 1);
    std::vector<Ort::Value> encIn;
    for (const std::string& name : m.encoderInputs)
        encIn.push_back(name == "attention_mask" ? onnx::tensor(e.mask, {1, n}) : onnx::tensor(ids, {1, n}));
    auto encOut = onnx::run(*m.encoder, m.encoderInputs, encIn, {m.encoderOutput});
    e.hiddenShape = encOut[0].GetTensorTypeAndShapeInfo().GetShape();
    e.hidden.assign(encOut[0].GetTensorData<float>(), encOut[0].GetTensorData<float>() + encOut[0].GetTensorTypeAndShapeInfo().GetElementCount());
    return e;
}

// Log-probabilidades del siguiente token de varias hipótesis de la misma longitud a la vez:
// [hipótesis][vocabulario]; con `allSteps`, las de todos los pasos de la primera (para puntuarla).
std::vector<std::vector<float>> nextLogProbs(MarianModel& m, const Encoded& e, const std::vector<std::vector<int64_t>>& hyps, bool allSteps = false)
{
    const int64_t batch = int64_t(hyps.size()), steps = int64_t(hyps[0].size());
    const int64_t n = e.hiddenShape[1], d = e.hiddenShape[2];
    std::vector<int64_t> tokens, mask;
    std::vector<float> hidden;
    for (const auto& h : hyps) {
        tokens.insert(tokens.end(), h.begin(), h.end());
        mask.insert(mask.end(), e.mask.begin(), e.mask.end());
        hidden.insert(hidden.end(), e.hidden.begin(), e.hidden.end());
    }
    std::vector<Ort::Value> decIn;
    for (const std::string& name : m.decoderInputs) {
        if (name == "encoder_attention_mask")
            decIn.push_back(onnx::tensor(mask, {batch, n}));
        else if (name == "encoder_hidden_states")
            decIn.push_back(onnx::tensor(hidden, {batch, n, d}));
        else
            decIn.push_back(onnx::tensor(tokens, {batch, steps}));
    }
    auto decOut = onnx::run(*m.decoder, m.decoderInputs, decIn, {m.decoderOutput});
    const auto shape = decOut[0].GetTensorTypeAndShapeInfo().GetShape(); // [hipótesis, pasos, vocabulario]
    const int64_t vocab = shape[2];
    const float* data = decOut[0].GetTensorData<float>();
    std::vector<std::vector<float>> out;
    auto logSoftmax = [&](const float* row) {
        std::vector<float> lp(row, row + vocab);
        lp[size_t(kPad)] = -1e30f; // el relleno nunca es una palabra
        const float top = *std::max_element(lp.begin(), lp.end());
        double sum = 0;
        for (float v : lp)
            sum += std::exp(double(v - top));
        const float norm = top + float(std::log(sum));
        for (float& v : lp)
            v -= norm;
        return lp;
    };
    if (allSteps) {
        for (int64_t s = 0; s < shape[1]; ++s)
            out.push_back(logSoftmax(data + s * vocab));
    } else {
        for (int64_t b = 0; b < shape[0]; ++b)
            out.push_back(logSoftmax(data + (b * shape[1] + shape[1] - 1) * vocab));
    }
    return out;
}

std::string translateOne(MarianModel& m, const std::string& text, int beams)
{
    const Encoded e = encode(m, text);
    const int64_t maxLength = std::min<int64_t>(512, 3 * e.hiddenShape[1] + 10);
    beams = std::max(1, beams);

    // Búsqueda en haz (como la de Hugging Face): en cada paso se amplían las hipótesis vivas con sus
    // mejores continuaciones y se quedan las `beams` mejores; las que terminan (</s>) se guardan con
    // su log-probabilidad media por token. Se para cuando ninguna viva puede superar a las guardadas.
    struct Hyp {
        std::vector<int64_t> tokens;
        double score = 0; // suma de log-probabilidades
    };
    std::vector<Hyp> alive{{{kPad}, 0.0}};
    std::vector<std::pair<double, std::vector<int64_t>>> finished; // (puntuación media, tokens)
    auto worstFinished = [&] {
        double w = 1e300;
        for (const auto& f : finished)
            w = std::min(w, f.first);
        return w;
    };
    while (!alive.empty() && int64_t(alive[0].tokens.size()) < maxLength) {
        std::vector<std::vector<int64_t>> batch;
        for (const Hyp& h : alive)
            batch.push_back(h.tokens);
        const auto lps = nextLogProbs(m, e, batch);
        struct Candidate {
            double score;
            size_t from;
            int64_t token;
        };
        std::vector<Candidate> candidates;
        for (size_t b = 0; b < alive.size(); ++b) {
            // Las 2·beams mejores de cada hipótesis bastan para elegir las beams mejores de todas.
            std::vector<int64_t> order(lps[b].size());
            std::iota(order.begin(), order.end(), 0);
            const size_t keep = std::min(order.size(), size_t(2 * beams));
            std::partial_sort(order.begin(), order.begin() + std::ptrdiff_t(keep), order.end(),
                              [&](int64_t x, int64_t y) { return lps[b][size_t(x)] > lps[b][size_t(y)]; });
            for (size_t k = 0; k < keep; ++k)
                candidates.push_back({alive[b].score + lps[b][size_t(order[k])], b, order[k]});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
        const double length = double(alive[0].tokens.size()); // con el inicio, sin el final (como HF)
        std::vector<Hyp> next;
        for (size_t rank = 0; rank < candidates.size() && int(next.size()) < beams; ++rank) {
            const Candidate& c = candidates[rank];
            if (c.token == kEos) {
                // Solo cuenta si está entre las mejores (si no, una hipótesis corta mala se colaría).
                if (int(rank) < beams) {
                    finished.emplace_back(c.score / length, alive[c.from].tokens);
                    if (int(finished.size()) > beams)
                        finished.erase(std::min_element(finished.begin(), finished.end()));
                }
                continue;
            }
            Hyp h = alive[c.from];
            h.tokens.push_back(c.token);
            h.score = c.score;
            next.push_back(std::move(h));
        }
        alive = std::move(next);
        if (int(finished.size()) >= beams && !alive.empty() && alive[0].score / (length + 1) <= worstFinished())
            break;
    }
    if (finished.empty() && !alive.empty())
        finished.emplace_back(alive[0].score / double(alive[0].tokens.size()), alive[0].tokens);
    const auto& out = std::max_element(finished.begin(), finished.end())->second;

    std::vector<std::string> outPieces;
    for (size_t i = 1; i < out.size(); ++i)
        if (size_t(out[i]) < m.pieces.size())
            outPieces.push_back(m.pieces[size_t(out[i])]);
    std::string result;
    m.target.Decode(outPieces, &result);
    return result;
}

} // namespace

bool translationScore(const std::string& modelFolder, const std::string& text, const std::string& translation, double& score,
                      std::string* error)
{
    if (!load(modelFolder, error))
        return false;
    try {
        MarianModel& m = marian();
        const Encoded e = encode(m, text);
        std::vector<int64_t> target = toIds(m, m.target, translation);
        target.push_back(kEos);
        std::vector<int64_t> input{kPad};
        input.insert(input.end(), target.begin(), target.end() - 1);
        const auto lps = nextLogProbs(m, e, {input}, true);
        double sum = 0;
        for (size_t i = 0; i < target.size(); ++i)
            sum += lps[i][size_t(target[i])];
        score = sum / double(target.size());
        return true;
    } catch (const std::exception& ex) {
        if (error)
            *error = std::string("La traducción falló: ") + ex.what();
        return false;
    }
}

bool translate(const std::string& modelFolder, const std::vector<std::string>& texts, std::vector<std::string>& out,
               std::string* error, const ProgressFn& progress, const CancelFn& cancel, int beams)
{
    out.clear();
    if (!load(modelFolder, error))
        return false;
    try {
        for (size_t i = 0; i < texts.size(); ++i) {
            if (cancel && cancel()) {
                if (error)
                    *error = "cancelado";
                return false;
            }
            // Las líneas de un subtítulo se traducen juntas (son una misma frase).
            std::string text = texts[i];
            std::replace(text.begin(), text.end(), '\n', ' ');
            out.push_back(text.find_first_not_of(" \t") == std::string::npos ? std::string() : translateOne(marian(), text, beams));
            if (progress)
                progress(double(i + 1) / double(texts.size()));
        }
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("La traducción falló: ") + e.what();
        return false;
    }
    return true;
}

} // namespace movix::ai
