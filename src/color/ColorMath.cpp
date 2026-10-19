#include "color/ColorMath.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>

namespace movix::color {

namespace {

constexpr float kLr = 0.2126f, kLg = 0.7152f, kLb = 0.0722f; // luma BT.709
constexpr float kGamma = 2.2f;
constexpr int kCurveTableSize = 1024;

float luma(Rgb c)
{
    return kLr * c.r + kLg * c.g + kLb * c.b;
}

float smoothstep(float e0, float e1, float x)
{
    if (e1 <= e0)
        return x < e0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float mixf(float a, float b, float t)
{
    return a + (b - a) * t;
}

Rgb mix(Rgb a, Rgb b, float t)
{
    return {mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)};
}

Rgb clamp01(Rgb c)
{
    return {std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f)};
}

template <typename F>
Rgb perChannel(Rgb c, F f)
{
    return {f(c.r, 0), f(c.g, 1), f(c.b, 2)};
}

// Tono HSV en grados (0..360) y croma (máx − mín).
void hueChroma(Rgb c, float& hue, float& chromaOut)
{
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    chromaOut = mx - mn;
    if (chromaOut <= 1e-6f) {
        hue = 0.0f;
        return;
    }
    float h;
    if (mx == c.r)
        h = std::fmod((c.g - c.b) / chromaOut + 6.0f, 6.0f);
    else if (mx == c.g)
        h = (c.b - c.r) / chromaOut + 2.0f;
    else
        h = (c.r - c.g) / chromaOut + 4.0f;
    hue = h * 60.0f;
}

// Rotación del tono alrededor del eje de grises (fórmula de Rodrigues).
Rgb rotateHue(Rgb c, float degrees)
{
    const float a = degrees * std::numbers::pi_v<float> / 180.0f;
    const float cs = std::cos(a), sn = std::sin(a);
    const float k = (1.0f - cs) / 3.0f, q = sn / std::numbers::sqrt3_v<float>;
    return {c.r * (cs + k) + c.g * (k - q) + c.b * (k + q),
            c.r * (k + q) + c.g * (cs + k) + c.b * (k - q),
            c.r * (k - q) + c.g * (k + q) + c.b * (cs + k)};
}

bool parseFloats(std::string_view line, float* out, int count)
{
    const char* p = line.data();
    const char* end = line.data() + line.size();
    for (int i = 0; i < count; ++i) {
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
        auto r = std::from_chars(p, end, out[i]);
        if (r.ec != std::errc())
            return false;
        p = r.ptr;
    }
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
        ++p;
    return p == end;
}

} // namespace

// --- CubeLut ---

Rgb CubeLut::apply(Rgb in) const
{
    if (size < 2)
        return in;
    const float v[3] = {in.r, in.g, in.b};
    float x[3];
    for (int i = 0; i < 3; ++i) {
        const float range = domainMax[size_t(i)] - domainMin[size_t(i)];
        x[i] = std::clamp(range > 0 ? (v[i] - domainMin[size_t(i)]) / range : 0.0f, 0.0f, 1.0f) * float(size - 1);
    }
    if (!is3D) {
        float out[3];
        for (int ch = 0; ch < 3; ++ch) {
            const int i0 = std::min(int(x[ch]), size - 2);
            const float t = x[ch] - float(i0);
            out[ch] = mixf(data[size_t(i0) * 3 + size_t(ch)], data[size_t(i0 + 1) * 3 + size_t(ch)], t);
        }
        return {out[0], out[1], out[2]};
    }
    int i0[3];
    float t[3];
    for (int i = 0; i < 3; ++i) {
        i0[i] = std::min(int(x[i]), size - 2);
        t[i] = x[i] - float(i0[i]);
    }
    const size_t n = size_t(size);
    auto at = [&](int r, int g, int b, int ch) {
        return data[((size_t(b) * n + size_t(g)) * n + size_t(r)) * 3 + size_t(ch)];
    };
    float out[3];
    for (int ch = 0; ch < 3; ++ch) {
        const int r = i0[0], g = i0[1], b = i0[2];
        const float c00 = mixf(at(r, g, b, ch), at(r + 1, g, b, ch), t[0]);
        const float c10 = mixf(at(r, g + 1, b, ch), at(r + 1, g + 1, b, ch), t[0]);
        const float c01 = mixf(at(r, g, b + 1, ch), at(r + 1, g, b + 1, ch), t[0]);
        const float c11 = mixf(at(r, g + 1, b + 1, ch), at(r + 1, g + 1, b + 1, ch), t[0]);
        out[ch] = mixf(mixf(c00, c10, t[1]), mixf(c01, c11, t[1]), t[2]);
    }
    return {out[0], out[1], out[2]};
}

std::shared_ptr<const CubeLut> parseCubeLut(std::string_view text, std::string* error)
{
    auto fail = [&](const std::string& msg) -> std::shared_ptr<const CubeLut> {
        if (error)
            *error = msg;
        return nullptr;
    };
    auto lut = std::make_shared<CubeLut>();
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos)
            eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.empty() || line.front() == '#')
            continue;
        auto keyword = [&](std::string_view k) {
            return line.size() > k.size() && line.substr(0, k.size()) == k && (line[k.size()] == ' ' || line[k.size()] == '\t');
        };
        auto rest = [&](std::string_view k) { return line.substr(k.size() + 1); };
        if (keyword("TITLE")) {
            std::string_view t = rest("TITLE");
            while (!t.empty() && (t.front() == ' ' || t.front() == '"'))
                t.remove_prefix(1);
            while (!t.empty() && t.back() == '"')
                t.remove_suffix(1);
            lut->title = std::string(t);
        } else if (keyword("LUT_3D_SIZE") || keyword("LUT_1D_SIZE")) {
            lut->is3D = keyword("LUT_3D_SIZE");
            float n = 0;
            if (!parseFloats(rest("LUT_3D_SIZE"), &n, 1) || n < 2 || n > (lut->is3D ? 256 : 65536))
                return fail("Tamaño de LUT inválido en la línea " + std::to_string(lineNo));
            lut->size = int(n);
        } else if (keyword("DOMAIN_MIN") || keyword("DOMAIN_MAX")) {
            float v[3];
            if (!parseFloats(rest("DOMAIN_MIN"), v, 3))
                return fail("Dominio inválido en la línea " + std::to_string(lineNo));
            auto& d = keyword("DOMAIN_MIN") ? lut->domainMin : lut->domainMax;
            d = {v[0], v[1], v[2]};
        } else if (keyword("LUT_3D_INPUT_RANGE") || keyword("LUT_1D_INPUT_RANGE")) {
            float v[2];
            if (!parseFloats(rest("LUT_3D_INPUT_RANGE"), v, 2))
                return fail("Rango de entrada inválido en la línea " + std::to_string(lineNo));
            lut->domainMin = {v[0], v[0], v[0]};
            lut->domainMax = {v[1], v[1], v[1]};
        } else if ((line.front() >= '0' && line.front() <= '9') || line.front() == '-' || line.front() == '.') {
            float v[3];
            if (!parseFloats(line, v, 3))
                return fail("Dato inválido en la línea " + std::to_string(lineNo));
            lut->data.insert(lut->data.end(), v, v + 3);
        } else {
            // Palabras clave desconocidas (p. ej. de otros programas): se ignoran.
        }
    }
    if (lut->size == 0)
        return fail("Falta LUT_3D_SIZE o LUT_1D_SIZE");
    const size_t expected = (lut->is3D ? size_t(lut->size) * size_t(lut->size) * size_t(lut->size) : size_t(lut->size)) * 3;
    if (lut->data.size() != expected)
        return fail("La LUT tiene " + std::to_string(lut->data.size() / 3) + " entradas y se esperaban " + std::to_string(expected / 3));
    return lut;
}

std::shared_ptr<const CubeLut> loadCubeLut(const std::string& path, std::string* error)
{
    // Ruta UTF-8 (std::filesystem la convierte a la codificación del sistema, también con acentos).
    std::ifstream f(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.c_str()))), std::ios::binary);
    if (!f) {
        if (error)
            *error = "No se pudo abrir el archivo";
        return nullptr;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseCubeLut(ss.str(), error);
}

// --- Curve ---

Curve::Curve(const std::vector<core::CurvePoint>& input)
{
    if (input.empty())
        return;
    std::vector<core::CurvePoint> pts = input;
    std::sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.x < b.x; });
    if (pts.front().x > 1e-4)
        pts.insert(pts.begin(), {0.0, 0.0});
    if (pts.back().x < 1.0 - 1e-4)
        pts.push_back({1.0, 1.0});
    const size_t n = pts.size();
    // Pendientes de Fritsch–Carlson (monótona por tramos, sin oscilaciones).
    std::vector<double> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i)
        d[i] = (pts[i + 1].y - pts[i].y) / std::max(1e-9, pts[i + 1].x - pts[i].x);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = (d[i - 1] * d[i] <= 0) ? 0.0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        const double a = m[i] / d[i], b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    m_table.resize(kCurveTableSize + 1);
    size_t seg = 0;
    for (int i = 0; i <= kCurveTableSize; ++i) {
        const double x = double(i) / kCurveTableSize;
        while (seg + 2 < n && x > pts[seg + 1].x)
            ++seg;
        const double h = pts[seg + 1].x - pts[seg].x;
        const double t = h > 0 ? std::clamp((x - pts[seg].x) / h, 0.0, 1.0) : 0.0;
        const double t2 = t * t, t3 = t2 * t;
        const double y = (2 * t3 - 3 * t2 + 1) * pts[seg].y + (t3 - 2 * t2 + t) * h * m[seg] + (-2 * t3 + 3 * t2) * pts[seg + 1].y
                         + (t3 - t2) * h * m[seg + 1];
        m_table[size_t(i)] = float(std::clamp(y, 0.0, 1.0));
    }
}

float Curve::operator()(float x) const
{
    if (m_table.empty())
        return x;
    const float p = std::clamp(x, 0.0f, 1.0f) * kCurveTableSize;
    const int i = std::min(int(p), kCurveTableSize - 1);
    return mixf(m_table[size_t(i)], m_table[size_t(i) + 1], p - float(i));
}

// --- GradeEvaluator ---

GradeEvaluator::GradeEvaluator(const core::ColorGrade& grade, std::shared_ptr<const CubeLut> lut, std::function<Rgb(Rgb)> input)
    : m_input(std::move(input)), m_g(grade), m_lut(std::move(lut)), m_master(grade.curveMaster), m_red(grade.curveRed), m_green(grade.curveGreen),
      m_blue(grade.curveBlue)
{
    // Balance de blancos: ganancias por canal normalizadas para no cambiar la luminancia.
    const float t = float(grade.temperature), tint = float(grade.tint);
    std::array<float, 3> gains{1.0f + 0.6f * t, 1.0f - 0.6f * tint, 1.0f - 0.6f * t};
    const float l = kLr * gains[0] + kLg * gains[1] + kLb * gains[2];
    for (float& gch : gains)
        gch /= l;
    const float exposure = std::exp2(float(grade.exposure));
    for (float& gch : gains)
        gch *= exposure;
    m_balance = gains;
    m_linearStage = grade.exposure != 0.0 || grade.temperature != 0.0 || grade.tint != 0.0;
    m_hsl = std::any_of(grade.hsl.begin(), grade.hsl.end(), [](const core::HslBand& b) { return !b.isNeutral(); });
}

Rgb GradeEvaluator::apply(Rgb c) const
{
    const core::ColorGrade& g = m_g;
    // 0. Transformación de entrada (material de cámara → señal del proyecto).
    if (m_input)
        c = m_input(c);
    // 1. Balance y exposición en luz lineal.
    if (m_linearStage) {
        c = perChannel(c, [&](float v, int ch) {
            const float lin = std::pow(std::max(v, 0.0f), kGamma) * m_balance[size_t(ch)];
            return std::pow(lin, 1.0f / kGamma);
        });
    }
    // 2. Puntos negro y blanco.
    if (g.blacks != 0.0 || g.whites != 0.0) {
        const float bp = -0.15f * float(g.blacks);
        const float wp = std::max(bp + 0.05f, 1.0f - 0.3f * float(g.whites));
        c = perChannel(c, [&](float v, int) { return (v - bp) / (wp - bp); });
    }
    // 3. Sombras y luces, ponderadas por la luminancia.
    if (g.shadows != 0.0 || g.highlights != 0.0) {
        const float l = std::clamp(luma(c), 0.0f, 1.0f);
        const float delta = 0.3f * float(g.shadows) * (1.0f - smoothstep(0.0f, 0.6f, l))
                            + 0.3f * float(g.highlights) * smoothstep(0.4f, 1.0f, l);
        c = perChannel(c, [&](float v, int) { return v + delta; });
    }
    // 4. Contraste alrededor del gris medio.
    if (g.contrast != 0.0) {
        const float k = 1.0f + float(g.contrast);
        c = perChannel(c, [&](float v, int) { return (v - 0.5f) * k + 0.5f; });
    }
    // 5. Ruedas: lift (sombras), gain (luces) y gamma (medios).
    if (!g.lift.isNeutral() || !g.gamma.isNeutral() || !g.gain.isNeutral()) {
        auto ch = [](const core::ColorWheel& w, int i) { return float((i == 0 ? w.r : i == 1 ? w.g : w.b) + w.master); };
        c = perChannel(c, [&](float v, int i) {
            const float lift = 0.5f * ch(g.lift, i);
            const float gain = 1.0f + ch(g.gain, i);
            float out = gain * (v + lift * (1.0f - v));
            const float exponent = std::exp2(-ch(g.gamma, i));
            return std::pow(std::max(out, 0.0f), exponent);
        });
    }
    // 6. Saturación e intensidad (vibrance).
    if (g.saturation != 0.0 || g.vibrance != 0.0) {
        const float l = luma(c);
        const float sat = std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
        const float f = std::max(0.0f, (1.0f + float(g.saturation)) * (1.0f + float(g.vibrance) * (1.0f - std::clamp(sat, 0.0f, 1.0f))));
        c = perChannel(c, [&](float v, int) { return l + (v - l) * f; });
    }
    // 7. HSL por gamas: pesos que suman 1 entre los dos centros vecinos del tono.
    if (m_hsl) {
        c = clamp01(c);
        float hue, chromaValue;
        hueChroma(c, hue, chromaValue);
        const float strength = smoothstep(0.02f, 0.25f, chromaValue);
        if (strength > 0.0f) {
            size_t k = core::kHslBandHues.size() - 1;
            for (size_t i = 0; i + 1 < core::kHslBandHues.size(); ++i)
                if (hue >= float(core::kHslBandHues[i]) && hue < float(core::kHslBandHues[i + 1]))
                    k = i;
            const size_t next = (k + 1) % core::kHslBandHues.size();
            const float h0 = float(core::kHslBandHues[k]);
            const float h1 = next == 0 ? 360.0f : float(core::kHslBandHues[next]);
            const float s = smoothstep(0.0f, 1.0f, (hue - h0) / (h1 - h0));
            const core::HslBand& a = g.hsl[k];
            const core::HslBand& b = g.hsl[next];
            const float hueShift = mixf(float(a.hue), float(b.hue), s) * strength;
            const float satMul = 1.0f + mixf(float(a.saturation), float(b.saturation), s) * strength;
            const float lum = 0.25f * mixf(float(a.luminance), float(b.luminance), s) * strength;
            c = rotateHue(c, hueShift);
            const float m = (c.r + c.g + c.b) / 3.0f;
            c = perChannel(c, [&](float v, int) { return m + (v - m) * std::max(0.0f, satMul) + lum; });
        }
    }
    // 8. Curvas: la general y luego la de cada canal.
    if (!m_master.isIdentity() || !m_red.isIdentity() || !m_green.isIdentity() || !m_blue.isIdentity()) {
        c = clamp01(c);
        c = {m_red(m_master(c.r)), m_green(m_master(c.g)), m_blue(m_master(c.b))};
    }
    // 9. LUT creativa con su intensidad.
    if (m_lut && g.lutIntensity > 0.0) {
        c = clamp01(c);
        c = mix(c, m_lut->apply(c), float(g.lutIntensity));
    }
    return clamp01(c);
}

// --- Lut3D ---

std::shared_ptr<const Lut3D> bakeLut(const GradeEvaluator& grade, int size)
{
    auto lut = std::make_shared<Lut3D>();
    lut->size = size;
    lut->rgba.resize(size_t(size) * size_t(size) * size_t(size) * 4);
    const float step = 1.0f / float(size - 1);
    size_t i = 0;
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r) {
                const Rgb out = grade.apply({float(r) * step, float(g) * step, float(b) * step});
                lut->rgba[i++] = out.r;
                lut->rgba[i++] = out.g;
                lut->rgba[i++] = out.b;
                lut->rgba[i++] = 1.0f;
            }
    return lut;
}

std::shared_ptr<const Lut3D> mixLuts(const Lut3D& a, const Lut3D& b, double t)
{
    auto out = std::make_shared<Lut3D>();
    out->size = a.size;
    out->rgba.resize(a.rgba.size());
    const float k = float(std::clamp(t, 0.0, 1.0));
    for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
        out->rgba[i] = mixf(a.rgba[i], b.rgba[i], k);
    return out;
}

Rgb Lut3D::sample(Rgb in) const
{
    CubeLut view;
    view.size = size;
    view.data.reserve(rgba.size() / 4 * 3);
    for (size_t i = 0; i < rgba.size(); i += 4)
        view.data.insert(view.data.end(), {rgba[i], rgba[i + 1], rgba[i + 2]});
    return view.apply(in);
}

// --- Croma ---

std::array<float, 2> chroma(Rgb c)
{
    const float y = luma(c);
    return {(c.b - y) / 1.8556f, (c.r - y) / 1.5748f};
}

Rgb argbToRgb(std::uint32_t argb)
{
    return {float((argb >> 16) & 0xFF) / 255.0f, float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f};
}

KeyResult applyChromaKey(Rgb c, const core::ChromaKey& key)
{
    if (!key.enabled)
        return {c, 1.0f};
    const auto pc = chroma(c);
    const auto kc = chroma(argbToRgb(key.color));
    const float dist = 2.0f * std::hypot(pc[0] - kc[0], pc[1] - kc[1]);
    KeyResult r;
    r.alpha = smoothstep(float(key.tolerance), float(key.tolerance + key.softness), dist);
    // Supresión del derrame: se quita la componente del croma en la dirección del color clave.
    const float klen = std::hypot(kc[0], kc[1]);
    float cb = pc[0], cr = pc[1];
    if (klen > 1e-5f && key.spill > 0.0) {
        const float dx = kc[0] / klen, dy = kc[1] / klen;
        const float proj = std::max(0.0f, cb * dx + cr * dy);
        cb -= float(key.spill) * proj * dx;
        cr -= float(key.spill) * proj * dy;
    }
    const float y = luma(c);
    const float rr = y + 1.5748f * cr;
    const float bb = y + 1.8556f * cb;
    const float gg = (y - kLr * rr - kLb * bb) / kLg;
    r.color = clamp01({rr, gg, bb});
    return r;
}

// --- Fusión ---

float blendChannel(core::BlendMode mode, float b, float s)
{
    using core::BlendMode;
    switch (mode) {
    case BlendMode::Normal: return s;
    case BlendMode::Multiply: return b * s;
    case BlendMode::Screen: return b + s - b * s;
    case BlendMode::Overlay: return blendChannel(BlendMode::HardLight, s, b);
    case BlendMode::Darken: return std::min(b, s);
    case BlendMode::Lighten: return std::max(b, s);
    case BlendMode::ColorDodge:
        if (b <= 0.0f)
            return 0.0f;
        return s >= 1.0f ? 1.0f : std::min(1.0f, b / (1.0f - s));
    case BlendMode::ColorBurn:
        if (b >= 1.0f)
            return 1.0f;
        return s <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - b) / s);
    case BlendMode::HardLight:
        return s <= 0.5f ? b * 2.0f * s : blendChannel(BlendMode::Screen, b, 2.0f * s - 1.0f);
    case BlendMode::SoftLight: {
        if (s <= 0.5f)
            return b - (1.0f - 2.0f * s) * b * (1.0f - b);
        const float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : std::sqrt(b);
        return b + (2.0f * s - 1.0f) * (d - b);
    }
    case BlendMode::Difference: return std::abs(b - s);
    case BlendMode::Exclusion: return b + s - 2.0f * b * s;
    case BlendMode::Add: return std::min(1.0f, b + s);
    case BlendMode::Subtract: return std::max(0.0f, b - s);
    }
    return s;
}

Rgb blend(core::BlendMode mode, Rgb b, Rgb s)
{
    return {blendChannel(mode, b.r, s.r), blendChannel(mode, b.g, s.g), blendChannel(mode, b.b, s.b)};
}

} // namespace movix::color
