#include "ai/Scenes.h"

#include "media/VideoDecoder.h"

#include <algorithm>
#include <cmath>

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
    return true;
}

} // namespace movix::ai
