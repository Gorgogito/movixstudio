#include "AiController.h"
#include "ProjectController.h"
#include "TestMedia.h"
#include "media/AudioFilter.h"

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <filesystem>

using namespace movix;

namespace {

bool modelsAvailable()
{
    const std::filesystem::path dir(MOVIX_TEST_MODELS_DIR);
    return std::filesystem::exists(dir / "ggml-base.bin") && std::filesystem::exists(dir / "ggml-silero-v6.2.0.bin")
        && std::filesystem::exists(dir / "rnnoise-somnolent-hogwash.rnnn") && std::filesystem::exists(dir / "kokoro-82m/model.onnx")
        && std::filesystem::exists(dir / "opus-mt-es-en/decoder.onnx") && std::filesystem::exists(dir / "htdemucs/htdemucs.onnx");
}

// Importa un medio, lo pone en la línea de tiempo y selecciona su clip. Devuelve el id del clip.
core::Id placeMedia(ProjectController& project, const char* name)
{
    project.newProject();
    project.importFiles({QUrl::fromLocalFile(QString::fromStdString(test::mediaPath(name)))});
    REQUIRE(QTest::qWaitFor([&] { return project.importing() == 0 && !project.project().media.empty(); }, 10000));
    REQUIRE(project.appendToTimeline(qint64(project.project().media[0].id)));
    const auto& p = project.project();
    const auto& tracks = p.videoTracks[0].clips.empty() ? p.audioTracks : p.videoTracks;
    const core::Id clip = tracks[0].clips[0].id;
    project.select(qint64(clip), false);
    return clip;
}

void waitIdle(AiController& ai)
{
    REQUIRE(QTest::qWaitFor([&] { return !ai.busy(); }, 120000));
}

} // namespace

TEST_CASE("Aplicación: pulsos, escenas, subtítulos y silencios con el proceso de IA", "[ai][app]")
{
    if (!modelsAvailable())
        SKIP("Faltan los modelos de prueba");
    qputenv("MOVIX_MODELS_DIR", MOVIX_TEST_MODELS_DIR);
    ProjectController project;
    AiController ai;
    ai.setProject(&project);
    QSignalSpy errors(&ai, &AiController::errorOccurred);
    QSignalSpy notices(&ai, &AiController::notice);

    // El modelo de reducción de ruido instalado queda disponible para el audio.
    CHECK(ai.denoiseAvailable());
    CHECK_FALSE(media::denoiseModel().empty());

    SECTION("Pulsos y escenas de un clip con música")
    {
        const core::Id clip = placeMedia(project, "scenes_beats.mp4");
        ai.markBeats();
        CHECK(ai.busy());
        waitIdle(ai);
        INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
        REQUIRE(errors.isEmpty());
        const auto& markers = project.project().markers;
        // Pulsos en 0,25 + 0,5·k s (30 fps → 7,5 + 15·k fotogramas): ~12 en 6 s.
        REQUIRE(markers.size() >= 10);
        CHECK(markers.size() <= 12);
        for (const auto& m : markers) {
            const double frames = double(m.frame) - 7.5;
            const double phase = frames - 15.0 * std::round(frames / 15.0);
            CHECK(std::abs(phase) <= 1.6); // ±1,5 fotogramas
        }
        CHECK(project.markers().size() == qsizetype(markers.size()));

        project.select(qint64(clip), false);
        ai.splitScenes();
        waitIdle(ai);
        REQUIRE(errors.isEmpty());
        const auto& clips = project.project().videoTracks[0].clips;
        REQUIRE(clips.size() == 3);
        CHECK(clips[1].start == 60);
        CHECK(clips[2].start == 120);
        // Deshacer devuelve el clip entero.
        project.undo();
        CHECK(project.project().videoTracks[0].clips.size() == 1);
    }

    SECTION("Subtítulos y silencios sobre voz con ruido")
    {
        placeMedia(project, "speech_noisy.wav");
        ai.transcribe("whisper-base", "es", false, true);
        waitIdle(ai);
        INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
        REQUIRE(errors.isEmpty());
        const auto& p = project.project();
        REQUIRE(p.media[0].transcript);
        const auto& subtitles = p.videoTracks.back();
        CHECK(subtitles.clips.size() >= 2);
        CHECK(subtitles.clips.front().title.has_value());
        CHECK(ai.words().size() >= 10);

        const auto before = project.project().duration();
        ai.removeSilences(0.5, 600, 120);
        waitIdle(ai);
        REQUIRE(errors.isEmpty());
        const auto after = project.project().duration();
        INFO(before << " → " << after);
        CHECK(after < before - 90); // al menos 3 s menos (silencios de 0,6 + 2,9 + 1,3 s)
        CHECK(after > 120);         // la voz (~5 s) se queda

        // Traducir la pista de subtítulos al inglés: pista nueva, mismos tiempos, la original oculta.
        const size_t tracks = project.project().videoTracks.size();
        const auto originalTrack = project.project().videoTracks.back();
        ai.translateSubtitles("es", "en");
        waitIdle(ai);
        REQUIRE(errors.isEmpty());
        REQUIRE(project.project().videoTracks.size() == tracks + 1);
        const auto& translated = project.project().videoTracks.back();
        REQUIRE(translated.clips.size() == originalTrack.clips.size());
        CHECK(translated.clips[0].start == originalTrack.clips[0].start);
        CHECK(translated.clips[0].title->text != originalTrack.clips[0].title->text);
        CHECK(project.project().findTrack(originalTrack.id)->hidden);

        // Cortar las tres primeras palabras por texto.
        const auto words = ai.words().size();
        ai.deleteWords(0, 2);
        CHECK(ai.words().size() == words - 3);
    }

    SECTION("Texto a voz y separación de voz")
    {
        const core::Id clip = placeMedia(project, "speech_noisy.wav");
        project.player()->seekFrame(30);
        ai.speak("Hola, esta voz la genera el editor.", "ef_dora", 1.0);
        waitIdle(ai);
        INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
        REQUIRE(errors.isEmpty());
        const auto& p = project.project();
        REQUIRE(p.media.size() == 2);
        // El clip de voz está en el cabezal, en una pista de audio libre (no la del clip original).
        const core::Clip* voice = nullptr;
        for (const auto& t : p.audioTracks)
            for (const auto& c : t.clips)
                if (c.mediaId == p.media[1].id)
                    voice = &c;
        REQUIRE(voice);
        CHECK(voice->start == 30);
        CHECK(p.trackOfClip(voice->id) != p.trackOfClip(clip));

        project.select(qint64(clip), false);
        ai.separateVoice();
        waitIdle(ai);
        INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
        REQUIRE(errors.isEmpty());
        REQUIRE(project.project().media.size() == 4);
        CHECK_FALSE(project.project().findClip(clip)->enabled); // el original ya no suena
        int stems = 0;
        for (const auto& t : project.project().audioTracks)
            for (const auto& c : t.clips)
                if (c.mediaId == project.project().media[2].id || c.mediaId == project.project().media[3].id) {
                    ++stems;
                    CHECK(c.start == project.project().findClip(clip)->start);
                    CHECK(std::abs(c.duration - project.project().findClip(clip)->duration) <= 1);
                }
        CHECK(stems == 2);
        project.undo();
        CHECK(project.project().findClip(clip)->enabled);
    }
}
