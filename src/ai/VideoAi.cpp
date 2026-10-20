#include "ai/VideoAi.h"

#include "ai/Onnx.h"
#include "core/Edits.h"
#include "media/Encoder.h"
#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <cmath>

namespace movix::ai {

namespace {

onnx::Model& upscaler()
{
    static onnx::Model& m = *new onnx::Model; // sin destruir al salir (ver onnx::env)
    return m;
}

onnx::Model& interpolator()
{
    static onnx::Model& m = *new onnx::Model; // sin destruir al salir (ver onnx::env)
    return m;
}

constexpr int kTile = 192;    // tamaño de tesela de entrada (fijo: la GPU reutiliza sus sombreadores)
constexpr int kOverlap = 16;  // solapamiento por cada lado

uint8_t toByte(float v)
{
    return uint8_t(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

// Codificador H.264 de alta calidad para el resultado.
bool openEncoder(media::Encoder& encoder, const std::string& output, int w, int h, core::Rational rate, std::string* error)
{
    media::EncoderSettings settings;
    settings.path = output;
    settings.hasAudio = false;
    settings.video.codec = "libx264";
    settings.video.width = w;
    settings.video.height = h;
    settings.video.fpsNum = int(rate.num);
    settings.video.fpsDen = int(rate.den);
    settings.video.quality = 14;
    settings.video.preset = "medium";
    return encoder.open(settings, error);
}

struct Source {
    media::VideoDecoder decoder;
    TimeUs to = 0;
    core::Rational rate{30, 1};
};

bool openSource(Source& s, const std::string& mediaPath, TimeUs from, TimeUs to, std::string* error)
{
    if (!s.decoder.open(mediaPath, {}, error))
        return false;
    s.to = to > from ? to : s.decoder.duration();
    if (from > 0)
        s.decoder.seek(from);
    s.rate = core::edit::guessFrameRate(s.decoder.frameRate() > 0 ? s.decoder.frameRate() : 30.0);
    return true;
}

} // namespace

bool upscaleFrame(const std::string& modelPath, const uint8_t* rgba, int width, int height, int scale, std::vector<uint8_t>& out,
                  std::string* error)
{
    onnx::Model& m = upscaler();
    std::string why;
    if (!m.load(modelPath, true, &why)) {
        if (error)
            *error = "No se pudo cargar el modelo de escalado: " + why;
        return false;
    }
    constexpr int kModelScale = 4;
    const int outW = width * kModelScale, outH = height * kModelScale;
    std::vector<float> big(size_t(outW) * size_t(outH) * 3);
    std::vector<float> tile(size_t(3) * kTile * kTile);
    const int step = kTile - 2 * kOverlap;
    try {
        for (int ty = 0; ty < height; ty += step) {
            for (int tx = 0; tx < width; tx += step) {
                // Tesela con su margen; fuera de la imagen se repite el borde (tamaño siempre fijo).
                const int x0 = tx - kOverlap, y0 = ty - kOverlap;
                for (int y = 0; y < kTile; ++y)
                    for (int x = 0; x < kTile; ++x) {
                        const int sx = std::clamp(x0 + x, 0, width - 1), sy = std::clamp(y0 + y, 0, height - 1);
                        const uint8_t* px = rgba + (size_t(sy) * size_t(width) + size_t(sx)) * 4;
                        for (int c = 0; c < 3; ++c)
                            tile[size_t(c) * kTile * kTile + size_t(y) * kTile + size_t(x)] = px[c] / 255.0f;
                    }
                std::vector<Ort::Value> inputs;
                inputs.push_back(onnx::tensor(tile, {1, 3, kTile, kTile}));
                auto outputs = m.run({m.inputs().at(0)}, inputs, {m.outputs().at(0)});
                const float* o = outputs[0].GetTensorData<float>();
                const int ot = kTile * kModelScale;
                // Solo la parte central de la tesela (sin el margen) va al resultado.
                for (int y = kOverlap * kModelScale; y < (kOverlap + step) * kModelScale; ++y) {
                    const int dy = (y0 * kModelScale) + y;
                    if (dy < 0 || dy >= outH)
                        continue;
                    for (int x = kOverlap * kModelScale; x < (kOverlap + step) * kModelScale; ++x) {
                        const int dx = (x0 * kModelScale) + x;
                        if (dx < 0 || dx >= outW)
                            continue;
                        for (int c = 0; c < 3; ++c)
                            big[(size_t(dy) * size_t(outW) + size_t(dx)) * 3 + size_t(c)] =
                                o[size_t(c) * size_t(ot) * size_t(ot) + size_t(y) * size_t(ot) + size_t(x)];
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("El escalado falló: ") + e.what();
        return false;
    }
    // ×4 directo, o ×2 promediando bloques de 2 × 2.
    const int factor = kModelScale / std::clamp(scale, 2, 4);
    const int w = outW / factor, h = outH / factor;
    out.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                float sum = 0;
                for (int dy = 0; dy < factor; ++dy)
                    for (int dx = 0; dx < factor; ++dx)
                        sum += big[(size_t(y * factor + dy) * size_t(outW) + size_t(x * factor + dx)) * 3 + size_t(c)];
                out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = toByte(sum / float(factor * factor));
            }
            out[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] = 255;
        }
    return true;
}

bool upscaleVideo(const std::string& modelPath, const std::string& mediaPath, TimeUs from, TimeUs to, int scale,
                  const std::string& output, VideoResult& result, std::string* error, const ProgressFn& progress,
                  const CancelFn& cancel)
{
    result = VideoResult{};
    Source source;
    if (!openSource(source, mediaPath, from, to, error))
        return false;
    media::Encoder encoder;
    bool open = false;
    std::vector<uint8_t> rgba, big;
    while (auto frame = source.decoder.decodeNext()) {
        if (frame->pts >= source.to)
            break;
        if (cancel && cancel()) {
            encoder.abort();
            if (error)
                *error = "cancelado";
            return false;
        }
        const int w = frame->width, h = frame->height;
        if (!media::frameToRgba(*frame, w, h, rgba) || !upscaleFrame(modelPath, rgba.data(), w, h, scale, big, error)) {
            encoder.abort();
            return false;
        }
        const int ow = (w * std::clamp(scale, 2, 4)) & ~1, oh = (h * std::clamp(scale, 2, 4)) & ~1;
        if (!open) {
            if (!openEncoder(encoder, output, ow, oh, source.rate, error))
                return false;
            open = true;
            result.width = ow;
            result.height = oh;
        }
        if (!encoder.writeVideo(big.data(), w * std::clamp(scale, 2, 4) * 4)) {
            if (error)
                *error = encoder.lastError();
            encoder.abort();
            return false;
        }
        ++result.frames;
        if (progress && source.to > from)
            progress(std::clamp(double(frame->pts - from) / double(source.to - from), 0.0, 1.0));
    }
    if (!open) {
        if (error)
            *error = "El tramo no tiene fotogramas de video.";
        return false;
    }
    if (!encoder.finish(error))
        return false;
    result.output = output;
    result.fps = source.rate.value();
    return true;
}

bool interpolateFrame(const std::string& modelPath, const uint8_t* a, const uint8_t* b, int width, int height, double t,
                      std::vector<uint8_t>& out, std::string* error)
{
    onnx::Model& m = interpolator();
    std::string why;
    if (!m.load(modelPath, true, &why)) {
        if (error)
            *error = "No se pudo cargar el modelo de interpolación: " + why;
        return false;
    }
    const size_t plane = size_t(width) * size_t(height);
    std::vector<float> input(plane * 6);
    for (size_t i = 0; i < plane; ++i)
        for (size_t c = 0; c < 3; ++c) {
            input[c * plane + i] = a[i * 4 + c] / 255.0f;
            input[(3 + c) * plane + i] = b[i * 4 + c] / 255.0f;
        }
    std::vector<float> timestep{float(std::clamp(t, 0.01, 0.99))};
    try {
        std::vector<Ort::Value> inputs;
        std::vector<std::string> names;
        for (const std::string& name : m.inputs()) {
            names.push_back(name);
            if (name == "timestep") // escalar de dimensión 0
                inputs.push_back(Ort::Value::CreateTensor<float>(onnx::cpuMemory(), timestep.data(), 1, nullptr, 0));
            else
                inputs.push_back(onnx::tensor(input, {1, 6, height, width}));
        }
        auto outputs = m.run(names, inputs, {m.outputs().at(0)});
        const float* o = outputs[0].GetTensorData<float>();
        out.resize(plane * 4);
        for (size_t i = 0; i < plane; ++i) {
            for (size_t c = 0; c < 3; ++c)
                out[i * 4 + c] = toByte(o[c * plane + i]);
            out[i * 4 + 3] = 255;
        }
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("La interpolación falló: ") + e.what();
        return false;
    }
}

bool interpolateVideo(const std::string& modelPath, const std::string& mediaPath, TimeUs from, TimeUs to, int factor,
                      const std::string& output, VideoResult& result, std::string* error, const ProgressFn& progress,
                      const CancelFn& cancel)
{
    result = VideoResult{};
    factor = std::clamp(factor, 2, 8);
    Source source;
    if (!openSource(source, mediaPath, from, to, error))
        return false;
    media::Encoder encoder;
    bool open = false;
    std::vector<uint8_t> previous, current, middle;
    int w = 0, h = 0;
    auto write = [&](const std::vector<uint8_t>& rgba) {
        if (!encoder.writeVideo(rgba.data(), w * 4)) {
            if (error)
                *error = encoder.lastError();
            return false;
        }
        ++result.frames;
        return true;
    };
    while (auto frame = source.decoder.decodeNext()) {
        if (frame->pts >= source.to)
            break;
        if (cancel && cancel()) {
            encoder.abort();
            if (error)
                *error = "cancelado";
            return false;
        }
        if (!open) {
            w = frame->width & ~1;
            h = frame->height & ~1;
            core::Rational rate = source.rate;
            rate.num *= factor;
            if (!openEncoder(encoder, output, w, h, rate, error))
                return false;
            open = true;
            result.width = w;
            result.height = h;
            result.fps = rate.value();
        }
        if (!media::frameToRgba(*frame, w, h, current)) {
            encoder.abort();
            return false;
        }
        if (!previous.empty()) {
            // El original anterior y sus intermedios hasta el actual.
            if (!write(previous)) {
                encoder.abort();
                return false;
            }
            for (int k = 1; k < factor; ++k) {
                if (!interpolateFrame(modelPath, previous.data(), current.data(), w, h, double(k) / factor, middle, error)
                    || !write(middle)) {
                    encoder.abort();
                    return false;
                }
            }
        }
        std::swap(previous, current);
        if (progress && source.to > from)
            progress(std::clamp(double(frame->pts - from) / double(source.to - from), 0.0, 1.0));
    }
    if (!open) {
        if (error)
            *error = "El tramo no tiene fotogramas de video.";
        return false;
    }
    // El último original se mantiene lo que dura (factor fotogramas), así la duración no cambia.
    for (int k = 0; k < factor; ++k)
        if (!write(previous)) {
            encoder.abort();
            return false;
        }
    if (!encoder.finish(error))
        return false;
    result.output = output;
    return true;
}

} // namespace movix::ai
