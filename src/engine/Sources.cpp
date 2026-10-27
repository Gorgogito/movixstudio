#include "engine/Sources.h"

#include "color/Ocio.h"
#include "media/FrameConvert.h"
#include "media/ImageDecoder.h"

#include <algorithm>
#include <cstring>

namespace movix::engine {

namespace {

template <typename Map>
void evictLeastRecentlyUsed(Map& readers, size_t maxOpen)
{
    while (readers.size() > maxOpen) {
        auto oldest = std::min_element(readers.begin(), readers.end(),
                                       [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        readers.erase(oldest);
    }
}

} // namespace

std::shared_ptr<media::VideoFrame> loadStill(const std::string& path, std::string* error)
{
    return media::decodeImageRgba(path, error);
}

void VideoSources::clear()
{
    m_readers.clear();
    m_stills.clear();
    m_titleCache.clear();
    m_lutCache.clear();
    m_cubeFiles.clear();
    m_maskCache.clear();
    m_matteCache.clear();
    m_ofx.clear();
}

bool masksNeedRaster(const std::vector<core::LayerMask>& masks)
{
    return masks.size() > 1
           || std::any_of(masks.begin(), masks.end(), [](const core::LayerMask& m) { return m.shape == core::MaskShape::Path; });
}

std::shared_ptr<const MaskImage> VideoSources::maskImage(const std::vector<core::LayerMask>& masks, int layerWidth, int layerHeight)
{
    if (!m_maskRenderer)
        return nullptr;
    // Resolución de la máscara: la de la capa, con el lado mayor limitado a 1024 (el borde se
    // interpola al dibujar).
    int w = layerWidth, h = layerHeight;
    const double scale = std::min(1.0, 1024.0 / std::max(w, h));
    w = std::max(8, int(std::lround(w * scale)));
    h = std::max(8, int(std::lround(h * scale)));
    for (const CachedMask& c : m_maskCache)
        if (c.width == w && c.height == h && c.masks == masks)
            return c.image;
    auto image = m_maskRenderer->render(masks, w, h);
    if (m_maskCache.size() >= 16)
        m_maskCache.erase(m_maskCache.begin());
    m_maskCache.push_back({masks, w, h, image});
    return image;
}

std::shared_ptr<const MaskImage> VideoSources::matteImage(const core::VideoLayer& layer, const std::vector<core::LayerMask>& masks)
{
    // Un lector propio por aparición del clip (no comparte el de la imagen).
    const std::uint64_t key = layer.instance ^ 0x9E3779B97F4A7C15ull;
    Entry& e = m_readers[key];
    e.lastUse = ++m_tick;
    if (!e.reader || e.path != layer.matte->path) {
        e.reader = std::make_unique<VideoReader>();
        e.path = layer.matte->path;
        e.failed = !e.reader->open(e.path, m_hardware);
    }
    if (e.failed)
        return nullptr;
    const auto frame = e.reader->frameAt(std::max<TimeUs>(0, layer.sourceTime - layer.matte->start));
    if (!frame || frame->format == media::PixelFormat::RGBA || frame->y.empty())
        return nullptr;

    CachedMatte& cached = m_matteCache[key];
    if (cached.frame == frame && cached.masks == masks && cached.invert == layer.matte->invert)
        return cached.image;

    // Opacidad = luma del mate (expandida si es de rango limitado), invertida si se pide.
    auto image = std::make_shared<MaskImage>();
    image->width = frame->width;
    image->height = frame->height;
    image->alpha.resize(size_t(frame->width) * size_t(frame->height));
    const int bytes = frame->format == media::PixelFormat::P016 ? 2 : 1;
    for (size_t i = 0; i < image->alpha.size(); ++i) {
        int y = frame->y[i * bytes + (bytes - 1)];
        if (!frame->fullRange)
            y = std::clamp((y - 16) * 255 / 219, 0, 255);
        image->alpha[i] = uint8_t(layer.matte->invert ? 255 - y : y);
    }
    // Las máscaras del clip recortan además el mate.
    if (!masks.empty()) {
        if (const auto m = maskImage(masks, frame->width, frame->height); m && m->width > 0 && m->height > 0) {
            // La máscara puede estar a menor resolución: se toma la muestra más cercana.
            for (int y = 0; y < image->height; ++y) {
                const size_t my = size_t(std::min(m->height - 1, y * m->height / image->height));
                for (int x = 0; x < image->width; ++x) {
                    const size_t mx = size_t(std::min(m->width - 1, x * m->width / image->width));
                    uint8_t& a = image->alpha[size_t(y) * size_t(image->width) + size_t(x)];
                    a = uint8_t(a * m->alpha[my * size_t(m->width) + mx] / 255);
                }
            }
        }
    }
    cached = {frame, masks, layer.matte->invert, image};
    return image;
}

std::shared_ptr<const color::Lut3D> VideoSources::lut(const core::ColorGrade& grade)
{
    for (const CachedLut& c : m_lutCache)
        if (c.grade == grade)
            return c.lut;
    std::shared_ptr<const color::CubeLut> cube;
    if (!grade.lutPath.empty()) {
        auto it = m_cubeFiles.find(grade.lutPath);
        if (it == m_cubeFiles.end()) // un archivo ilegible se recuerda como nullptr y se ignora
            it = m_cubeFiles.emplace(grade.lutPath, color::loadCubeLut(grade.lutPath)).first;
        cube = it->second;
    }
    color::RgbTransform input;
    if (!grade.inputSpace.empty()) // espacio desconocido: se ignora y se aplica el resto
        input = color::ocioInputTransform(grade.inputSpace, m_colorMode);
    auto baked = color::bakeLut(color::GradeEvaluator(grade, cube, input));
    if (m_lutCache.size() >= 16)
        m_lutCache.erase(m_lutCache.begin());
    m_lutCache.push_back({grade, baked});
    return baked;
}

std::shared_ptr<const media::VideoFrame> VideoSources::still(const std::string& path)
{
    auto it = m_stills.find(path);
    if (it != m_stills.end())
        return it->second;
    auto frame = loadStill(path);
    if (m_stills.size() > 64)
        m_stills.clear(); // límite simple de memoria
    m_stills[path] = frame;
    return frame;
}

std::shared_ptr<const media::VideoFrame> VideoSources::title(const core::TitleStyle& style, int w, int h, int chars)
{
    if (!m_titles)
        return nullptr;
    for (const CachedTitle& c : m_titleCache)
        if (c.width == w && c.height == h && c.chars == chars && c.style == style)
            return c.frame;
    auto frame = m_titles->render(style, w, h, chars);
    if (m_titleCache.size() > 32)
        m_titleCache.erase(m_titleCache.begin());
    m_titleCache.push_back({style, w, h, chars, frame});
    return frame;
}

std::shared_ptr<const media::VideoFrame> VideoSources::frame(const core::VideoLayer& layer, int canvasWidth, int canvasHeight)
{
    if (layer.title)
        return title(*layer.title, canvasWidth, canvasHeight, layer.titleChars);
    if (!layer.media)
        return nullptr;
    if (layer.media->isStill)
        return still(layer.media->path);

    Entry& e = m_readers[layer.instance];
    e.lastUse = ++m_tick;
    // Los efectos OpenFX trabajan con la imagen en memoria: esos clips no usan la copia cero.
    void* device = layer.ofx ? nullptr : m_gpuDevice;
    const std::string path = m_pathMapper ? m_pathMapper(*layer.media) : layer.media->path;
    if (!e.reader || e.path != path || e.device != device) {
        e.reader = std::make_unique<VideoReader>();
        e.path = path;
        e.device = device;
        e.failed = !e.reader->open(e.path, m_hardware, nullptr, device);
    }
    std::shared_ptr<const media::VideoFrame> result = e.failed ? nullptr : e.reader->frameAt(layer.sourceTime);
    evictLeastRecentlyUsed(m_readers, m_maxOpen);
    return result;
}

FrameBundle VideoSources::build(const core::Project& project, core::FramePos frame)
{
    FrameBundle bundle;
    bundle.frame = frame;
    bundle.time = project.settings.frameToTime(frame);
    bundle.canvasWidth = project.settings.width;
    bundle.canvasHeight = project.settings.height;
    bundle.colorMode = project.settings.colorMode;
    bundle.linearLight = project.settings.linearLight;
    if (m_colorMode != bundle.colorMode) {
        m_colorMode = bundle.colorMode;
        m_lutCache.clear();
    }
    m_frameRate = project.settings.frameRate.value();
    ++m_tick;
    buildLayers(core::videoLayersAt(project, frame), bundle);
    // Las instancias OpenFX de clips que ya no se ven se liberan pasado un rato.
    std::erase_if(m_ofx, [&](const auto& kv) { return m_tick - kv.second.lastUse > 600; });
    return bundle;
}

void VideoSources::buildLayers(const std::vector<core::VideoLayer>& layers, FrameBundle& bundle)
{
    for (const core::VideoLayer& layer : layers) {
        if (layer.opacity <= 0.0)
            continue; // invisible: no hace falta decodificar
        std::shared_ptr<const media::VideoFrame> f;
        std::shared_ptr<FrameBundle> nested;
        int layerW = bundle.canvasWidth, layerH = bundle.canvasHeight;
        if (layer.compound) {
            nested = std::make_shared<FrameBundle>();
            nested->frame = bundle.frame;
            nested->time = bundle.time;
            nested->canvasWidth = bundle.canvasWidth;
            nested->canvasHeight = bundle.canvasHeight;
            nested->colorMode = bundle.colorMode;
            nested->linearLight = bundle.linearLight;
            buildLayers(layer.nested, *nested);
        } else {
            f = this->frame(layer, bundle.canvasWidth, bundle.canvasHeight);
            if (!f)
                continue;
            if (layer.ofx)
                f = applyOfx(layer, std::move(f));
            layerW = int(std::lround(f->height * f->displayAspect()));
            layerH = f->height;
        }
        LayerFrame out{layer.clipId, layer.trackIndex, std::move(f), layer.transform, layer.opacity, layer.crop};
        out.nested = std::move(nested);
        if (layer.grade) {
            out.rawSignal = !layer.grade->inputSpace.empty() || (layer.gradeTo && !layer.gradeTo->inputSpace.empty());
            out.lut = lut(*layer.grade);
            if (layer.gradeTo && out.lut) // entre dos keyframes de color: mezcla de sus LUT
                if (const auto to = lut(*layer.gradeTo))
                    out.lut = color::mixLuts(*out.lut, *to, layer.gradeMix);
        }
        out.chromaKey = layer.chromaKey;
        out.masks = layer.masks;
        if (layer.matte)
            out.maskImage = matteImage(layer, out.masks);
        else if (masksNeedRaster(out.masks))
            out.maskImage = maskImage(out.masks, layerW, layerH);
        out.blend = layer.blend;
        bundle.layers.push_back(std::move(out));
    }
}

std::shared_ptr<const media::VideoFrame> VideoSources::applyOfx(const core::VideoLayer& layer, std::shared_ptr<const media::VideoFrame> f)
{
    const bool any = std::any_of(layer.ofx->begin(), layer.ofx->end(), [](const core::OfxEffect& e) { return e.enabled; });
    if (!any || f->width <= 0 || f->height <= 0)
        return f;
    const int w = f->width, h = f->height;
    // El video de más de 8 bits (y todo el HDR) pasa en coma flotante, sin perder precisión, y sale
    // como RGBA16F con su señal; el resto, en 8 bits como RGBA.
    const bool deep = f->format == media::PixelFormat::P016 || f->transfer != media::Transfer::SDR;
    std::vector<uint8_t> rgba;
    std::vector<float> rgbaF;
    if (deep ? !media::frameToRgbaF(*f, rgbaF) : f->format == media::PixelFormat::RGBA ? (rgba = f->rgba, false)
                                                                                         : !media::frameToRgba(*f, w, h, rgba))
        return f;
    std::vector<uint8_t> out(rgba.size());
    std::vector<float> outF(rgbaF.size());
    bool applied = false;
    for (size_t i = 0; i < layer.ofx->size(); ++i) {
        const core::OfxEffect& e = (*layer.ofx)[i];
        if (!e.enabled)
            continue;
        OfxSlot& slot = m_ofx[{layer.instance, i}];
        slot.lastUse = m_tick;
        if (slot.pluginId != e.pluginId || (slot.effect && (slot.effect->width() != w || slot.effect->height() != h))) {
            slot.effect.reset();
            slot.failed = false;
            slot.pluginId = e.pluginId;
        }
        if (!slot.effect && !slot.failed) {
            std::string error;
            slot.effect = ofx::Effect::create(e.pluginId, w, h, m_frameRate, &error);
            slot.failed = !slot.effect;
            if (slot.failed)
                m_ofxError = error;
        }
        if (!slot.effect)
            continue;
        std::string error;
        const auto values = ofx::withDefaults(slot.effect->plugin(), e.params);
        const double time = double(layer.clipFrame);
        if (deep ? !slot.effect->render(values, time, rgbaF.data(), outF.data(), &error)
                 : !slot.effect->render(values, time, rgba.data(), out.data(), &error)) {
            m_ofxError = error;
            continue;
        }
        std::swap(rgba, out);
        std::swap(rgbaF, outF);
        applied = true;
    }
    if (!applied)
        return f;
    if (deep)
        media::floatToHalf(rgbaF, rgba);
    auto result = std::make_shared<media::VideoFrame>();
    result->format = deep ? media::PixelFormat::RGBA16F : media::PixelFormat::RGBA;
    result->width = w;
    result->height = h;
    result->sampleAspect = f->sampleAspect;
    result->pts = f->pts;
    result->duration = f->duration;
    result->colorSpace = f->colorSpace;
    result->transfer = f->transfer;
    result->fullRange = true;
    result->rgba = std::move(rgba);
    return result;
}

AudioReader* AudioMixer::readerFor(core::Id clipId)
{
    auto it = m_readers.find(clipId);
    return it == m_readers.end() ? nullptr : it->second.reader.get();
}

void AudioMixer::mix(const core::Project& project, int64_t samplePos, int frames, float* out)
{
    std::memset(out, 0, size_t(frames) * size_t(m_channels) * sizeof(float));
    const TimeUs t0 = sampleToTime(samplePos);
    const TimeUs t1 = sampleToTime(samplePos + frames);

    for (const core::AudioSegment& seg : core::audioSegmentsIn(project, t0, t1)) {
        // Rango de muestras del bloque cubierto por el tramo.
        const int64_t first = std::clamp<int64_t>(timeToSample(seg.timelineStart) - samplePos, 0, frames);
        const int64_t last = std::clamp<int64_t>(timeToSample(seg.timelineEnd) - samplePos, 0, frames);
        if (last <= first)
            continue;

        Entry& e = m_readers[seg.instance];
        e.lastUse = ++m_tick;
        if (!e.reader || e.path != seg.media->path) {
            e.reader = std::make_unique<AudioReader>();
            e.path = seg.media->path;
            e.failed = !e.reader->open(e.path, m_rate, m_channels);
        }
        if (e.failed)
            continue;
        if (seg.varispeed()) {
            // Instantes del medio en la primera y la última muestra cubiertas (recorrido lineal).
            const double span = double(seg.timelineEnd - seg.timelineStart);
            auto sourceAt = [&](int64_t sample) {
                const double f = span > 0 ? double(sampleToTime(samplePos + sample) - seg.timelineStart) / span : 0.0;
                return seg.sourceStart + TimeUs(std::llround(f * double(seg.sourceEnd - seg.sourceStart)));
            };
            e.reader->mixVarispeed(sourceAt(first), sourceAt(last), int(last - first), seg.gainStart, seg.gainEnd,
                                   out + size_t(first) * size_t(m_channels), seg.denoise);
            continue;
        }
        // Instante del medio que corresponde a la primera muestra cubierta.
        const TimeUs offset = sampleToTime(samplePos + first) - seg.timelineStart;
        const TimeUs sourceTime = seg.sourceStart + (seg.speed == 1.0 ? offset : TimeUs(double(offset) * seg.speed));
        e.reader->mixInto(sourceTime, int(last - first), seg.speed, seg.gainStart, seg.gainEnd,
                          out + size_t(first) * size_t(m_channels), seg.denoise);
    }
    evictLeastRecentlyUsed(m_readers, m_maxOpen);
}

} // namespace movix::engine
