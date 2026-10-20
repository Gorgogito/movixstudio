#include "ai/Tracking.h"

#include "ai/Vision.h"
#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <cmath>

namespace movix::ai {

namespace {

// Imagen reducida con tres canales (luma y crominancia, esta con más peso): un objeto que solo se
// distingue del fondo por su color (rojo sobre gris) apenas contrasta en luma.
constexpr int kChannels = 3;
constexpr float kChromaWeight = 2.0f;

struct Image {
    int w = 0, h = 0;
    std::vector<float> v; // planos consecutivos
    float at(int c, int x, int y) const
    {
        return v[size_t(c) * size_t(w) * size_t(h) + size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))];
    }
};

Image toImage(const media::VideoFrame& f, int maxWidth)
{
    Image g;
    const double scale = std::min(1.0, double(maxWidth) / f.width);
    g.w = std::max(16, int(std::lround(f.width * scale)));
    g.h = std::max(16, int(std::lround(f.height * scale)));
    g.v.resize(size_t(kChannels) * size_t(g.w) * size_t(g.h));
    const int bytes = f.format == media::PixelFormat::P016 ? 2 : 1;
    const size_t plane = size_t(g.w) * size_t(g.h);
    for (int y = 0; y < g.h; ++y) {
        const int sy = std::min(f.height - 1, int((y + 0.5) / scale));
        for (int x = 0; x < g.w; ++x) {
            const int sx = std::min(f.width - 1, int((x + 0.5) / scale));
            const size_t i = size_t(y) * size_t(g.w) + size_t(x);
            g.v[i] = f.y[(size_t(sy) * size_t(f.width) + size_t(sx)) * bytes + (bytes - 1)];
            const size_t uv = (size_t(sy / 2) * size_t(f.chromaWidth()) + size_t(sx / 2)) * 2;
            g.v[plane + i] = kChromaWeight * f.uv[uv * bytes + (bytes - 1)];
            g.v[2 * plane + i] = kChromaWeight * f.uv[(uv + 1) * bytes + (bytes - 1)];
        }
    }
    return g;
}

// Plantilla con pesos gaussianos: el centro de la región (el objeto) cuenta más que sus bordes
// (casi siempre fondo, que no se mueve con él).
struct Template {
    int w = 0, h = 0;
    std::vector<float> v, weight; // v: kChannels planos de w × h
    double weightSum = 0, var = 0;
    double mean[kChannels] = {0, 0, 0};

    void initWeights()
    {
        weight.resize(size_t(w) * size_t(h));
        const double sx = 0.35 * w, sy = 0.35 * h;
        weightSum = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = (x + 0.5 - w / 2.0) / sx, dy = (y + 0.5 - h / 2.0) / sy;
                weight[size_t(y) * size_t(w) + size_t(x)] = float(std::exp(-0.5 * (dx * dx + dy * dy)));
                weightSum += weight[size_t(y) * size_t(w) + size_t(x)];
            }
    }
    void updateStats()
    {
        const size_t n = weight.size();
        var = 0;
        for (int c = 0; c < kChannels; ++c) {
            mean[c] = 0;
            for (size_t i = 0; i < n; ++i)
                mean[c] += weight[i] * v[size_t(c) * n + i];
            mean[c] /= weightSum;
            for (size_t i = 0; i < n; ++i)
                var += weight[i] * std::pow(v[size_t(c) * n + i] - mean[c], 2);
        }
    }
};

// Correlación cruzada normalizada (ponderada, sobre los tres canales) con la esquina en (ox, oy).
float ncc(const Image& img, const Template& t, int ox, int oy)
{
    const size_t n = t.weight.size();
    double cross = 0, var = 0;
    for (int c = 0; c < kChannels; ++c) {
        double mean = 0;
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x)
                mean += t.weight[size_t(y) * size_t(t.w) + size_t(x)] * img.at(c, ox + x, oy + y);
        mean /= t.weightSum;
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x) {
                const size_t i = size_t(y) * size_t(t.w) + size_t(x);
                const double d = img.at(c, ox + x, oy + y) - mean;
                cross += t.weight[i] * d * (t.v[size_t(c) * n + i] - t.mean[c]);
                var += t.weight[i] * d * d;
            }
    }
    if (var <= 1e-6 || t.var <= 1e-6)
        return 0.0f;
    return float(cross / std::sqrt(var * t.var));
}

} // namespace

namespace {

// Reduce la imagen a 1/factor promediando bloques (nivel grueso de la pirámide).
Image downscale(const Image& g, int factor)
{
    Image s;
    s.w = std::max(4, g.w / factor);
    s.h = std::max(4, g.h / factor);
    s.v.resize(size_t(kChannels) * size_t(s.w) * size_t(s.h));
    for (int c = 0; c < kChannels; ++c)
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x) {
                float sum = 0;
                for (int dy = 0; dy < factor; ++dy)
                    for (int dx = 0; dx < factor; ++dx)
                        sum += g.at(c, x * factor + dx, y * factor + dy);
                s.v[size_t(c) * size_t(s.w) * size_t(s.h) + size_t(y) * size_t(s.w) + size_t(x)] = sum / float(factor * factor);
            }
    return s;
}

// Copia (o mezcla, con `blend`) en la plantilla la región de la imagen con la esquina en (x0, y0).
void capture(Template& t, const Image& g, int x0, int y0, double blend)
{
    const size_t n = size_t(t.w) * size_t(t.h);
    if (t.v.empty())
        t.v.assign(size_t(kChannels) * n, 0.0f);
    const bool first = blend >= 1.0;
    for (int c = 0; c < kChannels; ++c)
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x) {
                float& v = t.v[size_t(c) * n + size_t(y) * size_t(t.w) + size_t(x)];
                const float fresh = g.at(c, x0 + x, y0 + y);
                v = first ? fresh : float((1 - blend) * v + blend * fresh);
            }
    t.updateStats();
}

// Mejor posición de la plantilla alrededor de (cx, cy) en un radio, con el paso dado.
std::pair<int, int> search(const Image& g, const Template& t, int cx, int cy, int radius, int step, float& best)
{
    best = -2.0f;
    int bx = cx, by = cy;
    for (int dy = -radius; dy <= radius; dy += step)
        for (int dx = -radius; dx <= radius; dx += step) {
            const float s = ncc(g, t, cx + dx, cy + dy);
            if (s > best) {
                best = s;
                bx = cx + dx;
                by = cy + dy;
            }
        }
    return {bx, by};
}

} // namespace

bool trackRegion(const std::string& mediaPath, TimeUs from, TimeUs to, const Region& start, std::vector<TrackPoint>& out,
                 std::string* error, const ProgressFn& progress, const CancelFn& cancel)
{
    out.clear();
    media::VideoDecoder decoder;
    if (!decoder.open(mediaPath, {}, error))
        return false;
    if (to <= from)
        to = decoder.duration();
    decoder.seek(from);

    // Pirámide de dos niveles: búsqueda amplia a 1/4 de resolución y ajuste fino en la completa
    // (decenas de veces menos operaciones que buscar en la completa, con la misma precisión).
    constexpr int kWidth = 480;
    constexpr int kFactor = 4;
    Template fine, coarse;
    double px = 0, py = 0; // esquina de la región en píxeles de la imagen de trabajo

    while (auto frame = decoder.decodeNext()) {
        if (frame->pts >= to)
            break;
        if (cancel && cancel()) {
            if (error)
                *error = "cancelado";
            return false;
        }
        if (frame->format == media::PixelFormat::RGBA)
            break; // imágenes fijas: no hay movimiento que seguir
        const Image g = toImage(*frame, kWidth);
        const Image small = downscale(g, kFactor);
        if (fine.v.empty()) {
            fine.w = std::clamp(int(std::lround(start.w * g.w)), 8, g.w);
            fine.h = std::clamp(int(std::lround(start.h * g.h)), 8, g.h);
            fine.initWeights();
            coarse.w = std::max(4, fine.w / kFactor);
            coarse.h = std::max(4, fine.h / kFactor);
            coarse.initWeights();
            px = start.cx * g.w - fine.w / 2.0;
            py = start.cy * g.h - fine.h / 2.0;
            capture(fine, g, int(std::lround(px)), int(std::lround(py)), 1.0);
            capture(coarse, small, int(std::lround(px / kFactor)), int(std::lround(py / kFactor)), 1.0);
            out.push_back({frame->pts, start.cx, start.cy, 1.0f});
            continue;
        }
        float best = 0;
        // 1) Grueso: ventana del tamaño de la región, píxel a píxel en la imagen reducida.
        const int radius = std::max(4, std::max(coarse.w, coarse.h) / 2);
        const auto [sx, sy] = search(small, coarse, int(std::lround(px / kFactor)), int(std::lround(py / kFactor)), radius, 1, best);
        // 2) Fino: alrededor de la posición gruesa, en la imagen completa.
        const auto [fx, fy] = search(g, fine, sx * kFactor, sy * kFactor, kFactor, 1, best);
        px = fx;
        py = fy;
        // Las plantillas se adaptan despacio (cambios de luz, giros) solo si el ajuste es bueno.
        if (best > 0.6f) {
            capture(fine, g, fx, fy, 0.1);
            capture(coarse, small, int(std::lround(px / kFactor)), int(std::lround(py / kFactor)), 0.1);
        }
        out.push_back({frame->pts, (px + fine.w / 2.0) / g.w, (py + fine.h / 2.0) / g.h, std::max(0.0f, best)});
        if (progress && to > from)
            progress(std::clamp(double(frame->pts - from) / double(to - from), 0.0, 1.0));
    }
    if (out.empty()) {
        if (error)
            *error = "El tramo no tiene fotogramas de video.";
        return false;
    }
    return true;
}

bool subjectCenters(const std::string& mediaPath, TimeUs from, TimeUs to, TimeUs step, const std::string& portraitModel,
                    std::vector<TrackPoint>& out, std::string* error, const ProgressFn& progress, const CancelFn& cancel)
{
    out.clear();
    media::VideoDecoder decoder;
    if (!decoder.open(mediaPath, {}, error))
        return false;
    if (to <= from)
        to = decoder.duration();
    step = std::max<TimeUs>(step, 40'000);
    std::vector<uint8_t> rgba, alpha;
    for (TimeUs t = from; t < to; t += step) {
        if (cancel && cancel()) {
            if (error)
                *error = "cancelado";
            return false;
        }
        if (!decoder.seek(t))
            break;
        auto frame = decoder.decodeNext();
        if (!frame)
            break;
        constexpr int w = 320;
        const int h = std::max(16, int(std::lround(double(frame->height) * w / frame->width)));
        if (!media::frameToRgba(*frame, w, h, rgba))
            break;
        double cx = 0.5, weight = 0;
        // 1) Personas (MODNet).
        if (!portraitModel.empty() && matteFrame(portraitModel, MatteModel::Portrait, rgba.data(), w, h, alpha, nullptr)) {
            double sx = 0, sw = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const double a = alpha[size_t(y) * w + size_t(x)] / 255.0;
                    sx += a * x;
                    sw += a;
                }
            if (sw > 0.02 * w * h) { // al menos un 2 % de la imagen es una persona
                cx = sx / sw / w;
                weight = 1;
            }
        }
        // 2) Lo que más destaca del color medio (el fondo suele ocupar la mayor parte).
        if (weight == 0) {
            double mean[3] = {0, 0, 0};
            for (int i = 0; i < w * h; ++i)
                for (int c = 0; c < 3; ++c)
                    mean[c] += rgba[size_t(i) * 4 + size_t(c)];
            for (double& m : mean)
                m /= w * h;
            std::vector<double> d(size_t(w) * size_t(h));
            double maxD = 0;
            for (int i = 0; i < w * h; ++i) {
                double s = 0;
                for (int c = 0; c < 3; ++c)
                    s += std::pow(rgba[size_t(i) * 4 + size_t(c)] - mean[c], 2);
                d[size_t(i)] = std::sqrt(s);
                maxD = std::max(maxD, d[size_t(i)]);
            }
            // Solo cuenta lo que destaca de verdad (la mitad superior del rango de diferencias).
            double sx = 0, sw = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const double v = std::max(0.0, d[size_t(y) * w + size_t(x)] - 0.5 * maxD);
                    sx += v * x;
                    sw += v;
                }
            if (sw > 0)
                cx = sx / sw / w;
        }
        out.push_back({t, cx, 0.5, 1.0f});
        if (progress)
            progress(std::clamp(double(t - from) / double(std::max<TimeUs>(1, to - from)), 0.0, 1.0));
    }
    if (out.empty()) {
        if (error)
            *error = "El tramo no tiene fotogramas de video.";
        return false;
    }
    // Suavizado: media de los vecinos a ±0,5 s (el encuadre no debe saltar con cada gesto).
    std::vector<TrackPoint> smooth = out;
    for (size_t i = 0; i < out.size(); ++i) {
        double sum = 0;
        int n = 0;
        for (size_t j = 0; j < out.size(); ++j)
            if (std::llabs(out[j].time - out[i].time) <= 500'000) {
                sum += out[j].cx;
                ++n;
            }
        smooth[i].cx = sum / n;
    }
    out = std::move(smooth);
    return true;
}

} // namespace movix::ai

