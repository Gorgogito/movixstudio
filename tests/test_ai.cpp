#include "ai/AiWorkerClient.h"
#include "ai/ModelCatalog.h"
#include "ai/ModelDownloader.h"
#include "ai/Speech.h"
#include "ai/WorkerProtocol.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Transcript.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <algorithm>
#include <filesystem>

using namespace movix;
using namespace movix::core;

namespace {

std::string dataPath(const char* name)
{
    return (std::filesystem::path(MOVIX_TEST_DATA_DIR) / name).string();
}

std::string modelFile(const char* name)
{
    return (std::filesystem::path(MOVIX_TEST_MODELS_DIR) / name).string();
}

TimedWord tw(double s, double e, const char* text)
{
    return {fromSeconds(s), fromSeconds(e), text, kNoId};
}

// Minúsculas y sin puntuación (para comparar lo reconocido).
std::string normalized(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (!std::ispunct(static_cast<unsigned char>(c)))
            r += char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

const TranscriptWord* findWord(const Transcript& t, std::initializer_list<const char*> variants)
{
    for (const TranscriptWord& w : t.words)
        for (const char* v : variants)
            if (normalized(w.text) == v)
                return &w;
    return nullptr;
}

std::string joined(const Transcript& t)
{
    std::string s;
    for (const TranscriptWord& w : t.words)
        s += w.text + " ";
    return s;
}

// Proyecto con un medio de 20 s con audio y su transcripción (sin archivo real).
struct WordsFixture {
    Project p = Project::createDefault();
    Id media = kNoId;

    WordsFixture()
    {
        MediaItem m;
        m.path = "voz.wav";
        m.name = "voz";
        m.duration = 20 * kUsPerSecond;
        m.hasVideo = m.hasAudio = true;
        m.width = 1280;
        m.height = 720;
        m.frameRate = 30;
        media = edit::addMedia(p, m);
        auto t = std::make_shared<Transcript>();
        t->language = "es";
        // Una palabra por segundo: "w0" en [0, 0.5), "w1" en [1, 1.5)...
        for (int i = 0; i < 20; ++i)
            t->words.push_back({i * kUsPerSecond, i * kUsPerSecond + 500'000, "w" + std::to_string(i), 0.5f});
        edit::setTranscript(p, media, t);
    }
};

} // namespace

TEST_CASE("Subtítulos: agrupación por pausas, frases y longitud", "[ai][subtitles]")
{
    std::vector<TimedWord> words{
        tw(0.0, 0.3, "Hola,"), tw(0.35, 0.6, "esto"), tw(0.65, 0.8, "es"), tw(0.85, 1.2, "una"), tw(1.25, 1.7, "prueba."),
        tw(1.8, 2.1, "Sigue"), tw(2.15, 2.5, "aquí"),
        tw(4.0, 4.4, "Tras"), tw(4.45, 4.9, "pausa"),
    };
    const auto cues = makeCues(words);
    REQUIRE(cues.size() == 3);
    CHECK(cues[0].text == "Hola, esto es una prueba.");
    CHECK(cues[0].start == fromSeconds(0.0));
    CHECK(cues[0].end == fromSeconds(1.7));
    CHECK(cues[1].text == "Sigue aquí");
    CHECK(cues[1].end == fromSeconds(2.6)); // 0,8 s como mínimo
    CHECK(cues[2].text == "Tras pausa");
    CHECK(cues[2].start == fromSeconds(4.0));

    // Un texto largo se parte en dos líneas equilibradas y nunca supera el máximo de caracteres.
    std::vector<TimedWord> many;
    for (int i = 0; i < 40; ++i)
        many.push_back(tw(i * 0.2, i * 0.2 + 0.15, i % 2 ? "palabra" : "otra"));
    CueOptions o;
    o.maxDuration = 60 * kUsPerSecond;
    const auto longCues = makeCues(many, o);
    CHECK(longCues.size() >= 3);
    for (const Cue& c : longCues) {
        INFO("Subtítulo: [" << c.text << "]");
        const auto nl = c.text.find('\n');
        if (c.text.size() <= 42) {
            CHECK(nl == std::string::npos);
            continue;
        }
        REQUIRE(nl != std::string::npos);
        CHECK(c.text.find('\n', nl + 1) == std::string::npos);
        CHECK(nl <= 42);
        CHECK(c.text.size() - nl - 1 <= 42);
        CHECK(std::abs(int(nl) - int(c.text.size() - nl - 1)) <= 10);
    }

    // Duración mínima sin pisar el siguiente.
    const auto shortCues = makeCues({tw(0.0, 0.2, "Sí."), tw(0.5, 0.7, "No.")});
    REQUIRE(shortCues.size() == 2);
    CHECK(shortCues[0].end == shortCues[1].start);
    CHECK(shortCues[1].end == fromSeconds(0.5) + o.minDuration);

    CHECK(balanceLines("corto", 42, 2) == "corto");
    CHECK(balanceLines("uno dos tres cuatro", 10, 2) == "uno dos\ntres cuatro");
}

TEST_CASE("SRT: escritura y lectura", "[ai][subtitles]")
{
    const std::vector<Cue> cues{{fromSeconds(1.5), fromSeconds(3.25), "Hola\nmundo"},
                                {fromSeconds(3661.001), fromSeconds(3662), "Más tarde"}};
    const std::string srt = toSrt(cues);
    CHECK(srt == "1\n00:00:01,500 --> 00:00:03,250\nHola\nmundo\n\n2\n01:01:01,001 --> 01:01:02,000\nMás tarde\n\n");
    CHECK(parseSrt(srt) == cues);

    // BOM, CRLF, sin números, punto decimal y un bloque roto en medio.
    const std::string messy = "\xEF\xBB\xBF" "00:00:01.500 --> 00:00:03.250\r\nHola\r\nmundo\r\n\r\n"
                              "x\r\nbasura --> nada\r\n\r\n7\r\n01:01:01,001 --> 01:01:02,000 X1:0\r\nMás tarde\r\n";
    CHECK(parseSrt(messy) == cues);
}

TEST_CASE("Palabras de la transcripción en la línea de tiempo", "[ai][subtitles]")
{
    WordsFixture f;
    auto& p = f.p;
    const auto& s = p.settings; // 30 fps
    // Clip de video (con su audio) desde el segundo 2 del medio, colocado en el segundo 10.
    const Id v = edit::placeClip(p, p.videoTracks[0].id, f.media, 300, 2 * kUsPerSecond, 90);
    REQUIRE(v != kNoId);
    auto words = timelineWords(p);
    REQUIRE(words.size() == 3); // w2, w3, w4
    CHECK(words[0].text == "w2");
    CHECK(words[0].start == s.frameToTime(300));
    CHECK(words[0].clipId == v);
    CHECK(words[2].text == "w4");
    CHECK(words[2].start == s.frameToTime(300) + 2 * kUsPerSecond);

    // A doble velocidad caben el doble de palabras y se acercan entre sí.
    REQUIRE(edit::setClipSpeed(p, v, 2.0));
    words = timelineWords(p);
    REQUIRE(words.size() == 3); // la duración se reduce a la mitad: 45 fotogramas = 3 s de medio
    CHECK(words[1].start - words[0].start == 500'000);

    // Pista silenciada o audio separado: el audio de la pista de video no cuenta.
    REQUIRE(edit::setTrackMuted(p, p.videoTracks[0].id, true));
    CHECK(timelineWords(p).empty());
    REQUIRE(edit::setTrackMuted(p, p.videoTracks[0].id, false));
    REQUIRE(edit::setClipSpeed(p, v, 1.0));
    const Id a = edit::detachAudio(p, v);
    REQUIRE(a != kNoId);
    words = timelineWords(p);
    REQUIRE(words.size() == 3);
    CHECK(words[0].clipId == a); // ahora suena por el clip de audio
}

TEST_CASE("Pista de subtítulos y transcripción en el archivo de proyecto", "[ai][subtitles]")
{
    WordsFixture f;
    auto& p = f.p;
    edit::placeClip(p, p.videoTracks[0].id, f.media, 0, 0, 150);
    CueOptions options;
    options.maxGap = 300'000; // una palabra por subtítulo
    const auto cues = makeCues(timelineWords(p), options);
    REQUIRE(cues.size() == 5);
    const size_t before = p.videoTracks.size();
    const Id track = edit::addSubtitleTrack(p, cues, subtitleStyle(), "Subtítulos");
    REQUIRE(p.videoTracks.size() == before + 1);
    CHECK(p.videoTracks.back().id == track);
    const Track* t = p.findTrack(track);
    CHECK(t->name == "Subtítulos");
    REQUIRE(t->clips.size() == 5);
    CHECK(t->clips[0].title->text == "w0");
    CHECK(t->clips[0].start == 0);
    CHECK(t->clips[0].duration == 24); // 0,8 s de duración mínima a 30 fps
    CHECK(t->clips[1].start == 30);
    CHECK(t->clips[0].transform.y.value == Catch::Approx(kSubtitleOffsetY));
    CHECK(cuesOfTrack(p, *t).size() == 5);
    CHECK(cuesOfTrack(p, *t)[1].text == "w1");
    std::string why;
    CHECK(p.validate(&why));

    QTemporaryDir dir;
    const QString file = dir.filePath("p.mxproj");
    REQUIRE(saveProjectFile(p, file));
    const LoadResult loaded = loadProjectFile(file);
    REQUIRE(loaded.project);
    const MediaItem* m = loaded.project->findMedia(f.media);
    REQUIRE(m->transcript);
    CHECK(m->transcript->language == "es");
    CHECK(m->transcript->words.size() == 20);
    CHECK(*loaded.project == p);

    // Quitar la transcripción.
    CHECK(edit::setTranscript(p, f.media, nullptr));
    CHECK_FALSE(p.findMedia(f.media)->transcript);
    CHECK_FALSE(edit::setTranscript(p, f.media, nullptr));
}

TEST_CASE("Descarga de modelos verificada con SHA-256", "[ai][models]")
{
    QTemporaryDir dir;
    // Dos archivos de origen (un modelo puede tener varios).
    auto makeSource = [&](const QString& name, int size, int seed) {
        QByteArray payload(size, '\0');
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = char(i * seed + i / 255);
        QFile out(dir.filePath(name));
        REQUIRE(out.open(QIODevice::WriteOnly));
        out.write(payload);
        return ai::ModelFile{name + ".descargado", QUrl::fromLocalFile(dir.filePath(name)).toString(),
                             QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()),
                             payload.size()};
    };
    ai::ModelInfo model;
    model.id = "prueba";
    model.files = {makeSource("a.bin", 300'000, 7), makeSource("b.bin", 120'000, 13)};
    const QString folder = dir.filePath("modelos");

    ai::ModelDownloader downloader;
    QSignalSpy finished(&downloader, &ai::ModelDownloader::finished);
    QSignalSpy progress(&downloader, &ai::ModelDownloader::progress);
    REQUIRE(downloader.start(model, folder));
    REQUIRE(finished.wait(10000));
    CHECK(finished.at(0).at(1).toBool());
    CHECK(QFileInfo(folder + "/a.bin.descargado").size() == 300'000);
    CHECK(QFileInfo(folder + "/b.bin.descargado").size() == 120'000);
    CHECK_FALSE(QFileInfo::exists(folder + "/a.bin.descargado.part"));
    REQUIRE_FALSE(progress.isEmpty());
    CHECK(progress.last().at(1).toLongLong() == 420'000); // progreso del modelo completo
    CHECK(progress.last().at(2).toLongLong() == 420'000);

    // Lo ya descargado no se repite: con un archivo ya completo solo se baja el otro.
    QFile::remove(folder + "/b.bin.descargado");
    progress.clear();
    REQUIRE(downloader.start(model, folder));
    REQUIRE(finished.wait(10000));
    CHECK(finished.at(1).at(1).toBool());
    CHECK(QFileInfo(folder + "/b.bin.descargado").size() == 120'000);

    // Un hash distinto se rechaza y no deja archivos.
    model.files[1].sha256 = QString(64, QLatin1Char('0'));
    model.files[1].name = "malo.bin";
    REQUIRE(downloader.start(model, folder));
    REQUIRE(finished.wait(10000));
    CHECK_FALSE(finished.at(2).at(1).toBool());
    CHECK_FALSE(QFileInfo::exists(folder + "/malo.bin"));
    CHECK_FALSE(QFileInfo::exists(folder + "/malo.bin.part"));

    // El catálogo tiene modelos de cada tarea, todos con hash, tamaño y URL de descarga segura.
    CHECK(ai::findModel(ai::defaultTranscriptionModel()));
    CHECK(ai::findModel(ai::defaultVadModel()));
    CHECK(ai::findModel(ai::speechModel()));
    CHECK(ai::findModel(ai::separationModel()));
    CHECK(ai::translationModel("es", "en") == "opus-mt-es-en");
    CHECK(ai::translationModel("en", "es") == "opus-mt-en-es");
    CHECK(ai::translationModel("es", "fr").isEmpty());
    for (const ai::ModelInfo& m : ai::modelCatalog()) {
        REQUIRE_FALSE(m.files.empty());
        for (const ai::ModelFile& f : m.files) {
            INFO(m.id.toStdString() << "/" << f.name.toStdString());
            CHECK(f.sha256.size() == 64);
            CHECK(f.size > 0);
            CHECK(f.url.startsWith("https://"));
        }
    }
}

TEST_CASE("Audio a 16 kHz mono para el reconocimiento", "[ai][speech]")
{
    std::vector<float> pcm;
    std::string error;
    REQUIRE(ai::loadSpeechAudio(dataPath("speech_es.flac"), pcm, &error));
    CHECK(double(pcm.size()) / ai::kSpeechSampleRate == Catch::Approx(9.765).margin(0.02));
    float peak = 0;
    for (float v : pcm)
        peak = std::max(peak, std::abs(v));
    CHECK(peak > 0.1f);
    CHECK_FALSE(ai::loadSpeechAudio("no-existe.wav", pcm, &error));
}

TEST_CASE("Whisper transcribe la voz con tiempos por palabra", "[ai][speech][whisper]")
{
    const std::string model = modelFile("ggml-base.bin");
    const std::string vad = modelFile("ggml-silero-v6.2.0.bin");
    if (!std::filesystem::exists(model) || !std::filesystem::exists(vad))
        SKIP("Faltan los modelos de prueba en " << MOVIX_TEST_MODELS_DIR << " (sin red al compilar las pruebas)");
    std::vector<float> pcm;
    REQUIRE(ai::loadSpeechAudio(dataPath("speech_es.flac"), pcm));

    Transcript t;
    std::string error;
    double lastProgress = -1;
    ai::TranscribeOptions options; // idioma automático
    REQUIRE(ai::transcribe(model, pcm, options, t, &error, [&](double v) { lastProgress = v; }));
    INFO("Transcripción: " << joined(t));
    CHECK(t.language == "es");
    CHECK(lastProgress == Catch::Approx(1.0));
    REQUIRE(t.words.size() >= 12);
    CHECK(std::is_sorted(t.words.begin(), t.words.end(), [](auto& a, auto& b) { return a.start < b.start; }));
    // El modelo base reconoce bien la mayoría de palabras (no todas: confunde alguna "í").
    const TranscriptWord* hola = findWord(t, {"hola"});
    const TranscriptWord* prueba = findWord(t, {"prueba"});
    const TranscriptWord* generan = findWord(t, {"generan"});
    const TranscriptWord* automatica = findWord(t, {"automática", "automáticamente"});
    REQUIRE(hola);
    REQUIRE(prueba);
    REQUIRE(generan);
    REQUIRE(automatica);
    CHECK(findWord(t, {"editor"}));
    CHECK(findWord(t, {"forma"}));

    // Sin ajuste, DTW ya sitúa cada palabra en su frase (voz en 0,6–3,2 s y 6,1–8,5 s).
    auto inSpeech = [](const TranscriptWord& w, double tolerance) {
        const double s = toSeconds(w.start), e = toSeconds(w.end);
        const bool first = s > 0.6 - tolerance && e < 3.17 + tolerance;
        const bool second = s > 6.07 - tolerance && e < 8.49 + tolerance;
        return first || second;
    };
    for (const TranscriptWord& w : t.words) {
        INFO(w.text << " " << toSeconds(w.start) << "–" << toSeconds(w.end));
        CHECK(inSpeech(w, 0.45));
    }

    // Con los tramos de voz, los bordes se ajustan: ninguna palabra pisa el silencio.
    ai::VadOptions vadOptions;
    vadOptions.minSilenceMs = 200;
    vadOptions.padMs = 60;
    REQUIRE(ai::detectSpeech(vad, pcm, vadOptions, options.speech));
    options.language = "es";
    REQUIRE(ai::transcribe(model, pcm, options, t, &error));
    CHECK(t.language == "es");
    hola = findWord(t, {"hola"});
    generan = findWord(t, {"generan"});
    REQUIRE(hola);
    REQUIRE(generan);
    CHECK(toSeconds(hola->start) == Catch::Approx(0.6).margin(0.15));
    const TranscriptWord* los = &*std::find_if(t.words.begin(), t.words.end(), [](auto& w) { return w.start > fromSeconds(4); });
    CHECK(toSeconds(los->start) == Catch::Approx(6.07).margin(0.15));
    for (const TranscriptWord& w : t.words) {
        INFO(w.text << " " << toSeconds(w.start) << "–" << toSeconds(w.end));
        CHECK(inSpeech(w, 0.15)); // el VAD añade 60 ms de margen
        CHECK(w.end > w.start);
        CHECK(w.probability > 0.0f);
    }

    // Cancelación.
    CHECK_FALSE(ai::transcribe(model, pcm, options, t, &error, {}, [] { return true; }));
}

TEST_CASE("Silero VAD encuentra los tramos con voz", "[ai][speech][vad]")
{
    const std::string model = modelFile("ggml-silero-v6.2.0.bin");
    if (!std::filesystem::exists(model))
        SKIP("Falta el modelo " << model);
    std::vector<float> pcm;
    REQUIRE(ai::loadSpeechAudio(dataPath("speech_es.flac"), pcm));
    std::vector<ai::SpeechSpan> spans;
    ai::VadOptions options;
    options.minSilenceMs = 600; // las pausas entre palabras no parten la frase
    REQUIRE(ai::detectSpeech(model, pcm, options, spans));
    REQUIRE(spans.size() == 2);
    CHECK(toSeconds(spans[0].start) == Catch::Approx(0.6).margin(0.35));
    CHECK(toSeconds(spans[0].end) == Catch::Approx(3.17).margin(0.35));
    CHECK(toSeconds(spans[1].start) == Catch::Approx(6.07).margin(0.35));
    CHECK(toSeconds(spans[1].end) == Catch::Approx(8.49).margin(0.35));
}

TEST_CASE("Proceso de IA: trabajos, progreso, cancelación y caída", "[ai][worker]")
{
    const std::string model = modelFile("ggml-base.bin");
    const std::string vad = modelFile("ggml-silero-v6.2.0.bin");
    if (!std::filesystem::exists(model) || !std::filesystem::exists(vad))
        SKIP("Faltan los modelos de prueba");

    ai::AiWorkerClient client(QStringLiteral(MOVIX_AI_WORKER));
    QSignalSpy finished(&client, &ai::AiWorkerClient::finished);
    QSignalSpy failed(&client, &ai::AiWorkerClient::failed);
    QSignalSpy progress(&client, &ai::AiWorkerClient::progress);
    const QString media = QString::fromStdString(dataPath("speech_es.flac"));

    // Dos trabajos en cola: se ejecutan en orden.
    const int transcribeJob = client.submit({{"type", ai::protocol::kTranscribe},
                                             {"media", media},
                                             {"model", QString::fromStdString(model)},
                                             {"modelName", "whisper-base"},
                                             {"vadModel", QString::fromStdString(vad)},
                                             {"language", "auto"}});
    const int vadJob = client.submit({{"type", ai::protocol::kDetectSpeech},
                                      {"media", media},
                                      {"model", QString::fromStdString(vad)},
                                      {"minSilenceMs", 600}});
    QElapsedTimer timer;
    timer.start();
    while (finished.size() + failed.size() < 2 && timer.elapsed() < 120000)
        finished.wait(500);
    REQUIRE(failed.isEmpty());
    REQUIRE(finished.size() == 2);
    CHECK(finished.at(0).at(0).toInt() == transcribeJob);
    CHECK(finished.at(1).at(0).toInt() == vadJob);
    const Transcript t = transcriptFromJson(finished.at(0).at(1).toJsonObject()["transcript"].toObject());
    CHECK(t.language == "es");
    CHECK(t.model == "whisper-base");
    CHECK(findWord(t, {"prueba"}));
    CHECK(finished.at(1).at(1).toJsonObject()["speech"].toArray().size() == 2);
    CHECK(progress.size() > 3);
    CHECK_FALSE(client.hasJobs());

    // Cancelar un trabajo en curso.
    finished.clear();
    const int canceled = client.submit({{"type", ai::protocol::kTranscribe},
                                        {"media", media},
                                        {"model", QString::fromStdString(model)}});
    REQUIRE(QTest::qWaitFor([&] { return !progress.isEmpty() && progress.last().at(0).toInt() == canceled; }, 20000));
    client.cancel(canceled);
    REQUIRE(failed.wait(20000));
    CHECK(failed.last().at(0).toInt() == canceled);
    CHECK(failed.last().at(2).toBool());
    CHECK(finished.isEmpty());

    // Un medio inexistente falla con un mensaje, sin tumbar el proceso.
    client.submit({{"type", ai::protocol::kDetectSpeech}, {"media", "no-existe.wav"}, {"model", QString::fromStdString(vad)}});
    REQUIRE(failed.wait(20000));
    CHECK_FALSE(failed.last().at(2).toBool());
    CHECK_FALSE(failed.last().at(1).toString().isEmpty());

    // Un programa que no existe informa del fallo en diferido.
    ai::AiWorkerClient broken(QStringLiteral("no-existe-movix-ai-worker.exe"));
    QSignalSpy brokenFailed(&broken, &ai::AiWorkerClient::failed);
    broken.submit({{"type", ai::protocol::kDetectSpeech}});
    REQUIRE(brokenFailed.wait(5000));
}

TEST_CASE("Eliminar tramos de la línea de tiempo cierra los huecos en todas las pistas", "[ai][ranges]")
{
    WordsFixture f;
    auto& p = f.p;
    // V1: clip de 0 a 300 (10 s). A1: clip de 30 a 150. V2 bloqueada con un título de 0 a 300.
    const Id v = edit::placeClip(p, p.videoTracks[0].id, f.media, 0, 0, 300);
    const Id a = edit::placeClip(p, p.audioTracks[0].id, f.media, 30, 0, 120);
    edit::placeTitle(p, p.videoTracks[1].id, 0, 300, TitleStyle{});
    REQUIRE(edit::setTrackLocked(p, p.videoTracks[1].id, true));
    REQUIRE(v != kNoId);
    REQUIRE(a != kNoId);

    // Tramos desordenados y solapados: [60, 90) ∪ [80, 100) = [60, 100); y [200, 230).
    const FramePos removed = edit::removeRanges(p, {{200, 230}, {60, 90}, {80, 100}, {500, 400}});
    CHECK(removed == 70);
    std::string why;
    REQUIRE(p.validate(&why));
    const Track& v1 = p.videoTracks[0];
    REQUIRE(v1.clips.size() == 3);
    CHECK(v1.clips[0].start == 0);
    CHECK(v1.clips[0].duration == 60);
    CHECK(v1.clips[1].start == 60);
    CHECK(v1.clips[1].duration == 100);
    CHECK(v1.clips[1].sourceIn == p.settings.frameToTime(100)); // continúa donde terminó el corte
    CHECK(v1.clips[2].start == 160);
    CHECK(v1.clips[2].sourceIn == p.settings.frameToTime(230));
    CHECK(v1.clips[2].end() == 230);
    // A1 (30–150): se corta 60–100 y lo demás se adelanta 40.
    const Track& a1 = p.audioTracks[0];
    REQUIRE(a1.clips.size() == 2);
    CHECK(a1.clips[0].start == 30);
    CHECK(a1.clips[0].end() == 60);
    CHECK(a1.clips[1].start == 60);
    CHECK(a1.clips[1].end() == 110);
    // La pista bloqueada no cambia.
    REQUIRE(p.videoTracks[1].clips.size() == 1);
    CHECK(p.videoTracks[1].clips[0].duration == 300);

    CHECK(edit::removeRanges(p, {}) == 0);
    CHECK(edit::removeRanges(p, {{1000, 1100}}) == 0); // más allá del final: nada que quitar
}

TEST_CASE("Silencios de la línea de tiempo a partir de los tramos con voz", "[ai][ranges]")
{
    WordsFixture f;
    auto& p = f.p; // 30 fps
    // Clip desde el segundo 1 del medio, colocado en el fotograma 60 (2 s), de 8 s.
    edit::placeClip(p, p.videoTracks[0].id, f.media, 60, kUsPerSecond, 240);
    // Voz del medio: 1,5–3 s y 6–6,3 s y 8–20 s (en tiempo del medio).
    std::map<Id, std::vector<SpeechRange>> speech{
        {f.media, {{fromSeconds(1.5), fromSeconds(3.0)}, {fromSeconds(6.0), fromSeconds(6.3)}, {fromSeconds(8.0), fromSeconds(20.0)}}}};
    // En la línea de tiempo: voz 2,5–4 s, 7–7,3 s y 9–10 s; silencios 0–2,5 (empieza en el hueco
    // inicial), 4–7 y 7,3–9.
    auto silences = silentRanges(p, speech, 500'000, 100'000);
    REQUIRE(silences.size() == 3);
    CHECK(silences[0] == FrameRange{0, 72});   // 0 → 2,4 s (sin margen al principio)
    CHECK(silences[1] == FrameRange{123, 207}); // 4,1 → 6,9 s
    CHECK(silences[2] == FrameRange{222, 267}); // 7,4 → 8,9 s

    // Con silencio mínimo de 2 s solo quedan los largos.
    silences = silentRanges(p, speech, 2 * kUsPerSecond, 100'000);
    REQUIRE(silences.size() == 2);
    CHECK(silences[1] == FrameRange{123, 207});

    // Un medio no analizado (p. ej. música) no aporta voz: todo es silencio.
    CHECK(silentRanges(p, {}, 500'000, 0) == std::vector<FrameRange>{{0, 300}});

    // Cortar los silencios deja solo la voz, seguida.
    REQUIRE(edit::removeRanges(p, silentRanges(p, speech, 500'000, 100'000)) == 72 + 84 + 45);
    CHECK(p.duration() == 300 - 201);
    CHECK(p.videoTracks[0].clips.size() == 3);

    // Rango de unas palabras: del inicio de la primera al final de la última, a fotogramas completos.
    CHECK(wordsRange(p, {tw(1.01, 1.2, "a"), tw(1.3, 1.51, "b")}) == FrameRange{30, 46});
    CHECK(wordsRange(p, {}) == FrameRange{});
}
