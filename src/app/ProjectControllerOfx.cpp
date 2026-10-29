// Efectos OpenFX del controlador del proyecto: plugins instalados y efectos de cada clip.
#include "ProjectController.h"

#include "core/Edits.h"
#include "ofx/OfxHost.h"

#include <QDesktopServices>
#include <QDir>
#include <QStandardPaths>

#include <cmath>

using namespace movix;
using namespace movix::core;

namespace {

const char* typeName(ofx::ParamType t)
{
    switch (t) {
    case ofx::ParamType::Double: return "double";
    case ofx::ParamType::Int: return "int";
    case ofx::ParamType::Bool: return "bool";
    case ofx::ParamType::Choice: return "choice";
    case ofx::ParamType::Rgb: return "rgb";
    case ofx::ParamType::Rgba: return "rgba";
    case ofx::ParamType::Double2D: return "double2d";
    case ofx::ParamType::Int2D: return "int2d";
    case ofx::ParamType::Double3D: return "double3d";
    case ofx::ParamType::Int3D: return "int3d";
    default: return "";
    }
}

// Intervalo para un deslizador: el del plugin o, si no lo acota, uno razonable alrededor del valor
// por defecto.
std::pair<double, double> sliderRange(const ofx::ParamDef& d)
{
    const double def = d.defaults.empty() ? 0.0 : d.defaults[0];
    const bool integer = d.type == ofx::ParamType::Int || d.type == ofx::ParamType::Int2D || d.type == ofx::ParamType::Int3D;
    const double span = integer ? 100.0 : 1.0;
    double lo = d.min, hi = d.max;
    if (!(lo > -1e299))
        lo = def >= 0 ? 0.0 : def - std::max(span, std::abs(def));
    if (!(hi < 1e299))
        hi = def > 0 ? def + std::max(span, std::abs(def)) * 2 : lo + span;
    if (hi <= lo)
        hi = lo + span;
    return {lo, hi};
}

QVariantList toList(const std::vector<double>& v)
{
    QVariantList out;
    for (double x : v)
        out.append(x);
    return out;
}

std::vector<double> valuesAt(const std::vector<Animated>& components, FramePos rel)
{
    std::vector<double> out;
    for (const Animated& a : components)
        out.push_back(a.at(rel));
    return out;
}

QString interpolationName(Interpolation i)
{
    return i == Interpolation::Hold ? QStringLiteral("hold") : i == Interpolation::Smooth ? QStringLiteral("smooth") : QStringLiteral("linear");
}

// Fotograma del clip en el cabezal (dentro del clip): el de los keyframes de los efectos.
FramePos clipRel(const Clip& c, FramePos playhead)
{
    return std::clamp<FramePos>(playhead - c.start, 0, std::max<FramePos>(0, c.duration - 1));
}

// Efecto al que se refiere `index` (uno del clip, el generador o la transición) y su papel.
const OfxEffect* effectAt(const Clip& c, int index, ofx::Role& role)
{
    role = index == edit::kOfxGenerator ? ofx::Role::Generator : index == edit::kOfxTransition ? ofx::Role::Transition : ofx::Role::Filter;
    if (index == edit::kOfxGenerator)
        return c.generator ? &*c.generator : nullptr;
    if (index == edit::kOfxTransition)
        return c.transitionIn.type == TransitionType::Ofx ? &c.transitionIn.ofx : nullptr;
    return index >= 0 && index < int(c.ofx.size()) ? &c.ofx[size_t(index)] : nullptr;
}

QVariantList pluginList(ofx::Role role)
{
    QVariantList out;
    for (const ofx::PluginInfo& p : ofx::Registry::instance().plugins(role))
        out.append(QVariantMap{{"id", QString::fromStdString(p.id)},
                               {"label", QString::fromStdString(p.label)},
                               {"group", QString::fromStdString(p.group)},
                               {"description", QString::fromStdString(p.description)}});
    return out;
}

} // namespace

QString ProjectController::ofxFolder() const
{
    return qEnvironmentVariableIsSet("MOVIX_OFX_DIR") ? qEnvironmentVariable("MOVIX_OFX_DIR")
                                                     : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ofx");
}

QVariantList ProjectController::ofxPlugins() const
{
    return pluginList(ofx::Role::Filter);
}

QVariantList ProjectController::ofxGenerators() const
{
    return pluginList(ofx::Role::Generator);
}

QVariantList ProjectController::ofxTransitions() const
{
    return pluginList(ofx::Role::Transition);
}

bool ProjectController::addGenerator(const QString& pluginId, qint64 trackId, qint64 frame)
{
    const auto info = ofx::Registry::instance().find(pluginId.toStdString(), ofx::Role::Generator);
    if (!info)
        return false;
    const FramePos duration = project().settings.framesIn(5 * kUsPerSecond);
    const FramePos at = frame >= 0 ? frame : m_player->positionFrame();
    // Pista: la indicada o la superior si está libre en ese tramo; si no, una nueva encima de todo.
    Id target = trackId > 0 ? Id(trackId) : kNoId;
    Id clip = kNoId;
    const bool ok = apply(tr("Añadir «%1»").arg(QString::fromStdString(info->label)), [&](Project& p) {
        if (target == kNoId) {
            const Track& top = p.videoTracks.back();
            const bool free = std::none_of(top.clips.begin(), top.clips.end(),
                                           [&](const Clip& c) { return c.start < at + duration && c.end() > at; });
            target = free && !top.locked ? top.id : edit::addTrack(p, TrackKind::Video);
        }
        clip = edit::placeGenerator(p, target, at, duration, info->id, m_insertMode ? edit::PlaceMode::Insert : edit::PlaceMode::Overwrite);
        return clip != kNoId;
    });
    if (ok) {
        m_selection = {clip};
        m_clips.sync(project(), m_selection);
        emit selectionChanged();
    }
    return ok;
}

void ProjectController::rescanOfx()
{
    ofx::Registry::instance().rescan();
    emit ofxPluginsChanged();
    refresh(); // los efectos que faltaban pueden aplicarse ya
}

void ProjectController::openOfxFolder()
{
    QDir().mkpath(ofxFolder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(ofxFolder()));
}

QVariantList ProjectController::ofxEffects(qint64 clipId, const QString& kind) const
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return {};
    std::vector<int> indices;
    if (kind == QLatin1String("generator"))
        indices = {edit::kOfxGenerator};
    else if (kind == QLatin1String("transition"))
        indices = {edit::kOfxTransition};
    else
        for (size_t i = 0; i < c->ofx.size(); ++i)
            indices.push_back(int(i));
    QVariantList out;
    for (const int i : indices) {
        ofx::Role role = ofx::Role::Filter;
        const OfxEffect* found = effectAt(*c, i, role);
        if (!found)
            continue;
        const OfxEffect& e = *found;
        // Los parámetros de la transición son fijos (el progreso lo pone el anfitrión).
        const bool fixed = role == ofx::Role::Transition;
        const FramePos rel = fixed ? 0 : clipRel(*c, m_player->positionFrame());
        const auto info = ofx::Registry::instance().find(e.pluginId, role);
        QVariantList params;
        if (info) {
            const auto values = ofx::withDefaults(*info, e.params);
            for (const ofx::ParamDef& d : info->params) {
                const QString type = QLatin1String(typeName(d.type));
                if (type.isEmpty() || d.secret)
                    continue;
                const auto [lo, hi] = sliderRange(d);
                QVariantList options;
                for (const std::string& o : d.options)
                    options.append(QString::fromStdString(o));
                // Keyframes: los mismos fotogramas en todos los componentes, así que basta el primero.
                const std::vector<Animated>& components = values.at(d.name);
                const Animated& first = components.front();
                bool prev = false, next = false;
                for (const Keyframe& k : first.keys) {
                    prev = prev || k.frame < rel;
                    next = next || k.frame > rel;
                }
                const Keyframe* here = first.keyAt(rel);
                params.append(QVariantMap{{"name", QString::fromStdString(d.name)},
                                          {"label", QString::fromStdString(d.label)},
                                          {"hint", QString::fromStdString(d.hint)},
                                          {"type", type},
                                          {"value", toList(valuesAt(components, rel))},
                                          {"defaults", toList(d.defaults)},
                                          {"min", lo},
                                          {"max", hi},
                                          {"options", options},
                                          {"animates", d.animates && !fixed},
                                          {"animated", first.isAnimated()},
                                          {"keyHere", here != nullptr},
                                          {"interp", here ? interpolationName(here->interp) : QString()},
                                          {"hasPrev", prev},
                                          {"hasNext", next}});
            }
        }
        out.append(QVariantMap{{"index", i},
                               {"pluginId", QString::fromStdString(e.pluginId)},
                               {"label", info ? QString::fromStdString(info->label) : QString::fromStdString(e.pluginId)},
                               {"enabled", e.enabled},
                               {"installed", info != nullptr},
                               {"hasOverlay", info && info->hasOverlay},
                               {"params", params}});
    }
    return out;
}

void ProjectController::addOfxEffect(qint64 clipId, const QString& pluginId)
{
    const auto info = ofx::Registry::instance().find(pluginId.toStdString());
    const QString label = info ? QString::fromStdString(info->label) : pluginId;
    apply(tr("Añadir efecto «%1»").arg(label), [&](Project& p) { return edit::addOfxEffect(p, Id(clipId), pluginId.toStdString()) >= 0; });
}

void ProjectController::removeOfxEffect(qint64 clipId, int index)
{
    apply(tr("Quitar efecto"), [&](Project& p) { return edit::removeOfxEffect(p, Id(clipId), index); });
}

void ProjectController::setOfxEnabled(qint64 clipId, int index, bool enabled)
{
    apply(enabled ? tr("Activar efecto") : tr("Desactivar efecto"),
          [&](Project& p) { return edit::setOfxEffectEnabled(p, Id(clipId), index, enabled); });
}

void ProjectController::moveOfxEffect(qint64 clipId, int from, int to)
{
    apply(tr("Reordenar efectos"), [&](Project& p) { return edit::moveOfxEffect(p, Id(clipId), from, to); });
}

void ProjectController::setOfxParam(qint64 clipId, int index, const QString& name, const QVariantList& value)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    std::vector<double> v;
    for (const QVariant& x : value)
        v.push_back(x.toDouble());
    const FramePos rel = index == edit::kOfxTransition ? 0 : clipRel(*c, m_player->positionFrame());
    // Arrastrar un deslizador es un solo paso de deshacer.
    apply(tr("Ajustar efecto"), [&](Project& p) { return edit::setOfxParam(p, Id(clipId), index, name.toStdString(), v, rel); },
          QStringLiteral("ofx:%1:%2:%3").arg(clipId).arg(index).arg(name));
}

void ProjectController::toggleOfxKeyframe(qint64 clipId, int index, const QString& name)
{
    const Clip* c = project().findClip(Id(clipId));
    ofx::Role role = ofx::Role::Filter;
    const OfxEffect* e = c ? effectAt(*c, index, role) : nullptr;
    if (!e || role == ofx::Role::Transition)
        return;
    const auto info = ofx::Registry::instance().find(e->pluginId, role);
    if (!info)
        return;
    const auto def = std::find_if(info->params.begin(), info->params.end(), [&](const ofx::ParamDef& d) { return d.name == name.toStdString(); });
    const auto values = ofx::withDefaults(*info, e->params);
    const auto it = values.find(name.toStdString());
    if (def == info->params.end() || it == values.end())
        return;
    const FramePos rel = clipRel(*c, m_player->positionFrame());
    // Casillas y listas no tienen valores intermedios: saltan al llegar al keyframe siguiente.
    const bool step = def->type == ofx::ParamType::Bool || def->type == ofx::ParamType::Choice;
    apply(tr("Keyframe"), [&](Project& p) {
        return edit::toggleOfxKey(p, Id(clipId), index, name.toStdString(), rel, valuesAt(it->second, rel),
                                  step ? Interpolation::Hold : Interpolation::Linear);
    });
}

void ProjectController::setOfxKeyInterpolation(qint64 clipId, int index, const QString& name, const QString& interp)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const Interpolation mode = interp == QLatin1String("hold") ? Interpolation::Hold
                             : interp == QLatin1String("smooth") ? Interpolation::Smooth
                                                                 : Interpolation::Linear;
    const FramePos rel = clipRel(*c, m_player->positionFrame());
    apply(tr("Interpolación del keyframe"),
          [&](Project& p) { return edit::setOfxKeyInterpolation(p, Id(clipId), index, name.toStdString(), rel, mode); });
}

QVariantMap ProjectController::ofxOverlay() const
{
    if (!m_overlayClip)
        return {};
    return {{"clipId", m_overlayClip}, {"index", m_overlayIndex}};
}

void ProjectController::toggleOfxOverlay(qint64 clipId, int index)
{
    const bool same = m_overlayClip == clipId && m_overlayIndex == index;
    m_overlayClip = same ? 0 : clipId;
    m_overlayIndex = index;
    emit ofxOverlayChanged();
}

const OfxEffect* ProjectController::ofxEffectAt(qint64 clipId, int index, ofx::Role* role) const
{
    const Clip* c = project().findClip(Id(clipId));
    ofx::Role r = ofx::Role::Filter;
    const OfxEffect* e = c ? effectAt(*c, index, r) : nullptr;
    if (role)
        *role = r;
    return e;
}

QSize ProjectController::ofxImageSize(qint64 clipId) const
{
    // La del medio en los clips de video; la del lienzo en los generados y los compuestos.
    const Clip* c = project().findClip(Id(clipId));
    const MediaItem* m = c && !c->isGenerated() ? project().findMedia(c->mediaId) : nullptr;
    if (m && m->width > 0 && m->height > 0)
        return {m->width, m->height};
    return {project().settings.width, project().settings.height};
}

FramePos ProjectController::ofxFrame(qint64 clipId) const
{
    const Clip* c = project().findClip(Id(clipId));
    return c ? clipRel(*c, m_player->positionFrame()) : 0;
}
