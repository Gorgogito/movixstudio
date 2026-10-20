#include "ai/Vision.h"

#include "ai/Onnx.h"
#include "core/Edits.h"
#include "media/Encoder.h"
#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace movix::ai {

namespace {

onnx::Model& matteModel()
{
    static onnx::Model& model = *new onnx::Model; // sin destruir al salir (ver onnx::env)
    return model;
}

bool loadMatte(const std::string& path, std::string* error)
{
    std::string why;
    if (matteModel().load(path, true, &why))
        return true;
    if (error)
        *error = "No se pudo cargar el modelo de recorte: " + why;
    return false;
}

int roundTo32(double v)
{
    return std::max(32, int(std::lround(v / 32.0)) * 32);
}

} // namespace

void resizePlane(const float* src, int sw, int sh, std::vector<uint8_t>& dst, int dw, int dh)
{
    dst.resize(size_t(dw) * size_t(dh));
    for (int y = 0; y < dh; ++y) {
        const double fy = std::clamp((y + 0.5) * sh / dh - 0.5, 0.0, double(sh - 1));
        const int y0 = int(fy), y1 = std::min(y0 + 1, sh - 1);
        const float wy = float(fy - y0);
        for (int x = 0; x < dw; ++x) {
            const double fx = std::clamp((x + 0.5) * sw / dw - 0.5, 0.0, double(sw - 1));
            const int x0 = int(fx), x1 = std::min(x0 + 1, sw - 1);
            const float wx = float(fx - x0);
            const float top = src[y0 * sw + x0] * (1 - wx) + src[y0 * sw + x1] * wx;
            const float bottom = src[y1 * sw + x0] * (1 - wx) + src[y1 * sw + x1] * wx;
            dst[size_t(y) * size_t(dw) + size_t(x)] = uint8_t(std::clamp(std::lround((top * (1 - wy) + bottom * wy) * 255.0f), 0L, 255L));
        }
    }
}

bool matteFrame(const std::string& modelPath, MatteModel model, const uint8_t* rgba, int width, int height,
                std::vector<uint8_t>& alpha, std::string* error)
{
    if (!loadMatte(modelPath, error))
        return false;
    onnx::Model& s = matteModel();

    // Tamaño y normalización de cada modelo (ver su preprocessor_config.json).
    int w = 1024, h = 1024;
    float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
    if (model == MatteModel::Portrait) {
        const double scale = 512.0 / std::min(width, height);
        w = roundTo32(width * scale);
        h = roundTo32(height * scale);
        for (int c = 0; c < 3; ++c)
            mean[c] = stdev[c] = 0.5f;
    }
    // Remuestreo bilineal del RGBA de entrada al tamaño del modelo, en planos normalizados.
    std::vector<float> input(size_t(3) * size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) * height / h - 0.5, 0.0, double(height - 1));
        const int y0 = int(fy), y1 = std::min(y0 + 1, height - 1);
        const float wy = float(fy - y0);
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) * width / w - 0.5, 0.0, double(width - 1));
            const int x0 = int(fx), x1 = std::min(x0 + 1, width - 1);
            const float wx = float(fx - x0);
            for (int c = 0; c < 3; ++c) {
                auto px = [&](int xx, int yy) { return float(rgba[(size_t(yy) * size_t(width) + size_t(xx)) * 4 + size_t(c)]); };
                const float v = (px(x0, y0) * (1 - wx) + px(x1, y0) * wx) * (1 - wy) + (px(x0, y1) * (1 - wx) + px(x1, y1) * wx) * wy;
                input[size_t(c) * size_t(w) * size_t(h) + size_t(y) * size_t(w) + size_t(x)] = (v / 255.0f - mean[c]) / stdev[c];
            }
        }
    }
    try {
        std::vector<Ort::Value> inputs;
        inputs.push_back(onnx::tensor(input, {1, 3, h, w}));
        auto outputs = s.run({s.inputs().at(0)}, inputs, {s.outputs().at(0)});
        const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int oh = int(shape[shape.size() - 2]), ow = int(shape.back());
        std::vector<float> out(outputs[0].GetTensorData<float>(), outputs[0].GetTensorData<float>() + size_t(ow) * size_t(oh));
        // BiRefNet devuelve logits; MODNet, ya la opacidad.
        const bool logits = std::any_of(out.begin(), out.end(), [](float v) { return v < -0.01f || v > 1.01f; });
        if (logits)
            for (float& v : out)
                v = 1.0f / (1.0f + std::exp(-v));
        resizePlane(out.data(), ow, oh, alpha, width, height);
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("El recorte falló: ") + e.what();
        return false;
    }
}

bool writeMatte(const std::string& mediaPath, TimeUs from, TimeUs to, const std::string& output, const MatteFn& matte,
                MatteResult& result, std::string* error, const ProgressFn& progress, const CancelFn& cancel)
{
    result = MatteResult{};
    media::VideoDecoder decoder;
    if (!decoder.open(mediaPath, {}, error))
        return false;
    if (to <= from)
        to = decoder.duration();
    if (from > 0)
        decoder.seek(from);

    media::Encoder encoder;
    bool encoderOpen = false;
    std::vector<uint8_t> rgba, alpha, gray;
    while (auto frame = decoder.decodeNext()) {
        if (frame->pts >= to)
            break;
        if (cancel && cancel()) {
            encoder.abort();
            if (error)
                *error = "cancelado";
            return false;
        }
        const int w = frame->width & ~1, h = frame->height & ~1; // H.264 4:2:0: dimensiones pares
        if (!encoderOpen) {
            const core::Rational rate = core::edit::guessFrameRate(decoder.frameRate() > 0 ? decoder.frameRate() : 30.0);
            media::EncoderSettings settings;
            settings.path = output;
            settings.hasAudio = false;
            settings.video.codec = "libx264";
            settings.video.width = w;
            settings.video.height = h;
            settings.video.fpsNum = int(rate.num);
            settings.video.fpsDen = int(rate.den);
            settings.video.quality = 10; // casi sin pérdidas: los bordes del mate importan
            settings.video.preset = "veryfast";
            if (!encoder.open(settings, error))
                return false;
            encoderOpen = true;
            result.start = frame->pts;
        }
        if (!media::frameToRgba(*frame, w, h, rgba) || !matte(rgba.data(), w, h, result.frames, alpha, error)) {
            encoder.abort();
            if (error && error->empty())
                *error = "No se pudo convertir el fotograma.";
            return false;
        }
        gray.resize(size_t(w) * size_t(h) * 4);
        for (size_t i = 0; i < alpha.size(); ++i) {
            gray[i * 4] = gray[i * 4 + 1] = gray[i * 4 + 2] = alpha[i];
            gray[i * 4 + 3] = 255;
        }
        if (!encoder.writeVideo(gray.data(), w * 4)) {
            if (error)
                *error = encoder.lastError();
            encoder.abort();
            return false;
        }
        ++result.frames;
        if (progress && to > from)
            progress(std::clamp(double(frame->pts - from) / double(to - from), 0.0, 1.0));
    }
    if (!encoderOpen) {
        if (error)
            *error = "El tramo no tiene fotogramas de video.";
        return false;
    }
    if (!encoder.finish(error))
        return false;
    result.output = output;
    return true;
}

bool removeBackground(const std::string& modelPath, MatteModel model, const std::string& mediaPath, TimeUs from, TimeUs to,
                      const std::string& output, MatteResult& result, std::string* error, const ProgressFn& progress,
                      const CancelFn& cancel)
{
    return writeMatte(
        mediaPath, from, to, output,
        [&](const uint8_t* rgba, int w, int h, int, std::vector<uint8_t>& alpha, std::string* err) {
            return matteFrame(modelPath, model, rgba, w, h, alpha, err);
        },
        result, error, progress, cancel);
}

// --- SAM 2.1: segmentar un objeto a partir de un punto ---

namespace {

struct Sam {
    std::string folder;
    onnx::Model encoder, decoder;
};

Sam& sam()
{
    static Sam& s = *new Sam;
    return s;
}

bool loadSam(const std::string& folder, std::string* error)
{
    Sam& s = sam();
    std::string why;
    if (s.folder == folder && s.encoder.loaded() && s.decoder.loaded())
        return true;
    if (!s.encoder.load(folder + "/vision_encoder.onnx", true, &why) || !s.decoder.load(folder + "/prompt_encoder_mask_decoder.onnx", true, &why)) {
        if (error)
            *error = "No se pudo cargar SAM 2: " + why;
        return false;
    }
    s.folder = folder;
    return true;
}

} // namespace

bool segmentFrame(const std::string& modelFolder, const uint8_t* rgba, int width, int height, const SegmentPrompt& prompt,
                  std::vector<uint8_t>& alpha, float* score, std::string* error)
{
    if (!loadSam(modelFolder, error))
        return false;
    Sam& s = sam();
    constexpr int kSize = 1024;
    // La imagen se lleva a 1024 × 1024 (sin conservar el aspecto, como el procesador de SAM 2).
    std::vector<float> pixels(size_t(3) * kSize * kSize);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
    for (int y = 0; y < kSize; ++y) {
        const double fy = std::clamp((y + 0.5) * height / kSize - 0.5, 0.0, double(height - 1));
        const int y0 = int(fy), y1 = std::min(y0 + 1, height - 1);
        const float wy = float(fy - y0);
        for (int x = 0; x < kSize; ++x) {
            const double fx = std::clamp((x + 0.5) * width / kSize - 0.5, 0.0, double(width - 1));
            const int x0 = int(fx), x1 = std::min(x0 + 1, width - 1);
            const float wx = float(fx - x0);
            for (int c = 0; c < 3; ++c) {
                auto px = [&](int xx, int yy) { return float(rgba[(size_t(yy) * size_t(width) + size_t(xx)) * 4 + size_t(c)]); };
                const float v = (px(x0, y0) * (1 - wx) + px(x1, y0) * wx) * (1 - wy) + (px(x0, y1) * (1 - wx) + px(x1, y1) * wx) * wy;
                pixels[size_t(c) * kSize * kSize + size_t(y) * kSize + size_t(x)] = (v / 255.0f - mean[c]) / stdev[c];
            }
        }
    }
    try {
        std::vector<Ort::Value> encIn;
        encIn.push_back(onnx::tensor(pixels, {1, 3, kSize, kSize}));
        auto embeddings = s.encoder.run({"pixel_values"}, encIn, {"image_embeddings.0", "image_embeddings.1", "image_embeddings.2"});

        // Prompt en la escala 1024: un punto positivo y, si se da, la caja.
        std::vector<float> points{float(prompt.x * kSize), float(prompt.y * kSize)};
        std::vector<int64_t> labels{1};
        std::vector<float> box{float(prompt.box[0] * kSize), float(prompt.box[1] * kSize), float(prompt.box[2] * kSize),
                               float(prompt.box[3] * kSize)};
        std::vector<Ort::Value> decIn;
        decIn.push_back(onnx::tensor(points, {1, 1, 1, 2}));
        decIn.push_back(onnx::tensor(labels, {1, 1, 1}));
        decIn.push_back(onnx::tensor(box, {1, prompt.useBox ? 1 : 0, 4}));
        for (auto& e : embeddings)
            decIn.push_back(std::move(e));
        auto out = s.decoder.run({"input_points", "input_labels", "input_boxes", "image_embeddings.0", "image_embeddings.1", "image_embeddings.2"},
                                 decIn, {"iou_scores", "pred_masks", "object_score_logits"});
        const float* iou = out[0].GetTensorData<float>();
        const auto shape = out[1].GetTensorTypeAndShapeInfo().GetShape(); // [1, 1, 3, 256, 256]
        const int masks = int(shape[2]), mh = int(shape[3]), mw = int(shape[4]);
        int best = 0;
        for (int i = 1; i < masks; ++i)
            if (iou[i] > iou[best])
                best = i;
        if (score)
            *score = iou[best];
        const bool present = out[2].GetTensorData<float>()[0] > 0.0f;
        const float* logits = out[1].GetTensorData<float>() + size_t(best) * size_t(mw) * size_t(mh);
        // Logits → probabilidad, ampliada al tamaño del fotograma y con borde suave.
        std::vector<float> prob(size_t(mw) * size_t(mh));
        for (size_t i = 0; i < prob.size(); ++i)
            prob[i] = present ? 1.0f / (1.0f + std::exp(-logits[i])) : 0.0f;
        resizePlane(prob.data(), mw, mh, alpha, width, height);
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("La segmentación falló: ") + e.what();
        return false;
    }
}

bool segmentObject(const std::string& modelFolder, const std::string& mediaPath, TimeUs from, TimeUs to, double x, double y,
                   const std::string& output, MatteResult& result, std::string* error, const ProgressFn& progress,
                   const CancelFn& cancel)
{
    SegmentPrompt prompt;
    prompt.x = x;
    prompt.y = y;
    return writeMatte(
        mediaPath, from, to, output,
        [&](const uint8_t* rgba, int w, int h, int index, std::vector<uint8_t>& alpha, std::string* err) {
            if (!segmentFrame(modelFolder, rgba, w, h, prompt, alpha, nullptr, err))
                return false;
            // El siguiente fotograma se guía por la caja (un 10 % mayor) y el centro de esta máscara.
            double sx = 0, sy = 0, sw = 0;
            int minX = w, minY = h, maxX = -1, maxY = -1;
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    if (alpha[size_t(yy) * size_t(w) + size_t(xx)] > 127) {
                        sx += xx;
                        sy += yy;
                        sw += 1;
                        minX = std::min(minX, xx);
                        maxX = std::max(maxX, xx);
                        minY = std::min(minY, yy);
                        maxY = std::max(maxY, yy);
                    }
            if (sw > 0) {
                const double mx = 0.1 * (maxX - minX), my = 0.1 * (maxY - minY);
                prompt.x = sx / sw / w;
                prompt.y = sy / sw / h;
                prompt.box = {std::max(0.0, (minX - mx) / w), std::max(0.0, (minY - my) / h), std::min(1.0, (maxX + mx) / w),
                              std::min(1.0, (maxY + my) / h)};
                prompt.useBox = true;
            }
            (void)index;
            return true;
        },
        result, error, progress, cancel);
}

} // namespace movix::ai


