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

// Posición de la plantilla en la imagen: centro en píxeles (el centro del píxel i está en i + 0,5),
// escala y giro (radianes, horario con y hacia abajo, como las máscaras).
struct Pose {
    double x = 0, y = 0, scale = 1, angle = 0;
};

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

// Muestrea (bilineal) la región de la imagen que cubre la plantilla en la posición dada, en el
// sistema de la plantilla: la deshace de la escala y el giro.
void warp(const Image& img, const Template& t, const Pose& pose, std::vector<float>& out)
{
    const size_t n = size_t(t.w) * size_t(t.h), plane = size_t(img.w) * size_t(img.h);
    out.resize(size_t(kChannels) * n);
    const double c = std::cos(pose.angle) * pose.scale, s = std::sin(pose.angle) * pose.scale;
    for (int j = 0; j < t.h; ++j) {
        const double ov = j + 0.5 - t.h / 2.0;
        for (int i = 0; i < t.w; ++i) {
            const double ou = i + 0.5 - t.w / 2.0;
            const double fx = pose.x + c * ou - s * ov - 0.5, fy = pose.y + s * ou + c * ov - 0.5;
            const double flx = std::floor(fx), fly = std::floor(fy);
            const float ax = float(fx - flx), ay = float(fy - fly);
            const int x0 = std::clamp(int(flx), 0, img.w - 1), x1 = std::clamp(int(flx) + 1, 0, img.w - 1);
            const int y0 = std::clamp(int(fly), 0, img.h - 1), y1 = std::clamp(int(fly) + 1, 0, img.h - 1);
            const size_t i00 = size_t(y0) * size_t(img.w) + size_t(x0), i01 = size_t(y0) * size_t(img.w) + size_t(x1);
            const size_t i10 = size_t(y1) * size_t(img.w) + size_t(x0), i11 = size_t(y1) * size_t(img.w) + size_t(x1);
            const size_t k = size_t(j) * size_t(t.w) + size_t(i);
            for (int ch = 0; ch < kChannels; ++ch) {
                const float* p = img.v.data() + size_t(ch) * plane;
                const float top = p[i00] + ax * (p[i01] - p[i00]), bottom = p[i10] + ax * (p[i11] - p[i10]);
                out[size_t(ch) * n + k] = top + ay * (bottom - top);
            }
        }
    }
}

// Correlación cruzada normalizada (ponderada, sobre los tres canales) de una muestra con la plantilla.
float ncc(const std::vector<float>& patch, const Template& t)
{
    const size_t n = t.weight.size();
    double cross = 0, var = 0;
    for (int c = 0; c < kChannels; ++c) {
        const float* p = patch.data() + size_t(c) * n;
        double mean = 0;
        for (size_t i = 0; i < n; ++i)
            mean += t.weight[i] * p[i];
        mean /= t.weightSum;
        const float* tv = t.v.data() + size_t(c) * n;
        for (size_t i = 0; i < n; ++i) {
            const double d = p[i] - mean;
            cross += t.weight[i] * d * (tv[i] - t.mean[c]);
            var += t.weight[i] * d * d;
        }
    }
    if (var <= 1e-6 || t.var <= 1e-6)
        return 0.0f;
    return float(cross / std::sqrt(var * t.var));
}

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

// Copia (o mezcla, con `blend`) en la plantilla la región de la imagen en la posición dada.
void capture(Template& t, const Image& g, const Pose& pose, double blend, std::vector<float>& buffer)
{
    warp(g, t, pose, buffer);
    if (blend >= 1.0 || t.v.empty())
        t.v = buffer;
    else
        for (size_t i = 0; i < t.v.size(); ++i)
            t.v[i] = float((1 - blend) * t.v[i] + blend * buffer[i]);
    t.updateStats();
}

Pose scaled(Pose p, int factor)
{
    p.x /= factor;
    p.y /= factor;
    return p;
}

// Mejor desplazamiento entero de la plantilla alrededor de `pose` en un radio (escala y giro fijos).
Pose search(const Image& g, const Template& t, const Pose& pose, int radius, float& best, std::vector<float>& buffer)
{
    best = -2.0f;
    Pose result = pose;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            Pose p = pose;
            p.x += dx;
            p.y += dy;
            warp(g, t, p, buffer);
            const float s = ncc(buffer, t);
            if (s > best) {
                best = s;
                result = p;
            }
        }
    return result;
}

} // namespace

bool trackRegion(const std::string& mediaPath, TimeUs from, TimeUs to, const Region& start, bool scaleRotation,
                 std::vector<TrackPoint>& out, std::string* error, const ProgressFn& progress, const CancelFn& cancel)
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
    constexpr double kPi = 3.14159265358979323846;
    // La plantilla adaptada sigue los cambios de luz y de aspecto; la del primer fotograma ancla la
    // escala y el giro (sin ella, los errores pequeños de cada fotograma se acumularían).
    Template fine, coarse, anchor;
    Pose pose;
    std::vector<float> buffer;
    const double angle0 = start.angle * kPi / 180.0;

    auto score = [&](const Image& g, const Pose& p, float* adapted) {
        warp(g, fine, p, buffer);
        const float a = ncc(buffer, fine);
        if (adapted)
            *adapted = a;
        return 0.7f * a + 0.3f * ncc(buffer, anchor);
    };

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
            pose = {start.cx * g.w, start.cy * g.h, 1.0, angle0};
            capture(fine, g, pose, 1.0, buffer);
            anchor = fine;
            capture(coarse, small, scaled(pose, kFactor), 1.0, buffer);
            out.push_back({frame->pts, start.cx, start.cy, 1.0f, 1.0, 0.0});
            continue;
        }
        float best = 0;
        // 1) Grueso: ventana del tamaño de la región, píxel a píxel en la imagen reducida.
        const int radius = std::max(4, std::max(coarse.w, coarse.h) / 2);
        Pose p = search(small, coarse, scaled(pose, kFactor), radius, best, buffer);
        p.x *= kFactor;
        p.y *= kFactor;
        // 2) Fino: alrededor de la posición gruesa, en la imagen completa.
        p = search(g, fine, p, kFactor, best, buffer);
        // 3) Ajuste: medio píxel y, si se pide, escala y giro, por descenso de coordenadas con pasos
        //    que se reducen a la mitad. La escala y el giro solo cambian si mejoran de forma clara
        //    (un objeto que solo se desplaza no debe temblar).
        float adapted = 0;
        float total = score(g, p, &adapted);
        double steps[4] = {0.5, 0.5, 0.03, 2.0 * kPi / 180.0};
        const int params = scaleRotation ? 4 : 2;
        for (int round = 0; round < 3; ++round) {
            for (int pass = 0; pass < 8; ++pass) {
                bool improved = false;
                for (int k = 0; k < params; ++k)
                    for (const double sign : {1.0, -1.0}) {
                        Pose c = p;
                        if (k == 0)
                            c.x += sign * steps[0];
                        else if (k == 1)
                            c.y += sign * steps[1];
                        else if (k == 2)
                            c.scale = std::clamp(sign > 0 ? c.scale * (1 + steps[2]) : c.scale / (1 + steps[2]), 0.1, 10.0);
                        else
                            c.angle += sign * steps[3];
                        float a = 0;
                        const float s = score(g, c, &a);
                        if (s > total + (k < 2 ? 1e-5f : 2e-4f)) {
                            p = c;
                            total = s;
                            adapted = a;
                            improved = true;
                            break;
                        }
                    }
                if (!improved)
                    break;
            }
            for (double& s : steps)
                s /= 2;
        }
        pose = p;
        // Las plantillas se adaptan despacio (cambios de luz, deformaciones) solo si el ajuste es bueno.
        if (adapted > 0.6f) {
            capture(fine, g, pose, 0.1, buffer);
            capture(coarse, small, scaled(pose, kFactor), 0.1, buffer);
        }
        out.push_back({frame->pts, pose.x / g.w, pose.y / g.h, std::max(0.0f, adapted), pose.scale,
                       (pose.angle - angle0) * 180.0 / kPi});
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

