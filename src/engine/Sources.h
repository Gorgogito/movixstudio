#pragma once

#include "core/Project.h"
#include "color/ColorMath.h"
#include "core/Timeline.h"
#include "engine/AudioReader.h"
#include "engine/VideoReader.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace movix::engine {

// Capa de video ya decodificada (o generada), lista para componer.
struct LayerFrame {
    core::Id clipId = core::kNoId;
    int trackIndex = 0;
    std::shared_ptr<const media::VideoFrame> frame;
    core::LayerTransform transform;
    double opacity = 1.0;
    core::CropRect crop;
    std::shared_ptr<const color::Lut3D> lut; // corrección de color (nullptr = ninguna)
    core::ChromaKey chromaKey;
    // Una sola máscara geométrica se calcula en el shader (`masks`); varias, o alguna de forma
    // libre, se rasterizan en `maskImage`.
    std::vector<core::LayerMask> masks;
    std::shared_ptr<const struct MaskImage> maskImage;
    // Clip compuesto: en lugar de `frame`, las capas de su secuencia (se componen en una textura).
    std::shared_ptr<const struct FrameBundle> nested;
    // La corrección incluye una transformación de entrada (OCIO) que ya lleva la señal a la del
    // proyecto: el compositor no debe convertirla.
    bool rawSignal = false;
    core::BlendMode blend = core::BlendMode::Normal;
};

// Todo lo necesario para dibujar un fotograma de la línea de tiempo.
struct FrameBundle {
    core::FramePos frame = 0;
    TimeUs time = 0;
    int canvasWidth = 1920;
    int canvasHeight = 1080;
    core::ColorMode colorMode = core::ColorMode::SDR; // señal de trabajo (SDR, PQ o HLG)
    std::vector<LayerFrame> layers; // de abajo arriba
};

// Alfa de las máscaras de una capa, ya combinadas (8 bits, filas de arriba abajo).
struct MaskImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> alpha;
};

// ¿Hace falta rasterizar las máscaras (varias, o alguna de forma libre)?
bool masksNeedRaster(const std::vector<core::LayerMask>& masks);

// Rasteriza las máscaras de una capa (en coordenadas de la capa) a una imagen de width × height.
// La implementa la biblioteca de render (QPainter).
class MaskRenderer {
public:
    virtual ~MaskRenderer() = default;
    virtual std::shared_ptr<const MaskImage> render(const std::vector<core::LayerMask>& masks, int width, int height) = 0;
};

// Genera la imagen de un título (RGBA del tamaño del lienzo). La implementa la biblioteca de
// render (QPainter) para que el motor no dependa de la interfaz gráfica.
class TitleRenderer {
public:
    virtual ~TitleRenderer() = default;
    virtual std::shared_ptr<const media::VideoFrame> render(const core::TitleStyle& style, int canvasWidth, int canvasHeight) = 0;
};

// Lectores de video abiertos, uno por clip (para que cada clip decodifique en orden), con un
// máximo de decodificadores abiertos: se cierra el que lleva más tiempo sin usarse. Las imágenes
// fijas se decodifican una vez y se comparten; los títulos se generan y se guardan en caché.
class VideoSources {
public:
    explicit VideoSources(bool hardware = true, size_t maxOpen = 12) : m_hardware(hardware), m_maxOpen(maxOpen) {}

    void setTitleRenderer(std::shared_ptr<TitleRenderer> renderer) { m_titles = std::move(renderer); }
    void setMaskRenderer(std::shared_ptr<MaskRenderer> renderer) { m_maskRenderer = std::move(renderer); }
    std::shared_ptr<const media::VideoFrame> frame(const core::VideoLayer& layer, int canvasWidth, int canvasHeight);
    FrameBundle build(const core::Project& project, core::FramePos frame);
    void clear();
    size_t openCount() const { return m_readers.size(); }

private:
    std::shared_ptr<const media::VideoFrame> still(const std::string& path);
    std::shared_ptr<const media::VideoFrame> title(const core::TitleStyle& style, int w, int h);
    std::shared_ptr<const color::Lut3D> lut(const core::ColorGrade& grade);
    std::shared_ptr<const MaskImage> maskImage(const std::vector<core::LayerMask>& masks, int layerWidth, int layerHeight);
    // Opacidad del mate de IA de la capa (multiplicada por sus máscaras), o nullptr si no se puede leer.
    std::shared_ptr<const MaskImage> matteImage(const core::VideoLayer& layer, const std::vector<core::LayerMask>& masks);
    void buildLayers(const std::vector<core::VideoLayer>& layers, FrameBundle& bundle);

    struct Entry {
        std::unique_ptr<VideoReader> reader;
        std::string path;
        uint64_t lastUse = 0;
        bool failed = false;
    };
    bool m_hardware;
    size_t m_maxOpen;
    uint64_t m_tick = 0;
    std::unordered_map<std::uint64_t, Entry> m_readers; // por aparición del clip (VideoLayer::instance)
    std::unordered_map<std::string, std::shared_ptr<const media::VideoFrame>> m_stills;
    struct CachedTitle {
        core::TitleStyle style;
        int width = 0, height = 0;
        std::shared_ptr<const media::VideoFrame> frame;
    };
    std::vector<CachedTitle> m_titleCache; // pocos títulos: búsqueda lineal
    std::shared_ptr<TitleRenderer> m_titles;
    struct CachedLut {
        core::ColorGrade grade;
        std::shared_ptr<const color::Lut3D> lut;
    };
    std::vector<CachedLut> m_lutCache; // por corrección (pocas distintas a la vez)
    std::unordered_map<std::string, std::shared_ptr<const color::CubeLut>> m_cubeFiles;
    std::shared_ptr<MaskRenderer> m_maskRenderer;
    core::ColorMode m_colorMode = core::ColorMode::SDR; // las LUT con OCIO dependen de él
    struct CachedMask {
        std::vector<core::LayerMask> masks;
        int width = 0, height = 0;
        std::shared_ptr<const MaskImage> image;
    };
    std::vector<CachedMask> m_maskCache;
    struct CachedMatte {
        std::shared_ptr<const media::VideoFrame> frame; // el fotograma del mate del que salió
        std::vector<core::LayerMask> masks;
        bool invert = false;
        std::shared_ptr<const MaskImage> image;
    };
    std::unordered_map<std::uint64_t, CachedMatte> m_matteCache; // por aparición del clip
};

// Mezclador de audio de la línea de tiempo: suma todos los tramos que suenan en un intervalo.
// El tiempo se expresa en muestras a la frecuencia del proyecto, sin errores acumulados.
class AudioMixer {
public:
    AudioMixer(int sampleRate = 48000, int channels = 2, size_t maxOpen = 16)
        : m_rate(sampleRate), m_channels(channels), m_maxOpen(maxOpen) {}

    // Escribe en `out` (frames × canales, intercalado) el audio de [samplePos, samplePos + frames).
    void mix(const core::Project& project, int64_t samplePos, int frames, float* out);
    void clear() { m_readers.clear(); }
    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }
    AudioReader* readerFor(core::Id clipId); // para pruebas

    TimeUs sampleToTime(int64_t s) const { return s * kUsPerSecond / m_rate; }
    int64_t timeToSample(TimeUs t) const { return (t * m_rate + kUsPerSecond - 1) / kUsPerSecond; } // hacia arriba

private:
    struct Entry {
        std::unique_ptr<AudioReader> reader;
        std::string path;
        double speed = 1.0;
        uint64_t lastUse = 0;
        bool failed = false;
    };
    int m_rate;
    int m_channels;
    size_t m_maxOpen;
    uint64_t m_tick = 0;
    std::unordered_map<std::uint64_t, Entry> m_readers;
};

// Decodifica una imagen fija a RGBA (conserva la transparencia de PNG/WebP).
std::shared_ptr<media::VideoFrame> loadStill(const std::string& path, std::string* error = nullptr);

} // namespace movix::engine
