#include "TestMedia.h"
#include "core/Edits.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/AudioReader.h"
#include "media/AudioDecoder.h"
#include "media/AudioFilter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace movix;

namespace {

std::string rnnoiseModel()
{
    return (std::filesystem::path(MOVIX_TEST_MODELS_DIR) / "rnnoise-somnolent-hogwash.rnnn").string();
}

std::vector<float> decodeAll(const std::string& path, int rate)
{
    media::AudioDecoder dec;
    REQUIRE(dec.open(path, rate, 1));
    std::vector<float> pcm;
    media::AudioChunk chunk;
    while (dec.read(chunk))
        pcm.insert(pcm.end(), chunk.samples.begin(), chunk.samples.end());
    return pcm;
}

double rms(const std::vector<float>& v, double fromSec, double toSec, int rate)
{
    const size_t a = size_t(fromSec * rate), b = std::min(v.size(), size_t(toSec * rate));
    double sum = 0;
    for (size_t i = a; i < b; ++i)
        sum += double(v[i]) * v[i];
    return std::sqrt(sum / double(std::max<size_t>(1, b - a)));
}

// Desfase (en muestras) que maximiza la correlación de `b` respecto de `a` en un tramo.
int bestLag(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t count, int range)
{
    int best = 0;
    double bestValue = -1e30;
    for (int lag = -range; lag <= range; ++lag) {
        double c = 0;
        for (size_t i = from; i < from + count; ++i)
            c += double(a[i]) * b[size_t(int64_t(i) + lag)];
        if (c > bestValue) {
            bestValue = c;
            best = lag;
        }
    }
    return best;
}

} // namespace

TEST_CASE("Reducción de ruido con RNNoise: menos ruido, voz intacta y sin desfase", "[ai][denoise]")
{
    if (!std::filesystem::exists(rnnoiseModel()))
        SKIP("Falta el modelo " << rnnoiseModel());
    const std::vector<float> in = decodeAll(test::mediaPath("speech_noisy.wav"), 48000);
    REQUIRE(in.size() > 400000);

    media::Denoiser denoiser;
    REQUIRE(denoiser.init(48000, 1, rnnoiseModel(), 1.0)); // la ruta lleva "D:" y barras: se escapa
    REQUIRE(denoiser.isValid());
    std::vector<float> out;
    for (size_t i = 0; i < in.size(); i += 1000)
        denoiser.process(in.data() + i, int(std::min<size_t>(1000, in.size() - i)), out);
    denoiser.flush(out);
    REQUIRE(out.size() == in.size());

    // Silencio entre frases (3,6–5,6 s): el ruido baja al menos 15 dB. Voz (1,0–2,5 s): se conserva.
    const double noiseIn = rms(in, 3.6, 5.6, 48000), noiseOut = rms(out, 3.6, 5.6, 48000);
    const double speechIn = rms(in, 1.0, 2.5, 48000), speechOut = rms(out, 1.0, 2.5, 48000);
    INFO("ruido " << noiseIn << " → " << noiseOut << "; voz " << speechIn << " → " << speechOut);
    CHECK(20 * std::log10(noiseIn / std::max(noiseOut, 1e-9)) > 15.0);
    CHECK(speechOut > 0.5 * speechIn);
    // Sin desfase: la salida queda alineada con la entrada (el retardo de 10 ms se compensa).
    CHECK(std::abs(bestLag(in, out, 60000, 40000, 600)) <= 1);

    // Con intensidad 0,5 queda a medio camino.
    REQUIRE(denoiser.init(48000, 1, rnnoiseModel(), 0.5));
    std::vector<float> half;
    denoiser.process(in.data(), int(in.size()), half);
    denoiser.flush(half);
    const double noiseHalf = rms(half, 3.6, 5.6, 48000);
    CHECK(noiseHalf < noiseIn);
    CHECK(noiseHalf > noiseOut);

    // Un modelo inexistente deja pasar el audio sin tocarlo.
    REQUIRE_FALSE(denoiser.init(48000, 1, "no-existe.rnnn", 1.0));
    std::vector<float> same;
    denoiser.process(in.data(), 4800, same);
    CHECK(same == std::vector<float>(in.begin(), in.begin() + 4800));
}

TEST_CASE("La reducción de ruido es determinista al vaciar un final incompleto", "[ai][denoise]")
{
    if (!std::filesystem::exists(rnnoiseModel()))
        SKIP("Falta el modelo " << rnnoiseModel());
    // Una longitud que no es múltiplo del bloque de RNNoise (480): antes se le pasaba el fin de
    // flujo y arnndn leía el último bloque fuera del búfer (salida variable o cierre del proceso).
    const auto full = decodeAll(test::mediaPath("speech_noisy.wav"), 48000);
    REQUIRE(full.size() > 48000 * 2);
    const std::vector<float> in(full.begin(), full.begin() + 48000 * 2 + 317);
    std::vector<float> first;
    for (int run = 0; run < 12; ++run) {
        media::Denoiser denoiser;
        REQUIRE(denoiser.init(48000, 1, rnnoiseModel(), 1.0));
        std::vector<float> out;
        for (size_t i = 0; i < in.size(); i += 1100)
            denoiser.process(in.data() + i, int(std::min<size_t>(1100, in.size() - i)), out);
        denoiser.flush(out);
        REQUIRE(out.size() == in.size());
        REQUIRE(std::all_of(out.begin(), out.end(), [](float v) { return std::isfinite(v); }));
        if (run == 0)
            first = out;
        else
            REQUIRE(out == first);
    }
}

TEST_CASE("El lector aplica la reducción de ruido del clip y busca sin desfase", "[ai][denoise]")
{
    if (!std::filesystem::exists(rnnoiseModel()))
        SKIP("Falta el modelo " << rnnoiseModel());
    const std::string path = test::mediaPath("speech_noisy.wav");
    media::setDenoiseModel(rnnoiseModel());
    engine::AudioReader reader;
    REQUIRE(reader.open(path, 48000, 2));
    auto block = [&](double at, double denoise) {
        std::vector<float> out(48000 * 2, 0.0f);
        reader.mixInto(fromSeconds(at), 48000, 1.0, 1.0f, 1.0f, out.data(), denoise);
        std::vector<float> mono(48000);
        for (int i = 0; i < 48000; ++i)
            mono[size_t(i)] = out[size_t(i) * 2];
        return mono;
    };
    const auto noisy = block(4.0, 0.0);
    const auto clean = block(4.0, 1.0); // cambia el ajuste: vuelve a leer desde 4,0 s
    const auto cleanAgain = block(4.0, 1.0);
    CHECK(rms(clean, 0, 1, 48000) < 0.2 * rms(noisy, 0, 1, 48000));
    CHECK(rms(cleanAgain, 0, 1, 48000) < 0.2 * rms(noisy, 0, 1, 48000));
    // En plena voz, la versión filtrada sigue alineada con la original.
    const auto voice = block(1.0, 0.0);
    const auto voiceClean = block(1.0, 1.0);
    CHECK(std::abs(bestLag(voice, voiceClean, 6000, 30000, 600)) <= 1);
    media::setDenoiseModel({});
    const auto plain = block(4.0, 1.0); // sin modelo no se filtra
    CHECK(rms(plain, 0, 1, 48000) == Catch::Approx(rms(noisy, 0, 1, 48000)).epsilon(0.01));

    // El ajuste del clip llega a los tramos de audio y se guarda en el proyecto.
    auto p = core::Project::createDefault();
    core::MediaItem m;
    m.path = path;
    m.duration = 9 * kUsPerSecond;
    m.hasAudio = true;
    const core::Id media = core::edit::addMedia(p, m);
    const core::Id clip = core::edit::placeClip(p, p.audioTracks[0].id, media, 0);
    REQUIRE(core::edit::setDenoise(p, clip, 0.8));
    CHECK_FALSE(core::edit::setDenoise(p, clip, 0.8));
    CHECK(core::audioSegmentsIn(p, 0, kUsPerSecond).at(0).denoise == Catch::Approx(0.8));
    QTemporaryDir dir;
    REQUIRE(core::saveProjectFile(p, dir.filePath("d.mxproj")));
    const auto loaded = core::loadProjectFile(dir.filePath("d.mxproj"));
    REQUIRE(loaded.project);
    CHECK(loaded.project->findClip(clip)->denoise == Catch::Approx(0.8));
}
