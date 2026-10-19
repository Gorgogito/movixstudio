#include "ai/Translate.h"

#include "ai/Onnx.h"

#include <sentencepiece_processor.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <map>
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
    static MarianModel m;
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

std::string translateOne(MarianModel& m, const std::string& text)
{
    // Texto → piezas de SentencePiece → ids del vocabulario compartido, terminado en </s>.
    std::vector<std::string> pieces;
    m.source.Encode(text, &pieces);
    std::vector<int64_t> ids;
    for (const std::string& p : pieces) {
        auto it = m.vocab.find(p);
        ids.push_back(it != m.vocab.end() ? it->second : m.unk);
    }
    ids.push_back(kEos);
    const int64_t n = int64_t(ids.size());
    std::vector<int64_t> mask(size_t(n), 1);

    std::vector<Ort::Value> encIn;
    for (const std::string& name : m.encoderInputs)
        encIn.push_back(name == "attention_mask" ? onnx::tensor(mask, {1, n}) : onnx::tensor(ids, {1, n}));
    auto encOut = onnx::run(*m.encoder, m.encoderInputs, encIn, {m.encoderOutput});
    const auto hiddenShape = encOut[0].GetTensorTypeAndShapeInfo().GetShape();
    std::vector<float> hidden(encOut[0].GetTensorData<float>(),
                              encOut[0].GetTensorData<float>() + encOut[0].GetTensorTypeAndShapeInfo().GetElementCount());

    // Decodificación voraz: en cada paso, el token más probable (nunca el de relleno).
    std::vector<int64_t> out{kPad};
    const int64_t maxLength = std::min<int64_t>(512, 3 * n + 10);
    while (int64_t(out.size()) < maxLength) {
        std::vector<Ort::Value> decIn;
        for (const std::string& name : m.decoderInputs) {
            if (name == "encoder_attention_mask")
                decIn.push_back(onnx::tensor(mask, {1, n}));
            else if (name == "encoder_hidden_states")
                decIn.push_back(onnx::tensor(hidden, hiddenShape));
            else
                decIn.push_back(onnx::tensor(out, {1, int64_t(out.size())}));
        }
        auto decOut = onnx::run(*m.decoder, m.decoderInputs, decIn, {m.decoderOutput});
        const auto shape = decOut[0].GetTensorTypeAndShapeInfo().GetShape(); // [1, pasos, vocabulario]
        const int64_t vocabSize = shape[2];
        const float* last = decOut[0].GetTensorData<float>() + (shape[1] - 1) * vocabSize;
        int64_t best = 0;
        float bestLogit = -1e30f;
        for (int64_t v = 0; v < vocabSize; ++v)
            if (v != kPad && last[v] > bestLogit) {
                bestLogit = last[v];
                best = v;
            }
        if (best == kEos)
            break;
        out.push_back(best);
    }

    std::vector<std::string> outPieces;
    for (size_t i = 1; i < out.size(); ++i)
        if (size_t(out[i]) < m.pieces.size())
            outPieces.push_back(m.pieces[size_t(out[i])]);
    std::string result;
    m.target.Decode(outPieces, &result);
    return result;
}

} // namespace

bool translate(const std::string& modelFolder, const std::vector<std::string>& texts, std::vector<std::string>& out,
               std::string* error, const ProgressFn& progress, const CancelFn& cancel)
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
            out.push_back(text.find_first_not_of(" \t") == std::string::npos ? std::string() : translateOne(marian(), text));
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
