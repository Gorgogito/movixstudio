#pragma once

#include "core/Project.h"

#include <QAbstractListModel>
#include <QSet>
#include <QtQml/qqmlregistration.h>

#include <unordered_map>
#include <vector>

// Modelos de solo lectura que reflejan el proyecto en QML. Se sincronizan tras cada edición
// comparando por id: solo cambian las filas afectadas, así los delegados no se recrean
// (importante mientras se arrastra un clip).

class SyncingListModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

protected:
    // Lleva `current` a `next` quitando, actualizando y añadiendo solo las filas necesarias.
    template <typename Item, typename IdOf>
    void syncRows(std::vector<Item>& current, std::vector<Item> next, IdOf idOf)
    {
        std::unordered_map<movix::core::Id, size_t> nextIndex;
        for (size_t i = 0; i < next.size(); ++i)
            nextIndex[idOf(next[i])] = i;
        for (int row = int(current.size()) - 1; row >= 0; --row) {
            if (!nextIndex.contains(idOf(current[size_t(row)]))) {
                beginRemoveRows({}, row, row);
                current.erase(current.begin() + row);
                endRemoveRows();
            }
        }
        std::unordered_map<movix::core::Id, size_t> currentIndex;
        for (size_t i = 0; i < current.size(); ++i) {
            currentIndex[idOf(current[i])] = i;
            const Item& updated = next[nextIndex[idOf(current[i])]];
            if (!(current[i] == updated)) {
                current[i] = updated;
                emit dataChanged(index(int(i)), index(int(i)));
            }
        }
        std::vector<Item> added;
        for (const Item& item : next)
            if (!currentIndex.contains(idOf(item)))
                added.push_back(item);
        if (!added.empty()) {
            beginInsertRows({}, int(current.size()), int(current.size() + added.size()) - 1);
            current.insert(current.end(), added.begin(), added.end());
            endInsertRows();
        }
    }
};

class MediaListModel : public SyncingListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Lo crea ProjectController")
public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, PathRole, DurationRole, HasVideoRole, HasAudioRole, WidthRole, HeightRole,
                FrameRateRole, ThumbnailRole, UsageRole };
    using SyncingListModel::SyncingListModel;

    int rowCount(const QModelIndex& = {}) const override { return int(m_items.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sync(const movix::core::Project& project);

private:
    struct Item {
        movix::core::MediaItem media;
        int usage = 0; // clips que lo usan
        bool operator==(const Item&) const = default;
    };
    std::vector<Item> m_items;
};

class TrackListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Lo crea ProjectController")
public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, KindRole, MutedRole, HiddenRole, LockedRole, IndexRole };
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& = {}) const override { return int(m_items.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sync(const movix::core::Project& project);

    // Fila de pantalla de una pista: video de arriba abajo (Vn…V1) y después audio (A1…An).
    static int displayRow(const movix::core::Project& p, movix::core::TrackKind kind, int index);

private:
    struct Item {
        movix::core::Id id;
        QString name;
        movix::core::TrackKind kind;
        bool muted, hidden, locked;
        int index;
        bool operator==(const Item&) const = default;
    };
    std::vector<Item> m_items;
};

class ClipListModel : public SyncingListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Lo crea ProjectController")
public:
    enum Role { IdRole = Qt::UserRole + 1, TrackIdRole, TrackRowRole, StartRole, DurationRole, MediaIdRole, NameRole, PathRole,
                SourceInRole, MediaDurationRole, KindRole, HasAudioRole, HasVideoRole, EnabledRole, GainRole, SelectedRole,
                IsTitleRole, IsStillRole, SpeedRole, TransitionRole, FadeInRole, FadeOutRole, IsCompoundRole, IsGeneratorRole };
    using SyncingListModel::SyncingListModel;

    int rowCount(const QModelIndex& = {}) const override { return int(m_items.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sync(const movix::core::Project& project, const QSet<movix::core::Id>& selection);

private:
    struct Item {
        movix::core::Clip clip;
        movix::core::Id trackId;
        int trackRow;
        movix::core::TrackKind kind;
        QString name;
        QString path;
        qint64 mediaDuration;
        bool hasAudio, hasVideo, selected, isStill;
        bool operator==(const Item&) const = default;
    };
    std::vector<Item> m_items;
};
