#include "ai/Beats.h"

#include "media/AudioDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>

namespace movix::ai {

namespace {

constexpr int kFrame = 1024; // 46 ms a 22,05 kHz
constexpr int kHop = 256;    // 11,6 ms

// FFT compleja radix-2 en el sitio (n potencia de 2).
void fft(std::vector<std::complex<float>>& a)
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
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * std::numbers::pi_v<float> / float(len);
        const std::complex<float> wl(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Envolvente de ataques: flujo espectral (aumentos de la magnitud logarítmica), sin la tendencia
// local y normalizada a desviación típica 1.
std::vector<float> onsetEnvelope(const std::vector<float>& pcm, int rate)
{
    std::vector<float> env;
    if (pcm.size() < size_t(kFrame) * 2)
        return env;
    float peak = 0;
    for (float v : pcm)
        peak = std::max(peak, std::abs(v));
    if (peak < 1e-4f)
        return env;

    std::vector<float> window(kFrame);
    for (int i = 0; i < kFrame; ++i)
        window[size_t(i)] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * float(i) / float(kFrame));
    // Bandas logarítmicas de 30 Hz a 8 kHz: el bombo (pocas frecuencias graves) pesa tanto como la
    // caja o los platos (muchas agudas), como en el oído.
    constexpr int kBands = 24;
    std::array<int, kBands + 1> edges{};
    for (int b = 0; b <= kBands; ++b) {
        const double hz = 30.0 * std::pow(8000.0 / 30.0, double(b) / kBands);
        edges[size_t(b)] = std::clamp(int(std::lround(hz * kFrame / rate)), 1, kFrame / 2);
    }
    for (int b = 1; b <= kBands; ++b) // bandas de al menos un bin
        edges[size_t(b)] = std::max(edges[size_t(b)], edges[size_t(b - 1)] + 1);
    std::vector<std::complex<float>> buf(kFrame);
    std::array<float, kBands> prev{}, cur{};
    const size_t frames = (pcm.size() - kFrame) / kHop + 1;
    env.resize(frames, 0.0f);
    for (size_t t = 0; t < frames; ++t) {
        const float* x = pcm.data() + t * kHop;
        for (int i = 0; i < kFrame; ++i)
            buf[size_t(i)] = {x[i] / peak * window[size_t(i)], 0.0f};
        fft(buf);
        float flux = 0;
        for (int b = 0; b < kBands; ++b) {
            float e = 0;
            for (int k = edges[size_t(b)]; k < edges[size_t(b + 1)] && k < kFrame / 2; ++k)
                e += std::norm(buf[size_t(k)]);
            cur[size_t(b)] = std::log1p(1000.0f * e / float(edges[size_t(b + 1)] - edges[size_t(b)]));
            if (t > 0)
                flux += std::max(0.0f, cur[size_t(b)] - prev[size_t(b)]);
        }
        env[t] = flux;
        prev = cur;
    }
    // Sin la media móvil (~0,3 s) y rectificada: quedan los picos de los ataques.
    const int half = std::max(1, int(0.15 * rate / kHop));
    std::vector<float> smooth(frames);
    std::vector<double> prefix(frames + 1, 0.0);
    for (size_t i = 0; i < frames; ++i)
        prefix[i + 1] = prefix[i] + env[i];
    for (size_t i = 0; i < frames; ++i) {
        const size_t a = i >= size_t(half) ? i - size_t(half) : 0, b = std::min(frames, i + size_t(half) + 1);
        smooth[i] = float((prefix[b] - prefix[a]) / double(b - a));
    }
    double sum2 = 0;
    for (size_t i = 0; i < frames; ++i) {
        env[i] = std::max(0.0f, env[i] - smooth[i]);
        sum2 += double(env[i]) * env[i];
    }
    const float sd = float(std::sqrt(sum2 / double(frames)));
    if (sd > 0)
        for (float& v : env)
            v /= sd;
    return env;
}

// Autocorrelación de la envolvente sin su media en [first, last), corregida por el solapamiento,
// para los retardos hasta maxLag + 1.
std::vector<double> autocorrelation(const std::vector<float>& o, size_t first, size_t last, int minLag, int maxLag, double* energy)
{
    const size_t n = last - first;
    double mean = 0;
    for (size_t i = first; i < last; ++i)
        mean += o[i];
    mean /= double(n);
    std::vector<double> centered(n);
    double e = 0;
    for (size_t i = 0; i < n; ++i) {
        centered[i] = o[first + i] - mean;
        e += centered[i] * centered[i];
    }
    if (energy)
        *energy = e / double(n);
    std::vector<double> ac(size_t(maxLag + 2), 0.0);
    for (int lag = std::max(1, minLag - 1); lag <= maxLag + 1 && size_t(lag) < n; ++lag) {
        double sum = 0;
        for (size_t i = size_t(lag); i < n; ++i)
            sum += centered[i] * centered[i - size_t(lag)];
        ac[size_t(lag)] = sum / double(n - size_t(lag));
    }
    return ac;
}

// Puntuación de cada retardo como periodo: un periodo no entero reparte la correlación entre dos
// retardos vecinos, así que cuenta con ellos; ponderada (log-gaussiana) hacia 120 BPM.
double lagScore(const std::vector<double>& ac, int lag, double fr)
{
    const double bpm = 60.0 * fr / lag;
    const double w = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0), 2));
    const double local = 0.5 * std::max(0.0, ac[size_t(lag - 1)]) + std::max(0.0, ac[size_t(lag)]) + 0.5 * std::max(0.0, ac[size_t(lag + 1)]);
    return local * w;
}

// Ajuste parabólico del máximo en `best` para un periodo fraccionario.
double refineLag(const std::vector<double>& ac, int best)
{
    const double a = ac[size_t(best - 1)], b = ac[size_t(best)], c = ac[size_t(best + 1)];
    const double den = a - 2 * b + c;
    return std::abs(den) > 1e-12 ? best + std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : double(best);
}

// Periodo local (en fotogramas de envolvente) para cada fotograma: tempo en ventanas de 8 s cada
// segundo y, entre ellas, el recorrido más probable que no cambia a saltos (Viterbi: un cambio de
// una octava cuesta tanto que solo se acepta si la música lo pide de verdad). `strength`: mediana,
// por ventanas, de la correlación en el periodo elegido respecto de la energía (cuánto ritmo hay).
std::vector<double> tempoCurve(const std::vector<float>& o, double fr, int minLag, int maxLag, double& strength)
{
    strength = 0;
    const size_t n = o.size();
    const size_t window = size_t(8.0 * fr), hop = size_t(fr);
    std::vector<size_t> centers;
    for (size_t c = window / 2; c + window / 2 <= n; c += hop)
        centers.push_back(c);
    if (centers.size() < 2)
        return {};
    const int lags = maxLag - minLag + 1;
    std::vector<std::vector<double>> acs;
    std::vector<double> energies(centers.size(), 0.0);
    std::vector<std::vector<double>> emit(centers.size(), std::vector<double>(size_t(lags)));
    for (size_t w = 0; w < centers.size(); ++w) {
        acs.push_back(autocorrelation(o, centers[w] - window / 2, centers[w] + window / 2, minLag, maxLag, &energies[w]));
        double total = 1e-12;
        for (int l = 0; l < lags; ++l)
            total += emit[w][size_t(l)] = lagScore(acs[w], minLag + l, fr);
        for (double& e : emit[w])
            e = std::log(e / total + 1e-6);
    }
    constexpr double kChange = 60.0; // coste de cambiar el periodo, por (octavas)²
    std::vector<double> cost(emit[0]);
    std::vector<std::vector<int>> from(centers.size(), std::vector<int>(size_t(lags), 0));
    for (size_t w = 1; w < centers.size(); ++w) {
        std::vector<double> next(size_t(lags), 0.0);
        for (int l = 0; l < lags; ++l) {
            double bestValue = -1e300;
            for (int k = 0; k < lags; ++k) {
                const double change = std::log2(double(minLag + l) / double(minLag + k));
                const double v = cost[size_t(k)] - kChange * change * change;
                if (v > bestValue) {
                    bestValue = v;
                    from[w][size_t(l)] = k;
                }
            }
            next[size_t(l)] = bestValue + emit[w][size_t(l)];
        }
        cost = std::move(next);
    }
    std::vector<double> path(centers.size()), ratios(centers.size());
    int l = int(std::max_element(cost.begin(), cost.end()) - cost.begin());
    for (size_t w = centers.size(); w-- > 0;) {
        path[w] = refineLag(acs[w], minLag + l);
        ratios[w] = energies[w] > 0 ? acs[w][size_t(minLag + l)] / energies[w] : 0.0;
        l = from[w][size_t(l)];
    }
    std::nth_element(ratios.begin(), ratios.begin() + std::ptrdiff_t(ratios.size() / 2), ratios.end());
    strength = ratios[ratios.size() / 2];
    // Interpolado entre los centros de las ventanas (constante antes del primero y tras el último).
    std::vector<double> curve(n);
    for (size_t t = 0; t < n; ++t) {
        if (t <= centers.front()) {
            curve[t] = path.front();
        } else if (t >= centers.back()) {
            curve[t] = path.back();
        } else {
            const size_t w = (t - centers.front()) / hop;
            const double f = double(t - centers[w]) / double(centers[w + 1] - centers[w]);
            curve[t] = path[w] + (path[w + 1] - path[w]) * f;
        }
    }
    return curve;
}

} // namespace

bool detectBeats(const std::vector<float>& pcm, int rate, BeatResult& out)
{
    out = BeatResult{};
    const std::vector<float> o = onsetEnvelope(pcm, rate);
    const double fr = double(rate) / kHop; // fotogramas de envolvente por segundo
    if (o.size() < size_t(fr * 3))
        return false; // menos de 3 s

    // Tempo global: autocorrelación en 40..220 BPM (sin la media: el ruido da ~0).
    const int minLag = int(std::floor(fr * 60.0 / 220.0)), maxLag = int(std::ceil(fr * 60.0 / 40.0));
    double energy = 0;
    const std::vector<double> ac = autocorrelation(o, 0, o.size(), minLag, maxLag, &energy);
    int best = -1;
    double bestScore = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
        if (const double s = lagScore(ac, lag, fr); s > bestScore) {
            bestScore = s;
            best = lag;
        }
    // Periodicidad clara en todo el tramo o, si el tempo cambia, en cada ventana siguiendo su tempo.
    // Sin ella (voz, ruido, silencio) no hay ritmo.
    const bool steady = best >= 0 && energy > 0 && ac[size_t(best)] / energy >= 0.15;
    double rhythm = 0;
    auto curve = tempoCurve(o, fr, minLag, maxLag, rhythm);
    const bool varying = !curve.empty() && rhythm >= 0.15;
    if (!steady && !varying)
        return false;
    const double globalLag = steady ? refineLag(ac, best) : curve[curve.size() / 2];
    out.bpm = 60.0 * fr / globalLag;

    // Periodo de cada instante: el global salvo que el tempo cambie de verdad (más de un 4 %).
    std::vector<double> period(o.size(), globalLag);
    if (varying) {
        const bool varies = !steady || std::any_of(curve.begin(), curve.end(), [&](double l) { return std::abs(l / globalLag - 1.0) > 0.04; });
        if (varies) {
            period = std::move(curve);
            out.variableTempo = true;
            std::vector<double> sorted = period;
            std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(sorted.size() / 2), sorted.end());
            out.bpm = 60.0 * fr / sorted[sorted.size() / 2];
        }
    }

    // Pulsos: cada uno suma su ataque y premia que el anterior esté a un periodo de distancia.
    const double tightness = 400.0;
    const size_t n = o.size();
    std::vector<double> score(n);
    std::vector<int64_t> back(n, -1);
    for (size_t t = 0; t < n; ++t) {
        double bestPrev = 0;
        int64_t arg = -1;
        const double lag = period[t];
        const int64_t lo = int64_t(t) - int64_t(std::lround(2 * lag)), hi = int64_t(t) - int64_t(std::lround(lag / 2));
        for (int64_t tau = std::max<int64_t>(0, lo); tau <= hi; ++tau) {
            const double dev = std::log(double(int64_t(t) - tau) / lag);
            const double s = score[size_t(tau)] - tightness * dev * dev;
            if (arg < 0 || s > bestPrev) {
                bestPrev = s;
                arg = tau;
            }
        }
        score[t] = o[t] + (arg >= 0 ? std::max(0.0, bestPrev) : 0.0);
        back[t] = arg >= 0 && bestPrev > 0 ? arg : -1;
    }
    // El último pulso: el mejor del último periodo.
    size_t end = n - 1;
    for (size_t t = n - std::min(n, size_t(std::lround(period.back()))); t < n; ++t)
        if (score[t] > score[end] || end == n - 1)
            end = t;
    std::vector<int64_t> frames;
    for (int64_t t = int64_t(end); t >= 0; t = back[size_t(t)])
        frames.push_back(t);
    std::reverse(frames.begin(), frames.end());

    // El seguimiento extrapola pulsos en los silencios del principio y del final: se quitan los de
    // los extremos cuyo ataque es muy débil frente al típico.
    auto strength = [&](int64_t t) {
        float m = 0;
        for (int64_t k = std::max<int64_t>(0, t - 2); k <= std::min<int64_t>(int64_t(n) - 1, t + 2); ++k)
            m = std::max(m, o[size_t(k)]);
        return m;
    };
    if (!frames.empty()) {
        std::vector<float> s;
        for (int64_t t : frames)
            s.push_back(strength(t));
        std::nth_element(s.begin(), s.begin() + std::ptrdiff_t(s.size() / 2), s.end());
        const float limit = 0.3f * s[s.size() / 2];
        while (!frames.empty() && strength(frames.front()) < limit)
            frames.erase(frames.begin());
        while (!frames.empty() && strength(frames.back()) < limit)
            frames.pop_back();
    }

    // Instante: el flujo sube cuando el ataque llega a la zona central de la ventana (medido con
    // pistas sintéticas: ≈ 0,45 ventanas tras su comienzo).
    for (int64_t t : frames) {
        const double seconds = (double(t) * kHop + kFrame * 0.45) / rate;
        out.beats.push_back(fromSeconds(seconds));
    }
    return out.beats.size() >= 2;
}

bool detectBeats(const std::string& path, TimeUs from, TimeUs to, BeatResult& out, std::string* error,
                 const ProgressFn& progress, const CancelFn& cancel)
{
    media::AudioDecoder decoder;
    if (!decoder.open(path, kBeatSampleRate, 1, error))
        return false;
    if (to <= from)
        to = decoder.duration();
    if (from > 0)
        decoder.seek(from);
    std::vector<float> pcm;
    pcm.reserve(size_t(toSeconds(to - from) * kBeatSampleRate) + kBeatSampleRate);
    const size_t wanted = size_t(toSeconds(to - from) * kBeatSampleRate);
    media::AudioChunk chunk;
    int blocks = 0;
    while (pcm.size() < wanted && decoder.read(chunk)) {
        pcm.insert(pcm.end(), chunk.samples.begin(), chunk.samples.begin() + chunk.frames);
        if (++blocks % 64 == 0) {
            if (cancel && cancel()) {
                if (error)
                    *error = "cancelado";
                return false;
            }
            if (progress)
                progress(0.8 * double(pcm.size()) / double(std::max<size_t>(1, wanted)));
        }
    }
    pcm.resize(std::min(pcm.size(), wanted));
    if (!detectBeats(pcm, kBeatSampleRate, out)) {
        if (error)
            *error = "No se encontró un ritmo claro en el audio.";
        return false;
    }
    for (TimeUs& b : out.beats)
        b += from;
    if (progress)
        progress(1.0);
    return true;
}

} // namespace movix::ai
