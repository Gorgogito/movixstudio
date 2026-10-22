#include "ai/AudioSync.h"

#include "media/AudioDecoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace movix::ai {

namespace {

using Cf = std::complex<float>;

// FFT compleja radix-2 en el sitio (n potencia de 2), con las rotaciones calculadas en doble
// precisión: las grabaciones largas necesitan millones de puntos (la de libavutil se vuelve
// cuadrática por encima de 2^17).
void fft(std::vector<Cf>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    std::vector<Cf> twiddle(n / 2);
    const double sign = inverse ? 1.0 : -1.0;
    for (size_t k = 0; k < n / 2; ++k) {
        const double angle = sign * 2.0 * std::numbers::pi * double(k) / double(n);
        twiddle[k] = Cf(float(std::cos(angle)), float(std::sin(angle)));
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < len / 2; ++j) {
                const Cf u = a[i + j];
                const Cf v = a[i + j + len / 2] * twiddle[j * step];
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
            }
        }
    }
}

bool hasSignal(const std::vector<float>& x)
{
    double energy = 0;
    for (float v : x)
        energy += double(v) * v;
    return !x.empty() && energy / double(x.size()) > 1e-8; // > −80 dBFS de media
}

// Desfase (en muestras, con fracción) en que `y` empieza dentro de `x`, en [−maxLag, maxLag].
double gccPhat(const std::vector<float>& x, const std::vector<float>& y, int maxLag, double& confidence)
{
    confidence = 0;
    size_t n = 1;
    while (n < x.size() + y.size())
        n <<= 1;
    // Las dos señales en una sola FFT: x en la parte real, y en la imaginaria.
    std::vector<Cf> z(n, Cf(0.0f, 0.0f));
    double mx = 0, my = 0;
    for (float v : x)
        mx += v;
    for (float v : y)
        my += v;
    mx /= double(std::max<size_t>(1, x.size()));
    my /= double(std::max<size_t>(1, y.size()));
    for (size_t i = 0; i < x.size(); ++i)
        z[i].real(float(x[i] - mx));
    for (size_t i = 0; i < y.size(); ++i)
        z[i].imag(float(y[i] - my));
    fft(z, false);

    // X·conj(Y) con ponderación de fase: solo cuenta la fase de cada frecuencia.
    using C = std::complex<double>;
    std::vector<Cf> cross(n);
    for (size_t k = 0; k < n; ++k) {
        const C zk(z[k]);
        const C zn = std::conj(C(z[(n - k) % n]));
        const C X = 0.5 * (zk + zn);
        const C Y = C(0, -0.5) * (zk - zn);
        C c = X * std::conj(Y);
        const double mag = std::abs(c);
        cross[k] = mag > 1e-20 ? Cf(c / mag) : Cf(0.0f, 0.0f);
    }
    cross[0] = Cf(0.0f, 0.0f); // sin continua
    fft(cross, true);

    auto at = [&](long long lag) { return double(cross[size_t((lag % (long long)n + (long long)n) % (long long)n)].real()); };
    // Solo los desfases en que las grabaciones se solapan (y sin dar la vuelta a la FFT).
    const long long minLag = -std::min<long long>(maxLag, (long long)y.size() - 2);
    const long long topLag = std::min<long long>(maxLag, (long long)x.size() - 2);
    if (topLag <= minLag)
        return 0;
    long long best = 0;
    double bestValue = -1e300;
    for (long long lag = minLag; lag <= topLag; ++lag)
        if (at(lag) > bestValue) {
            bestValue = at(lag);
            best = lag;
        }
    // Segundo pico, lejos del primero (más de 50 ms).
    const long long guard = kSyncSampleRate / 20;
    double second = 1e-12;
    for (long long lag = minLag; lag <= topLag; ++lag)
        if (std::llabs(lag - best) > guard)
            second = std::max(second, at(lag));
    confidence = bestValue / second;

    // Interpolación parabólica alrededor del pico.
    const double a = at(best - 1), b = at(best), c = at(best + 1);
    const double denom = a - 2 * b + c;
    const double frac = std::abs(denom) > 1e-20 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
    return double(best) + frac;
}

} // namespace

SyncResult syncByAudio(const std::vector<std::vector<float>>& pcm, int sampleRate, TimeUs maxOffset)
{
    SyncResult r;
    r.offsets.assign(pcm.size(), 0);
    r.confidence.assign(pcm.size(), 0.0);
    r.synced.assign(pcm.size(), false);
    size_t ref = pcm.size();
    for (size_t i = 0; i < pcm.size() && ref == pcm.size(); ++i)
        if (hasSignal(pcm[i]))
            ref = i;
    if (ref == pcm.size())
        return r;
    r.synced[ref] = true;
    const int maxLag = int(std::min<int64_t>(int64_t(maxOffset) * sampleRate / kUsPerSecond, INT32_MAX / 2));
    for (size_t i = 0; i < pcm.size(); ++i) {
        if (i == ref || !hasSignal(pcm[i]))
            continue;
        double confidence = 0;
        const double lag = gccPhat(pcm[ref], pcm[i], maxLag, confidence);
        r.confidence[i] = confidence;
        if (confidence < 2.0)
            continue; // sin un pico claro no se arriesga un desfase
        r.offsets[i] = TimeUs(std::llround(lag * double(kUsPerSecond) / double(sampleRate)));
        r.synced[i] = true;
    }
    return r;
}

SyncResult syncFilesByAudio(const std::vector<std::string>& paths, double maxSeconds, const ProgressFn& progress,
                            const CancelFn& cancel)
{
    std::vector<std::vector<float>> pcm(paths.size());
    const size_t maxFrames = size_t(maxSeconds * kSyncSampleRate);
    for (size_t i = 0; i < paths.size(); ++i) {
        media::AudioDecoder dec;
        if (!dec.open(paths[i], kSyncSampleRate, 1))
            continue;
        media::AudioChunk chunk;
        bool first = true;
        while (pcm[i].size() < maxFrames && dec.read(chunk)) {
            if (cancel && cancel())
                return {};
            if (first && chunk.pts > 0) // el audio empieza después del inicio del medio
                pcm[i].assign(size_t(chunk.pts * kSyncSampleRate / kUsPerSecond), 0.0f);
            first = false;
            pcm[i].insert(pcm[i].end(), chunk.samples.begin(), chunk.samples.end());
        }
        if (pcm[i].size() > maxFrames)
            pcm[i].resize(maxFrames);
        if (progress)
            progress(0.7 * double(i + 1) / double(paths.size()));
    }
    SyncResult r = syncByAudio(pcm, kSyncSampleRate, TimeUs(maxSeconds * kUsPerSecond));
    if (progress)
        progress(1.0);
    return r;
}

} // namespace movix::ai
