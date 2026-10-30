#pragma once

#include "core/Project.h"
#include "ofx/OfxHost.h"

#include <QImage>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <string>

class ProjectController;

// Interfaz de un efecto OpenFX sobre el visor: la imagen que dibuja el plugin (ver ofx::Overlay),
// del tamaño del elemento, que se coloca sobre la capa del clip. El ratón va al plugin y los
// parámetros que cambia pasan al proyecto como una edición (un paso de deshacer por arrastre, con
// keyframe si el parámetro está animado). Lo que el plugin no usa pasa a lo que haya debajo. Al
// hacer clic sobre ella toma el foco y el teclado también va al plugin: las teclas que use no llegan
// a los atajos de la aplicación.
class OfxOverlayItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(ProjectController* project READ project WRITE setProject NOTIFY targetChanged)
    Q_PROPERTY(qint64 clipId READ clipId WRITE setClipId NOTIFY targetChanged)
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY targetChanged)
    // Hay una interfaz que mostrar (el plugin la tiene y se pudo crear).
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)

public:
    explicit OfxOverlayItem(QQuickItem* parent = nullptr);
    ~OfxOverlayItem() override;

    ProjectController* project() const { return m_project; }
    void setProject(ProjectController* project);
    qint64 clipId() const { return m_clipId; }
    void setClipId(qint64 id);
    int index() const { return m_index; }
    void setIndex(int index);
    bool available() const { return m_overlay != nullptr; }

    void paint(QPainter* painter) override;
    // Vuelve a dibujar con el proyecto actual (también lo hace solo al cambiar el proyecto o el cabezal).
    void refresh();
    // Evento del ratón en coordenadas del elemento. Devuelve si el plugin lo usó.
    bool pen(movix::ofx::Overlay::Pen kind, QPointF position);
    // Tecla (`qtKey`: un Qt::Key; `text`: lo que escribe). Devuelve si el plugin la usó.
    bool key(movix::ofx::Overlay::Key kind, int qtKey, const QString& text);
    const QImage& image() const { return m_image; }

signals:
    void targetChanged();
    void availableChanged();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void rebuild(); // instancia del plugin para el efecto actual (se rehace si cambia)
    QSize viewSize() const;
    // Lleva al proyecto los parámetros que el plugin cambió en el fotograma `rel`.
    void commit(const movix::ofx::ParamValues& before, const movix::ofx::ParamValues& after, movix::core::FramePos rel);

    ProjectController* m_project = nullptr;
    qint64 m_clipId = 0;
    int m_index = 0;
    std::string m_pluginId;
    int m_role = -1;
    QSize m_imageSize;
    std::unique_ptr<movix::ofx::Effect> m_effect;
    std::unique_ptr<movix::ofx::Overlay> m_overlay;
    QImage m_image;
    bool m_pressed = false;
    // La tecla ya fue al plugin al decidir sobre los atajos (y si la usó): no se le repite.
    bool m_keyAsked = false, m_keyTaken = false;
    QMetaObject::Connection m_history, m_position;
};
