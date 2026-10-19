#include "ai/Separation.h"
#include "ai/Speech.h"
#include "ai/Translate.h"
#include "ai/Tts.h"
#include "media/AudioDecoder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>

using namespace movix;

namespace {

std::filesystem::path models()
{
    return std::filesystem::path(MOVIX_TEST_MODELS_DIR);
}

// La carpeta que contiene espeak-ng-data (se copia junto a los ejecutables).
std::string espeakParent()
{
    return QCoreApplication::applicationDirPath().toStdString();
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

double rms(const std::vector<float>& v, size_t from, size_t to, size_t stride = 1, size_t offset = 0)
{
    double sum = 0;
    size_t n = 0;
    for (size_t i = from * stride + offset; i < std::min(v.size(), to * stride); i += stride, ++n)
        sum += double(v[i]) * v[i];
    return n ? std::sqrt(sum / double(n)) : 0.0;
}

double correlation(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to, size_t stride = 1)
{
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = from * stride; i < std::min({a.size(), b.size(), to * stride}); i += stride) {
        ab += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    return ab / std::sqrt(std::max(aa * bb, 1e-20));
}

} // namespace

TEST_CASE("Fonemas con eSpeak NG en el formato de Kokoro", "[ai][tts]")
{
    std::string error;
    const std::string es = ai::phonemize(espeakParent(), "es", "¡Hola, esto es una prueba!", &error);
    INFO(error << " → " << es);
    CHECK(es == "ˈola, ˈesto ˈes ˈuna pɾuˈeβa!");
    const std::string en = ai::phonemize(espeakParent(), "en-us", "Hello, my name is Joe.", &error);
    INFO(en);
    CHECK(en.find("həlˈO,") != std::string::npos); // oʊ → O
    CHECK(en.find(" mI ") != std::string::npos);   // aɪ → I ("my" sin acento)
    CHECK(en.back() == '.');
    CHECK(ai::phonemize(espeakParent(), "xx-no-existe", "hola", &error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Kokoro sintetiza voz que Whisper entiende", "[ai][tts]")
{
    const auto kokoro = models() / "kokoro-82m";
    const auto whisper = models() / "ggml-base.bin";
    if (!std::filesystem::exists(kokoro / "model.onnx") || !std::filesystem::exists(whisper))
        SKIP("Faltan los modelos de prueba");
    std::vector<float> audio;
    std::string error;
    ai::TtsOptions options; // ef_dora
    double lastProgress = 0;
    const std::string text = "Hola, esto es una prueba de síntesis de voz. El editor habla español.";
    REQUIRE(ai::synthesize(kokoro.string(), espeakParent(), text, options, audio, &error, [&](double p) { lastProgress = p; }));
    INFO(error);
    const double seconds = double(audio.size()) / ai::kTtsSampleRate;
    CHECK(seconds > 2.5);
    CHECK(seconds < 9.0);
    CHECK(lastProgress == Catch::Approx(1.0));
    CHECK(rms(audio, 0, audio.size()) > 0.02);

    // Más despacio dura más.
    std::vector<float> slow;
    options.speed = 0.7;
    REQUIRE(ai::synthesize(kokoro.string(), espeakParent(), "Hola, esto es una prueba.", options, slow, &error));
    std::vector<float> normal;
    options.speed = 1.0;
    REQUIRE(ai::synthesize(kokoro.string(), espeakParent(), "Hola, esto es una prueba.", options, normal, &error));
    CHECK(double(slow.size()) > 1.2 * double(normal.size()));

    // Ida y vuelta: se guarda como WAV, se vuelve a leer a 16 kHz y Whisper reconoce el texto.
    QTemporaryDir dir;
    const std::string wav = dir.filePath("voz.wav").toStdString();
    REQUIRE(ai::writeWav(wav, audio, ai::kTtsSampleRate, 1, &error));
    std::vector<float> pcm;
    REQUIRE(ai::loadSpeechAudio(wav, pcm, &error));
    core::Transcript t;
    ai::TranscribeOptions to;
    to.language = "es";
    REQUIRE(ai::transcribe(whisper.string(), pcm, to, t, &error));
    std::string heard;
    for (const auto& w : t.words)
        heard += lower(w.text) + " ";
    INFO("Whisper oyó: " << heard);
    CHECK(heard.find("hola") != std::string::npos);
    CHECK(heard.find("prueba") != std::string::npos);
    CHECK(heard.find("voz") != std::string::npos);
    CHECK(heard.find("editor") != std::string::npos);

    // Voz desconocida y cancelación.
    options.voice = "zz_nadie";
    CHECK_FALSE(ai::synthesize(kokoro.string(), espeakParent(), text, options, audio, &error));
    options.voice = "ef_dora";
    CHECK_FALSE(ai::synthesize(kokoro.string(), espeakParent(), text, options, audio, &error, {}, [] { return true; }));
}

TEST_CASE("Opus-MT traduce entre español e inglés", "[ai][translate]")
{
    const auto esEn = models() / "opus-mt-es-en";
    const auto enEs = models() / "opus-mt-en-es";
    if (!std::filesystem::exists(esEn / "decoder.onnx") || !std::filesystem::exists(enEs / "decoder.onnx"))
        SKIP("Faltan los modelos de prueba");
    std::vector<std::string> out;
    std::string error;
    REQUIRE(ai::translate(esEn.string(), {"Hola, ¿cómo estás?", "El gato duerme en la casa.", "", "Buenos\ndías."}, out, &error));
    REQUIRE(out.size() == 4);
    INFO(out[0] << " | " << out[1] << " | " << out[3]);
    CHECK(lower(out[0]).find("how are you") != std::string::npos);
    CHECK(lower(out[1]).find("cat") != std::string::npos);
    CHECK(lower(out[1]).find("house") != std::string::npos);
    CHECK(out[2].empty());
    CHECK(lower(out[3]).find("good morning") != std::string::npos);

    REQUIRE(ai::translate(enEs.string(), {"Good morning, my friend.", "I like to edit videos."}, out, &error));
    REQUIRE(out.size() == 2);
    INFO(out[0] << " | " << out[1]);
    CHECK(out[0].find("Buenos d") != std::string::npos);
    CHECK(lower(out[0]).find("amigo") != std::string::npos);
    CHECK(lower(out[1]).find("v\xC3\xAD""deos") != std::string::npos); // vídeos

    CHECK_FALSE(ai::translate((models() / "no-existe").string(), {"hola"}, out, &error));
}

TEST_CASE("HTDemucs separa la voz de la percusión", "[ai][separation]")
{
    const auto model = models() / "htdemucs" / "htdemucs.onnx";
    if (!std::filesystem::exists(model))
        SKIP("Falta el modelo de prueba");
    constexpr int rate = ai::kSeparationSampleRate;
    // Voz (la de prueba) + percusión sintética (bombo y charles a 120 BPM), en estéreo a 44,1 kHz.
    std::vector<float> voice;
    {
        media::AudioDecoder dec;
        REQUIRE(dec.open((std::filesystem::path(MOVIX_TEST_DATA_DIR) / "speech_es.flac").string(), rate, 2));
        media::AudioChunk chunk;
        while (dec.read(chunk))
            voice.insert(voice.end(), chunk.samples.begin(), chunk.samples.end());
    }
    const size_t frames = voice.size() / 2;
    std::vector<float> drums(voice.size(), 0.0f);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (double t = 0.25; t < double(frames) / rate - 0.3; t += 0.25) {
        const bool kick = std::fmod(t - 0.25, 0.5) < 1e-6;
        const size_t start = size_t(t * rate);
        for (size_t i = 0; i < size_t(0.2 * rate) && start + i < frames; ++i) {
            const double s = double(i) / rate;
            const float v = kick ? float(0.6 * std::sin(2 * std::numbers::pi * 55 * s) * std::exp(-s / 0.07))
                                 : float(0.15 * noise(rng) * std::exp(-s / 0.02));
            drums[2 * (start + i)] += v;
            drums[2 * (start + i) + 1] += v;
        }
    }
    std::vector<float> mix(voice.size());
    for (size_t i = 0; i < mix.size(); ++i)
        mix[i] = voice[i] + drums[i];

    ai::Stems stems;
    std::string error;
    double lastProgress = 0;
    REQUIRE(ai::separate(model.string(), mix, stems, &error, [&](double p) { lastProgress = p; }));
    INFO(error);
    REQUIRE(stems.vocals.size() == mix.size());
    REQUIRE(stems.accompaniment.size() == mix.size());
    CHECK(lastProgress == Catch::Approx(1.0));

    // En la pausa de la voz (3,6–5,6 s) la pista de voz casi no tiene percusión...
    const size_t a = size_t(3.6 * rate), b = size_t(5.6 * rate);
    const double drumsInPause = rms(drums, a, b, 2);
    const double vocalsInPause = rms(stems.vocals, a, b, 2);
    INFO("percusión " << drumsInPause << ", voz separada en la pausa " << vocalsInPause);
    CHECK(vocalsInPause < 0.2 * drumsInPause);
    // ...y en las frases (0,6–3,1 s y 6,1–8,4 s) la voz separada se parece a la original y el
    // acompañamiento, a la percusión.
    for (auto [from, to] : {std::pair{0.6, 3.1}, std::pair{6.1, 8.4}}) {
        const double cv = correlation(stems.vocals, voice, size_t(from * rate), size_t(to * rate), 2);
        const double cd = correlation(stems.accompaniment, drums, size_t(from * rate), size_t(to * rate), 2);
        INFO("correlación voz " << cv << ", percusión " << cd);
        CHECK(cv > 0.8);
        CHECK(cd > 0.8);
    }
}
