#include "ai/Tts.h"

#include "ai/Onnx.h"

#include <espeak-ng/speak_lib.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>

namespace movix::ai {

namespace {

std::mutex g_espeakMutex;
std::string g_espeakPath;
bool g_espeakReady = false;

bool initEspeak(const std::string& dataParent, std::string* error)
{
    if (g_espeakReady && g_espeakPath == dataParent)
        return true;
    if (g_espeakReady)
        espeak_Terminate();
    g_espeakReady = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, dataParent.c_str(), espeakINITIALIZE_DONT_EXIT) > 0;
    g_espeakPath = dataParent;
    if (!g_espeakReady && error)
        *error = "No se pudo iniciar eSpeak NG (datos en " + dataParent + ").";
    return g_espeakReady;
}

// Siguiente carácter UTF-8 (como cadena) a partir de `i`.
std::string nextChar(const std::string& s, size_t& i)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    std::string ch = s.substr(i, len);
    i += len;
    return ch;
}

bool isPunctuation(const std::string& ch)
{
    static const char* const marks[] = {",", ".", ";", ":", "!", "?", "\xC2\xA1" /*¡*/, "\xC2\xBF" /*¿*/,
                                        "\xE2\x80\x94" /*—*/, "\xE2\x80\xA6" /*…*/, "\"", "(", ")",
                                        "\xE2\x80\x9C" /*“*/, "\xE2\x80\x9D" /*”*/, "\xC2\xAB" /*«*/, "\xC2\xBB" /*»*/};
    return std::any_of(std::begin(marks), std::end(marks), [&](const char* m) { return ch == m; });
}

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}

// Equivalencias de misaki (el fonetizador de Kokoro) para la salida de eSpeak con unión '^'.
void espeakToMisaki(std::string& ps, const std::string& language)
{
    static const std::pair<const char*, const char*> common[] = {
        {"a^\xC9\xAA", "I"},          // aɪ
        {"a^\xCA\x8A", "W"},          // aʊ
        {"d^z", "\xCA\xA3"},          // ʣ
        {"d^\xCA\x92", "\xCA\xA4"},   // dʒ → ʤ
        {"e^\xC9\xAA", "A"},          // eɪ
        {"o^\xCA\x8A", "O"},          // oʊ
        {"\xC9\x99^\xCA\x8A", "Q"},   // əʊ
        {"s^s", "S"},
        {"t^s", "\xCA\xA6"},          // ʦ
        {"t^\xCA\x83", "\xCA\xA7"},   // tʃ → ʧ
        {"\xC9\x94^\xC9\xAA", "Y"},   // ɔɪ
    };
    for (const auto& [from, to] : common)
        replaceAll(ps, from, to);
    if (language.rfind("en", 0) == 0) {
        // Inglés por eSpeak (Kokoro usa un léxico; esto lo aproxima).
        static const std::pair<const char*, const char*> english[] = {
            {"\xC9\x9A", "\xC9\x99\xC9\xB9"}, // ɚ → əɹ
            {"r", "\xC9\xB9"},                // r → ɹ
            {"\xC9\x90", "\xC9\x99"},         // ɐ → ə
            {"\xC9\xAC", "l"},                // ɬ → l
            {"\xCA\xB2", ""},                 // ʲ
        };
        for (const auto& [from, to] : english)
            replaceAll(ps, from, to);
        if (language == "en-us")
            replaceAll(ps, "\xCB\x90", ""); // ː (el inglés americano de Kokoro no marca vocales largas)
    }
    replaceAll(ps, "^", "");
}

// Fonemas de un tramo sin puntuación (eSpeak devuelve una cláusula por llamada).
std::string clausePhonemes(const std::string& text)
{
    std::string out;
    const void* ptr = text.c_str();
    const int mode = 0x02 | 0x80 | ('^' << 8); // IPA, con '^' uniendo los fonemas de varios símbolos
    while (ptr) {
        const char* ph = espeak_TextToPhonemes(&ptr, espeakCHARS_UTF8, mode);
        if (ph && *ph) {
            if (!out.empty())
                out += ' ';
            out += ph;
        }
    }
    return out;
}

struct KokoroModel {
    std::string folder;
    std::unique_ptr<Ort::Session> session;
    std::map<std::string, int64_t> vocab; // carácter UTF-8 → id
    std::vector<std::string> inputs;
    std::string output;
};

KokoroModel& kokoro()
{
    static KokoroModel& m = *new KokoroModel; // sin destruir al salir (ver onnx::env)
    return m;
}

bool loadKokoro(const std::string& folder, std::string* error)
{
    KokoroModel& m = kokoro();
    if (m.session && m.folder == folder)
        return true;
    try {
        QFile config(QString::fromStdString(folder + "/config.json"));
        if (!config.open(QIODevice::ReadOnly))
            throw std::runtime_error("no se encontró config.json");
        const QJsonObject vocab = QJsonDocument::fromJson(config.readAll()).object()["vocab"].toObject();
        m.vocab.clear();
        for (auto it = vocab.begin(); it != vocab.end(); ++it)
            m.vocab[it.key().toStdString()] = it.value().toInteger();
        m.session = onnx::openSession(folder + "/model.onnx");
        m.inputs = onnx::inputNames(*m.session);
        m.output = onnx::outputNames(*m.session).at(0);
        m.folder = folder;
        return true;
    } catch (const std::exception& e) {
        m.session.reset();
        if (error)
            *error = std::string("No se pudo cargar el modelo de voz: ") + e.what();
        return false;
    }
}

// Ids de los fonemas (los símbolos que el modelo no conoce se omiten).
std::vector<int64_t> tokenize(const std::string& phonemes)
{
    std::vector<int64_t> ids;
    const auto& vocab = kokoro().vocab;
    for (size_t i = 0; i < phonemes.size();) {
        const std::string ch = nextChar(phonemes, i);
        if (auto it = vocab.find(ch); it != vocab.end())
            ids.push_back(it->second);
    }
    return ids;
}

// Frases (cortando tras . ! ? … y saltos de línea); las muy largas, por comas.
std::vector<std::string> sentences(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    for (size_t i = 0; i < text.size();) {
        const std::string ch = nextChar(text, i);
        current += ch;
        const bool end = ch == "." || ch == "!" || ch == "?" || ch == "\n" || ch == "\xE2\x80\xA6";
        const bool longComma = (ch == "," || ch == ";") && current.size() > 300;
        if (end || longComma) {
            if (current.find_first_not_of(" \n\t.") != std::string::npos)
                out.push_back(current);
            current.clear();
        }
    }
    if (current.find_first_not_of(" \n\t") != std::string::npos)
        out.push_back(current);
    return out;
}

} // namespace

std::vector<Voice> ttsVoices()
{
    return {{"ef_dora", "es"}, {"em_alex", "es"}, {"em_santa", "es"}, {"af_heart", "en-us"}, {"am_michael", "en-us"}};
}

std::string phonemize(const std::string& espeakDataParent, const std::string& language, const std::string& text, std::string* error)
{
    std::lock_guard lock(g_espeakMutex);
    if (!initEspeak(espeakDataParent, error))
        return {};
    if (espeak_SetVoiceByName(language.c_str()) != EE_OK) {
        if (error)
            *error = "eSpeak NG no tiene el idioma " + language;
        return {};
    }
    // Se fonetiza cada tramo entre signos de puntuación y los signos se conservan (Kokoro los usa
    // para la entonación y las pausas).
    std::string result, chunk;
    auto flush = [&] {
        if (chunk.find_first_not_of(" \t\n") != std::string::npos) {
            if (!result.empty() && result.back() != ' ')
                result += ' ';
            result += clausePhonemes(chunk);
        }
        chunk.clear();
    };
    for (size_t i = 0; i < text.size();) {
        const std::string ch = nextChar(text, i);
        if (!isPunctuation(ch)) {
            chunk += ch;
            continue;
        }
        flush();
        if (ch == "\xC2\xA1" || ch == "\xC2\xBF") // ¡ ¿ no existen en el vocabulario de Kokoro
            continue;
        const std::string mark = ch == "\xC2\xAB" ? "\xE2\x80\x9C" : ch == "\xC2\xBB" ? "\xE2\x80\x9D" : ch;
        while (!result.empty() && result.back() == ' ' && mark != "(" && mark != "\xE2\x80\x9C")
            result.pop_back();
        result += mark;
        result += ' ';
    }
    flush();
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    espeakToMisaki(result, language);
    return result;
}

bool synthesize(const std::string& modelFolder, const std::string& espeakDataParent, const std::string& text,
                const TtsOptions& options, std::vector<float>& audio, std::string* error, const ProgressFn& progress,
                const CancelFn& cancel)
{
    audio.clear();
    const auto voices = ttsVoices();
    const auto voice = std::find_if(voices.begin(), voices.end(), [&](const Voice& v) { return v.id == options.voice; });
    if (voice == voices.end()) {
        if (error)
            *error = "Voz desconocida: " + options.voice;
        return false;
    }
    if (!loadKokoro(modelFolder, error))
        return false;

    // Estilo de la voz: 510 vectores de 256, uno por longitud de la secuencia de fonemas.
    std::vector<float> styles;
    {
        std::ifstream in(QString::fromStdString(modelFolder + "/" + voice->id + ".bin").toStdWString(), std::ios::binary);
        if (!in) {
            if (error)
                *error = "No se encontró la voz " + voice->id;
            return false;
        }
        in.seekg(0, std::ios::end);
        styles.resize(size_t(in.tellg()) / sizeof(float));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(styles.data()), std::streamsize(styles.size() * sizeof(float)));
    }
    const size_t styleCount = styles.size() / 256;

    const auto parts = sentences(text);
    if (parts.empty()) {
        if (error)
            *error = "No hay texto que leer.";
        return false;
    }
    KokoroModel& m = kokoro();
    const std::vector<float> pause(size_t(kTtsSampleRate * 0.12), 0.0f); // entre frases
    for (size_t p = 0; p < parts.size(); ++p) {
        if (cancel && cancel()) {
            if (error)
                *error = "cancelado";
            return false;
        }
        const std::string phonemes = phonemize(espeakDataParent, voice->language, parts[p], error);
        std::vector<int64_t> ids = tokenize(phonemes);
        if (ids.empty())
            continue;
        if (ids.size() > 510)
            ids.resize(510); // el contexto del modelo; las frases tan largas se cortan
        const size_t row = std::min(ids.size(), styleCount - 1);
        std::vector<float> style(styles.begin() + std::ptrdiff_t(row * 256), styles.begin() + std::ptrdiff_t((row + 1) * 256));
        ids.insert(ids.begin(), 0);
        ids.push_back(0);
        std::vector<float> speed{float(std::clamp(options.speed, 0.5, 2.0))};
        try {
            std::vector<Ort::Value> inputs;
            std::vector<std::string> names;
            for (const std::string& name : m.inputs) {
                names.push_back(name);
                if (name == "input_ids" || name == "tokens")
                    inputs.push_back(onnx::tensor(ids, {1, int64_t(ids.size())}));
                else if (name == "style" || name == "ref_s")
                    inputs.push_back(onnx::tensor(style, {1, 256}));
                else
                    inputs.push_back(onnx::tensor(speed, {1}));
            }
            auto outputs = onnx::run(*m.session, names, inputs, {m.output});
            const float* data = outputs[0].GetTensorData<float>();
            const size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (!audio.empty())
                audio.insert(audio.end(), pause.begin(), pause.end());
            audio.insert(audio.end(), data, data + count);
        } catch (const std::exception& e) {
            if (error)
                *error = std::string("La síntesis de voz falló: ") + e.what();
            return false;
        }
        if (progress)
            progress(double(p + 1) / double(parts.size()));
    }
    if (audio.empty() && error)
        *error = "No se pudo pronunciar el texto.";
    return !audio.empty();
}

bool writeWav(const std::string& path, const std::vector<float>& samples, int sampleRate, int channels, std::string* error)
{
    QFile out(QString::fromStdString(path));
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = "No se pudo escribir " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataBytes = uint32_t(samples.size() * 2);
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(uint16_t(channels));
    u32(uint32_t(sampleRate));
    u32(uint32_t(sampleRate * channels * 2));
    u16(uint16_t(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        pcm[i] = int16_t(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
    out.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * 2));
    return out.error() == QFile::NoError;
}

} // namespace movix::ai
