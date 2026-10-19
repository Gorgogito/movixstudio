#pragma once

#include "ScopeFeed.h"
#include "playback/TimelinePlayer.h"

#include <QPointer>
#include <QQuickRhiItem>
#include <QtQml/qqmlregistration.h>

#include <memory>

// Visor: muestra en la GPU (Qt RHI) el lienzo del proyecto con el fotograma compuesto que el
// reproductor indica para cada refresco de pantalla. Con `scopesActive`, publica además una copia
// reducida del lienzo (unas 15 veces por segundo) para los scopes.
class VideoView : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(movix::playback::TimelinePlayer* player READ player WRITE setPlayer NOTIFY playerChanged)
    Q_PROPERTY(bool scopesActive READ scopesActive WRITE setScopesActive NOTIFY scopesActiveChanged)

public:
    explicit VideoView(QQuickItem* parent = nullptr);

    movix::playback::TimelinePlayer* player() const { return m_player; }
    void setPlayer(movix::playback::TimelinePlayer* player);
    bool scopesActive() const { return m_feed->active(); }
    void setScopesActive(bool active);
    std::shared_ptr<const ScopeFeed> scopeFeed() const { return m_feed; }

signals:
    void playerChanged();
    void scopesActiveChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    friend class VideoRenderer;
    void onAfterAnimating();

    QPointer<movix::playback::TimelinePlayer> m_player;
    QMetaObject::Connection m_animatingConnection;
    std::shared_ptr<ScopeFeed> m_feed = std::make_shared<ScopeFeed>();
};
