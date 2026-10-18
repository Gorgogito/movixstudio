#pragma once

#include "core/Project.h"
#include "core/Timeline.h"
#include "engine/AudioReader.h"
#include "engine/VideoReader.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace movix::engine {

// Capa de video ya decodificada, lista para componer.
struct LayerFrame {
    core::Id clipId = core::kNoId;
    int trackIndex = 0;
    std::shared_ptr<const media::VideoFrame> frame;
};

// Todo lo necesario para dibujar un fotograma de la línea de tiempo.
struct FrameBundle {
    core::FramePos frame = 0;
    TimeUs time = 0;
    int canvasWidth = 1920;
    int canvasHeight = 1080;
    std::vector<LayerFrame> layers; // de abajo arriba
};

// Lectores de video abiertos, uno por clip (para que cada clip decodifique en orden), con un
// máximo de decodificadores abiertos: se cierra el que lleva más tiempo sin usarse.
class VideoSources {
public:
    explicit VideoSources(bool hardware = true, size_t maxOpen = 12) : m_hardware(hardware), m_maxOpen(maxOpen) {}

    std::shared_ptr<const media::VideoFrame> frame(const core::VideoLayer& layer);
    FrameBundle build(const core::Project& project, core::FramePos frame);
    void clear() { m_readers.clear(); }
    size_t openCount() const { return m_readers.size(); }

private:
    struct Entry {
        std::unique_ptr<VideoReader> reader;
        std::string path;
        uint64_t lastUse = 0;
        bool failed = false;
    };
    bool m_hardware;
    size_t m_maxOpen;
    uint64_t m_tick = 0;
    std::unordered_map<core::Id, Entry> m_readers;
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
        uint64_t lastUse = 0;
        bool failed = false;
    };
    int m_rate;
    int m_channels;
    size_t m_maxOpen;
    uint64_t m_tick = 0;
    std::unordered_map<core::Id, Entry> m_readers;
};

} // namespace movix::engine
