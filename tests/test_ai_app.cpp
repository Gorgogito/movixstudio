#include "AiController.h"
#include "ProjectController.h"
#include "TestMedia.h"
#include "media/AudioFilter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <cmath>
#include <filesystem>

using namespace movix;

namespace {

bool modelsAvailable()
{
    const std::filesystem::path dir(MOVIX_TEST_MODELS_DIR);
    return std::filesystem::exists(dir / "ggml-base.bin") && std::filesystem::exists(dir / "ggml-silero-v6.2.0.bin")
        && std::filesystem::exists(dir / "rnnoise-somnolent-hogwash.rnnn") && std::filesystem::exists(dir / "kokoro-82m/model.onnx")
        && std::filesystem::exists(dir / "opus-mt-es-en/decoder.onnx") && std::filesystem::exists(dir / "htdemucs/htdemucs.onnx")
        && std::filesystem::exists(dir / "modnet/model.onnx") && std::filesystem::exists(dir / "realesrgan-x4v3/model.onnx")
        && std::filesystem::exists(dir / "rife-4/model.onnx") && std::filesystem::exists(dir / "sam2-tiny/vision_encoder.onnx_data");
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

    SECTION("IA visual: recorte, objeto, seguimiento, reencuadre, escalado y cámara lenta")
    {
        const core::Id clip = placeMedia(project, "subject_ball.mp4");
        auto run = [&](auto action) {
            project.select(qint64(clip), false);
            action();
            waitIdle(ai);
            INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
            REQUIRE(errors.isEmpty());
        };
        const auto& p = [&]() -> const core::Project& { return project.project(); };

        run([&] { ai.removeBackground("portrait"); });
        CHECK(p().findClip(clip)->matte.active());
        project.removeMatte(qint64(clip));
        CHECK_FALSE(p().findClip(clip)->matte.active());

        // Objeto con un clic (cabezal a mitad del clip, clic en el centro de la bola): el mate cubre
        // todo el clip, también lo anterior al cabezal.
        project.player()->seekFrame(p().settings.timeToFrame(fromSeconds(0.5)));
        run([&] { ai.segmentObject((320.0 + 100.0 * std::sin(1.5)) / 640.0, 0.5); });
        CHECK(p().findClip(clip)->matte.active());
        CHECK(p().findClip(clip)->matte.start == 0);
        project.player()->seekFrame(0);

        // Seguimiento: una máscara rectangular sobre la bola sigue su movimiento.
        REQUIRE(project.addMask(qint64(clip), "rectangle") == 0);
        project.setClipProperty(qint64(clip), "mask.0.x", 0.5);
        project.setClipProperty(qint64(clip), "mask.0.width", 0.25);
        project.setClipProperty(qint64(clip), "mask.0.height", 0.44);
        run([&] { ai.trackMotion(); });
        const auto& mx = p().findClip(clip)->masks[0].x;
        REQUIRE(mx.isAnimated());
        CHECK(mx.keys.size() >= 9);
        CHECK(mx.at(p().settings.timeToFrame(fromSeconds(0.5))) * 640.0 == Catch::Approx(320.0 + 100.0 * std::sin(1.5)).margin(10));

        // Reencuadre vertical: el proyecto pasa a 9:16 y el clip cubre el lienzo siguiendo a la bola.
        run([&] { ai.autoReframe("9:16"); });
        CHECK(p().settings.width == 360);
        CHECK(p().settings.height == 640);
        CHECK(p().findClip(clip)->transform.scale.value == Catch::Approx(640.0 / 360.0 * 640.0 / 360.0).epsilon(0.01));
        CHECK(p().findClip(clip)->transform.x.isAnimated());

        // Escalado ×2 y cámara lenta ×2 (el medio se sustituye; la velocidad baja a la mitad).
        const core::FramePos duration = p().findClip(clip)->duration;
        run([&] { ai.upscale(2); });
        CHECK(p().findMedia(p().findClip(clip)->mediaId)->width == 1280);
        run([&] { ai.slowMotion(2); });
        CHECK(p().findClip(clip)->speed == Catch::Approx(0.5));
        CHECK(p().findClip(clip)->duration == 2 * duration);
        CHECK(p().findMedia(p().findClip(clip)->mediaId)->frameRate == Catch::Approx(20.0));
    }

    SECTION("Seguimiento con escala y giro de la máscara y de un título adjunto")
    {
        // Tarjeta que avanza (x = 200 + 120·t px), crece (1 + 0,4·t) y gira (0,6·t rad, horario).
        const core::Id clip = placeMedia(project, "subject_card.mp4");
        const auto& p = [&]() -> const core::Project& { return project.project(); };
        REQUIRE(p().settings.width == 640);
        project.player()->seekFrame(0);
        REQUIRE(project.addTitle(0, -1, 0));
        core::Id title = core::kNoId;
        for (const auto& t : p().videoTracks)
            for (const auto& c : t.clips)
                if (c.id != clip)
                    title = c.id;
        REQUIRE(title != core::kNoId);
        project.setClipProperty(qint64(title), "x", 0.0);
        project.setClipProperty(qint64(title), "y", -0.25); // 90 px por encima del centro del lienzo
        REQUIRE(project.addMask(qint64(clip), "rectangle") == 0);
        project.setClipProperty(qint64(clip), "mask.0.x", 200.0 / 640.0);
        project.setClipProperty(qint64(clip), "mask.0.y", 0.5);
        project.setClipProperty(qint64(clip), "mask.0.width", 130.0 / 640.0);
        project.setClipProperty(qint64(clip), "mask.0.height", 90.0 / 360.0);
        project.select(qint64(clip), false);
        project.select(qint64(title), true);
        ai.trackMotion(true);
        waitIdle(ai);
        INFO((errors.isEmpty() ? QString() : errors.last().at(0).toString()).toStdString());
        REQUIRE(errors.isEmpty());

        const core::Clip& c = *p().findClip(clip);
        const core::FramePos last = c.duration - 1;
        const double t = toSeconds(p().settings.frameToTime(last)), angle = 0.6 * t * 180 / 3.14159265, scale = 1 + 0.4 * t;
        const core::Mask& mask = c.masks[0];
        CHECK(mask.x.at(last) * 640 == Catch::Approx(200 + 120 * t).margin(3));
        CHECK(mask.width.at(last) == Catch::Approx(130.0 / 640.0 * scale).epsilon(0.03));
        CHECK(mask.height.at(last) == Catch::Approx(90.0 / 360.0 * scale).epsilon(0.03));
        CHECK(mask.rotation.at(last) == Catch::Approx(angle).margin(1.5));
        // El título conserva su separación del objeto (120 px a la derecha y 90 px arriba al empezar),
        // girada y escalada con él.
        const core::Clip& tc = *p().findClip(title);
        const core::FramePos tl = last + c.start - tc.start;
        CHECK(tc.transform.scale.at(tl) == Catch::Approx(scale).epsilon(0.03));
        CHECK(tc.transform.rotation.at(tl) == Catch::Approx(angle).margin(1.5));
        const double a = 0.6 * t, dx = 120, dy = -90;
        CHECK(tc.transform.x.at(tl) * 640 == Catch::Approx(200 + 120 * t - 320 + scale * (dx * std::cos(a) - dy * std::sin(a))).margin(6));
        CHECK(tc.transform.y.at(tl) * 360 == Catch::Approx(scale * (dx * std::sin(a) + dy * std::cos(a))).margin(6));
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
