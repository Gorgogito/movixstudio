#include "TimelineModels.h"

#include "ThumbnailProvider.h"

#include <QHash>

using namespace movix;
using namespace movix::core;



// --- Medios ---

QVariant MediaListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || size_t(index.row()) >= m_items.size())
        return {};
    const Item& item = m_items[size_t(index.row())];
    const MediaItem& m = item.media;
    switch (role) {
    case IdRole: return qint64(m.id);
    case NameRole: return QString::fromStdString(m.name);
    case PathRole: return QString::fromStdString(m.path);
    case DurationRole: return toSeconds(m.duration);
    case HasVideoRole: return m.hasVideo;
    case HasAudioRole: return m.hasAudio;
    case WidthRole: return m.width;
    case HeightRole: return m.height;
    case FrameRateRole: return m.frameRate;
    case ThumbnailRole:
        // Miniatura al 10 % del medio (evita fundidos a negro del principio).
        return m.hasVideo ? QStringLiteral("image://thumbs/") + ThumbnailProvider::idFor(QString::fromStdString(m.path), m.duration / 10)
                          : QString();
    case UsageRole: return item.usage;
    }
    return {};
}

QHash<int, QByteArray> MediaListModel::roleNames() const
{
    return {{IdRole, "mediaId"}, {NameRole, "name"}, {PathRole, "path"}, {DurationRole, "duration"},
            {HasVideoRole, "hasVideo"}, {HasAudioRole, "hasAudio"}, {WidthRole, "mediaWidth"}, {HeightRole, "mediaHeight"},
            {FrameRateRole, "mediaFrameRate"}, {ThumbnailRole, "thumbnail"}, {UsageRole, "usage"}};
}

void MediaListModel::sync(const Project& project)
{
    std::unordered_map<Id, int> usage;
    for (const auto* list : {&project.videoTracks, &project.audioTracks})
        for (const auto& t : *list)
            for (const auto& c : t.clips)
                ++usage[c.mediaId];
    std::vector<Item> next;
    for (const MediaItem& m : project.media)
        next.push_back({m, usage[m.id]});
    syncRows(m_items, std::move(next), [](const Item& i) { return i.media.id; });
}

// --- Pistas ---

int TrackListModel::displayRow(const Project& p, TrackKind kind, int index)
{
    return kind == TrackKind::Video ? int(p.videoTracks.size()) - 1 - index : int(p.videoTracks.size()) + index;
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || size_t(index.row()) >= m_items.size())
        return {};
    const Item& t = m_items[size_t(index.row())];
    switch (role) {
    case IdRole: return qint64(t.id);
    case NameRole: return t.name;
    case KindRole: return t.kind == TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio");
    case MutedRole: return t.muted;
    case HiddenRole: return t.hidden;
    case LockedRole: return t.locked;
    case IndexRole: return t.index;
    }
    return {};
}

QHash<int, QByteArray> TrackListModel::roleNames() const
{
    return {{IdRole, "trackId"}, {NameRole, "name"}, {KindRole, "kind"}, {MutedRole, "muted"},
            {HiddenRole, "hidden"}, {LockedRole, "locked"}, {IndexRole, "trackIndex"}};
}

void TrackListModel::sync(const Project& project)
{
    std::vector<Item> next;
    for (int i = int(project.videoTracks.size()) - 1; i >= 0; --i) {
        const Track& t = project.videoTracks[size_t(i)];
        next.push_back({t.id, QString::fromStdString(t.name), t.kind, t.muted, t.hidden, t.locked, i});
    }
    for (int i = 0; i < int(project.audioTracks.size()); ++i) {
        const Track& t = project.audioTracks[size_t(i)];
        next.push_back({t.id, QString::fromStdString(t.name), t.kind, t.muted, t.hidden, t.locked, i});
    }
    // El orden de las pistas importa: si cambia el conjunto u orden, se reinicia el modelo.
    bool sameOrder = next.size() == m_items.size();
    for (size_t i = 0; sameOrder && i < next.size(); ++i)
        sameOrder = next[i].id == m_items[i].id;
    if (!sameOrder) {
        beginResetModel();
        m_items = std::move(next);
        endResetModel();
        return;
    }
    for (size_t i = 0; i < next.size(); ++i) {
        if (!(next[i] == m_items[i])) {
            m_items[i] = next[i];
            emit dataChanged(index(int(i)), index(int(i)));
        }
    }
}

// --- Clips ---

QVariant ClipListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || size_t(index.row()) >= m_items.size())
        return {};
    const Item& i = m_items[size_t(index.row())];
    switch (role) {
    case IdRole: return qint64(i.clip.id);
    case TrackIdRole: return qint64(i.trackId);
    case TrackRowRole: return i.trackRow;
    case StartRole: return qint64(i.clip.start);
    case DurationRole: return qint64(i.clip.duration);
    case MediaIdRole: return qint64(i.clip.mediaId);
    case NameRole: return i.name;
    case PathRole: return i.path;
    case SourceInRole: return toSeconds(i.clip.sourceIn);
    case MediaDurationRole: return toSeconds(i.mediaDuration);
    case KindRole: return i.kind == TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio");
    case HasAudioRole: return i.hasAudio;
    case HasVideoRole: return i.hasVideo;
    case EnabledRole: return i.clip.enabled;
    case GainRole: return i.clip.volume.isAnimated() ? -1.0 : i.clip.volume.value; // -1: animado
    case SelectedRole: return i.selected;
    case IsTitleRole: return i.clip.title.has_value();
    case IsCompoundRole: return i.clip.isCompound();
    case IsStillRole: return i.isStill;
    case SpeedRole: return i.clip.speed;
    case TransitionRole: return i.clip.transitionIn.active() ? qint64(i.clip.transitionIn.duration) : qint64(0);
    case FadeInRole: return qint64(i.clip.fadeIn);
    case FadeOutRole: return qint64(i.clip.fadeOut);
    }
    return {};
}

QHash<int, QByteArray> ClipListModel::roleNames() const
{
    return {{IdRole, "clipId"}, {TrackIdRole, "trackId"}, {TrackRowRole, "trackRow"}, {StartRole, "start"},
            {DurationRole, "duration"}, {MediaIdRole, "mediaId"}, {NameRole, "name"}, {PathRole, "path"},
            {SourceInRole, "sourceIn"}, {MediaDurationRole, "mediaDuration"}, {KindRole, "kind"},
            {HasAudioRole, "hasAudio"}, {HasVideoRole, "hasVideo"}, {EnabledRole, "clipEnabled"}, {GainRole, "gain"},
            {SelectedRole, "selected"}, {IsTitleRole, "isTitle"}, {IsCompoundRole, "isCompound"}, {IsStillRole, "isStill"}, {SpeedRole, "speed"},
            {TransitionRole, "transitionFrames"}, {FadeInRole, "fadeIn"}, {FadeOutRole, "fadeOut"}};
}

void ClipListModel::sync(const Project& project, const QSet<Id>& selection)
{
    std::vector<Item> next;
    // Multicámara: la secuencia y el ángulo que se ve.
    auto multicamName = [](const Project& p, const Clip& c) {
        const auto* angles = p.videoTracksOf(c.sequenceId);
        const QString angle = angles && c.angle < int(angles->size()) ? QString::fromStdString((*angles)[size_t(c.angle)].name) : QString();
        return QStringLiteral("%1 · %2").arg(QString::fromStdString(p.sequenceName(c.sequenceId)), angle);
    };
    auto collect = [&](const std::vector<Track>& tracks, TrackKind kind) {
        for (int ti = 0; ti < int(tracks.size()); ++ti) {
            const Track& t = tracks[size_t(ti)];
            for (const Clip& c : t.clips) {
                const MediaItem* m = c.isGenerated() ? nullptr : project.findMedia(c.mediaId);
                // Los títulos se identifican por su texto (primera línea); los compuestos, por su secuencia.
                const QString name = c.title          ? QString::fromStdString(c.title->text).section(QLatin1Char('\n'), 0, 0)
                                     : c.isMulticam() ? multicamName(project, c)
                                     : c.isCompound() ? QString::fromStdString(project.sequenceName(c.sequenceId))
                                     : m              ? QString::fromStdString(m->name)
                                                      : QString();
                const bool audio = c.isCompound() || (m && m->hasAudio && !(kind == TrackKind::Video && c.audioDetached));
                const qint64 sourceDuration = c.isCompound() ? qint64(project.settings.frameToTime(project.sequenceDuration(c.sequenceId)))
                                              : m            ? qint64(m->duration)
                                                             : 0;
                next.push_back({c, t.id, TrackListModel::displayRow(project, kind, ti), kind, name,
                                m ? QString::fromStdString(m->path) : QString(), sourceDuration, audio,
                                c.isGenerated() || (m && m->hasVideo), selection.contains(c.id), m && m->isStill});
            }
        }
    };
    collect(project.videoTracks, TrackKind::Video);
    collect(project.audioTracks, TrackKind::Audio);
    syncRows(m_items, std::move(next), [](const Item& i) { return i.clip.id; });
}
