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
// `rate`: muestras por segundo (para no tomar por segundo pico el entorno del primero).
double gccPhat(const std::vector<float>& x, const std::vector<float>& y, int maxLag, int rate, double& confidence)
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
    const long long guard = std::max(1, rate / 20);
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
    r.speed.assign(pcm.size(), 1.0);
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
        const double lag = gccPhat(pcm[ref], pcm[i], maxLag, sampleRate, confidence);
        r.confidence[i] = confidence;
        if (confidence < 2.0)
            continue; // sin un pico claro no se arriesga un desfase
        r.offsets[i] = TimeUs(std::llround(lag * double(kUsPerSecond) / double(sampleRate)));
        r.synced[i] = true;
    }
    return r;
}

void OnsetEnvelope::push(const float* samples, size_t count)
{
    if (!m_ready) {
        // Pasabandas de RBJ (Q = 1) centrados en la voz grave, media, aguda y las sibilantes.
        const double centers[4] = {200, 600, 1500, 3000};
        for (int b = 0; b < 4; ++b) {
            const double w = 2 * std::numbers::pi * centers[b] / kSyncSampleRate, alpha = std::sin(w) / 2.0;
            const double a0 = 1 + alpha;
            m_bands[b].b0 = alpha / a0;
            m_bands[b].b2 = -alpha / a0;
            m_bands[b].a1 = -2 * std::cos(w) / a0;
            m_bands[b].a2 = (1 - alpha) / a0;
        }
        m_ready = true;
    }
    constexpr int kHop = kSyncSampleRate / kEnvelopeRate;
    for (size_t i = 0; i < count; ++i) {
        const double x = samples[i];
        for (Band& b : m_bands) {
            const double y = b.b0 * x + b.b2 * b.x2 - b.a1 * b.y1 - b.a2 * b.y2;
            b.x2 = b.x1;
            b.x1 = x;
            b.y2 = b.y1;
            b.y1 = y;
            b.energy += y * y;
        }
        if (++m_count < kHop)
            continue;
        // Solo cuentan los aumentos de energía (en dB): no dependen del nivel ni del micrófono.
        double onset = 0;
        for (Band& b : m_bands) {
            const double level = std::log10(b.energy / kHop + 1e-9);
            onset += std::max(0.0, level - b.previous);
            b.previous = level;
            b.energy = 0;
        }
        m_values.push_back(m_values.empty() ? 0.0f : float(onset));
        m_count = 0;
    }
}

namespace {

double sumOfSquares(const std::vector<float>& x)
{
    double e = 0;
    for (float v : x)
        e += double(v) * v;
    return e;
}

} // namespace

SyncResult syncSources(const std::vector<SyncSource>& sources)
{
    SyncResult r;
    const size_t n = sources.size();
    r.offsets.assign(n, 0);
    r.confidence.assign(n, 0.0);
    r.synced.assign(n, false);
    r.speed.assign(n, 1.0);
    auto audible = [&](size_t i) { return sources[i].read && sumOfSquares(sources[i].envelope) > 1e-6; };
    size_t ref = n;
    for (size_t i = 0; i < n && ref == n; ++i)
        if (audible(i))
            ref = i;
    if (ref == n)
        return r;
    r.synced[ref] = true;
    const SyncSource& reference = sources[ref];
    for (size_t i = 0; i < n; ++i) {
        if (i == ref || !audible(i))
            continue;
        const SyncSource& other = sources[i];
        // 1) Grueso: cualquier desfase, con la envolvente (10 ms por valor).
        double coarseConfidence = 0;
        const int maxLag = int(reference.envelope.size() + other.envelope.size());
        const double coarse = gccPhat(reference.envelope, other.envelope, maxLag, kEnvelopeRate, coarseConfidence) / kEnvelopeRate;

        // 2) Fino, a 8 kHz, en hasta tres puntos del solape: cada uno da qué instante de la
        //    referencia (T) corresponde a uno de la otra grabación (τ).
        const double refSeconds = double(reference.duration) / kUsPerSecond, otherSeconds = double(other.duration) / kUsPerSecond;
        const double from = std::max(0.0, coarse), to = std::min(refSeconds, coarse + otherSeconds);
        if (to - from < 2.0)
            continue;
        const double window = std::clamp((to - from) / 4.0, 2.0, 30.0), margin = 0.5;
        std::vector<double> centers{(from + to) / 2};
        if (to - from >= 60.0)
            centers = {from + 0.15 * (to - from), (from + to) / 2, from + 0.85 * (to - from)};
        std::vector<std::pair<double, double>> points; // (τ, T)
        double best = 0;
        for (const double center : centers) {
            const double refStart = std::clamp(center - window / 2, 0.0, std::max(0.0, refSeconds - window));
            const double otherStart = refStart - coarse - margin;
            const auto a = reference.read(TimeUs(refStart * kUsPerSecond), TimeUs(window * kUsPerSecond));
            const auto b = other.read(TimeUs(otherStart * kUsPerSecond), TimeUs((window + 2 * margin) * kUsPerSecond));
            if (!hasSignal(a) || !hasSignal(b))
                continue;
            double confidence = 0;
            const double lag = gccPhat(a, b, int(2 * margin * kSyncSampleRate), kSyncSampleRate, confidence);
            const double T = refStart + lag / kSyncSampleRate;
            if (confidence < 2.0 || std::abs(T - otherStart - coarse) > 0.3)
                continue; // sin un pico claro, o lejos del desfase grueso
            points.emplace_back(otherStart, T);
            best = std::max(best, confidence);
        }
        if (points.empty())
            continue;
        // T = o + k·τ: la pendiente es la deriva (solo si los puntos están separados y es creíble).
        double k = 1.0;
        if (points.size() >= 2 && points.back().first - points.front().first >= 30.0) {
            double mt = 0, mT = 0;
            for (const auto& [t, T] : points) {
                mt += t / double(points.size());
                mT += T / double(points.size());
            }
            double num = 0, den = 0;
            for (const auto& [t, T] : points) {
                num += (t - mt) * (T - mT);
                den += (t - mt) * (t - mt);
            }
            const double slope = den > 0 ? num / den : 1.0;
            // Más de 1000 ppm no es deriva de un reloj; menos de 2 ms en todo el solape no se nota.
            if (std::abs(slope - 1.0) < 1e-3 && std::abs(slope - 1.0) * (to - from) >= 0.002)
                k = slope;
        }
        double offset = 0;
        for (const auto& [t, T] : points)
            offset += (T - k * t) / double(points.size());
        r.offsets[i] = TimeUs(std::llround(offset * kUsPerSecond));
        r.confidence[i] = best;
        r.synced[i] = true;
        r.speed[i] = 1.0 / k;
    }
    return r;
}

namespace {

// Tramo [start, start + length) del audio de un archivo (mono, 8 kHz), con ceros fuera de él.
std::vector<float> readSegment(const std::string& path, TimeUs start, TimeUs length)
{
    const size_t count = size_t(length * kSyncSampleRate / kUsPerSecond);
    std::vector<float> out(count, 0.0f);
    media::AudioDecoder dec;
    if (!dec.open(path, kSyncSampleRate, 1) || !dec.seek(std::max<TimeUs>(0, start)))
        return out;
    media::AudioChunk chunk;
    while (dec.read(chunk)) {
        const long long first = (long long)((chunk.pts - start) * kSyncSampleRate / kUsPerSecond);
        if (first >= (long long)count)
            break;
        for (size_t k = 0; k < chunk.samples.size(); ++k) {
            const long long at = first + (long long)k;
            if (at >= 0 && at < (long long)count)
                out[size_t(at)] = chunk.samples[k];
        }
    }
    return out;
}

} // namespace

SyncResult syncFilesByAudio(const std::vector<std::string>& paths, const ProgressFn& progress, const CancelFn& cancel)
{
    // Una sola decodificación por archivo: la envolvente de todo y el audio de los primeros minutos.
    const size_t maxFrames = size_t(kExactSyncSeconds * kSyncSampleRate);
    std::vector<std::vector<float>> pcm(paths.size());
    std::vector<SyncSource> sources(paths.size());
    bool longRecordings = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        media::AudioDecoder dec;
        if (!dec.open(paths[i], kSyncSampleRate, 1))
            continue;
        OnsetEnvelope envelope;
        media::AudioChunk chunk;
        size_t total = 0;
        bool first = true;
        while (dec.read(chunk)) {
            if (cancel && cancel())
                return {};
            if (first && chunk.pts > 0) { // el audio empieza después del inicio del medio
                const std::vector<float> silence(size_t(chunk.pts * kSyncSampleRate / kUsPerSecond), 0.0f);
                envelope.push(silence.data(), silence.size());
                pcm[i] = silence;
                total = silence.size();
            }
            first = false;
            envelope.push(chunk.samples.data(), chunk.samples.size());
            total += chunk.samples.size();
            if (pcm[i].size() < maxFrames)
                pcm[i].insert(pcm[i].end(), chunk.samples.begin(),
                              chunk.samples.begin() + std::ptrdiff_t(std::min(chunk.samples.size(), maxFrames - pcm[i].size())));
            if (progress && dec.duration() > 0)
                progress(0.8 * (double(i) + std::min(1.0, double(chunk.pts) / double(dec.duration()))) / double(paths.size()));
        }
        longRecordings = longRecordings || total > maxFrames;
        sources[i].envelope = envelope.values();
        sources[i].duration = TimeUs(double(total) * kUsPerSecond / kSyncSampleRate);
        sources[i].read = [path = paths[i]](TimeUs start, TimeUs length) { return readSegment(path, start, length); };
    }
    SyncResult r = longRecordings ? syncSources(sources) : syncByAudio(pcm, kSyncSampleRate, TimeUs(kExactSyncSeconds * kUsPerSecond));
    if (progress)
        progress(1.0);
    return r;
}

} // namespace movix::ai
