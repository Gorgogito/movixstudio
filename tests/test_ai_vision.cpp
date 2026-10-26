#include "TestMedia.h"
#include "ai/Onnx.h"
#include "ai/Tracking.h"
#include "ai/VideoAi.h"
#include "ai/Vision.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "engine/MediaImport.h"
#include "engine/Sources.h"
#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>

using namespace movix;

namespace {

std::string visionModel(const char* folder)
{
    return (std::filesystem::path(MOVIX_TEST_MODELS_DIR) / folder / "model.onnx").string();
}

// Centro de la bola de subject_ball.mp4 en el instante t.
double ballX(double t)
{
    return 320.0 + 100.0 * std::sin(3.0 * t);
}

// Opacidad media dentro de un círculo (r < inner) y lejos de él (r > outer).
std::pair<double, double> insideOutside(const std::vector<uint8_t>& alpha, int w, int h, double cx, double cy, double inner,
                                        double outer)
{
    double in = 0, out = 0;
    int nin = 0, nout = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double r = std::hypot(x - cx, y - cy);
            const double a = alpha[size_t(y) * size_t(w) + size_t(x)] / 255.0;
            if (r < inner) {
                in += a;
                ++nin;
            } else if (r > outer) {
                out += a;
                ++nout;
            }
        }
    return {in / std::max(1, nin), out / std::max(1, nout)};
}

std::vector<uint8_t> firstFrameRgba(int& w, int& h)
{
    media::VideoDecoder dec;
    REQUIRE(dec.open(test::mediaPath("subject_ball.mp4")));
    auto f = dec.decodeNext();
    REQUIRE(f);
    w = f->width;
    h = f->height;
    std::vector<uint8_t> rgba;
    REQUIRE(media::frameToRgba(*f, w, h, rgba));
    return rgba;
}

} // namespace

TEST_CASE("Recorte de fondo: BiRefNet separa el sujeto y MODNet produce un mate válido", "[ai][vision][matte]")
{
    if (!std::filesystem::exists(visionModel("birefnet-lite")) || !std::filesystem::exists(visionModel("modnet")))
        SKIP("Faltan los modelos de prueba");
    int w = 0, h = 0;
    const auto rgba = firstFrameRgba(w, h);
    std::vector<uint8_t> alpha;
    std::string error;
    QElapsedTimer timer;
    timer.start();
    const bool ok = ai::matteFrame(visionModel("birefnet-lite"), ai::MatteModel::General, rgba.data(), w, h, alpha, &error);
    INFO(error);
    REQUIRE(ok);
    REQUIRE(alpha.size() == size_t(w) * size_t(h));
    const auto [in, out] = insideOutside(alpha, w, h, ballX(0), 180, 50, 110);
    INFO("BiRefNet (" << (ai::onnx::gpuAvailable() ? ai::onnx::gpuDescription() : std::string("CPU")) << ", " << timer.elapsed()
                      << " ms con la carga): sujeto " << in << ", fondo " << out);
    CHECK(in > 0.8);
    CHECK(out < 0.1);

    // MODNet está entrenado con personas: aquí solo se comprueba que funciona y el formato.
    REQUIRE(ai::matteFrame(visionModel("modnet"), ai::MatteModel::Portrait, rgba.data(), w, h, alpha, &error));
    REQUIRE(alpha.size() == size_t(w) * size_t(h));
}

TEST_CASE("Recorte de fondo de un video: mate en escala de grises que usa el motor", "[ai][vision][matte]")
{
    if (!std::filesystem::exists(visionModel("birefnet-lite")))
        SKIP("Falta el modelo de prueba");
    QTemporaryDir dir;
    const std::string output = QDir::toNativeSeparators(dir.filePath("mate.mp4")).toStdString(); // como guarda el proyecto
    ai::MatteResult result;
    std::string error;
    double lastProgress = 0;
    // BiRefNet es lento (~1 s por fotograma en la GPU, ~10 s en la CPU): bastan los primeros 4 fotogramas.
    REQUIRE(ai::removeBackground(visionModel("birefnet-lite"), ai::MatteModel::General, test::mediaPath("subject_ball.mp4"), 0,
                                 fromSeconds(0.35), output, result, &error, [&](double p) { lastProgress = p; }));
    INFO(error);
    CHECK(result.frames == 4);
    CHECK(result.start == 0);
    CHECK(lastProgress > 0.8);

    // El mate se decodifica como un video normal y sigue a la bola.
    media::VideoDecoder dec;
    REQUIRE(dec.open(output));
    CHECK(dec.frameRate() == Catch::Approx(10.0));
    REQUIRE(dec.seek(fromSeconds(0.2)));
    auto f = dec.decodeNext();
    REQUIRE(f);
    std::vector<uint8_t> luma(f->y.begin(), f->y.begin() + std::ptrdiff_t(f->width) * f->height);
    for (uint8_t& v : luma)
        v = uint8_t(std::clamp((v - 16) * 255 / 219, 0, 255));
    const auto [in, out] = insideOutside(luma, f->width, f->height, ballX(0.2), 180, 50, 110);
    INFO("mate a 0,2 s: sujeto " << in << ", fondo " << out);
    CHECK(in > 0.8);
    CHECK(out < 0.1);

    // En un proyecto: el clip con mate da a su capa una imagen de opacidad que deja ver la bola.
    auto p = core::Project::createDefault();
    auto item = engine::probeMediaItem(test::mediaPath("subject_ball.mp4"), &error);
    REQUIRE(item);
    const core::Id media = core::edit::addMedia(p, *item);
    const core::Id clip = core::edit::placeClip(p, p.videoTracks[0].id, media, 0);
    REQUIRE(core::edit::setMatte(p, clip, {output, result.start, false}));
    CHECK_FALSE(core::edit::setMatte(p, clip, {output, result.start, false}));
    engine::VideoSources sources(false);
    const int frame5 = int(p.settings.timeToFrame(fromSeconds(0.2)));
    auto bundle = sources.build(p, frame5);
    REQUIRE(bundle.layers.size() == 1);
    const auto& mask = bundle.layers[0].maskImage;
    REQUIRE(mask);
    CHECK(mask->width == 640);
    const auto [min, mout] = insideOutside(mask->alpha, mask->width, mask->height, ballX(0.2), 180, 50, 110);
    CHECK(min > 0.8);
    CHECK(mout < 0.1);
    // Invertido: se conserva el fondo.
    REQUIRE(core::edit::setMatte(p, clip, {output, result.start, true}));
    bundle = sources.build(p, frame5);
    const auto [iin, iout] = insideOutside(bundle.layers[0].maskImage->alpha, 640, 360, ballX(0.2), 180, 50, 110);
    CHECK(iin < 0.2);
    CHECK(iout > 0.9);

    // Se guarda en el proyecto.
    REQUIRE(core::saveProjectFile(p, dir.filePath("m.mxproj")));
    const auto loaded = core::loadProjectFile(dir.filePath("m.mxproj"));
    REQUIRE(loaded.project);
    CHECK(loaded.project->findClip(clip)->matte == p.findClip(clip)->matte);
    CHECK(loaded.missingMedia.isEmpty());
}


namespace {

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (i % 4 == 3)
            continue; // alfa
        const double d = double(a[i]) - b[i];
        se += d * d;
        ++n;
    }
    return 10 * std::log10(255.0 * 255.0 / std::max(se / double(n), 1e-9));
}

// Píxeles claramente rojos (la bola) y su centro.
std::pair<int, double> redBlob(const std::vector<uint8_t>& rgba, int w, int h)
{
    int count = 0;
    double sx = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            if (p[0] > 180 && p[1] < 90 && p[2] < 90) {
                ++count;
                sx += x;
            }
        }
    return {count, count ? sx / count : 0.0};
}

std::vector<uint8_t> frameAt(const std::string& path, double t, int w, int h)
{
    media::VideoDecoder dec;
    REQUIRE(dec.open(path));
    REQUIRE(dec.seek(fromSeconds(t)));
    auto f = dec.decodeNext();
    REQUIRE(f);
    std::vector<uint8_t> rgba;
    REQUIRE(media::frameToRgba(*f, w, h, rgba));
    return rgba;
}

} // namespace

TEST_CASE("Real-ESRGAN aumenta la resolución recuperando la nitidez que pierde el bilineal", "[ai][vision][upscale]")
{
    if (!std::filesystem::exists(visionModel("realesrgan-x4v3")))
        SKIP("Falta el modelo de prueba");
    const auto original = frameAt(test::mediaPath("subject_ball.mp4"), 0, 640, 360);
    const auto small = frameAt(test::mediaPath("subject_ball.mp4"), 0, 160, 90);
    std::vector<uint8_t> big;
    std::string error;
    REQUIRE(ai::upscaleFrame(visionModel("realesrgan-x4v3"), small.data(), 160, 90, 4, big, &error));
    REQUIRE(big.size() == original.size());
    // Referencia: el mismo fotograma pequeño ampliado de forma bilineal (por canal).
    std::vector<uint8_t> bilinear(original.size(), 255);
    for (int c = 0; c < 3; ++c) {
        std::vector<float> plane(160 * 90);
        for (size_t i = 0; i < plane.size(); ++i)
            plane[i] = small[i * 4 + size_t(c)] / 255.0f;
        std::vector<uint8_t> up;
        ai::resizePlane(plane.data(), 160, 90, up, 640, 360);
        for (size_t i = 0; i < up.size(); ++i)
            bilinear[i * 4 + size_t(c)] = up[i];
    }
    // Una red generativa reconstruye bordes en lugar de minimizar el error medio (el PSNR favorece
    // el desenfoque), así que se mide lo que promete: bordes más nítidos con una fidelidad razonable.
    auto edgeSharpness = [&](const std::vector<uint8_t>& img) {
        double sum = 0;
        int n = 0;
        for (int y = 1; y < 359; ++y)
            for (int x = 1; x < 639; ++x) {
                const double r = std::hypot(x - ballX(0), y - 180.0);
                if (r < 64 || r > 76)
                    continue;
                auto g = [&](int xx, int yy) { return double(img[(size_t(yy) * 640 + size_t(xx)) * 4 + 1]); }; // canal verde
                sum += std::hypot(g(x + 1, y) - g(x - 1, y), g(x, y + 1) - g(x, y - 1));
                ++n;
            }
        return sum / n;
    };
    const double esrgan = psnr(big, original), plain = psnr(bilinear, original);
    const double sharpEsrgan = edgeSharpness(big), sharpPlain = edgeSharpness(bilinear), sharpOriginal = edgeSharpness(original);
    INFO("PSNR Real-ESRGAN " << esrgan << " dB, bilineal " << plain << " dB; nitidez del borde " << sharpEsrgan << " frente a "
                             << sharpPlain << " (original " << sharpOriginal << ")");
    CHECK(esrgan > 26.0);
    CHECK(sharpPlain < 0.9 * sharpOriginal);  // el bilineal pierde nitidez...
    CHECK(sharpEsrgan > 0.95 * sharpOriginal); // ...y Real-ESRGAN la recupera
    // La bola sigue en su sitio y del mismo tamaño.
    const auto [count, cx] = redBlob(big, 640, 360);
    CHECK(count == Catch::Approx(3.14159 * 70 * 70).epsilon(0.1));
    CHECK(cx == Catch::Approx(ballX(0)).margin(3));

    // ×2: la mitad del tamaño de ×4.
    REQUIRE(ai::upscaleFrame(visionModel("realesrgan-x4v3"), small.data(), 160, 90, 2, big, &error));
    CHECK(big.size() == size_t(320) * 180 * 4);

    // Video completo ×2.
    QTemporaryDir dir;
    ai::VideoResult result;
    REQUIRE(ai::upscaleVideo(visionModel("realesrgan-x4v3"), test::mediaPath("subject_ball.mp4"), 0, 0, 2,
                             dir.filePath("x2.mp4").toStdString(), result, &error));
    CHECK(result.width == 1280);
    CHECK(result.height == 720);
    CHECK(result.frames == 10);
    media::VideoDecoder dec;
    REQUIRE(dec.open(result.output));
    CHECK(dec.width() == 1280);
}

TEST_CASE("RIFE crea fotogramas intermedios con el movimiento correcto", "[ai][vision][interpolate]")
{
    if (!std::filesystem::exists(visionModel("rife-4")))
        SKIP("Falta el modelo de prueba");
    const auto a = frameAt(test::mediaPath("subject_ball.mp4"), 0.0, 640, 360);
    const auto b = frameAt(test::mediaPath("subject_ball.mp4"), 0.1, 640, 360);
    std::vector<uint8_t> mid;
    std::string error;
    REQUIRE(ai::interpolateFrame(visionModel("rife-4"), a.data(), b.data(), 640, 360, 0.5, mid, &error));
    // Una sola bola nítida a mitad de camino (un fundido daría dos medias bolas, ninguna roja intensa).
    const auto [count, cx] = redBlob(mid, 640, 360);
    INFO("bola intermedia: " << count << " píxeles, centro " << cx << " (esperado " << ballX(0.05) << ")");
    CHECK(count > 0.8 * 3.14159 * 70 * 70);
    CHECK(cx == Catch::Approx(ballX(0.05)).margin(6));

    // Video ×2: el doble de fotogramas y de ritmo, la misma duración.
    QTemporaryDir dir;
    ai::VideoResult result;
    double lastProgress = 0;
    REQUIRE(ai::interpolateVideo(visionModel("rife-4"), test::mediaPath("subject_ball.mp4"), 0, 0, 2,
                                 dir.filePath("x2.mp4").toStdString(), result, &error, [&](double p) { lastProgress = p; }));
    CHECK(result.frames == 20);
    CHECK(result.fps == Catch::Approx(20.0));
    media::VideoDecoder dec;
    REQUIRE(dec.open(result.output));
    CHECK(dec.frameRate() == Catch::Approx(20.0));
    CHECK(toSeconds(dec.duration()) == Catch::Approx(1.0).margin(0.06));
    // El fotograma 1 (0,05 s) es el intermedio: la bola entre las posiciones de 0 y 0,1 s.
    const auto f1 = frameAt(result.output, 0.05, 640, 360);
    const auto [n1, x1] = redBlob(f1, 640, 360);
    CHECK(x1 == Catch::Approx(ballX(0.05)).margin(6));
    CHECK(n1 > 0.8 * 3.14159 * 70 * 70);
}

TEST_CASE("Seguimiento de movimiento y localización del sujeto", "[ai][vision][tracking]")
{
    std::vector<ai::TrackPoint> points;
    std::string error;
    // La región de la bola en t = 0 (160 × 160 px sobre 640 × 360).
    const ai::Region start{ballX(0) / 640.0, 0.5, 160.0 / 640.0, 160.0 / 360.0};
    REQUIRE(ai::trackRegion(test::mediaPath("subject_ball.mp4"), 0, 0, start, points, &error));
    REQUIRE(points.size() == 10);
    double worst = 0;
    for (const auto& p : points) {
        worst = std::max(worst, std::abs(p.cx * 640.0 - ballX(toSeconds(p.time))));
        CHECK(p.cy * 360.0 == Catch::Approx(180.0).margin(6));
    }
    INFO("error máximo " << worst << " px");
    CHECK(worst < 8.0);

    // Desde la mitad del clip (como desde el cabezal).
    REQUIRE(ai::trackRegion(test::mediaPath("subject_ball.mp4"), fromSeconds(0.5), 0,
                            {ballX(0.5) / 640.0, 0.5, 0.25, 0.44}, points, &error));
    CHECK(points.size() == 5);
    CHECK(points.back().cx * 640.0 == Catch::Approx(ballX(0.9)).margin(8));

    // Sujeto sin personas: lo que destaca del fondo (la bola), suavizado a ±0,5 s.
    REQUIRE(ai::subjectCenters(test::mediaPath("subject_ball.mp4"), 0, 0, fromSeconds(0.1), {}, points, &error));
    REQUIRE(points.size() == 10);
    double mean = 0;
    for (const auto& p : points)
        mean += p.cx * 640.0 / points.size();
    double expected = 0;
    for (int i = 0; i < 10; ++i)
        expected += ballX(i * 0.1) / 10;
    CHECK(mean == Catch::Approx(expected).margin(15));
}

TEST_CASE("SAM 2.1 segmenta el objeto señalado y lo sigue por el clip", "[ai][vision][sam]")
{
    const auto folder = std::filesystem::path(MOVIX_TEST_MODELS_DIR) / "sam2-tiny";
    if (!std::filesystem::exists(folder / "vision_encoder.onnx_data"))
        SKIP("Falta el modelo de prueba");
    int w = 0, h = 0;
    const auto rgba = firstFrameRgba(w, h);
    std::vector<uint8_t> alpha;
    std::string error;
    float score = 0;
    ai::SegmentPrompt prompt;
    prompt.x = ballX(0) / 640.0;
    prompt.y = 0.5;
    const bool ok = ai::segmentFrame(folder.string(), rgba.data(), w, h, prompt, alpha, &score, &error);
    INFO(error);
    REQUIRE(ok);
    const auto [in, out] = insideOutside(alpha, w, h, ballX(0), 180, 55, 90);
    INFO("SAM: objeto " << in << ", fondo " << out << ", confianza " << score);
    CHECK(in > 0.9);
    CHECK(out < 0.05);
    CHECK(score > 0.5);

    // Por todo el clip: a 0,6 s la máscara sigue sobre la bola.
    QTemporaryDir dir;
    ai::MatteResult result;
    REQUIRE(ai::segmentObject(folder.string(), test::mediaPath("subject_ball.mp4"), 0, 0, ballX(0) / 640.0, 0.5,
                              QDir::toNativeSeparators(dir.filePath("obj.mp4")).toStdString(), result, &error));
    CHECK(result.frames == 10);
    media::VideoDecoder dec;
    REQUIRE(dec.open(result.output));
    REQUIRE(dec.seek(fromSeconds(0.6)));
    auto f = dec.decodeNext();
    REQUIRE(f);
    std::vector<uint8_t> luma(f->y.begin(), f->y.begin() + std::ptrdiff_t(f->width) * f->height);
    for (uint8_t& v : luma)
        v = uint8_t(std::clamp((v - 16) * 255 / 219, 0, 255));
    const auto [in6, out6] = insideOutside(luma, f->width, f->height, ballX(0.6), 180, 55, 90);
    INFO("a 0,6 s: objeto " << in6 << ", fondo " << out6);
    CHECK(in6 > 0.9);
    CHECK(out6 < 0.05);
}
