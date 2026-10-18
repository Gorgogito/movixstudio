#include "TimelineModels.h"
#include "core/Edits.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractItemModelTester>

#include <map>
#include <random>

using namespace movix;
using namespace movix::core;

namespace {

MediaItem media(bool video, bool audio)
{
    MediaItem m;
    m.path = "C:\\media\\m.mp4";
    m.name = "m.mp4";
    m.duration = 20 * kUsPerSecond;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

// Comprueba que el modelo de clips refleja exactamente el proyecto.
void checkClips(const ClipListModel& model, const Project& p, const QSet<Id>& selection)
{
    std::map<Id, std::tuple<FramePos, FramePos, int, bool>> expected;
    for (int ti = 0; ti < int(p.videoTracks.size()); ++ti)
        for (const Clip& c : p.videoTracks[size_t(ti)].clips)
            expected[c.id] = {c.start, c.duration, TrackListModel::displayRow(p, TrackKind::Video, ti), selection.contains(c.id)};
    for (int ti = 0; ti < int(p.audioTracks.size()); ++ti)
        for (const Clip& c : p.audioTracks[size_t(ti)].clips)
            expected[c.id] = {c.start, c.duration, TrackListModel::displayRow(p, TrackKind::Audio, ti), selection.contains(c.id)};

    REQUIRE(model.rowCount() == int(expected.size()));
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex i = model.index(row);
        const Id id = Id(model.data(i, ClipListModel::IdRole).toLongLong());
        INFO("clip " << id);
        REQUIRE(expected.contains(id));
        const auto& [start, duration, trackRow, selected] = expected[id];
        CHECK(model.data(i, ClipListModel::StartRole).toLongLong() == start);
        CHECK(model.data(i, ClipListModel::DurationRole).toLongLong() == duration);
        CHECK(model.data(i, ClipListModel::TrackRowRole).toInt() == trackRow);
        CHECK(model.data(i, ClipListModel::SelectedRole).toBool() == selected);
    }
}

} // namespace

TEST_CASE("Los modelos de la línea de tiempo siguen al proyecto con señales correctas", "[models]")
{
    ClipListModel clips;
    TrackListModel tracks;
    MediaListModel mediaModel;
    // Valida cada inserción, eliminación y cambio que emiten los modelos.
    QAbstractItemModelTester clipTester(&clips, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QAbstractItemModelTester trackTester(&tracks, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QAbstractItemModelTester mediaTester(&mediaModel, QAbstractItemModelTester::FailureReportingMode::Fatal);

    std::mt19937 rng(42);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    Project p = Project::createDefault();
    const Id video = edit::addMedia(p, media(true, true));
    const Id audio = edit::addMedia(p, media(false, true));
    QSet<Id> selection;

    for (int step = 0; step < 600; ++step) {
        std::vector<Id> ids;
        for (const auto* list : {&p.videoTracks, &p.audioTracks})
            for (const auto& t : *list)
                for (const auto& c : t.clips)
                    ids.push_back(c.id);
        const Id someClip = ids.empty() ? kNoId : ids[size_t(rnd(0, int(ids.size()) - 1))];
        const auto& list = rnd(0, 1) ? p.videoTracks : p.audioTracks;
        const Id track = list[size_t(rnd(0, int(list.size()) - 1))].id;

        switch (rnd(0, 7)) {
        case 0:
        case 1: edit::placeClip(p, track, rnd(0, 1) ? video : audio, rnd(0, 600), 0, rnd(10, 120)); break;
        case 2: edit::moveClips(p, {someClip}, rnd(-60, 60), rnd(-1, 1)); break;
        case 3: edit::splitClip(p, someClip, rnd(0, 700)); break;
        case 4: edit::deleteClips(p, {someClip}, rnd(0, 1) == 1); break;
        case 5: edit::trimClip(p, someClip, rnd(0, 1) ? edit::Edge::Start : edit::Edge::End, rnd(0, 700)); break;
        case 6:
            if (rnd(0, 5) == 0)
                edit::addTrack(p, rnd(0, 1) ? TrackKind::Video : TrackKind::Audio, rnd(0, 2));
            break;
        default:
            if (someClip != kNoId) {
                if (!selection.remove(someClip))
                    selection.insert(someClip);
            }
            break;
        }
        for (auto it = selection.begin(); it != selection.end();)
            it = p.findClip(*it) ? std::next(it) : selection.erase(it);

        clips.sync(p, selection);
        tracks.sync(p);
        mediaModel.sync(p);
        INFO("paso " << step);
        checkClips(clips, p, selection);
        REQUIRE(tracks.rowCount() == int(p.videoTracks.size() + p.audioTracks.size()));
    }
}
