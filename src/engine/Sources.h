#pragma once

#include "core/Project.h"
#include "color/ColorMath.h"
#include "core/Timeline.h"
#include "engine/AudioReader.h"
#include "engine/VideoReader.h"
#include "ofx/OfxHost.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
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
    bool linearLight = false;       // componer en luz lineal
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
    // `visibleChars`: solo los primeros caracteres (máquina de escribir), sin mover los demás; −1 = todos.
    virtual std::shared_ptr<const media::VideoFrame> render(const core::TitleStyle& style, int canvasWidth, int canvasHeight,
                                                            int visibleChars = -1) = 0;
};

// Compone una secuencia anidada en una imagen en memoria del tamaño de su lienzo, sobre fondo
// transparente y con el alfa sin premultiplicar: RGBA de 8 bits o, con `deep`, RGBA16F en la señal
// del proyecto. La implementa la biblioteca de render (en la GPU); la usan los compuestos con
// efectos OpenFX, que necesitan sus píxeles. Se crea y se usa en el mismo hilo.
class BundleRenderer {
public:
    virtual ~BundleRenderer() = default;
    virtual std::shared_ptr<const media::VideoFrame> render(const FrameBundle& bundle, bool deep) = 0;
};

// Lectores de video abiertos, uno por clip (para que cada clip decodifique en orden), con un
// máximo de decodificadores abiertos: se cierra el que lleva más tiempo sin usarse. Las imágenes
// fijas se decodifican una vez y se comparten; los títulos se generan y se guardan en caché.
class VideoSources {
public:
    explicit VideoSources(bool hardware = true, size_t maxOpen = 12) : m_hardware(hardware), m_maxOpen(maxOpen) {}

    void setTitleRenderer(std::shared_ptr<TitleRenderer> renderer) { m_titles = std::move(renderer); }
    void setMaskRenderer(std::shared_ptr<MaskRenderer> renderer) { m_maskRenderer = std::move(renderer); }
    // Sin él, los efectos OpenFX de los clips compuestos no se aplican.
    void setBundleRenderer(std::shared_ptr<BundleRenderer> renderer) { m_bundleRenderer = std::move(renderer); }
    // Copia cero: los clips se decodifican en este dispositivo (ID3D11Device* del compositor) y sus
    // fotogramas no bajan a memoria. Los que necesitan la imagen en la CPU (efectos OpenFX, también
    // los clips dentro de un compuesto con efectos) no.
    void setGpuDevice(void* device) { m_gpuDevice = device; }
    // Archivo que se lee para cada medio (p. ej. su proxy en la vista previa); sin ella, el original.
    void setPathMapper(std::function<std::string(const core::MediaItem&)> mapper) { m_pathMapper = std::move(mapper); }
    std::shared_ptr<const media::VideoFrame> frame(const core::VideoLayer& layer, int canvasWidth, int canvasHeight);
    FrameBundle build(const core::Project& project, core::FramePos frame);
    void clear();
    size_t openCount() const { return m_readers.size(); }
    // Último error de un efecto OpenFX (plugin que falta o que falló); se borra al leerlo.
    std::string takeOfxError() { return std::exchange(m_ofxError, {}); }

private:
    std::shared_ptr<const media::VideoFrame> still(const std::string& path);
    std::shared_ptr<const media::VideoFrame> title(const core::TitleStyle& style, int w, int h, int chars = -1);
    std::shared_ptr<const color::Lut3D> lut(const core::ColorGrade& grade);
    std::shared_ptr<const MaskImage> maskImage(const std::vector<core::LayerMask>& masks, int layerWidth, int layerHeight);
    // Opacidad del mate de IA de la capa (multiplicada por sus máscaras), o nullptr si no se puede leer.
    std::shared_ptr<const MaskImage> matteImage(const core::VideoLayer& layer, const std::vector<core::LayerMask>& masks);
    void buildLayers(const std::vector<core::VideoLayer>& layers, FrameBundle& bundle);
    // Compuesto con efectos OpenFX: su secuencia compuesta en memoria (nullptr si no se puede).
    std::shared_ptr<const media::VideoFrame> flattenCompound(const FrameBundle& nested);
    // Imagen de un generador OpenFX del tamaño del lienzo (nullptr si el plugin falta o falla).
    std::shared_ptr<const media::VideoFrame> generated(const core::VideoLayer& layer, int width, int height);
    // Transición OpenFX: compone cada lado en memoria y el plugin los mezcla. Si no se puede (sin
    // renderizador de secuencias o sin el plugin), queda una disolvencia.
    void buildTransition(const core::VideoLayer& layer, FrameBundle& bundle);
    // Instancia OpenFX de una aparición del clip (`slot`: la posición del efecto, o la del generador
    // o la transición), creada o rehecha si cambia el plugin o el tamaño; nullptr si no se puede.
    ofx::Effect* ofxInstance(std::uint64_t instance, size_t slot, const std::string& pluginId, ofx::Role role, int width, int height);
    // Aplica los efectos OpenFX de la capa; si ninguno se puede aplicar, devuelve el fotograma igual.
    std::shared_ptr<const media::VideoFrame> applyOfx(const core::VideoLayer& layer, std::shared_ptr<const media::VideoFrame> f);

    struct Entry {
        std::unique_ptr<VideoReader> reader;
        std::string path;
        uint64_t lastUse = 0;
        bool failed = false;
        void* device = nullptr; // decodifica con copia cero en este dispositivo
    };
    bool m_hardware;
    size_t m_maxOpen;
    uint64_t m_tick = 0;
    std::unordered_map<std::uint64_t, Entry> m_readers; // por aparición del clip (VideoLayer::instance)
    std::unordered_map<std::string, std::shared_ptr<const media::VideoFrame>> m_stills;
    struct CachedTitle {
        core::TitleStyle style;
        int width = 0, height = 0;
        int chars = -1;
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
    std::shared_ptr<BundleRenderer> m_bundleRenderer;
    int m_inMemory = 0; // > 0: dentro de un compuesto con efectos (sin copia cero)
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
    // Instancias OpenFX por aparición del clip y posición del efecto (el plugin guarda estado); el
    // generador y la transición van en las posiciones kGeneratorSlot y kTransitionSlot.
    struct OfxSlot {
        std::string pluginId;
        ofx::Role role = ofx::Role::Filter;
        std::unique_ptr<ofx::Effect> effect;
        bool failed = false; // no se pudo crear: no se reintenta mientras no cambie el plugin
        uint64_t lastUse = 0;
    };
    std::map<std::pair<std::uint64_t, size_t>, OfxSlot> m_ofx;
    std::string m_ofxError;
    double m_frameRate = 30.0;
    void* m_gpuDevice = nullptr;
    std::function<std::string(const core::MediaItem&)> m_pathMapper;
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
