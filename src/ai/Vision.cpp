#include "ai/Vision.h"

#include "ai/Onnx.h"
#include "core/Edits.h"
#include "media/Encoder.h"
#include "media/FrameConvert.h"
#include "media/VideoDecoder.h"

#include <QByteArray>

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
        if (!media::frameToRgba(*frame, w, h, rgba) || !matte(rgba.data(), w, h, frame->pts, alpha, error)) {
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
        [&](const uint8_t* rgba, int w, int h, TimeUs, std::vector<uint8_t>& alpha, std::string* err) {
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

namespace {

// Probabilidad del objeto a la resolución de salida de SAM (`mw` × `mh`, todo el fotograma estirado).
bool segmentLowRes(const std::string& modelFolder, const uint8_t* rgba, int width, int height, const SegmentPrompt& prompt,
                   std::vector<float>& prob, int& mw, int& mh, float* score, std::string* error)
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
        const int masks = int(shape[2]);
        mh = int(shape[3]);
        mw = int(shape[4]);
        int best = 0;
        for (int i = 1; i < masks; ++i)
            if (iou[i] > iou[best])
                best = i;
        if (score)
            *score = iou[best];
        const bool present = out[2].GetTensorData<float>()[0] > 0.0f;
        const float* logits = out[1].GetTensorData<float>() + size_t(best) * size_t(mw) * size_t(mh);
        prob.resize(size_t(mw) * size_t(mh));
        for (size_t i = 0; i < prob.size(); ++i)
            prob[i] = present ? 1.0f / (1.0f + std::exp(-logits[i])) : 0.0f;
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("La segmentación falló: ") + e.what();
        return false;
    }
}

// El fotograma vecino (en cualquier sentido) se guía por la caja (un 10 % mayor) y el centro de esta
// máscara. Sin máscara, el prompt no cambia.
void followMask(const std::vector<float>& prob, int mw, int mh, SegmentPrompt& prompt)
{
    double sx = 0, sy = 0, sw = 0;
    int minX = mw, minY = mh, maxX = -1, maxY = -1;
    for (int y = 0; y < mh; ++y)
        for (int x = 0; x < mw; ++x)
            if (prob[size_t(y) * size_t(mw) + size_t(x)] > 0.5f) {
                sx += x + 0.5;
                sy += y + 0.5;
                sw += 1;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    if (sw == 0)
        return;
    const double mx = 0.1 * (maxX + 1 - minX), my = 0.1 * (maxY + 1 - minY);
    prompt.x = sx / sw / mw;
    prompt.y = sy / sw / mh;
    prompt.box = {std::max(0.0, (minX - mx) / mw), std::max(0.0, (minY - my) / mh), std::min(1.0, (maxX + 1 + mx) / mw),
                  std::min(1.0, (maxY + 1 + my) / mh)};
    prompt.useBox = true;
}

// Fotograma en RGBA a una resolución suficiente para SAM (que lo lleva a 1024 × 1024).
bool samRgba(const media::VideoFrame& frame, std::vector<uint8_t>& rgba, int& w, int& h)
{
    const double scale = std::min(1.0, 1024.0 / std::max(frame.width, frame.height));
    w = std::max(2, int(std::lround(frame.width * scale)) & ~1);
    h = std::max(2, int(std::lround(frame.height * scale)) & ~1);
    return media::frameToRgba(frame, w, h, rgba);
}

// Máscara de baja resolución guardada (comprimida) hasta escribir el mate en orden.
struct StoredMask {
    int w = 0, h = 0;
    QByteArray data; // bytes 0..255 comprimidos
};

} // namespace

bool segmentFrame(const std::string& modelFolder, const uint8_t* rgba, int width, int height, const SegmentPrompt& prompt,
                  std::vector<uint8_t>& alpha, float* score, std::string* error)
{
    std::vector<float> prob;
    int mw = 0, mh = 0;
    if (!segmentLowRes(modelFolder, rgba, width, height, prompt, prob, mw, mh, score, error))
        return false;
    // Ampliada al tamaño del fotograma, con borde suave.
    resizePlane(prob.data(), mw, mh, alpha, width, height);
    return true;
}

bool segmentObject(const std::string& modelFolder, const std::string& mediaPath, TimeUs from, TimeUs to, TimeUs at, double x,
                   double y, const std::string& output, MatteResult& result, std::string* error, const ProgressFn& progress,
                   const CancelFn& cancel)
{
    SegmentPrompt click;
    click.x = x;
    click.y = y;
    std::vector<float> prob;
    int mw = 0, mh = 0;

    // 1) Hacia atrás, desde el fotograma del clic hasta `from`: por tramos de un segundo que se
    //    decodifican hacia delante y se recorren al revés. Las máscaras se guardan a la resolución
    //    de SAM hasta escribir el mate en orden.
    std::map<TimeUs, StoredMask> backward;
    TimeUs clickPts = from;
    double backwardShare = 0; // parte del progreso que corresponde a esta pasada
    if (at > from) {
        media::VideoDecoder decoder;
        if (!decoder.open(mediaPath, {}, error))
            return false;
        const TimeUs end = to > from ? to : decoder.duration();
        backwardShare = std::clamp(double(at - from) / double(std::max<TimeUs>(1, end - from)), 0.0, 1.0);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        decoder.seek(at);
        auto first = decoder.decodeNext();
        if (!first || !samRgba(*first, rgba, w, h)) {
            if (error)
                *error = "No se pudo decodificar el fotograma del clic.";
            return false;
        }
        clickPts = first->pts;
        // El fotograma del clic da la guía de partida.
        SegmentPrompt prompt = click;
        if (!segmentLowRes(modelFolder, rgba.data(), w, h, prompt, prob, mw, mh, nullptr, error))
            return false;
        followMask(prob, mw, mh, prompt);
        constexpr TimeUs kChunk = 1'000'000;
        TimeUs chunkEnd = first->pts;
        std::vector<std::pair<TimeUs, std::vector<uint8_t>>> frames;
        while (chunkEnd > from) {
            const TimeUs chunkStart = std::max(from, chunkEnd - kChunk);
            frames.clear();
            decoder.seek(chunkStart);
            while (auto f = decoder.decodeNext()) {
                if (f->pts >= chunkEnd)
                    break;
                if (!samRgba(*f, rgba, w, h))
                    break;
                frames.emplace_back(f->pts, rgba);
            }
            if (frames.empty())
                break;
            for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
                if (cancel && cancel()) {
                    if (error)
                        *error = "cancelado";
                    return false;
                }
                if (!segmentLowRes(modelFolder, it->second.data(), w, h, prompt, prob, mw, mh, nullptr, error))
                    return false;
                followMask(prob, mw, mh, prompt);
                QByteArray bytes(qsizetype(prob.size()), Qt::Uninitialized);
                for (size_t i = 0; i < prob.size(); ++i)
                    bytes[qsizetype(i)] = char(uint8_t(std::lround(std::clamp(prob[i], 0.0f, 1.0f) * 255.0f)));
                backward[it->first] = {mw, mh, qCompress(bytes, 1)};
                if (progress)
                    progress(backwardShare * std::clamp(double(at - it->first) / double(std::max<TimeUs>(1, at - from)), 0.0, 1.0));
            }
            if (chunkStart <= from)
                break;
            chunkEnd = frames.front().first;
        }
    }

    // 2) El mate completo, hacia delante: lo guardado y, desde el clic, cada fotograma guiado por el anterior.
    SegmentPrompt prompt = click;
    std::vector<float> stored;
    const ProgressFn forwardProgress = [&](double p) {
        if (progress)
            progress(std::max(p, backwardShare));
    };
    return writeMatte(
        mediaPath, from, to, output,
        [&](const uint8_t* rgba, int w, int h, TimeUs pts, std::vector<uint8_t>& alpha, std::string* err) {
            if (const auto it = backward.find(pts); it != backward.end()) {
                const QByteArray bytes = qUncompress(it->second.data);
                stored.resize(size_t(bytes.size()));
                for (qsizetype i = 0; i < bytes.size(); ++i)
                    stored[size_t(i)] = uint8_t(bytes[i]) / 255.0f;
                resizePlane(stored.data(), it->second.w, it->second.h, alpha, w, h);
                return true;
            }
            // Antes del clic sin máscara guardada (no debería pasar): solo con el punto del clic.
            const bool before = pts < clickPts;
            SegmentPrompt single = click;
            if (!segmentLowRes(modelFolder, rgba, w, h, before ? single : prompt, prob, mw, mh, nullptr, err))
                return false;
            if (!before)
                followMask(prob, mw, mh, prompt);
            resizePlane(prob.data(), mw, mh, alpha, w, h);
            return true;
        },
        result, error, forwardProgress, cancel);
}

} // namespace movix::ai
