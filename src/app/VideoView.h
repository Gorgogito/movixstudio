#pragma once

#include "playback/TimelinePlayer.h"

#include <QPointer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

// Visor: muestra en la GPU (Qt RHI) el lienzo del proyecto con el fotograma compuesto que el
// reproductor indica para cada refresco de pantalla.
class VideoView : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(movix::playback::TimelinePlayer* player READ player WRITE setPlayer NOTIFY playerChanged)

public:
    explicit VideoView(QQuickItem* parent = nullptr);

    movix::playback::TimelinePlayer* player() const { return m_player; }
    void setPlayer(movix::playback::TimelinePlayer* player);

signals:
    void playerChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    friend class VideoRenderer;
    void onAfterAnimating();

    QPointer<movix::playback::TimelinePlayer> m_player;
    QMetaObject::Connection m_animatingConnection;
};
