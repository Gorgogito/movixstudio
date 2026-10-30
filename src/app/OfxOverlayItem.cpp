#include "OfxOverlayItem.h"

#include "ProjectController.h"

#include <QKeyEvent>
#include <QPainter>
#include <QQuickWindow>

using namespace movix;
using namespace movix::core;

OfxOverlayItem::OfxOverlayItem(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

OfxOverlayItem::~OfxOverlayItem()
{
    m_overlay.reset(); // antes que la instancia del efecto
}

void OfxOverlayItem::setProject(ProjectController* project)
{
    if (m_project == project)
        return;
    disconnect(m_history);
    disconnect(m_position);
    m_project = project;
    if (m_project) {
        m_history = connect(m_project, &ProjectController::historyChanged, this, &OfxOverlayItem::refresh);
        m_position = connect(m_project->player(), &playback::TimelinePlayer::positionChanged, this, &OfxOverlayItem::refresh);
    }
    emit targetChanged();
    refresh();
}

void OfxOverlayItem::setClipId(qint64 id)
{
    if (m_clipId == id)
        return;
    m_clipId = id;
    emit targetChanged();
    refresh();
}

void OfxOverlayItem::setIndex(int index)
{
    if (m_index == index)
        return;
    m_index = index;
    emit targetChanged();
    refresh();
}

QSize OfxOverlayItem::viewSize() const
{
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    return {int(std::lround(width() * dpr)), int(std::lround(height() * dpr))};
}

void OfxOverlayItem::rebuild()
{
    const bool had = m_overlay != nullptr;
    ofx::Role role = ofx::Role::Filter;
    const OfxEffect* e = m_project && m_clipId ? m_project->ofxEffectAt(m_clipId, m_index, &role) : nullptr;
    const QSize size = e ? m_project->ofxImageSize(m_clipId) : QSize();
    if (!e) {
        m_overlay.reset();
        m_effect.reset();
        m_pluginId.clear();
    } else if (!m_effect || m_pluginId != e->pluginId || m_role != int(role) || m_imageSize != size) {
        m_overlay.reset();
        m_effect = ofx::Effect::create(e->pluginId, size.width(), size.height(), m_project->project().settings.frameRate.value(), nullptr, role);
        m_overlay = m_effect && m_effect->plugin().hasOverlay ? ofx::Overlay::create(*m_effect) : nullptr;
        m_pluginId = e->pluginId;
        m_role = int(role);
        m_imageSize = size;
    }
    if (had != (m_overlay != nullptr))
        emit availableChanged();
}

void OfxOverlayItem::refresh()
{
    rebuild();
    const QSize view = viewSize();
    m_image = {};
    if (m_overlay && !view.isEmpty()) {
        ofx::Role role;
        const OfxEffect* e = m_project->ofxEffectAt(m_clipId, m_index, &role);
        std::vector<uint8_t> rgba;
        if (e && m_overlay->draw(ofx::withDefaults(m_effect->plugin(), e->params), double(m_project->ofxFrame(m_clipId, m_index)), view.width(),
                                 view.height(), rgba))
            m_image = QImage(rgba.data(), view.width(), view.height(), view.width() * 4, QImage::Format_RGBA8888_Premultiplied).copy();
        m_image.setDevicePixelRatio(window() ? window()->effectiveDevicePixelRatio() : 1.0);
        m_overlay->takeRedraw();
    }
    update();
}

void OfxOverlayItem::paint(QPainter* painter)
{
    if (!m_image.isNull())
        painter->drawImage(boundingRect(), m_image);
}

bool OfxOverlayItem::pen(ofx::Overlay::Pen kind, QPointF position)
{
    if (!m_overlay)
        return false;
    ofx::Role role;
    const OfxEffect* e = m_project->ofxEffectAt(m_clipId, m_index, &role);
    if (!e)
        return false;
    const QSize view = viewSize();
    const qreal sx = width() > 0 ? view.width() / width() : 1.0, sy = height() > 0 ? view.height() / height() : 1.0;
    const FramePos rel = m_project->ofxFrame(m_clipId, m_index);
    const ofx::ParamValues before = ofx::withDefaults(m_effect->plugin(), e->params);
    ofx::ParamValues after = before;
    const bool used = m_overlay->pen(kind, position.x() * sx, position.y() * sy, double(rel), view.width(), view.height(), after);
    commit(before, after, rel);
    if (kind == ofx::Overlay::Pen::Up)
        m_project->endInteraction();
    if (m_overlay->takeRedraw() || used)
        refresh();
    return used;
}

bool OfxOverlayItem::key(ofx::Overlay::Key kind, int qtKey, const QString& text)
{
    const OfxEffect* e = m_overlay ? m_project->ofxEffectAt(m_clipId, m_index, nullptr) : nullptr;
    if (!e)
        return false;
    const QSize view = viewSize();
    const FramePos rel = m_project->ofxFrame(m_clipId, m_index);
    const ofx::ParamValues before = ofx::withDefaults(m_effect->plugin(), e->params);
    ofx::ParamValues after = before;
    const bool used = m_overlay->key(kind, qtKey, text.toStdString(), double(rel), view.width(), view.height(), after);
    commit(before, after, rel);
    m_project->endInteraction(); // cada pulsación, un paso de deshacer
    if (m_overlay->takeRedraw() || used)
        refresh();
    return used;
}

void OfxOverlayItem::commit(const ofx::ParamValues& before, const ofx::ParamValues& after, FramePos rel)
{
    // Lo que el plugin cambió en el instante del cabezal pasa al proyecto (setOfxParam crea el
    // keyframe si el parámetro está animado).
    for (const auto& [name, components] : after) {
        auto it = before.find(name);
        QVariantList value;
        bool changed = it == before.end() || it->second.size() != components.size();
        for (size_t i = 0; i < components.size(); ++i) {
            const double v = components[i].at(rel);
            value.append(v);
            changed = changed || std::abs(v - it->second[i].at(rel)) > 1e-12;
        }
        if (changed)
            m_project->setOfxParam(m_clipId, m_index, QString::fromStdString(name), value);
    }
}

void OfxOverlayItem::mousePressEvent(QMouseEvent* event)
{
    if (m_overlay)
        forceActiveFocus(Qt::MouseFocusReason);
    m_pressed = pen(ofx::Overlay::Pen::Down, event->position());
    event->setAccepted(m_pressed); // si el plugin no lo usa, para lo de debajo (mover el clip...)
}

void OfxOverlayItem::mouseMoveEvent(QMouseEvent* event)
{
    if (m_pressed)
        pen(ofx::Overlay::Pen::Motion, event->position());
}

void OfxOverlayItem::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_pressed)
        pen(ofx::Overlay::Pen::Up, event->position());
    m_pressed = false;
}

void OfxOverlayItem::hoverMoveEvent(QHoverEvent* event)
{
    // Sin botón: el plugin puede resaltar lo que hay bajo el ratón.
    if (!m_pressed && m_overlay)
        pen(ofx::Overlay::Pen::Motion, event->position());
    event->ignore();
}

bool OfxOverlayItem::event(QEvent* event)
{
    // Antes de un atajo de la aplicación, la tecla va al plugin: si la usa, el atajo no salta.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(event);
        m_keyTaken = key(k->isAutoRepeat() ? ofx::Overlay::Key::Repeat : ofx::Overlay::Key::Down, k->key(), k->text());
        m_keyAsked = true;
        event->setAccepted(m_keyTaken);
        return true;
    }
    return QQuickPaintedItem::event(event);
}

void OfxOverlayItem::keyPressEvent(QKeyEvent* event)
{
    const bool used = std::exchange(m_keyAsked, false)
                        ? m_keyTaken
                        : key(event->isAutoRepeat() ? ofx::Overlay::Key::Repeat : ofx::Overlay::Key::Down, event->key(), event->text());
    event->setAccepted(used);
}

void OfxOverlayItem::keyReleaseEvent(QKeyEvent* event)
{
    event->setAccepted(!event->isAutoRepeat() && key(ofx::Overlay::Key::Up, event->key(), event->text()));
}

void OfxOverlayItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        refresh();
}
