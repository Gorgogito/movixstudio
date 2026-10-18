#pragma once

#include "media/Time.h"

#include <cstdint>
#include <string>
#include <vector>

namespace movix::core {

using Id = std::uint64_t;
inline constexpr Id kNoId = 0;

// Posición o duración en fotogramas a la velocidad del proyecto. Las ediciones de la línea de
// tiempo trabajan siempre en fotogramas enteros: así no se acumulan errores de redondeo.
using FramePos = std::int64_t;

struct Rational {
    std::int64_t num = 30;
    std::int64_t den = 1;
    double value() const { return den ? double(num) / double(den) : 0.0; }
    bool operator==(const Rational&) const = default;
};

struct ProjectSettings {
    int width = 1920;
    int height = 1080;
    Rational frameRate{30, 1};
    int sampleRate = 48000;
    int channels = 2;

    // Instante (µs) en que empieza el fotograma `f`.
    TimeUs frameToTime(FramePos f) const;
    // Fotograma que contiene el instante `t` (redondeo hacia abajo, tolerante a ±1 µs).
    FramePos timeToFrame(TimeUs t) const;
    // Número de fotogramas completos que caben en `duration`.
    FramePos framesIn(TimeUs duration) const;
    TimeUs frameDuration() const { return frameToTime(1); }
    bool operator==(const ProjectSettings&) const = default;
};

// Archivo importado en el proyecto (la "biblioteca de medios").
struct MediaItem {
    Id id = kNoId;
    std::string path;      // ruta absoluta UTF-8
    std::string name;      // nombre visible (por defecto, el del archivo)
    TimeUs duration = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    bool operator==(const MediaItem&) const = default;
};

enum class TrackKind { Video, Audio };

struct Clip {
    Id id = kNoId;
    Id mediaId = kNoId;
    FramePos start = 0;    // posición en la línea de tiempo
    FramePos duration = 0;
    TimeUs sourceIn = 0;   // instante del medio que se ve/oye en `start`
    float gain = 1.0f;     // volumen lineal del audio del clip
    bool enabled = true;

    FramePos end() const { return start + duration; }
    bool operator==(const Clip&) const = default;
};

// Pista: clips ordenados por `start` y sin solaparse (invariante que mantienen las ediciones).
struct Track {
    Id id = kNoId;
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool muted = false;    // silencia el audio de la pista
    bool hidden = false;   // oculta el video de la pista
    bool locked = false;   // impide editarla
    std::vector<Clip> clips;
    bool operator==(const Track&) const = default;
};

struct Project {
    ProjectSettings settings;
    std::vector<MediaItem> media;
    // Pistas de video: índice 0 = V1, la capa inferior; las siguientes se componen encima.
    std::vector<Track> videoTracks;
    // Pistas de audio: A1, A2...
    std::vector<Track> audioTracks;
    Id nextId = 1;

    static Project createDefault();

    Id newId() { return nextId++; }

    const MediaItem* findMedia(Id id) const;
    MediaItem* findMedia(Id id);
    Track* findTrack(Id id);
    const Track* findTrack(Id id) const;
    std::vector<Track>& tracksOf(TrackKind kind) { return kind == TrackKind::Video ? videoTracks : audioTracks; }
    const std::vector<Track>& tracksOf(TrackKind kind) const { return kind == TrackKind::Video ? videoTracks : audioTracks; }

    struct ClipLocation {
        Track* track = nullptr;
        size_t index = 0;
        Clip* clip() const { return track ? &track->clips[index] : nullptr; }
    };
    ClipLocation locateClip(Id clipId);
    Clip* findClip(Id clipId) { return locateClip(clipId).clip(); }
    const Clip* findClip(Id clipId) const;
    const Track* trackOfClip(Id clipId) const;

    FramePos duration() const; // final del último clip
    TimeUs durationTime() const { return settings.frameToTime(duration()); }

    // Comprueba los invariantes (orden, solapes, referencias, ids únicos). Para pruebas y carga.
    bool validate(std::string* why = nullptr) const;

    bool operator==(const Project&) const = default;
};

std::string trackKindName(TrackKind kind);

} // namespace movix::core
