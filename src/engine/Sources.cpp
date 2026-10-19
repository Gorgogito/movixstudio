#include "engine/Sources.h"

#include "color/Ocio.h"
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

std::shared_ptr<const media::VideoFrame> VideoSources::title(const core::TitleStyle& style, int w, int h)
{
    if (!m_titles)
        return nullptr;
    for (const CachedTitle& c : m_titleCache)
        if (c.width == w && c.height == h && c.style == style)
            return c.frame;
    auto frame = m_titles->render(style, w, h);
    if (m_titleCache.size() > 32)
        m_titleCache.erase(m_titleCache.begin());
    m_titleCache.push_back({style, w, h, frame});
    return frame;
}

std::shared_ptr<const media::VideoFrame> VideoSources::frame(const core::VideoLayer& layer, int canvasWidth, int canvasHeight)
{
    if (layer.title)
        return title(*layer.title, canvasWidth, canvasHeight);
    if (!layer.media)
        return nullptr;
    if (layer.media->isStill)
        return still(layer.media->path);

    Entry& e = m_readers[layer.instance];
    e.lastUse = ++m_tick;
    if (!e.reader || e.path != layer.media->path) {
        e.reader = std::make_unique<VideoReader>();
        e.path = layer.media->path;
        e.failed = !e.reader->open(e.path, m_hardware);
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
    if (m_colorMode != bundle.colorMode) {
        m_colorMode = bundle.colorMode;
        m_lutCache.clear();
    }
    buildLayers(core::videoLayersAt(project, frame), bundle);
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
            buildLayers(layer.nested, *nested);
        } else {
            f = this->frame(layer, bundle.canvasWidth, bundle.canvasHeight);
            if (!f)
                continue;
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
        if (masksNeedRaster(out.masks))
            out.maskImage = maskImage(out.masks, layerW, layerH);
        out.blend = layer.blend;
        bundle.layers.push_back(std::move(out));
    }
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
        // Instante del medio que corresponde a la primera muestra cubierta.
        const TimeUs offset = sampleToTime(samplePos + first) - seg.timelineStart;
        const TimeUs sourceTime = seg.sourceStart + (seg.speed == 1.0 ? offset : TimeUs(double(offset) * seg.speed));
        e.reader->mixInto(sourceTime, int(last - first), seg.speed, seg.gainStart, seg.gainEnd,
                          out + size_t(first) * size_t(m_channels));
    }
    evictLeastRecentlyUsed(m_readers, m_maxOpen);
}

} // namespace movix::engine
