#pragma once

#include "playback/Player.h"

#include <QPointer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

// Visor de video: muestra en la GPU (Qt RHI: Direct3D 11/12, Vulkan, Metal u OpenGL) el
// fotograma que el reproductor indica para cada refresco de pantalla, ajustado al área
// visible con su relación de aspecto.
class VideoView : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(movix::playback::Player* player READ player WRITE setPlayer NOTIFY playerChanged)

public:
    explicit VideoView(QQuickItem* parent = nullptr);

    movix::playback::Player* player() const { return m_player; }
    void setPlayer(movix::playback::Player* player);

signals:
    void playerChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    friend class VideoRenderer;
    void onAfterAnimating();

    QPointer<movix::playback::Player> m_player;
    QMetaObject::Connection m_animatingConnection;
};
