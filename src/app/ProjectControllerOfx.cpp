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

} // namespace

QString ProjectController::ofxFolder() const
{
    return qEnvironmentVariableIsSet("MOVIX_OFX_DIR") ? qEnvironmentVariable("MOVIX_OFX_DIR")
                                                     : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ofx");
}

QVariantList ProjectController::ofxPlugins() const
{
    QVariantList out;
    for (const ofx::PluginInfo& p : ofx::Registry::instance().plugins())
        out.append(QVariantMap{{"id", QString::fromStdString(p.id)},
                               {"label", QString::fromStdString(p.label)},
                               {"group", QString::fromStdString(p.group)},
                               {"description", QString::fromStdString(p.description)}});
    return out;
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

QVariantList ProjectController::ofxEffects(qint64 clipId) const
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return {};
    QVariantList out;
    for (size_t i = 0; i < c->ofx.size(); ++i) {
        const OfxEffect& e = c->ofx[i];
        const auto info = ofx::Registry::instance().find(e.pluginId);
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
                params.append(QVariantMap{{"name", QString::fromStdString(d.name)},
                                          {"label", QString::fromStdString(d.label)},
                                          {"hint", QString::fromStdString(d.hint)},
                                          {"type", type},
                                          {"value", toList(values.at(d.name))},
                                          {"defaults", toList(d.defaults)},
                                          {"min", lo},
                                          {"max", hi},
                                          {"options", options}});
            }
        }
        out.append(QVariantMap{{"index", int(i)},
                               {"pluginId", QString::fromStdString(e.pluginId)},
                               {"label", info ? QString::fromStdString(info->label) : QString::fromStdString(e.pluginId)},
                               {"enabled", e.enabled},
                               {"installed", info != nullptr},
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
    std::vector<double> v;
    for (const QVariant& x : value)
        v.push_back(x.toDouble());
    // Arrastrar un deslizador es un solo paso de deshacer.
    apply(tr("Ajustar efecto"), [&](Project& p) { return edit::setOfxParam(p, Id(clipId), index, name.toStdString(), v); },
          QStringLiteral("ofx:%1:%2:%3").arg(clipId).arg(index).arg(name));
}
