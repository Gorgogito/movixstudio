#include "ai/Separation.h"

#include "ai/Onnx.h"
#include "media/AudioDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace movix::ai {

namespace {

struct DemucsModel {
    std::string path;
    std::unique_ptr<Ort::Session> session;
    std::string input, output;
    int64_t segment = 343980; // 7,8 s a 44,1 kHz (tamaño fijo de la entrada del grafo)
};

DemucsModel& demucs()
{
    static DemucsModel& m = *new DemucsModel; // sin destruir al salir (ver onnx::env)
    return m;
}

bool load(const std::string& path, std::string* error)
{
    DemucsModel& m = demucs();
    if (m.session && m.path == path)
        return true;
    try {
        m.session = onnx::openSession(path); // en CPU: en la GPU crear la sesión cuesta más de lo que ahorra
        m.input = onnx::inputNames(*m.session).at(0);
        m.output = onnx::outputNames(*m.session).at(0);
        const auto shape = m.session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 3 && shape[2] > 0)
            m.segment = shape[2];
        m.path = path;
        return true;
    } catch (const std::exception& e) {
        m.session.reset();
        if (error)
            *error = std::string("No se pudo cargar el modelo de separación: ") + e.what();
        return false;
    }
}

} // namespace

bool separate(const std::string& modelPath, const std::vector<float>& stereo, Stems& out, std::string* error,
              const ProgressFn& progress, const CancelFn& cancel)
{
    out = Stems{};
    if (!load(modelPath, error))
        return false;
    DemucsModel& m = demucs();
    const int64_t length = int64_t(stereo.size() / 2);
    if (length == 0) {
        if (error)
            *error = "No hay audio que separar.";
        return false;
    }

    // Normalización de toda la pista con la media y la desviación del canal medio (como Demucs).
    double mean = 0, var = 0;
    for (int64_t i = 0; i < length; ++i)
        mean += 0.5 * (stereo[size_t(2 * i)] + stereo[size_t(2 * i + 1)]);
    mean /= double(length);
    for (int64_t i = 0; i < length; ++i) {
        const double v = 0.5 * (stereo[size_t(2 * i)] + stereo[size_t(2 * i + 1)]) - mean;
        var += v * v;
    }
    const double stdev = std::max(1e-8, std::sqrt(var / double(length)));

    const int64_t segment = m.segment;
    const int64_t stride = segment * 3 / 4; // 25 % de solapamiento
    // Peso triangular de cada ventana (sube y baja linealmente).
    std::vector<float> weight(static_cast<size_t>(segment));
    for (int64_t i = 0; i < segment; ++i)
        weight[size_t(i)] = float(std::min(i + 1, segment - i));

    std::vector<float> vocals(size_t(length) * 2, 0.0f), rest(size_t(length) * 2, 0.0f);
    std::vector<float> total(static_cast<size_t>(length), 0.0f);
    std::vector<float> chunk(static_cast<size_t>(segment) * 2);
    const int64_t chunks = length <= segment ? 1 : 1 + (length - segment + stride - 1) / stride;
    try {
        for (int64_t c = 0; c < chunks; ++c) {
            if (cancel && cancel()) {
                if (error)
                    *error = "cancelado";
                return false;
            }
            const int64_t start = c * stride;
            const int64_t valid = std::min(segment, length - start);
            // Entrada plana [1, 2, segment]: canal izquierdo y luego derecho; el final, con ceros.
            std::fill(chunk.begin(), chunk.end(), 0.0f);
            for (int64_t i = 0; i < valid; ++i) {
                chunk[size_t(i)] = float((stereo[size_t(2 * (start + i))] - mean) / stdev);
                chunk[size_t(segment + i)] = float((stereo[size_t(2 * (start + i) + 1)] - mean) / stdev);
            }
            std::vector<Ort::Value> inputs;
            inputs.push_back(onnx::tensor(chunk, {1, 2, segment}));
            const auto t0 = std::chrono::steady_clock::now();
            auto outputs = onnx::run(*m.session, {m.input}, inputs, {m.output});
            if (std::getenv("MOVIX_AI_VERBOSE"))
                std::fprintf(stderr, "demucs: ventana %lld en %.0f ms\n", (long long)c,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            // Salida [1, 4 fuentes, 2 canales, segment]: batería, bajo, resto, voz.
            const float* src = outputs[0].GetTensorData<float>();
            for (int64_t i = 0; i < valid; ++i) {
                const float w = weight[size_t(i)];
                total[size_t(start + i)] += w;
                for (int64_t ch = 0; ch < 2; ++ch) {
                    auto at = [&](int64_t source) { return src[(source * 2 + ch) * segment + i]; };
                    vocals[size_t(2 * (start + i) + ch)] += w * at(3);
                    rest[size_t(2 * (start + i) + ch)] += w * (at(0) + at(1) + at(2));
                }
            }
            if (progress)
                progress(double(c + 1) / double(chunks));
        }
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("La separación falló: ") + e.what();
        return false;
    }
    for (int64_t i = 0; i < length; ++i) {
        const float t = std::max(total[size_t(i)], 1e-8f);
        for (int64_t ch = 0; ch < 2; ++ch) {
            const size_t k = size_t(2 * i + ch);
            // Deshacer la normalización: la media va con el acompañamiento.
            vocals[k] = float(vocals[k] / t * stdev);
            rest[k] = float(rest[k] / t * stdev + mean);
        }
    }
    out.vocals = std::move(vocals);
    out.accompaniment = std::move(rest);
    return true;
}

bool separateFile(const std::string& modelPath, const std::string& mediaPath, TimeUs from, TimeUs to, Stems& out,
                  std::string* error, const ProgressFn& progress, const CancelFn& cancel)
{
    media::AudioDecoder decoder;
    if (!decoder.open(mediaPath, kSeparationSampleRate, 2, error))
        return false;
    if (to <= from)
        to = decoder.duration();
    if (from > 0)
        decoder.seek(from);
    const size_t wanted = size_t(toSeconds(to - from) * kSeparationSampleRate) * 2;
    std::vector<float> stereo;
    stereo.reserve(wanted);
    media::AudioChunk chunk;
    while (stereo.size() < wanted && decoder.read(chunk))
        stereo.insert(stereo.end(), chunk.samples.begin(), chunk.samples.end());
    stereo.resize(std::min(stereo.size(), wanted));
    if (progress)
        progress(0.05);
    return separate(modelPath, stereo, out, error,
                    [&](double p) {
                        if (progress)
                            progress(0.05 + 0.95 * p);
                    },
                    cancel);
}

} // namespace movix::ai


