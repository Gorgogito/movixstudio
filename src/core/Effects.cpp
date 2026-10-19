#include "core/Effects.h"

#include <algorithm>
#include <cmath>

namespace movix::core {

bool ColorGrade::isIdentity() const
{
    const bool basic = exposure == 0.0 && contrast == 0.0 && saturation == 0.0 && vibrance == 0.0 && temperature == 0.0
                       && tint == 0.0 && highlights == 0.0 && shadows == 0.0 && whites == 0.0 && blacks == 0.0;
    const bool wheels = lift.isNeutral() && gamma.isNeutral() && gain.isNeutral();
    const bool curves = curveMaster.empty() && curveRed.empty() && curveGreen.empty() && curveBlue.empty();
    const bool hslNeutral = std::all_of(hsl.begin(), hsl.end(), [](const HslBand& b) { return b.isNeutral(); });
    const bool lut = lutPath.empty() || lutIntensity <= 0.0;
    return basic && wheels && curves && hslNeutral && lut && inputSpace.empty();
}

} // namespace movix::core

namespace movix::core {

namespace {

double easeFor(Interpolation interp, double t)
{
    switch (interp) {
    case Interpolation::Hold: return 0.0;
    case Interpolation::Smooth: return t * t * (3.0 - 2.0 * t);
    case Interpolation::Linear: break;
    }
    return t;
}

double mixd(double a, double b, double t)
{
    return a + (b - a) * t;
}

ColorWheel lerpWheel(const ColorWheel& a, const ColorWheel& b, double t)
{
    return {mixd(a.r, b.r, t), mixd(a.g, b.g, t), mixd(a.b, b.b, t), mixd(a.master, b.master, t)};
}

std::vector<CurvePoint> lerpCurve(const std::vector<CurvePoint>& a, const std::vector<CurvePoint>& b, double t)
{
    if (a.size() != b.size())
        return t < 0.5 ? a : b;
    std::vector<CurvePoint> out(a.size());
    for (size_t i = 0; i < a.size(); ++i)
        out[i] = {mixd(a[i].x, b[i].x, t), mixd(a[i].y, b[i].y, t)};
    return out;
}

} // namespace

ColorGrade lerpGrade(const ColorGrade& a, const ColorGrade& b, double t)
{
    if (t <= 0.0)
        return a;
    if (t >= 1.0)
        return b;
    ColorGrade g = a;
    for (double ColorGrade::*f : {&ColorGrade::exposure, &ColorGrade::contrast, &ColorGrade::saturation, &ColorGrade::vibrance,
                                  &ColorGrade::temperature, &ColorGrade::tint, &ColorGrade::highlights, &ColorGrade::shadows,
                                  &ColorGrade::whites, &ColorGrade::blacks, &ColorGrade::lutIntensity})
        g.*f = mixd(a.*f, b.*f, t);
    g.lift = lerpWheel(a.lift, b.lift, t);
    g.gamma = lerpWheel(a.gamma, b.gamma, t);
    g.gain = lerpWheel(a.gain, b.gain, t);
    g.curveMaster = lerpCurve(a.curveMaster, b.curveMaster, t);
    g.curveRed = lerpCurve(a.curveRed, b.curveRed, t);
    g.curveGreen = lerpCurve(a.curveGreen, b.curveGreen, t);
    g.curveBlue = lerpCurve(a.curveBlue, b.curveBlue, t);
    for (size_t i = 0; i < g.hsl.size(); ++i)
        g.hsl[i] = {mixd(a.hsl[i].hue, b.hsl[i].hue, t), mixd(a.hsl[i].saturation, b.hsl[i].saturation, t),
                    mixd(a.hsl[i].luminance, b.hsl[i].luminance, t)};
    if (a.lutPath != b.lutPath)
        g.lutPath = t < 0.5 ? a.lutPath : b.lutPath;
    if (a.inputSpace != b.inputSpace)
        g.inputSpace = t < 0.5 ? a.inputSpace : b.inputSpace;
    return g;
}

bool AnimatedGrade::isIdentity() const
{
    if (keys.empty())
        return value.isIdentity();
    return std::all_of(keys.begin(), keys.end(), [](const GradeKey& k) { return k.grade.isIdentity(); });
}

AnimatedGrade::Sample AnimatedGrade::at(std::int64_t rel) const
{
    if (keys.empty())
        return {&value, nullptr, 0.0};
    if (rel <= keys.front().frame)
        return {&keys.front().grade, nullptr, 0.0};
    if (rel >= keys.back().frame)
        return {&keys.back().grade, nullptr, 0.0};
    auto next = std::upper_bound(keys.begin(), keys.end(), rel, [](std::int64_t f, const GradeKey& k) { return f < k.frame; });
    auto prev = next - 1;
    const double t = easeFor(prev->interp, double(rel - prev->frame) / double(next->frame - prev->frame));
    if (t <= 0.0)
        return {&prev->grade, nullptr, 0.0};
    return {&prev->grade, &next->grade, t};
}

ColorGrade AnimatedGrade::valueAt(std::int64_t rel) const
{
    const Sample s = at(rel);
    return s.to ? lerpGrade(*s.from, *s.to, s.t) : *s.from;
}

void AnimatedGrade::setKey(std::int64_t rel, const ColorGrade& g, Interpolation interp)
{
    auto it = std::lower_bound(keys.begin(), keys.end(), rel, [](const GradeKey& k, std::int64_t f) { return k.frame < f; });
    if (it != keys.end() && it->frame == rel) {
        it->grade = g;
        it->interp = interp;
    } else {
        keys.insert(it, GradeKey{rel, g, interp});
    }
}

bool AnimatedGrade::removeKey(std::int64_t rel)
{
    auto it = std::find_if(keys.begin(), keys.end(), [&](const GradeKey& k) { return k.frame == rel; });
    if (it == keys.end())
        return false;
    if (keys.size() == 1)
        value = it->grade;
    keys.erase(it);
    return true;
}

const GradeKey* AnimatedGrade::keyAt(std::int64_t rel) const
{
    for (const GradeKey& k : keys)
        if (k.frame == rel)
            return &k;
    return nullptr;
}

void AnimatedGrade::set(std::int64_t rel, const ColorGrade& g)
{
    if (keys.empty()) {
        value = g;
        return;
    }
    const GradeKey* existing = keyAt(rel);
    setKey(rel, g, existing ? existing->interp : Interpolation::Linear);
}

std::vector<PathPoint> defaultMaskPath()
{
    std::vector<PathPoint> pts;
    for (int i = 0; i < 5; ++i) {
        const double a = -1.5707963267948966 + i * 2 * 3.141592653589793 / 5;
        pts.push_back({0.5 * std::cos(a), 0.5 * std::sin(a)});
    }
    return pts;
}

} // namespace movix::core
