#include "core/Edits.h"
#include "core/History.h"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace movix;
using namespace movix::core;

namespace {

MediaItem media(TimeUs duration, bool video, bool audio)
{
    MediaItem m;
    m.path = "C:\\media\\m.mp4";
    m.duration = duration;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

} // namespace

TEST_CASE("Deshacer y rehacer restauran exactamente cada estado", "[core][history]")
{
    History h;
    Id mediaId = kNoId;
    REQUIRE(h.apply("Importar", [&](Project& p) { return (mediaId = edit::addMedia(p, media(10 * kUsPerSecond, true, true))) != kNoId; }));
    const Project afterImport = h.project();
    const Id v1 = afterImport.videoTracks[0].id;

    Id clip = kNoId;
    REQUIRE(h.apply("Añadir clip", [&](Project& p) { return (clip = edit::placeClip(p, v1, mediaId, 0)) != kNoId; }));
    const Project afterPlace = h.project();
    REQUIRE(h.apply("Dividir", [&](Project& p) { return edit::splitClip(p, clip, 100) != kNoId; }));
    const Project afterSplit = h.project();

    CHECK(h.undoText() == "Dividir");
    REQUIRE(h.undo());
    CHECK(h.project() == afterPlace);
    REQUIRE(h.undo());
    CHECK(h.project() == afterImport);
    CHECK(h.redoText() == "Añadir clip");
    REQUIRE(h.redo());
    REQUIRE(h.redo());
    CHECK(h.project() == afterSplit);
    CHECK_FALSE(h.redo());

    // Una edición nueva descarta lo que se podía rehacer.
    REQUIRE(h.undo());
    REQUIRE(h.apply("Eliminar", [&](Project& p) { return edit::deleteClips(p, {clip}); }));
    CHECK_FALSE(h.canRedo());

    // Una edición que no cambia nada no se registra.
    CHECK_FALSE(h.apply("Nada", [&](Project& p) { return edit::deleteClips(p, {12345}); }));
    CHECK(h.undoText() == "Eliminar");
}

TEST_CASE("Estado guardado y agrupación de ediciones continuas", "[core][history]")
{
    History h;
    Id mediaId = kNoId;
    h.apply("Importar", [&](Project& p) { return (mediaId = edit::addMedia(p, media(5 * kUsPerSecond, false, true))) != kNoId; });
    Id clip = kNoId;
    const Id a1 = h.project().audioTracks[0].id;
    h.apply("Añadir", [&](Project& p) { return (clip = edit::placeClip(p, a1, mediaId, 0)) != kNoId; });
    h.markClean();
    CHECK(h.isClean());

    // Arrastrar el volumen: muchas ediciones con la misma clave → un único paso de deshacer.
    for (float g : {0.9f, 0.8f, 0.7f, 0.5f})
        h.apply("Volumen", [&](Project& p) { return edit::setClipGain(p, clip, g); }, "gain");
    CHECK_FALSE(h.isClean());
    h.breakMerge();
    REQUIRE(h.undo());
    CHECK(h.project().findClip(clip)->gain == 1.0f);
    CHECK(h.isClean()); // volver al estado guardado lo marca como limpio
    REQUIRE(h.redo());
    CHECK(h.project().findClip(clip)->gain == 0.5f);
}

TEST_CASE("Ediciones aleatorias mantienen los invariantes y son reversibles", "[core][history][fuzz]")
{
    std::mt19937 rng(20261017);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    History h;
    std::vector<Id> mediaIds;
    for (int i = 0; i < 3; ++i) {
        h.apply("Importar", [&](Project& p) {
            mediaIds.push_back(edit::addMedia(p, media(TimeUs(rnd(2, 20)) * kUsPerSecond, i != 2, i != 0)));
            return true;
        });
    }

    auto anyClip = [&](const Project& p) -> Id {
        std::vector<Id> ids;
        for (const auto* list : {&p.videoTracks, &p.audioTracks})
            for (const auto& t : *list)
                for (const auto& c : t.clips)
                    ids.push_back(c.id);
        return ids.empty() ? kNoId : ids[size_t(rnd(0, int(ids.size()) - 1))];
    };

    std::vector<Project> states{h.project()};
    for (int step = 0; step < 3000; ++step) {
        const int op = rnd(0, 9);
        h.apply("op", [&](Project& p) {
            const auto& tracks = rnd(0, 1) ? p.videoTracks : p.audioTracks;
            const Id track = tracks[size_t(rnd(0, int(tracks.size()) - 1))].id;
            const Id clip = anyClip(p);
            switch (op) {
            case 0:
            case 1:
                return edit::placeClip(p, track, mediaIds[size_t(rnd(0, 2))], rnd(0, 900), TimeUs(rnd(0, 3)) * kUsPerSecond,
                                       rnd(0, 200)) != kNoId;
            case 2: return edit::moveClips(p, {clip}, rnd(-120, 120), rnd(-1, 1));
            case 3: return edit::trimClip(p, clip, rnd(0, 1) ? edit::Edge::Start : edit::Edge::End, rnd(0, 1000)).has_value();
            case 4: return edit::splitClip(p, clip, rnd(0, 1000)) != kNoId;
            case 5: return !edit::splitAt(p, rnd(0, 1000)).empty();
            case 6: return edit::deleteClips(p, {clip}, rnd(0, 1) == 1);
            case 7: return edit::setTrackLocked(p, track, rnd(0, 3) == 0);
            case 8: return rnd(0, 4) == 0 && edit::addTrack(p, rnd(0, 1) ? TrackKind::Video : TrackKind::Audio) != kNoId;
            default: return edit::setClipGain(p, clip, float(rnd(0, 20)) / 10.0f);
            }
        });
        std::string why;
        INFO("paso " << step << ": " << why);
        REQUIRE(h.project().validate(&why));
        if (states.back() != h.project())
            states.push_back(h.project());
    }
    INFO("estados distintos: " << states.size());
    CHECK(states.size() > 1000);

    // Deshacer todo recorre exactamente los estados en orden inverso (dentro del límite del historial).
    size_t idx = states.size() - 1;
    while (h.canUndo()) {
        REQUIRE(h.undo());
        --idx;
        REQUIRE(h.project() == states[idx]);
    }
    while (h.canRedo()) {
        REQUIRE(h.redo());
        ++idx;
        REQUIRE(h.project() == states[idx]);
    }
    CHECK(idx == states.size() - 1);
}
