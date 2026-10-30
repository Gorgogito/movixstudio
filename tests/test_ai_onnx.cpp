#include "ai/EnglishG2P.h"
#include "ai/Onnx.h"
#include "ai/OnnxRewrite.h"
#include "ai/Separation.h"
#include "ai/Speech.h"
#include "ai/Translate.h"
#include "ai/Tts.h"
#include "media/AudioDecoder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

TEST_CASE("Fonemas del inglés con el léxico de Kokoro", "[ai][tts]")
{
    const auto kokoro = models() / "kokoro-82m";
    if (!std::filesystem::exists(kokoro / "us_gold.json"))
        SKIP("Faltan los modelos de prueba");
    ai::EnglishLexicon lexicon;
    std::string error;
    REQUIRE(lexicon.load(kokoro.string(), &error));
    auto word = [&](const char* w, ai::EnglishLexicon::Context ctx = {}) {
        return lexicon.word(QString::fromUtf8(w), ctx).value_or(QStringLiteral("∅")).toStdString();
    };
    const ai::EnglishLexicon::Context beforeVowel{true, false}, beforeConsonant{false, false};
    CHECK(word("hello") == "həlˈO");
    CHECK(word("Hello") == "həlˈO");
    CHECK(word("read") == "ɹˈid"); // sin etiquetas, la pronunciación habitual
    // Según lo que sigue: «my» acentuado al final, «the» y «to» ante vocal o consonante.
    CHECK(word("my") == "mˈI");
    CHECK(word("my", beforeConsonant) == "mI");
    CHECK(word("the", beforeVowel) == "ði");
    CHECK(word("the", beforeConsonant) == "ðə");
    CHECK(word("to", beforeVowel) == "tʊ");
    CHECK(word("to", beforeConsonant) == "tə");
    // Formas que no están en el léxico, con las reglas de -s, -ed e -ing.
    CHECK(word("videos") == "vˈɪdiOz");
    CHECK(word("cities") == "sˈɪɾiz");
    CHECK(word("waited") == "wˈAɾᵻd");
    CHECK(word("aardvarks") == "ˈɑɹdvˌɑɹks");
    CHECK(word("abeaming") == "əbˈimɪŋ");
    // Siglas, letra a letra.
    CHECK(word("FBI") == "ˌɛfbˌiˈI");
    CHECK(word("U.S") == "jˌuˈɛs");
    CHECK(word("zzqx") == "∅");

    // Frases: puntuación conservada, contexto de la palabra siguiente y respaldo para lo demás.
    auto fallback = [](const QString& w) { return QLatin1Char('<') + w + QLatin1Char('>'); };
    CHECK(lexicon.text(QStringLiteral("Hello, my name is Joe."), fallback).toStdString() == "həlˈO, mI nˈAm ɪz ʤˈO.");
    const std::string mixed = lexicon.text(QStringLiteral("I saw 3 zzqx in the city."), fallback).toStdString();
    INFO(mixed);
    CHECK(mixed.find("<3> <zzqx> ɪn ðə sˈɪTi.") != std::string::npos); // ɾ → T (Kokoro 1.0)
    CHECK(mixed.rfind("ˌI ", 0) == 0);

    // Con eSpeak de respaldo, desde phonemize.
    const std::string en = ai::phonemize(espeakParent(), "en-us", "Hello, world. Zzqx!", &error, kokoro.string());
    INFO(en);
    CHECK(en.rfind("həlˈO, wˈɜɹld. ", 0) == 0);
    CHECK(en.back() == '!');
    CHECK(en.size() > std::string("həlˈO, wˈɜɹld. !").size()); // eSpeak fonetizó «Zzqx»
}

// Medición (no corre con las demás): frases en inglés leídas con eSpeak solo y con el léxico; cuántas
// palabras reconoce Whisper en cada caso.
TEST_CASE("Kokoro en inglés: eSpeak frente al léxico", "[.][tts-ingles]")
{
    const auto kokoro = models() / "kokoro-82m";
    const auto whisper = models() / "ggml-base.bin";
    if (!std::filesystem::exists(kokoro / "us_gold.json") || !std::filesystem::exists(whisper))
        SKIP("Faltan los modelos de prueba");
    // El mismo modelo sin el léxico.
    QTemporaryDir bare;
    for (const auto& entry : std::filesystem::directory_iterator(kokoro))
        if (entry.path().extension() != ".json" || entry.path().filename() == "config.json")
            std::filesystem::copy_file(entry.path(), std::filesystem::path(bare.path().toStdWString()) / entry.path().filename());
    const std::vector<std::string> sentences = {
        "The quick brown fox jumps over the lazy dog.",
        "I read the book yesterday, and I will read another one tomorrow.",
        "Our new video editor exports high quality clips in seconds.",
        "She didn't know whether the weather would change by the afternoon.",
        "Thousands of people gathered in the square to celebrate the victory.",
        "Please subscribe to the channel and leave a comment below.",
        "The colonel's yacht was anchored near the island.",
        "Artificial intelligence runs entirely on your own computer.",
    };
    auto words = [](const std::string& s) {
        std::vector<std::string> out;
        std::string w;
        for (char c : s + " ") {
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'') {
                w += char(std::tolower(static_cast<unsigned char>(c)));
            } else if (!w.empty()) {
                out.push_back(w);
                w.clear();
            }
        }
        return out;
    };
    std::string error;
    ai::TranscribeOptions to;
    to.language = "en";
    ai::TtsOptions options;
    QTemporaryDir dir;
    const std::string wav = dir.filePath("voz.wav").toStdString();
    for (const char* voice : {"af_heart", "am_michael"}) {
        options.voice = voice;
        int total = 0, heardEspeak = 0, heardLexicon = 0;
        for (const std::string& sentence : sentences) {
            const auto reference = words(sentence);
            total += int(reference.size());
            for (int lexicon = 0; lexicon < 2; ++lexicon) {
                const std::string folder = lexicon ? kokoro.string() : bare.path().toStdString();
                std::vector<float> audio, pcm;
                REQUIRE(ai::synthesize(folder, espeakParent(), sentence, options, audio, &error));
                REQUIRE(ai::writeWav(wav, audio, ai::kTtsSampleRate, 1, &error));
                REQUIRE(ai::loadSpeechAudio(wav, pcm, &error));
                core::Transcript t;
                REQUIRE(ai::transcribe(whisper.string(), pcm, to, t, &error));
                std::string heard;
                for (const auto& w : t.words)
                    heard += w.text + " ";
                // Palabras de la frase que Whisper oyó, en orden (subsecuencia común más larga).
                const auto got = words(heard);
                std::vector<std::vector<int>> lcs(reference.size() + 1, std::vector<int>(got.size() + 1, 0));
                for (size_t i = 1; i <= reference.size(); ++i)
                    for (size_t j = 1; j <= got.size(); ++j)
                        lcs[i][j] = reference[i - 1] == got[j - 1] ? lcs[i - 1][j - 1] + 1 : std::max(lcs[i - 1][j], lcs[i][j - 1]);
                const int ok = lcs[reference.size()][got.size()];
                (lexicon ? heardLexicon : heardEspeak) += ok;
                std::printf("[inglés] %s %s %d/%zu: %s\n           %s\n", voice, lexicon ? "léxico" : "eSpeak", ok, reference.size(),
                            heard.c_str(), ai::phonemize(espeakParent(), "en-us", sentence, &error, folder).c_str());
            }
        }
        std::printf("[inglés] %s: eSpeak %d/%d palabras, léxico %d/%d\n", voice, heardEspeak, total, heardLexicon, total);
        CHECK(heardLexicon >= heardEspeak);
    }
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

    // En inglés, con el léxico.
    options.voice = "af_heart";
    std::vector<float> english;
    REQUIRE(ai::synthesize(kokoro.string(), espeakParent(), "The editor reads the videos and waited for the city lights.", options,
                           english, &error));
    REQUIRE(ai::writeWav(wav, english, ai::kTtsSampleRate, 1, &error));
    REQUIRE(ai::loadSpeechAudio(wav, pcm, &error));
    to.language = "en";
    REQUIRE(ai::transcribe(whisper.string(), pcm, to, t, &error));
    heard.clear();
    for (const auto& w : t.words)
        heard += lower(w.text) + " ";
    INFO("Whisper oyó: " << heard);
    for (const char* w : {"editor", "videos", "waited", "city", "lights"})
        CHECK(heard.find(w) != std::string::npos);

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
    CHECK((lower(out[0]).find("how are you") != std::string::npos || lower(out[0]).find("how you doing") != std::string::npos));
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

TEST_CASE("Opus-MT: la búsqueda en haz encuentra traducciones al menos tan probables como la voraz", "[ai][translate]")
{
    const auto esEn = models() / "opus-mt-es-en";
    const auto enEs = models() / "opus-mt-en-es";
    if (!std::filesystem::exists(esEn / "decoder.onnx") || !std::filesystem::exists(enEs / "decoder.onnx"))
        SKIP("Faltan los modelos de prueba");
    const auto [folder, texts] = GENERATE_COPY(table<std::string, std::vector<std::string>>({
        {esEn.string(),
         {
             "Si hubiera sabido que vendrías, habría preparado algo de cenar.",
             "Lo que más me gusta de este pueblo es que la gente se saluda por la calle.",
             "No es que no quiera ir, es que no me da tiempo.",
             "El montaje de un vídeo lleva más tiempo del que parece.",
             "Ella le dijo que se lo devolvería mañana sin falta.",
             "Hace un frío que pela.",
         }},
        {enEs.string(),
         {
             "If I had known you were coming, I would have cooked something.",
             "What I like most about this town is that people greet each other on the street.",
             "Editing a video always takes longer than you think.",
             "She promised to give it back tomorrow without fail.",
             "It's raining cats and dogs.",
         }},
    }));
    std::vector<std::string> greedy, beam;
    std::string error;
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(ai::translate(folder, texts, greedy, &error, {}, {}, 1));
    const double greedySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    REQUIRE(ai::translate(folder, texts, beam, &error, {}, {}, ai::kTranslationBeams));
    const double beamSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int better = 0;
    for (size_t i = 0; i < texts.size(); ++i) {
        double g = 0, b = 0;
        REQUIRE(ai::translationScore(folder, texts[i], greedy[i], g, &error));
        REQUIRE(ai::translationScore(folder, texts[i], beam[i], b, &error));
        std::printf("[traducción] %.3f %s\n             %.3f %s\n", g, greedy[i].c_str(), b, beam[i].c_str());
        INFO(texts[i]);
        CHECK(b >= g - 1e-3);
        better += b > g + 1e-3 ? 1 : 0;
    }
    std::printf("[traducción] voraz %.2f s, en haz %.2f s; %d de %zu frases mejoran\n", greedySeconds, beamSeconds, better,
                texts.size());
}

TEST_CASE("Reescritura de ONNX para la GPU con los mismos resultados", "[ai][onnx]")
{
    // Split de 17 salidas, Sum de tres entradas y ConvTranspose 1D de núcleo 128 (data/onnx/generar_reescritura.py).
    const auto path = std::filesystem::path(MOVIX_TEST_DATA_DIR) / "onnx" / "reescritura.onnx";
    std::ifstream file(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE(!original.empty());

    ai::onnx::RewriteStats stats;
    const std::string rewritten = ai::onnx::rewriteForGpu(original.data(), original.size(), 8, &stats);
    REQUIRE(!rewritten.empty());
    CHECK(stats.splits == 1);
    CHECK(stats.sums == 1);
    CHECK(stats.convTransposes == 1);
    // Si el sombreador admite búferes de sobra, el Split se queda como está.
    ai::onnx::rewriteForGpu(original.data(), original.size(), 32, &stats);
    CHECK(stats.splits == 0);
    CHECK(stats.sums == 1);
    // Lo que no es un modelo no se toca.
    CHECK(ai::onnx::rewriteForGpu("\xff\xff\xff", 3, 8).empty());

    std::vector<float> x(34 * 5);
    std::mt19937 rng(5);
    std::normal_distribution<float> normal;
    for (float& v : x)
        v = normal(rng);
    const std::vector<std::string> outputs = {"unido", "p16", "istft"};
    auto runModel = [&](Ort::Session& session) {
        std::vector<Ort::Value> inputs;
        inputs.push_back(ai::onnx::tensor(x, {1, 34, 5}));
        auto values = ai::onnx::run(session, {"x"}, inputs, outputs);
        std::vector<std::vector<float>> result;
        for (Ort::Value& v : values) {
            const float* p = v.GetTensorData<float>();
            result.emplace_back(p, p + v.GetTensorTypeAndShapeInfo().GetElementCount());
        }
        return result;
    };
    auto maxDifference = [](const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b) {
        double worst = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            REQUIRE(a[i].size() == b[i].size());
            for (size_t j = 0; j < a[i].size(); ++j)
                worst = std::max(worst, double(std::abs(a[i][j] - b[i][j])));
        }
        return worst;
    };

    Ort::SessionOptions options;
    Ort::Session reference(ai::onnx::env(), path.wstring().c_str(), options);
    Ort::Session changed(ai::onnx::env(), rewritten.data(), rewritten.size(), options);
    const auto expected = runModel(reference);
    REQUIRE(expected.size() == 3);
    CHECK(expected[2].size() == 3 * 256);
    CHECK(maxDifference(expected, runModel(changed)) < 1e-4);

    // En la GPU, openSession reescribe al cargar.
    if (ai::onnx::gpuAvailable()) {
        auto gpu = ai::onnx::openSession(path.string(), 0, true);
        const double difference = maxDifference(expected, runModel(*gpu));
        INFO("diferencia en la GPU " << difference);
        CHECK(difference < 1e-3);
    }
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
