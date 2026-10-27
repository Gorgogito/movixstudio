// Parte de ProjectController dedicada a la corrección de color, el croma, la máscara y la fusión.
#include "ProjectController.h"

#include "EffectNames.h"
#include "color/ColorMath.h"
#include "color/Ocio.h"
#include "media/ColorMatrix.h"
#include "media/FrameConvert.h"
#include "media/GpuFrames.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <cmath>

using namespace movix;
using namespace movix::core;
using movix::app::colorName;
using movix::app::colorValue;

namespace {

double* gradeField(ColorGrade& g, const QString& key)
{
    static const QHash<QString, double ColorGrade::*> kBasic{
        {"exposure", &ColorGrade::exposure},     {"contrast", &ColorGrade::contrast},
        {"saturation", &ColorGrade::saturation}, {"vibrance", &ColorGrade::vibrance},
        {"temperature", &ColorGrade::temperature}, {"tint", &ColorGrade::tint},
        {"highlights", &ColorGrade::highlights}, {"shadows", &ColorGrade::shadows},
        {"whites", &ColorGrade::whites},         {"blacks", &ColorGrade::blacks},
        {"lutIntensity", &ColorGrade::lutIntensity},
    };
    if (auto it = kBasic.find(key); it != kBasic.end())
        return &(g.*it.value());
    const QStringList parts = key.split(QLatin1Char('.'));
    if (parts.size() == 2) {
        ColorWheel* w = parts[0] == QLatin1String("lift")    ? &g.lift
                        : parts[0] == QLatin1String("gamma") ? &g.gamma
                        : parts[0] == QLatin1String("gain")  ? &g.gain
                                                             : nullptr;
        if (!w)
            return nullptr;
        if (parts[1] == QLatin1String("r"))
            return &w->r;
        if (parts[1] == QLatin1String("g"))
            return &w->g;
        if (parts[1] == QLatin1String("b"))
            return &w->b;
        if (parts[1] == QLatin1String("master"))
            return &w->master;
        return nullptr;
    }
    if (parts.size() == 3 && parts[0] == QLatin1String("hsl")) {
        bool ok = false;
        const int band = parts[1].toInt(&ok);
        if (!ok || band < 0 || band >= int(g.hsl.size()))
            return nullptr;
        HslBand& b = g.hsl[size_t(band)];
        if (parts[2] == QLatin1String("hue"))
            return &b.hue;
        if (parts[2] == QLatin1String("saturation"))
            return &b.saturation;
        if (parts[2] == QLatin1String("luminance"))
            return &b.luminance;
    }
    return nullptr;
}

ColorWheel* wheelField(ColorGrade& g, const QString& name)
{
    if (name == QLatin1String("lift"))
        return &g.lift;
    if (name == QLatin1String("gamma"))
        return &g.gamma;
    if (name == QLatin1String("gain"))
        return &g.gain;
    return nullptr;
}

std::vector<CurvePoint>* curveField(ColorGrade& g, const QString& channel)
{
    if (channel == QLatin1String("master"))
        return &g.curveMaster;
    if (channel == QLatin1String("red"))
        return &g.curveRed;
    if (channel == QLatin1String("green"))
        return &g.curveGreen;
    if (channel == QLatin1String("blue"))
        return &g.curveBlue;
    return nullptr;
}

QVariantList wheelList(const ColorWheel& w)
{
    return {w.r, w.g, w.b, w.master};
}

QVariantList curveList(const std::vector<CurvePoint>& curve)
{
    QVariantList out;
    for (const CurvePoint& pt : curve)
        out.append(QVariant(QVariantList{pt.x, pt.y}));
    return out;
}

// Color medio de un entorno de 5×5 píxeles del fotograma en (u, v).
std::optional<std::uint32_t> sampleFrame(const media::VideoFrame& f, double u, double v)
{
    if (f.width <= 0 || f.height <= 0 || u < 0 || u > 1 || v < 0 || v > 1)
        return std::nullopt;
    const int cx = std::clamp(int(u * f.width), 0, f.width - 1);
    const int cy = std::clamp(int(v * f.height), 0, f.height - 1);
    const auto m = media::yuvToRgbMatrix(f.colorSpace, f.fullRange);
    double sum[3] = {0, 0, 0};
    int n = 0;
    for (int y = std::max(0, cy - 2); y <= std::min(f.height - 1, cy + 2); ++y) {
        for (int x = std::max(0, cx - 2); x <= std::min(f.width - 1, cx + 2); ++x) {
            double rgb[3];
            if (f.format == media::PixelFormat::RGBA) {
                const size_t i = (size_t(y) * size_t(f.width) + size_t(x)) * 4;
                for (int c = 0; c < 3; ++c)
                    rgb[c] = f.rgba[i + size_t(c)] / 255.0;
            } else if (f.format == media::PixelFormat::RGBA16F) {
                const size_t i = (size_t(y) * size_t(f.width) + size_t(x)) * 4;
                for (int c = 0; c < 3; ++c)
                    rgb[c] = std::clamp(double(media::halfAt(f.rgba, i + size_t(c))), 0.0, 1.0);
            } else if (f.format == media::PixelFormat::P016) {
                auto at = [](const std::vector<uint8_t>& v, size_t i) { return (v[i * 2] | (v[i * 2 + 1] << 8)) / 65535.0; };
                const auto m16 = media::yuvToRgbMatrix(f.colorSpace, f.fullRange, true);
                const double yy = at(f.y, size_t(y) * size_t(f.width) + size_t(x));
                const size_t ci = (size_t(y / 2) * size_t(f.chromaWidth()) + size_t(x / 2)) * 2;
                const double cb = at(f.uv, ci), cr = at(f.uv, ci + 1);
                for (size_t c = 0; c < 3; ++c)
                    rgb[c] = std::clamp(m16[c * 4] * yy + m16[c * 4 + 1] * cb + m16[c * 4 + 2] * cr + m16[c * 4 + 3], 0.0, 1.0);
            } else {
                const double yy = f.y[size_t(y) * size_t(f.width) + size_t(x)] / 255.0;
                const size_t ci = (size_t(y / 2) * size_t(f.chromaWidth()) + size_t(x / 2)) * 2;
                const double cb = f.uv[ci] / 255.0, cr = f.uv[ci + 1] / 255.0;
                for (size_t c = 0; c < 3; ++c)
                    rgb[c] = std::clamp(m[c * 4] * yy + m[c * 4 + 1] * cb + m[c * 4 + 2] * cr + m[c * 4 + 3], 0.0, 1.0);
            }
            for (int c = 0; c < 3; ++c)
                sum[c] += rgb[c];
            ++n;
        }
    }
    auto ch = [&](int c) { return std::uint32_t(std::lround(sum[c] / n * 255)); };
    return 0xFF000000u | (ch(0) << 16) | (ch(1) << 8) | ch(2);
}

} // namespace

QVariantMap ProjectController::colorGrade(qint64 clipId) const
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return {};
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    const ColorGrade g = c->color.valueAt(rel);
    bool prev = false, next = false;
    for (const GradeKey& k : c->color.keys) {
        prev = prev || k.frame < rel;
        next = next || k.frame > rel;
    }
    QVariantList hsl;
    for (const HslBand& b : g.hsl)
        hsl.append(QVariant(QVariantList{b.hue, b.saturation, b.luminance}));
    return {
        {"exposure", g.exposure},
        {"contrast", g.contrast},
        {"saturation", g.saturation},
        {"vibrance", g.vibrance},
        {"temperature", g.temperature},
        {"tint", g.tint},
        {"highlights", g.highlights},
        {"shadows", g.shadows},
        {"whites", g.whites},
        {"blacks", g.blacks},
        {"lift", wheelList(g.lift)},
        {"gamma", wheelList(g.gamma)},
        {"gain", wheelList(g.gain)},
        {"curves", QVariantMap{{"master", curveList(g.curveMaster)},
                               {"red", curveList(g.curveRed)},
                               {"green", curveList(g.curveGreen)},
                               {"blue", curveList(g.curveBlue)}}},
        {"hsl", hsl},
        {"lutPath", QString::fromStdString(g.lutPath)},
        {"lutName", g.lutPath.empty() ? QString() : QFileInfo(QString::fromStdString(g.lutPath)).completeBaseName()},
        {"lutIntensity", g.lutIntensity},
        {"identity", c->color.isIdentity()},
        {"inputSpace", QString::fromStdString(g.inputSpace)},
        {"animated", c->color.isAnimated()},
        {"keyHere", c->color.keyAt(rel) != nullptr},
        {"interp", c->color.keyAt(rel) ? QString(c->color.keyAt(rel)->interp == Interpolation::Hold     ? "hold"
                                                 : c->color.keyAt(rel)->interp == Interpolation::Smooth ? "smooth"
                                                                                                      : "linear")
                                       : QString()},
        {"hasPrev", prev},
        {"hasNext", next},
    };
}

void ProjectController::setColorValue(qint64 clipId, const QString& key, double value)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    ColorGrade g = c->color.valueAt(rel);
    double* field = gradeField(g, key);
    if (!field)
        return;
    *field = value;
    // Arrastrar un control agrupa los cambios en un único paso de deshacer.
    apply(tr("Corrección de color"), [&](Project& p) { return edit::setColorGrade(p, Id(clipId), g, rel); },
          QStringLiteral("color:%1:%2").arg(clipId).arg(key));
}

void ProjectController::setColorWheel(qint64 clipId, const QString& wheel, double r, double g, double b)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    ColorGrade grade = c->color.valueAt(rel);
    ColorWheel* w = wheelField(grade, wheel);
    if (!w)
        return;
    w->r = r;
    w->g = g;
    w->b = b;
    apply(tr("Corrección de color"), [&](Project& p) { return edit::setColorGrade(p, Id(clipId), grade, rel); },
          QStringLiteral("wheel:%1:%2").arg(clipId).arg(wheel));
}

void ProjectController::setColorCurve(qint64 clipId, const QString& channel, const QVariantList& points)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    ColorGrade g = c->color.valueAt(rel);
    auto* curve = curveField(g, channel);
    if (!curve)
        return;
    curve->clear();
    for (const QVariant& v : points) {
        const QVariantList pt = v.toList();
        if (pt.size() == 2)
            curve->push_back({pt[0].toDouble(), pt[1].toDouble()});
    }
    apply(tr("Curva de color"), [&](Project& p) { return edit::setColorGrade(p, Id(clipId), g, rel); },
          QStringLiteral("curve:%1:%2").arg(clipId).arg(channel));
}

bool ProjectController::setColorLut(qint64 clipId, const QUrl& file)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return false;
    const QString path = QDir::toNativeSeparators(file.toLocalFile());
    std::string error;
    if (!color::loadCubeLut(path.toStdString(), &error)) {
        emit errorOccurred(tr("No se pudo usar la LUT %1: %2").arg(QFileInfo(path).fileName(), QString::fromStdString(error)));
        return false;
    }
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    ColorGrade g = c->color.valueAt(rel);
    g.lutPath = path.toStdString();
    if (g.lutIntensity <= 0.0)
        g.lutIntensity = 1.0;
    return apply(tr("Aplicar LUT"), [&](Project& p) { return edit::setColorGrade(p, Id(clipId), g, rel); });
}

QVariantList ProjectController::ocioInputSpaces() const
{
    QVariantList out;
    for (const color::InputSpace& s : color::ocioInputSpaces())
        out.append(QVariantMap{{"name", QString::fromStdString(s.name)}, {"family", QString::fromStdString(s.family)}});
    return out;
}

void ProjectController::setColorInputSpace(qint64 clipId, const QString& space)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    ColorGrade g = c->color.valueAt(rel);
    g.inputSpace = space.toStdString();
    apply(tr("Espacio de entrada"), [&](Project& p) { return edit::setColorGrade(p, Id(clipId), g, rel); });
}

void ProjectController::resetColor(qint64 clipId, const QString& section)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    ColorGrade g = c->color.valueAt(rel);
    const ColorGrade def;
    if (section == QLatin1String("basic")) {
        for (double ColorGrade::*f : {&ColorGrade::exposure, &ColorGrade::contrast, &ColorGrade::saturation, &ColorGrade::vibrance,
                                      &ColorGrade::temperature, &ColorGrade::tint, &ColorGrade::highlights, &ColorGrade::shadows,
                                      &ColorGrade::whites, &ColorGrade::blacks})
            g.*f = 0.0;
    } else if (section == QLatin1String("wheels")) {
        g.lift = g.gamma = g.gain = ColorWheel{};
    } else if (section == QLatin1String("curves")) {
        g.curveMaster.clear();
        g.curveRed.clear();
        g.curveGreen.clear();
        g.curveBlue.clear();
    } else if (section == QLatin1String("hsl")) {
        g.hsl = def.hsl;
    } else if (section == QLatin1String("lut")) {
        g.lutPath.clear();
        g.lutIntensity = 1.0;
    } else { // todo: también quita los keyframes de color
        apply(tr("Restablecer color"), [&](Project& p) { return edit::setAnimatedGrade(p, Id(clipId), AnimatedGrade{}); });
        return;
    }
    apply(tr("Restablecer color"), [&](Project& p) { return edit::setColorGrade(p, Id(clipId), g, rel); });
}

void ProjectController::toggleColorKeyframe(qint64 clipId)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    apply(tr("Keyframe de color"), [&](Project& p) { return edit::toggleColorKeyframe(p, Id(clipId), rel); });
}

void ProjectController::copyColor(qint64 clipId)
{
    if (const Clip* c = project().findClip(Id(clipId))) {
        m_colorClipboard = c->color;
        emit clipboardChanged();
        emit notice(tr("Corrección de color copiada"));
    }
}

void ProjectController::pasteColor()
{
    if (!m_colorClipboard || m_selection.isEmpty())
        return;
    const std::vector<Id> ids(m_selection.begin(), m_selection.end());
    apply(tr("Pegar corrección de color"), [&](Project& p) {
        bool any = false;
        for (Id id : ids)
            any = edit::setAnimatedGrade(p, id, *m_colorClipboard) || any;
        return any;
    });
}

void ProjectController::setChromaKey(qint64 clipId, const QVariantMap& values)
{
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    ChromaKey k = c->chromaKey;
    if (values.contains("enabled"))
        k.enabled = values["enabled"].toBool();
    if (values.contains("color"))
        k.color = colorValue(values["color"], k.color);
    if (values.contains("tolerance"))
        k.tolerance = values["tolerance"].toDouble();
    if (values.contains("softness"))
        k.softness = values["softness"].toDouble();
    if (values.contains("spill"))
        k.spill = values["spill"].toDouble();
    apply(tr("Clave de croma"), [&](Project& p) { return edit::setChromaKey(p, Id(clipId), k); },
          QStringLiteral("key:%1:%2").arg(clipId).arg(values.keys().join(QLatin1Char(','))));
}

bool ProjectController::pickKeyColor(qint64 clipId, double u, double v)
{
    const auto bundle = m_player->displayedFrame();
    if (!bundle)
        return false;
    for (const auto& layer : bundle->layers) {
        if (layer.clipId != Id(clipId) || !layer.frame)
            continue;
        // Con copia cero, el fotograma está en la GPU: se baja a memoria solo para esto.
        const auto cpu = layer.frame->onGpu() ? media::downloadGpuFrame(*layer.frame) : nullptr;
        if (layer.frame->onGpu() && !cpu)
            return false;
        const auto color = sampleFrame(cpu ? *cpu : *layer.frame, u, v);
        if (!color)
            return false;
        setChromaKey(clipId, {{"enabled", true}, {"color", colorName(*color)}});
        endInteraction();
        return true;
    }
    return false;
}

int ProjectController::addMask(qint64 clipId, const QString& shape)
{
    const auto s = app::enumFromName<MaskShape>(shape, app::kMaskShapeNames);
    int index = -1;
    if (s)
        apply(tr("Añadir máscara"), [&](Project& p) { return (index = edit::addMask(p, Id(clipId), *s)) >= 0; });
    return index;
}

void ProjectController::removeMask(qint64 clipId, int index)
{
    apply(tr("Quitar máscara"), [&](Project& p) { return edit::removeMask(p, Id(clipId), index); });
}

void ProjectController::setMaskShape(qint64 clipId, int index, const QString& shape)
{
    if (const auto s = app::enumFromName<MaskShape>(shape, app::kMaskShapeNames))
        apply(tr("Máscara"), [&](Project& p) { return edit::setMaskShape(p, Id(clipId), index, *s); });
}

void ProjectController::setMaskOptions(qint64 clipId, int index, const QString& mode, bool invert)
{
    if (const auto m = app::enumFromName<MaskMode>(mode, app::kMaskModeNames))
        apply(tr("Máscara"), [&](Project& p) { return edit::setMaskOptions(p, Id(clipId), index, *m, invert); });
}

void ProjectController::setMaskPoints(qint64 clipId, int index, const QVariantList& points)
{
    std::vector<PathPoint> pts;
    for (const QVariant& v : points) {
        const QVariantList a = v.toList();
        if (a.size() >= 2)
            pts.push_back({a[0].toDouble(), a[1].toDouble(), a.value(2).toDouble(), a.value(3).toDouble(), a.value(4).toDouble(),
                           a.value(5).toDouble()});
    }
    const Clip* c = project().findClip(Id(clipId));
    if (!c)
        return;
    const FramePos rel = std::clamp<FramePos>(playheadRel(*c), 0, c->duration - 1);
    apply(tr("Forma de la máscara"), [&](Project& p) { return edit::setMaskPoints(p, Id(clipId), index, pts, rel); },
          QStringLiteral("maskpts:%1:%2:%3").arg(clipId).arg(index).arg(rel));
}

QVariantList ProjectController::smoothMaskPoints(const QVariantList& points, bool smooth) const
{
    const qsizetype n = points.size();
    QVariantList out;
    for (qsizetype i = 0; i < n; ++i) {
        const QVariantList cur = points[i].toList();
        const double x = cur.value(0).toDouble(), y = cur.value(1).toDouble();
        double tx = 0, ty = 0;
        if (smooth && n >= 3) { // tangente de Catmull-Rom: un sexto de la recta entre los vecinos
            const QVariantList prev = points[(i + n - 1) % n].toList();
            const QVariantList next = points[(i + 1) % n].toList();
            tx = (next.value(0).toDouble() - prev.value(0).toDouble()) / 6.0;
            ty = (next.value(1).toDouble() - prev.value(1).toDouble()) / 6.0;
        }
        out.append(QVariant(QVariantList{x, y, -tx, -ty, tx, ty}));
    }
    return out;
}

void ProjectController::setBlendMode(qint64 clipId, const QString& mode)
{
    if (const auto m = app::enumFromName<BlendMode>(mode, app::kBlendModeNames))
        apply(tr("Modo de fusión"), [&](Project& p) { return edit::setBlendMode(p, Id(clipId), *m); });
}
