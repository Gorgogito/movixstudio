#include "ai/Scenes.h"

#include "media/VideoDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <tuple>

namespace movix::ai {

SceneFeatures sceneFeatures(const media::VideoFrame& f)
{
    SceneFeatures out;
    if (f.format == media::PixelFormat::RGBA || f.width <= 0 || f.height <= 0)
        return out;
    const int bytes = f.format == media::PixelFormat::P016 ? 2 : 1;
    // Muestras de 8 bits (en P016 el byte alto).
    auto lumaAt = [&](int x, int y) { return f.y[(size_t(y) * size_t(f.width) + size_t(x)) * bytes + (bytes - 1)]; };
    auto chromaAt = [&](int cx, int cy, int c) {
        return f.uv[(size_t(cy) * size_t(f.chromaWidth()) * 2 + size_t(cx) * 2 + size_t(c)) * bytes + (bytes - 1)];
    };

    // Se muestrea una rejilla de 64 × 36 puntos: suficiente y barato a cualquier resolución.
    constexpr int kSx = 64, kSy = 36;
    std::array<float, 16 * 9> sums{};
    std::array<int, 16 * 9> counts{};
    for (int sy = 0; sy < kSy; ++sy) {
        const int y = (2 * sy + 1) * f.height / (2 * kSy);
        for (int sx = 0; sx < kSx; ++sx) {
            const int x = (2 * sx + 1) * f.width / (2 * kSx);
            const int l = lumaAt(x, y);
            const int cell = (sy * 9 / kSy) * 16 + sx * 16 / kSx;
            sums[size_t(cell)] += float(l);
            ++counts[size_t(cell)];
            const int cb = chromaAt(std::min(x / 2, f.chromaWidth() - 1), std::min(y / 2, f.chromaHeight() - 1), 0);
            const int cr = chromaAt(std::min(x / 2, f.chromaWidth() - 1), std::min(y / 2, f.chromaHeight() - 1), 1);
            out.hist[size_t((l >> 5) * 16 + (cb >> 6) * 4 + (cr >> 6))] += 1.0f;
        }
    }
    for (size_t i = 0; i < sums.size(); ++i)
        out.grid[i] = counts[i] ? sums[i] / (255.0f * float(counts[i])) : 0.0f;
    for (float& h : out.hist)
        h /= float(kSx * kSy);
    return out;
}

float sceneDistance(const SceneFeatures& a, const SceneFeatures& b)
{
    float grid = 0, hist = 0;
    for (size_t i = 0; i < a.grid.size(); ++i)
        grid += std::abs(a.grid[i] - b.grid[i]);
    for (size_t i = 0; i < a.hist.size(); ++i)
        hist += std::abs(a.hist[i] - b.hist[i]);
    grid /= float(a.grid.size());
    hist *= 0.5f; // L1 de dos distribuciones: 0..2 → 0..1
    // La rejilla ve los cambios de composición; el histograma, los de luz y color.
    return std::min(1.0f, 0.5f * hist + 1.5f * grid);
}

std::vector<TimeUs> findCuts(const std::vector<TimeUs>& times, const std::vector<float>& d, const SceneOptions& o)
{
    std::vector<TimeUs> cuts;
    const size_t n = std::min(times.size(), d.size());
    if (n < 2)
        return cuts;
    // Entorno: ±1 s aproximado (según la separación media de los fotogramas).
    const double step = double(times[n - 1] - times[0]) / double(n - 1);
    const int window = std::clamp(int(std::lround(kUsPerSecond / std::max(step, 1.0))), 2, 60);
    TimeUs last = times[0];
    for (size_t i = 1; i < n; ++i) {
        if (d[i] < o.threshold)
            continue;
        // Media del entorno sin el propio fotograma ni sus vecinos inmediatos.
        double sum = 0;
        int count = 0;
        for (int k = -window; k <= window; ++k) {
            const int64_t j = int64_t(i) + k;
            if (std::abs(k) <= 1 || j < 1 || j >= int64_t(n))
                continue;
            sum += d[size_t(j)];
            ++count;
        }
        const double around = count ? sum / count : 0.0;
        // Máximo local: un corte es un pico aislado.
        const bool peak = d[i] >= d[i - 1] && (i + 1 >= n || d[i] >= d[i + 1]);
        if (peak && d[i] > o.contrast * around + 0.02 && times[i] - last >= o.minScene) {
            cuts.push_back(times[i]);
            last = times[i];
        }
    }
    return cuts;
}

SceneGrid sceneGrid(const SceneFeatures& features)
{
    SceneGrid g;
    for (size_t i = 0; i < g.size(); ++i)
        g[i] = uint8_t(std::lround(std::clamp(features.grid[i], 0.0f, 1.0f) * 255.0f));
    return g;
}

namespace {

// Variación de la luma entre celdas (0 = imagen lisa).
double spread(const SceneGrid& g)
{
    double sum = 0, sum2 = 0;
    for (const uint8_t v : g) {
        sum += v / 255.0;
        sum2 += (v / 255.0) * (v / 255.0);
    }
    const double mean = sum / double(g.size());
    return std::sqrt(std::max(0.0, sum2 / double(g.size()) - mean * mean));
}

// Imagen lisa (negro, blanco o un color uniforme).
bool isFlat(const SceneGrid& g)
{
    return spread(g) < 0.03;
}

struct Transition {
    size_t start = 0, end = 0, cut = 0; // primer fotograma de la mezcla, primero ya en el destino, mitad
    double distance = 0; // cambio medio de la luma reducida entre los extremos
    bool fromFlat = false, toFlat = false;
};

// ¿Es el tramo [a, b] una mezcla de sus extremos? Cada fotograma intermedio debe quedar en el
// segmento que los une (residuo pequeño respecto de su distancia), avanzando sin volver atrás y sin
// saltos (un corte es un salto; un movimiento se sale de la recta).
bool linearMix(const std::vector<SceneGrid>& g, size_t a, size_t b, const SceneOptions& o, Transition& t)
{
    constexpr size_t kCells = std::tuple_size_v<SceneGrid>;
    std::array<double, kCells> delta;
    double len2 = 0, l1 = 0;
    for (size_t k = 0; k < kCells; ++k) {
        delta[k] = (double(g[b][k]) - double(g[a][k])) / 255.0;
        len2 += delta[k] * delta[k];
        l1 += std::abs(delta[k]);
    }
    if (l1 / double(kCells) < o.fadeThreshold)
        return false;
    double peak = 0, previous = 0, worst = 0;
    int halfway = 0;
    t = Transition{};
    t.start = t.end = t.cut = b;
    for (size_t j = a + 1; j < b; ++j) {
        double dot = 0, norm2 = 0;
        for (size_t k = 0; k < kCells; ++k) {
            const double x = (double(g[j][k]) - double(g[a][k])) / 255.0;
            dot += x * delta[k];
            norm2 += x * x;
        }
        const double alpha = dot / len2;
        worst = std::max(worst, std::sqrt(std::max(0.0, norm2 - dot * dot / len2) / len2));
        if (worst > 0.2 || alpha < peak - 0.1 || alpha - previous > 0.35)
            return false;
        peak = std::max(peak, alpha);
        previous = alpha;
        if (alpha > 0.1 && alpha < 0.9)
            ++halfway;
        if (t.start == b && alpha > 0.05)
            t.start = j;
        if (t.cut == b && alpha >= 0.5)
            t.cut = j;
        if (t.end == b && alpha >= 0.95)
            t.end = j;
    }
    if (1.0 - previous > 0.35 || halfway < 2)
        return false;
    t.distance = l1 / double(kCells);
    t.fromFlat = isFlat(g[a]);
    t.toFlat = isFlat(g[b]);
    return true;
}

} // namespace

std::vector<TimeUs> withGradualCuts(const std::vector<TimeUs>& times, const std::vector<SceneGrid>& grids,
                                    const std::vector<TimeUs>& hardCuts, const SceneOptions& o)
{
    const size_t n = std::min(times.size(), grids.size());
    if (n < 8)
        return hardCuts;
    // Ventanas de 1/3 s a 2,7 s que se solapan en 3/4: cualquier transición de hasta ~2 s cabe
    // entera, con algo de los planos de antes y de después, en alguna.
    const double step = double(times[n - 1] - times[0]) / double(n - 1);
    std::vector<Transition> found;
    for (const double seconds : {0.33, 0.67, 1.33, 2.67}) {
        const size_t w = std::max<size_t>(4, size_t(std::lround(seconds * kUsPerSecond / std::max(step, 1.0))));
        if (w >= n)
            break;
        for (size_t a = 0; a + w < n; a += std::max<size_t>(1, w / 4)) {
            Transition t;
            if (linearMix(grids, a, a + w, o, t))
                found.push_back(t);
        }
    }
    // Varias ventanas encuentran la misma transición, y un trozo de una mezcla también es una mezcla:
    // de las que se solapan se queda la de más cambio entre extremos (la transición completa).
    std::sort(found.begin(), found.end(), [](const Transition& x, const Transition& y) { return x.start < y.start; });
    std::vector<Transition> merged;
    size_t groupEnd = 0;
    for (const Transition& t : found) {
        if (!merged.empty() && t.start < groupEnd) {
            if (t.distance > merged.back().distance)
                merged.back() = t;
            groupEnd = std::max(groupEnd, t.end);
        } else {
            merged.push_back(t);
            groupEnd = t.end;
        }
    }

    // Un corte dentro de una transición (o en sus bordes) es parte de ella: en los colores lisos, el
    // histograma salta cuando muchos píxeles cruzan a la vez el límite de una de sus casillas.
    std::vector<TimeUs> cuts;
    for (const TimeUs c : hardCuts) {
        const bool inside = std::any_of(merged.begin(), merged.end(), [&](const Transition& t) {
            return c >= times[t.start >= 2 ? t.start - 2 : 0] && c <= times[std::min(n - 1, t.end + 2)];
        });
        if (!inside)
            cuts.push_back(c);
    }

    std::vector<bool> flat(n);
    for (size_t i = 0; i < n; ++i)
        flat[i] = isFlat(grids[i]);
    auto cutBetween = [&](size_t first, size_t last) {
        return std::any_of(cuts.begin(), cuts.end(), [&](TimeUs c) { return c >= times[first] && c <= times[last]; });
    };
    // El cambio en un tramo liso va en su fotograma más uniforme (el más oscuro de un fundido a negro;
    // en un negro sostenido, el centro): la imagen nueva puede aparecer despacio y alargar el tramo.
    auto flattest = [&](size_t first, size_t last) {
        double least = 1e9;
        for (size_t i = first; i <= last; ++i)
            least = std::min(least, spread(grids[i]));
        size_t from = last, to = first;
        for (size_t i = first; i <= last; ++i)
            if (spread(grids[i]) <= least + 0.002) {
                from = std::min(from, i);
                to = std::max(to, i);
            }
        return times[(from + to + 1) / 2];
    };
    std::vector<TimeUs> candidates;
    std::vector<std::pair<size_t, size_t>> usedFlat; // tramos lisos ya resueltos por un fundido de salida
    // Fundidos a un color liso: el cambio va en el centro del tramo liso, si después vuelve la imagen.
    for (const Transition& t : merged) {
        if (t.fromFlat || !t.toFlat)
            continue;
        size_t first = t.end, last = t.end;
        while (first > t.start && flat[first - 1])
            --first;
        while (last + 1 < n && flat[last + 1])
            ++last;
        if (last + 1 >= n || cutBetween(t.start, last + 1))
            continue; // fundido al final, o un corte ya separa las escenas
        usedFlat.emplace_back(first, last);
        candidates.push_back(flattest(first, last));
    }
    // Apariciones desde un color liso que no siguen a un fundido ya resuelto.
    for (const Transition& t : merged) {
        if (!t.fromFlat || t.toFlat)
            continue;
        size_t last = t.start > 0 ? t.start - 1 : 0, first = last;
        while (first > 0 && flat[first - 1])
            --first;
        const bool used = std::any_of(usedFlat.begin(), usedFlat.end(), [&](const auto& r) { return last >= r.first && first <= r.second; });
        if (used || first == 0 || cutBetween(first - 1, t.end))
            continue; // ya resuelta, aparición desde el principio, o un corte ya separa las escenas
        candidates.push_back(flattest(first, last));
    }
    // Disolvencias (y cambios entre dos colores lisos): a mitad de la mezcla.
    for (const Transition& t : merged)
        if (t.fromFlat == t.toFlat && !cutBetween(t.start, t.end))
            candidates.push_back(times[t.cut]);

    // Duración mínima de las escenas, respecto del principio y de los cortes ya encontrados.
    std::sort(candidates.begin(), candidates.end());
    std::vector<TimeUs> result = cuts;
    for (const TimeUs c : candidates) {
        const bool near = c - times[0] < o.minScene
            || std::any_of(result.begin(), result.end(), [&](TimeUs k) { return std::llabs(c - k) < o.minScene; });
        if (!near)
            result.push_back(c);
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool detectScenes(const std::string& path, TimeUs from, TimeUs to, const SceneOptions& options, std::vector<TimeUs>& cuts,
                  std::string* error, const ProgressFn& progress, const CancelFn& cancel)
{
    cuts.clear();
    media::VideoDecoder decoder;
    if (!decoder.open(path, {}, error))
        return false;
    if (to <= from)
        to = decoder.duration();
    if (from > 0)
        decoder.seek(from);
    std::vector<TimeUs> times;
    std::vector<float> distances;
    std::vector<SceneGrid> grids;
    SceneFeatures previous;
    bool first = true;
    int frames = 0;
    while (auto frame = decoder.decodeNext()) {
        if (frame->pts >= to)
            break;
        if (frame->pts < from)
            continue;
        const SceneFeatures features = sceneFeatures(*frame);
        times.push_back(frame->pts);
        distances.push_back(first ? 0.0f : sceneDistance(previous, features));
        if (options.gradual)
            grids.push_back(sceneGrid(features));
        previous = features;
        first = false;
        if (++frames % 15 == 0) {
            if (cancel && cancel()) {
                if (error)
                    *error = "cancelado";
                return false;
            }
            if (progress && to > from)
                progress(std::clamp(double(frame->pts - from) / double(to - from), 0.0, 1.0));
        }
    }
    if (times.empty()) {
        if (error)
            *error = "El archivo no tiene video.";
        return false;
    }
    cuts = findCuts(times, distances, options);
    if (options.gradual)
        cuts = withGradualCuts(times, grids, cuts, options);
    return true;
}

} // namespace movix::ai
