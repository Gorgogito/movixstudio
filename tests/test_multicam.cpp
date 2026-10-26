#include "ProjectController.h"
#include "TestMedia.h"
#include "ai/AudioSync.h"
#include "core/Edits.h"
#include "core/Otio.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "render/OffscreenRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <random>

using namespace movix;
using namespace movix::core;
using Catch::Approx;

namespace {

// Señal parecida a la voz: ruido con una envolvente de sílabas irregular.
std::vector<float> speechLike(int seconds, int rate, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::uniform_real_distribution<float> syllable(0.08f, 0.3f);
    std::vector<float> out(size_t(seconds) * size_t(rate));
    size_t i = 0;
    while (i < out.size()) {
        const size_t len = size_t(syllable(rng) * float(rate));
        const float level = std::uniform_real_distribution<float>(0.0f, 0.5f)(rng);
        for (size_t k = 0; k < len && i < out.size(); ++k, ++i)
            out[i] = level * std::sin(float(k) / float(len) * 3.14159f) * noise(rng);
    }
    return out;
}

MediaItem fakeMedia(const char* name, double seconds, bool video, bool audio)
{
    MediaItem m;
    m.path = name;
    m.name = name;
    m.duration = TimeUs(seconds * kUsPerSecond);
    m.hasVideo = video;
    m.hasAudio = audio;
    m.width = video ? 1920 : 0;
    m.height = video ? 1080 : 0;
    return m;
}

// Proyecto con dos cámaras (10 s y 8 s) y un micrófono (12 s): cam2 empieza 2 s después que cam1 y
// el micrófono 1 s antes.
struct Fixture {
    Project p = Project::createDefault();
    Id cam1, cam2, mic, clip;

    Fixture()
    {
        p.settings.frameRate = {25, 1};
        cam1 = edit::addMedia(p, fakeMedia("cam1.mp4", 10, true, true));
        cam2 = edit::addMedia(p, fakeMedia("cam2.mp4", 8, true, true));
        mic = edit::addMedia(p, fakeMedia("mic.wav", 12, false, true));
        clip = edit::createMulticam(p, {{cam1, 0}, {cam2, 2 * kUsPerSecond}, {mic, -kUsPerSecond}}, "Entrevista",
                                    p.videoTracks[0].id, 50);
        REQUIRE(clip != kNoId);
        REQUIRE(p.validate());
    }
    const Clip& mc() const { return *p.findClip(clip); }
};

std::vector<Id> mediaOf(const std::vector<VideoLayer>& layers)
{
    std::vector<Id> ids;
    for (const VideoLayer& l : layers) {
        if (l.compound)
            for (Id id : mediaOf(l.nested))
                ids.push_back(id);
        else
            ids.push_back(l.mediaId);
    }
    return ids;
}

std::vector<const MediaItem*> audioMedia(const Project& p, FramePos frame)
{
    std::vector<const MediaItem*> out;
    const TimeUs t = p.settings.frameToTime(frame);
    for (const AudioSegment& s : audioSegmentsIn(p, t, t + 10000))
        out.push_back(s.media);
    return out;
}

} // namespace

TEST_CASE("Sincronización por audio con GCC-PHAT", "[multicam][sync]")
{
    const int rate = ai::kSyncSampleRate;
    const auto event = speechLike(30, rate, 11);
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.02f);

    // b: empieza 1,7 s después que la referencia, a la mitad de nivel, con ruido y sin agudos.
    std::vector<float> b(event.begin() + std::ptrdiff_t(1.7 * rate), event.begin() + 20 * rate);
    float lp = 0;
    for (float& v : b) {
        lp += 0.3f * (v - lp);
        v = 0.5f * lp + noise(rng);
    }
    // c: empieza 0,6 s antes que la referencia (lo anterior es otro sonido).
    std::vector<float> c = speechLike(1, rate, 99);
    c.resize(size_t(0.6 * rate));
    c.insert(c.end(), event.begin(), event.begin() + 15 * rate);
    const std::vector<float> silent(size_t(rate) * 5, 0.0f);
    const auto unrelated = speechLike(10, rate, 1234);

    const auto r = ai::syncByAudio({event, b, c, silent, unrelated}, rate, 20 * kUsPerSecond);
    REQUIRE(r.offsets.size() == 5);
    CHECK(r.synced[0]);
    CHECK(r.offsets[0] == 0);
    REQUIRE(r.synced[1]);
    CHECK(std::abs(r.offsets[1] - 1'700'000) <= 1000); // ±1 ms
    REQUIRE(r.synced[2]);
    CHECK(std::abs(r.offsets[2] + 600'000) <= 1000);
    CHECK(r.confidence[1] > 2.0);
    CHECK_FALSE(r.synced[3]); // sin audio
    CHECK_FALSE(r.synced[4]); // otra grabación: sin pico claro
    CHECK(r.offsets[4] == 0);
}

TEST_CASE("Sincronización por audio de archivos reales", "[multicam][sync]")
{
    const auto r = ai::syncFilesByAudio({test::mediaPath("mc_cam1.mp4"), test::mediaPath("mc_cam2.mp4"), test::mediaPath("mc_mic.wav")});
    REQUIRE(r.offsets.size() == 3);
    INFO("desfases " << r.offsets[1] << " y " << r.offsets[2] << " µs; confianza " << r.confidence[1] << ", " << r.confidence[2]);
    REQUIRE(r.synced[1]);
    REQUIRE(r.synced[2]);
    // Tolerancia de 10 ms: el retardo inicial de AAC y el remuestreo.
    CHECK(std::abs(r.offsets[1] - 1'700'000) <= 10'000);
    CHECK(std::abs(r.offsets[2] - 600'000) <= 10'000);
}

TEST_CASE("Sincronización de grabaciones largas con deriva", "[multicam][sync]")
{
    const int rate = ai::kSyncSampleRate;
    // Referencia de 20 min. «late» empieza 11,5 min después, dura 8 min y su reloj adelanta 100 ppm
    // (su instante τ es el T = 690 + 1,0001·τ de la referencia); «early» empieza 30 s antes que la
    // referencia (antes suena otra cosa) y dura 5 min.
    const auto ref = speechLike(1200, rate, 21);
    constexpr double kLate = 690.0, kDrift = 1.0001;
    std::vector<float> late(size_t(480) * size_t(rate));
    std::mt19937 rng(8);
    std::normal_distribution<float> noise(0.0f, 0.01f);
    float lp = 0;
    for (size_t i = 0; i < late.size(); ++i) {
        const double t = (kLate + kDrift * double(i) / rate) * rate;
        const size_t k = size_t(t);
        const float v = k + 1 < ref.size() ? float(ref[k] + (t - double(k)) * (ref[k + 1] - ref[k])) : 0.0f;
        lp += 0.4f * (v - lp);
        late[i] = 0.5f * lp + noise(rng);
    }
    std::vector<float> early = speechLike(30, rate, 77);
    early.insert(early.end(), ref.begin(), ref.begin() + 270 * rate);

    auto source = [&](const std::vector<float>& pcm) {
        ai::SyncSource s;
        ai::OnsetEnvelope envelope;
        for (size_t i = 0; i < pcm.size(); i += 4096) // por bloques, como al decodificar
            envelope.push(pcm.data() + i, std::min<size_t>(4096, pcm.size() - i));
        s.envelope = envelope.values();
        s.duration = TimeUs(double(pcm.size()) * kUsPerSecond / rate);
        s.read = [&pcm, rate](TimeUs start, TimeUs length) {
            std::vector<float> out(size_t(length * rate / kUsPerSecond), 0.0f);
            const long long first = (long long)(start * rate / kUsPerSecond);
            for (size_t i = 0; i < out.size(); ++i)
                if (first + (long long)i >= 0 && first + (long long)i < (long long)pcm.size())
                    out[i] = pcm[size_t(first + (long long)i)];
            return out;
        };
        return s;
    };
    const auto r = ai::syncSources({source(ref), source(late), source(early)});
    REQUIRE(r.synced.size() == 3);
    INFO("late " << r.offsets[1] << " µs ×" << r.speed[1] << ", early " << r.offsets[2] << " µs ×" << r.speed[2]);
    REQUIRE(r.synced[1]);
    REQUIRE(r.synced[2]);
    CHECK(std::abs(r.offsets[1] - TimeUs(kLate * kUsPerSecond)) <= 2000); // ±2 ms
    CHECK(r.speed[1] == Approx(1.0 / kDrift).margin(5e-6));
    CHECK(std::abs(r.offsets[2] + 30 * kUsPerSecond) <= 2000);
    CHECK(r.speed[2] == 1.0);
}

TEST_CASE("Sincronización de archivos largos", "[multicam][sync]")
{
    // mc_long_late empieza 11 min después que mc_long_ref y su reloj adelanta 125 ppm (8001/8000).
    const auto r = ai::syncFilesByAudio({test::mediaPath("mc_long_ref.flac"), test::mediaPath("mc_long_late.flac")});
    REQUIRE(r.synced.size() == 2);
    INFO("desfase " << r.offsets[1] << " µs, velocidad " << r.speed[1] << ", confianza " << r.confidence[1]);
    REQUIRE(r.synced[1]);
    CHECK(std::abs(r.offsets[1] - 660 * kUsPerSecond) <= 5000);
    CHECK(r.speed[1] == Approx(8000.0 / 8001.0).margin(2e-5));

    // La multicámara reproduce la grabación a esa velocidad: termina donde debe.
    Project p = Project::createDefault();
    std::vector<edit::MulticamSource> sources;
    for (const char* name : {"mc_long_ref.flac", "mc_long_late.flac"}) {
        auto item = engine::probeMediaItem(test::mediaPath(name));
        REQUIRE(item);
        sources.push_back({edit::addMedia(p, *item), 0});
    }
    sources.insert(sources.begin(), {edit::addMedia(p, fakeMedia("cam.mp4", 800, true, false)), 0});
    sources[2].offset = r.offsets[1];
    sources[2].speed = r.speed[1];
    const Id mc = edit::createMulticam(p, sources, "Larga", p.videoTracks[0].id, 0);
    REQUIRE(mc != kNoId);
    const Clip& late = p.audioTracksOf(p.findClip(mc)->sequenceId)->at(1).clips[0];
    CHECK(late.speed == Approx(r.speed[1]));
    // 115 s de audio a 8000/8001: 115,0144 s de línea de tiempo.
    CHECK(toSeconds(p.settings.frameToTime(late.duration)) == Approx(115.0144).margin(0.05));
    CHECK(p.validate());
}

TEST_CASE("Cuadrícula multicámara", "[multicam]")
{
    const MulticamGrid g4 = multicamGridFor(4);
    CHECK(g4.cols == 2);
    CHECK(g4.rows == 2);
    CHECK(g4.scale() == 0.5);
    CHECK(g4.angleAt(0.75, 0.25, 4) == 1);
    CHECK(g4.angleAt(0.25, 0.75, 4) == 2);
    const MulticamGrid g3 = multicamGridFor(3);
    CHECK(g3.cols == 2);
    CHECK(g3.rows == 2);
    CHECK(g3.angleAt(0.75, 0.75, 3) == -1); // celda vacía
    const MulticamGrid g5 = multicamGridFor(5);
    CHECK(g5.cols == 3);
    CHECK(g5.rows == 2);
    CHECK(g5.image(0).width == Approx(1.0 / 3));
    CHECK(g5.angleAt(0.5, 0.05, 5) == -1); // banda vacía sobre la imagen (celdas más altas que ella)
    CHECK(g5.angleAt(0.5, 0.25, 5) == 1);
}

TEST_CASE("Crear una multicámara y cambiar de ángulo", "[core][multicam]")
{
    Fixture f;
    Project& p = f.p;
    const Clip& mc = f.mc();
    CHECK(mc.isMulticam());
    CHECK(mc.start == 50);
    CHECK(mc.angle == 0);
    CHECK(mc.audioAngle == 2); // el micrófono aparte
    CHECK(edit::multicamAngles(p, mc) == 2);
    CHECK(edit::multicamAudioSources(p, mc) == 3);
    // El micrófono empezó 1 s antes: es el origen. cam1 en 1 s, cam2 en 3 s; dura lo que el más largo.
    const auto& seqVideo = *p.videoTracksOf(mc.sequenceId);
    REQUIRE(seqVideo.size() == 2);
    CHECK(seqVideo[0].name == "cam1.mp4");
    CHECK(seqVideo[0].clips[0].start == 25);
    CHECK(seqVideo[1].clips[0].start == 75);
    CHECK(p.audioTracksOf(mc.sequenceId)->at(0).clips[0].start == 0);
    CHECK(mc.duration == 300); // 12 s del micrófono

    // Se ve solo el ángulo elegido; suena solo el micrófono.
    CHECK(mediaOf(videoLayersAt(p, 50 + 100)) == std::vector<Id>{f.cam1});
    REQUIRE(audioMedia(p, 150).size() == 1);
    CHECK(audioMedia(p, 150)[0]->id == f.mic);

    // Cambiar al ángulo 2 en el fotograma 150 divide el clip.
    const Id right = edit::switchAngle(p, f.clip, 150, 1);
    REQUIRE(right != kNoId);
    CHECK(right != f.clip);
    CHECK(p.findClip(f.clip)->angle == 0);
    CHECK(p.findClip(right)->angle == 1);
    CHECK(p.findClip(right)->start == 150);
    CHECK(mediaOf(videoLayersAt(p, 149)) == std::vector<Id>{f.cam1});
    CHECK(mediaOf(videoLayersAt(p, 150)) == std::vector<Id>{f.cam2});
    // Desde el inicio de un clip no se divide; el mismo ángulo no cambia nada.
    CHECK(edit::switchAngle(p, right, 150, 0) == right);
    CHECK(p.findClip(right)->angle == 0);
    CHECK(edit::switchAngle(p, right, 160, 0) == right);
    CHECK(edit::switchAngle(p, right, 160, 5) == kNoId);

    // El audio puede seguir al ángulo que se ve.
    REQUIRE(edit::setMulticamAudio(p, f.clip, -1));
    CHECK(audioMedia(p, 100)[0]->id == f.cam1);
    CHECK_FALSE(edit::setMulticamAudio(p, f.clip, 3));
    REQUIRE(p.validate());
}

TEST_CASE("Vista en cuadrícula de todos los ángulos", "[core][multicam]")
{
    Fixture f;
    Project& p = f.p;
    p.multicamGrid = true;
    // A 4 s del clip (fotograma 150): cam1 y cam2 se ven a la vez, cada uno en su celda.
    const auto layers = videoLayersAt(p, 150);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].nested.size() == 2);
    const VideoLayer& a = layers[0].nested[0];
    const VideoLayer& b = layers[0].nested[1];
    CHECK(a.transform.scale == 0.5);
    CHECK(a.transform.x == Approx(-0.25));
    CHECK(a.transform.y == Approx(0.0)); // dos ángulos: uno al lado del otro
    CHECK(b.transform.x == Approx(0.25));
    CHECK(mediaOf(a.nested) == std::vector<Id>{f.cam1});
    CHECK(mediaOf(b.nested) == std::vector<Id>{f.cam2});
    CHECK(a.instance != b.instance);
    // El audio no cambia con la cuadrícula.
    CHECK(audioMedia(p, 150)[0]->id == f.mic);
}

TEST_CASE("Romper una multicámara deja el ángulo y el audio elegidos", "[core][multicam]")
{
    Fixture f;
    Project& p = f.p;
    REQUIRE(edit::setMulticamAngle(p, f.clip, 1));
    const auto created = edit::breakCompound(p, f.clip);
    REQUIRE(created.size() == 2);
    const Clip* video = p.findClip(created[0]);
    const Clip* audio = p.findClip(created[1]);
    REQUIRE((video && audio));
    CHECK(video->mediaId == f.cam2);
    CHECK(video->start == 50 + 75);
    CHECK(video->audioDetached); // suena el micrófono
    CHECK(audio->mediaId == f.mic);
    CHECK(audio->start == 50);
    CHECK(p.trackOfClip(audio->id)->kind == TrackKind::Audio);
    CHECK(p.validate());
}

TEST_CASE("Multicámara en el archivo de proyecto y en OTIO", "[core][multicam]")
{
    Fixture f;
    Project& p = f.p;
    REQUIRE(edit::switchAngle(p, f.clip, 100, 1) != kNoId);
    const auto loaded = deserializeProject(serializeProject(p, QStringLiteral("C:/x/p.mxproj")), QStringLiteral("C:/x/p.mxproj"));
    REQUIRE(loaded.project);
    CHECK(loaded.project->videoTracks == p.videoTracks);

    // OTIO no tiene multicámara: cada tramo es una pila con todos los ángulos en la que solo están
    // activos el que se ve y el audio que suena (el micrófono).
    const QByteArray json = otio::exportTimeline(p, QStringLiteral("mc"));
    const QJsonObject tl = QJsonDocument::fromJson(json).object();
    const QJsonArray v1 = tl["tracks"].toObject()["children"].toArray()[0].toObject()["children"].toArray();
    REQUIRE(v1.size() == 3); // hueco y dos tramos
    for (int i : {1, 2}) {
        const QJsonArray inner = v1[i].toObject()["children"].toArray();
        // cam1, cam2, el micrófono y el audio de las dos cámaras
        REQUIRE(inner.size() == 5);
        QStringList active;
        for (const QJsonValue& t : inner)
            if (t.toObject()["enabled"].toBool())
                active << t.toObject()["kind"].toString() + ":" + t.toObject()["name"].toString();
        CHECK(active == QStringList{(i == 1 ? "Video:cam1.mp4" : "Video:cam2.mp4"), "Audio:mic.wav"});
    }

    // Al importar vuelve a ser una multicámara: los dos tramos comparten la secuencia, con sus
    // ángulos, y cada uno conserva el suyo y el audio del micrófono.
    const auto imported = otio::importTimeline(json, QStringLiteral("C:/x/mc.otio"),
                                               [&](const std::string& path) -> std::optional<MediaItem> {
                                                   for (const MediaItem& m : p.media)
                                                       if (m.path == path || path.ends_with("/" + m.path))
                                                           return m;
                                                   return std::nullopt;
                                               });
    INFO(imported.error.toStdString() << " " << imported.warnings.join("; ").toStdString());
    REQUIRE(imported.project);
    const Project& q = *imported.project;
    std::vector<const Clip*> segments;
    for (const Clip& c : q.videoTracks[0].clips)
        segments.push_back(&c);
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0]->isMulticam());
    REQUIRE(segments[1]->isMulticam());
    CHECK(segments[0]->sequenceId == segments[1]->sequenceId);
    CHECK(segments[0]->angle == 0);
    CHECK(segments[1]->angle == 1);
    CHECK(segments[0]->audioAngle == 2);
    CHECK(edit::multicamAngles(q, *segments[0]) == 2);
    CHECK(edit::multicamAudioSources(q, *segments[0]) == 3);
    for (const Track& t : *q.videoTracksOf(segments[0]->sequenceId))
        CHECK_FALSE(t.hidden);
    CHECK(segments[0]->start == 50);
    CHECK(segments[1]->start == 100);
    CHECK(segments[1]->end() == p.videoTracks[0].clips.back().end());
    CHECK(q.validate());
}

TEST_CASE("Multicámara con medios reales sincronizados", "[multicam][sync]")
{
    Project p = Project::createDefault();
    std::vector<edit::MulticamSource> sources;
    std::vector<std::string> paths;
    for (const char* name : {"mc_cam1.mp4", "mc_cam2.mp4", "mc_mic.wav"}) {
        auto item = engine::probeMediaItem(test::mediaPath(name));
        REQUIRE(item);
        paths.push_back(item->path);
        sources.push_back({edit::addMedia(p, *item), 0});
    }
    const auto sync = ai::syncFilesByAudio(paths);
    for (size_t i = 0; i < sources.size(); ++i)
        sources[i].offset = sync.offsets[i];
    const Id mc = edit::createMulticam(p, sources, "Toma", p.videoTracks[0].id, 0);
    REQUIRE(mc != kNoId);
    const Clip& c = *p.findClip(mc);
    // cam1 es el origen; cam2 empieza en 1,7 s (fotograma 51 a 30 fps) y el micrófono en 0,6 s (18).
    const auto& seqVideo = *p.videoTracksOf(c.sequenceId);
    CHECK(seqVideo[0].clips[0].start == 0);
    CHECK(std::abs(seqVideo[1].clips[0].start - 51) <= 1);
    CHECK(std::abs(p.audioTracksOf(c.sequenceId)->at(0).clips[0].start - 18) <= 1);
    CHECK(p.validate());
}

TEST_CASE("Multicámara desde la aplicación", "[multicam][app]")
{
    ProjectController project;
    QSignalSpy errors(&project, &ProjectController::errorOccurred);
    project.newProject();
    QList<QUrl> urls;
    for (const char* name : {"mc_cam1.mp4", "mc_cam2.mp4", "mc_mic.wav"})
        urls.append(QUrl::fromLocalFile(QString::fromStdString(test::mediaPath(name))));
    project.importFiles(urls);
    REQUIRE(QTest::qWaitFor([&] { return project.importing() == 0 && project.project().media.size() == 3; }, 10000));
    QVariantList ids;
    for (const MediaItem& m : project.project().media)
        ids.append(qint64(m.id));

    project.createMulticam(ids, true);
    REQUIRE(QTest::qWaitFor([&] { return project.importing() == 0 && project.project().duration() > 0; }, 20000));
    INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
    CHECK(errors.isEmpty());
    CHECK(project.multicamView()); // se abre la vista de ángulos
    const QVariantMap info = project.multicamAt(30);
    REQUIRE(info.contains("clipId"));
    CHECK(info["angles"].toList().size() == 2);
    CHECK(info["audioSources"].toList().size() == 3);
    CHECK(info["audioAngle"].toInt() == 2); // el micrófono
    CHECK(info["cells"].toList().size() == 2);

    // Cortar al segundo ángulo en el cabezal.
    project.player()->seekFrame(60);
    REQUIRE(project.switchAngle(1));
    CHECK(project.multicamAt(59)["angle"].toInt() == 0);
    CHECK(project.multicamAt(60)["angle"].toInt() == 1);
    CHECK(project.project().videoTracks[0].clips.size() == 2);
    project.undo();
    CHECK(project.project().videoTracks[0].clips.size() == 1);
    // La exportación no ve la cuadrícula: solo la vista previa.
    CHECK_FALSE(project.snapshot()->multicamGrid);
    // Para revisar la interfaz con un proyecto multicámara (fuera de las pruebas).
    if (const QString dump = qEnvironmentVariable("MOVIX_MULTICAM_DUMP"); !dump.isEmpty())
        REQUIRE(project.saveAs(QUrl::fromLocalFile(dump)));
    project.setMulticamView(false);
}

TEST_CASE("GPU: la cuadrícula multicámara reduce cada ángulo a su celda", "[multicam][render]")
{
    constexpr int W = 320, H = 180;
    Project p = Project::createDefault();
    std::vector<edit::MulticamSource> sources;
    for (const char* name : {"mc_cam1.mp4", "mc_cam2.mp4"}) {
        auto item = engine::probeMediaItem(test::mediaPath(name));
        REQUIRE(item);
        sources.push_back({edit::addMedia(p, *item), 0});
    }
    p.settings.width = W;
    p.settings.height = H;
    const Id mc = edit::createMulticam(p, sources, "Toma", p.videoTracks[0].id, 0);
    REQUIRE(mc != kNoId);

    render::OffscreenRenderer r;
    REQUIRE(r.init(W, H));
    engine::VideoSources sourcesCache(false);
    auto renderAt = [&](const Project& proj) {
        std::vector<uint8_t> px;
        REQUIRE(r.render(sourcesCache.build(proj, 45), px));
        return px;
    };
    auto pixel = [](const std::vector<uint8_t>& px, int x, int y, int c) { return int(px[(size_t(y) * W + size_t(x)) * 4 + size_t(c)]); };

    // Cada ángulo a pantalla completa, y la cuadrícula.
    const auto angle0 = renderAt(p);
    REQUIRE(edit::setMulticamAngle(p, mc, 1));
    const auto angle1 = renderAt(p);
    p.multicamGrid = true;
    const auto grid = renderAt(p);

    // Celda izquierda ≈ ángulo 0 reducido a la mitad; derecha ≈ ángulo 1; bandas de arriba y abajo negras.
    auto cellError = [&](const std::vector<uint8_t>& full, int offsetX) {
        double err = 0;
        int n = 0;
        for (int y = 4; y < H - 4; y += 4)
            for (int x = 4; x < W - 4; x += 4)
                for (int c = 0; c < 3; ++c, ++n)
                    err += std::abs(pixel(grid, offsetX + x / 2, H / 4 + y / 2, c) - pixel(full, x, y, c));
        return err / n;
    };
    const double left = cellError(angle0, 0), right = cellError(angle1, W / 2);
    const double crossed = cellError(angle1, 0);
    INFO("error celda izquierda " << left << ", derecha " << right << ", cruzado " << crossed);
    CHECK(left < 12);
    CHECK(right < 12);
    CHECK(crossed > 3 * left); // las celdas no son intercambiables: cada una muestra su ángulo
    for (int x = 0; x < W; x += 16) {
        CHECK(pixel(grid, x, 10, 0) + pixel(grid, x, 10, 1) + pixel(grid, x, 10, 2) <= 6);
        CHECK(pixel(grid, x, H - 10, 0) + pixel(grid, x, H - 10, 1) + pixel(grid, x, H - 10, 2) <= 6);
    }
}
