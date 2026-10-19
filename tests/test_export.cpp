#include "TestMedia.h"
#include "core/Edits.h"
#include "engine/MediaImport.h"
#include "export/Exporter.h"
#include "media/AudioDecoder.h"
#include "media/MediaInfo.h"
#include "media/VideoDecoder.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <numeric>

using namespace movix;
using namespace movix::core;

namespace {

Project projectWith(const char* mediaName, int width, int height, Rational fps)
{
    Project p = Project::createDefault();
    p.settings.width = width;
    p.settings.height = height;
    p.settings.frameRate = fps;
    auto item = engine::probeMediaItem(test::mediaPath(mediaName));
    REQUIRE(item);
    const Id media = edit::addMedia(p, *item);
    REQUIRE(edit::placeClip(p, p.videoTracks[0].id, media, 0) != kNoId);
    return p;
}

exporting::ExportSettings settingsFor(const QString& path, const Project& p)
{
    exporting::ExportSettings s;
    s.path = path;
    s.width = p.settings.width;
    s.height = p.settings.height;
    s.frameRate = p.settings.frameRate;
    return s;
}

bool runExport(const Project& p, const exporting::ExportSettings& s, exporting::ExportProgress* last = nullptr)
{
    exporting::Exporter exporter;
    std::atomic<bool> cancel{false};
    QString error;
    const bool ok = exporter.run(p, s, cancel, [&](const exporting::ExportProgress& pr) { if (last) *last = pr; }, &error);
    INFO(error.toStdString());
    CHECK(ok);
    return ok;
}

std::vector<std::shared_ptr<media::VideoFrame>> decodeAll(const std::string& path)
{
    media::VideoDecoder dec;
    REQUIRE(dec.open(path, media::VideoDecoderOptions{false}));
    std::vector<std::shared_ptr<media::VideoFrame>> frames;
    while (auto f = dec.decodeNext())
        frames.push_back(f);
    return frames;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    REQUIRE(a.size() == b.size());
    double mse = 0;
    for (size_t i = 0; i < a.size(); ++i)
        mse += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
    mse /= double(a.size());
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

TEST_CASE("Codificadores de video disponibles en este equipo", "[export]")
{
    const auto& encoders = media::Encoder::availableVideoEncoders();
    for (const auto& e : encoders)
        std::printf("[export] codificador: %-12s %s\n", e.name.c_str(), e.hardware ? "(GPU)" : "");
    const bool hasX264 = std::any_of(encoders.begin(), encoders.end(), [](const auto& e) { return e.name == "libx264"; });
    CHECK(hasX264);
}

TEST_CASE("Exportar reproduce fielmente la línea de tiempo", "[export]")
{
    QTemporaryDir dir;
    const Project p = projectWith("count_1080p60.mp4", 1920, 1080, {60, 1});
    auto s = settingsFor(dir.filePath("salida ñ.mp4"), p);
    s.quality = 16;
    s.rangeEnd = 60; // 1 s
    exporting::ExportProgress progress;
    REQUIRE(runExport(p, s, &progress));
    std::printf("[export] 1080p60 H.264: %.0f fps (decodificar %.1f · componer %.1f · codificar %.1f · audio %.1f ms)\n", progress.fps, progress.decodeMs, progress.renderMs, progress.encodeMs, progress.audioMs);
    CHECK(progress.framesDone == 60);

    const std::string out = s.path.toStdString();
    auto info = media::probe(out);
    REQUIRE(info);
    REQUIRE(info->video);
    CHECK(info->video->codec == "h264");
    CHECK(info->video->width == 1920);
    CHECK(info->video->height == 1080);
    CHECK(std::fabs(info->video->frameRate - 60.0) < 0.01);
    REQUIRE(info->audio);
    CHECK(info->audio->codec == "aac");
    CHECK(std::llabs(info->duration - kUsPerSecond) < 50'000);

    const auto exported = decodeAll(out);
    const auto source = decodeAll(test::mediaPath("count_1080p60.mp4"));
    REQUIRE(exported.size() == 60);
    double minPsnr = 99;
    for (size_t i = 0; i < exported.size(); ++i) {
        CHECK(std::llabs(exported[i]->pts - source[i]->pts) <= 1000);
        minPsnr = std::min(minPsnr, psnr(exported[i]->y, source[i]->y));
    }
    std::printf("[export] PSNR mínimo de luma frente a la fuente: %.1f dB\n", minPsnr);
    CHECK(minPsnr > 35.0); // dos conversiones de color de 8 bits + compresión
}

TEST_CASE("La sincronía A/V se conserva al exportar", "[export][sync]")
{
    QTemporaryDir dir;
    const Project p = projectWith("sync_720p30.mp4", 1280, 720, {30, 1});
    const auto s = settingsFor(dir.filePath("sync.mp4"), p);
    REQUIRE(runExport(p, s));
    const std::string out = s.path.toStdString();

    std::vector<TimeUs> flashes;
    for (const auto& f : decodeAll(out)) {
        const double mean = std::accumulate(f->y.begin(), f->y.end(), 0.0) / double(f->y.size());
        if (mean > 128)
            flashes.push_back(f->pts);
    }
    std::vector<TimeUs> beeps;
    media::AudioDecoder audio;
    REQUIRE(audio.open(out, 48000, 2));
    media::AudioChunk c;
    TimeUs lastLoud = kNoTime;
    while (audio.read(c)) {
        for (int i = 0; i < c.frames; ++i) {
            if (std::fabs(c.samples[size_t(i) * 2]) < 0.2f)
                continue;
            const TimeUs t = c.pts + TimeUs(i) * kUsPerSecond / 48000;
            if (lastLoud == kNoTime || t - lastLoud > 300'000)
                beeps.push_back(t);
            lastLoud = t;
        }
    }
    REQUIRE(flashes.size() == 5);
    REQUIRE(beeps.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        std::printf("[export] segundo %zu: desfase A/V %+.1f ms\n", i, (beeps[i] - flashes[i]) / 1000.0);
        CHECK(std::llabs(beeps[i] - flashes[i]) < 5000);
    }
}

TEST_CASE("Cada codificador disponible produce un archivo válido", "[export][codecs]")
{
    QTemporaryDir dir;
    const Project p = projectWith("count_1080p60.mp4", 1280, 720, {30, 1});
    for (const auto& enc : media::Encoder::availableVideoEncoders()) {
        auto s = settingsFor(QString(), p);
        s.videoCodec = enc.name;
        s.rangeEnd = 15; // 0,5 s
        QString ext = "mp4";
        if (enc.name == "libvpx-vp9") {
            ext = "webm";
            s.audioCodec = "libopus";
        } else if (enc.name == "prores_ks") {
            ext = "mov";
            s.audioCodec = "pcm_s16le";
        } else if (enc.name.find("av1") != std::string::npos) {
            ext = "mkv";
        }
        s.path = dir.filePath(QString::fromStdString(enc.name) + "." + ext);
        INFO("codificador " << enc.name);
        exporting::ExportProgress progress;
        REQUIRE(runExport(p, s, &progress));
        const auto frames = decodeAll(s.path.toStdString());
        std::printf("[export] %-12s → %zu fotogramas, %.0f fps (cod. %.1f ms), %lld KB\n", enc.name.c_str(), frames.size(), progress.fps, progress.encodeMs,
                    QFileInfo(s.path).size() / 1024);
        CHECK(frames.size() == 15);
        if (!frames.empty()) {
            CHECK(frames[0]->width == 1280);
            CHECK(frames[0]->height == 720);
        }
    }
}

TEST_CASE("Exportar a otra resolución y relación de aspecto", "[export]")
{
    QTemporaryDir dir;
    const Project p = projectWith("count_1080p60.mp4", 1920, 1080, {30, 1});
    auto s = settingsFor(dir.filePath("4x3.mp4"), p);
    s.width = 960;
    s.height = 720; // 4:3: el lienzo 16:9 queda con bandas arriba y abajo
    s.rangeEnd = 5;
    REQUIRE(runExport(p, s));
    const auto frames = decodeAll(s.path.toStdString());
    REQUIRE(!frames.empty());
    const auto& f = *frames[0];
    CHECK(f.width == 960);
    CHECK(f.height == 720);
    // 16:9 en 960 de ancho = 540 de alto → bandas de 90 px.
    CHECK(f.y[size_t(20) * 960 + 480] <= 17);            // banda superior: negro (Y≈16)
    CHECK(f.y[size_t(700) * 960 + 480] <= 17);           // banda inferior
    CHECK(f.y[size_t(360) * 960 + 480] > 17);            // contenido
}

TEST_CASE("Rendimiento de la exportación en cadena (1080p60, codificador por GPU)", "[export][perf]")
{
    const auto& encoders = media::Encoder::availableVideoEncoders();
    if (std::none_of(encoders.begin(), encoders.end(), [](const auto& e) { return e.name == "h264_nvenc"; }))
        SKIP("Sin NVENC en este equipo");
    QTemporaryDir dir;
    const Project p = projectWith("count_1080p60.mp4", 1920, 1080, {60, 1});
    auto s = settingsFor(dir.filePath("perf.mp4"), p);
    s.videoCodec = "h264_nvenc";
    exporting::ExportProgress progress;
    REQUIRE(runExport(p, s, &progress)); // 180 fotogramas
    std::printf("[export] cadena 1080p60 NVENC: %.0f fps (decodificar %.1f · componer %.1f · codificar %.1f ms)\n",
                progress.fps, progress.decodeMs, progress.renderMs, progress.encodeMs);
    // La suma secuencial de las etapas daría menos de lo que logra la cadena.
    CHECK(progress.fps > 1000.0 / (progress.decodeMs + progress.renderMs + progress.encodeMs));
}

TEST_CASE("Los títulos y transformaciones llegan al archivo exportado", "[export][f2]")
{
    QTemporaryDir dir;
    Project p = Project::createDefault();
    p.settings.width = 640;
    p.settings.height = 360;
    TitleStyle style;
    style.text = "MMMM";
    style.size = 0.3;
    style.shadow = false;
    const Id title = edit::placeTitle(p, p.videoTracks[0].id, 0, 10, style);
    edit::setProperty(p, title, edit::Property::PositionY, 0, -0.25); // en la mitad superior
    auto s = settingsFor(dir.filePath("titulo.mp4"), p);
    REQUIRE(runExport(p, s));
    const auto frames = decodeAll(s.path.toStdString());
    REQUIRE(frames.size() == 10);
    const auto& f = *frames[5];
    auto maxLuma = [&](int y0, int y1) {
        int m = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < f.width; ++x)
                m = std::max(m, int(f.y[size_t(y) * size_t(f.width) + size_t(x)]));
        return m;
    };
    CHECK(maxLuma(30, 150) > 200);  // el texto blanco, desplazado hacia arriba
    CHECK(maxLuma(220, 360) < 30);  // la mitad inferior queda negra
}

TEST_CASE("Cancelar la exportación borra el archivo parcial", "[export]")
{
    QTemporaryDir dir;
    const Project p = projectWith("count_1080p60.mp4", 1280, 720, {30, 1});
    const auto s = settingsFor(dir.filePath("cancelado.mp4"), p);
    exporting::Exporter exporter;
    std::atomic<bool> cancel{false};
    QString error;
    const bool ok = exporter.run(p, s, cancel, [&](const exporting::ExportProgress& pr) {
        if (pr.framesDone >= 10)
            cancel = true;
    }, &error);
    CHECK_FALSE(ok);
    CHECK(exporter.wasCancelled());
    CHECK_FALSE(QFileInfo::exists(s.path));
}
