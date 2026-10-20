#pragma once

#include "core/Animated.h"
#include "core/Effects.h"
#include "media/Time.h"

#include <cstdint>
#include <memory>
#include <optional>
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

// Espacio de color de trabajo y de salida del proyecto.
enum class ColorMode {
    SDR, // Rec.709, gamma BT.1886
    PQ,  // HDR10: Rec.2020 con PQ (SMPTE ST 2084)
    HLG, // Rec.2020 con HLG (ARIB STD-B67)
};

struct ProjectSettings {
    int width = 1920;
    int height = 1080;
    Rational frameRate{30, 1};
    int sampleRate = 48000;
    int channels = 2;
    ColorMode colorMode = ColorMode::SDR;

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
struct TranscriptWord {
    TimeUs start = 0;
    TimeUs end = 0;
    std::string text;         // sin espacios alrededor; incluye la puntuación pegada a la palabra
    float probability = 1.0f; // confianza del reconocedor (0..1)
    bool operator==(const TranscriptWord&) const = default;
};

// Transcripción del audio de un medio, en tiempos del medio (ver core/Transcript.h).
struct Transcript {
    std::string language; // código ISO 639-1 detectado o pedido ("es", "en"...)
    std::string model;    // modelo que la generó (informativo)
    std::vector<TranscriptWord> words; // ordenadas por `start`
    bool operator==(const Transcript&) const = default;
};

// Referencia compartida e inmutable que se compara por contenido.
struct TranscriptRef : std::shared_ptr<const Transcript> {
    using std::shared_ptr<const Transcript>::shared_ptr;
    TranscriptRef(std::shared_ptr<const Transcript> p) : std::shared_ptr<const Transcript>(std::move(p)) {}
    bool operator==(const TranscriptRef& o) const { return get() == o.get() || (get() && o.get() && *get() == *o.get()); }
};

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
    bool isStill = false;  // imagen fija: sin duración propia, se puede alargar sin límite
    // Transcripción del audio (inmutable y compartida: las instantáneas del historial no la copian).
    TranscriptRef transcript;
    bool operator==(const MediaItem&) const = default;
};

enum class TrackKind { Video, Audio };

// Transformación de una capa de video sobre el lienzo. La capa parte ajustada al lienzo
// (conservando su aspecto); x/y desplazan su centro en fracciones del ancho/alto del lienzo.
struct Transform {
    Animated x{0.0};
    Animated y{0.0};
    Animated scale{1.0};
    Animated rotation{0.0}; // grados, sentido horario
    Animated opacity{1.0};
    // Recorte de cada borde en fracción del ancho/alto de la capa (0 = sin recorte).
    Animated cropLeft{0.0};
    Animated cropTop{0.0};
    Animated cropRight{0.0};
    Animated cropBottom{0.0};
    bool operator==(const Transform&) const = default;
};

// Estilo de un clip de título (texto generado).
struct TitleStyle {
    std::string text = "Título";
    std::string font = "Segoe UI";
    double size = 0.09;              // altura de letra en fracción del alto del lienzo
    std::uint32_t color = 0xFFFFFFFF; // ARGB
    bool bold = true;
    bool italic = false;
    int align = 1;                   // 0 izquierda, 1 centro, 2 derecha
    double outline = 0.0;            // grosor del contorno en fracción de `size`
    std::uint32_t outlineColor = 0xFF000000;
    bool shadow = true;
    std::uint32_t background = 0;    // ARGB del recuadro de fondo (0 = sin fondo)
    bool operator==(const TitleStyle&) const = default;
};

enum class TransitionType { None, Dissolve, DipToBlack, WipeLeft, WipeRight, PushLeft, PushRight, SlideLeft };

// Transición desde el clip anterior de la misma pista. Solo tiene efecto si ambos clips son
// contiguos; se centra en el corte y usa material de más allá de los bordes (o lo congela).
struct TransitionIn {
    TransitionType type = TransitionType::None;
    FramePos duration = 0;
    bool active() const { return type != TransitionType::None && duration > 0; }
    bool operator==(const TransitionIn&) const = default;
};

struct Clip {
    Id id = kNoId;
    Id mediaId = kNoId;    // kNoId en los clips generados (títulos) y en los compuestos
    Id sequenceId = kNoId; // clip compuesto: la secuencia que contiene (sourceIn es su instante)
    FramePos start = 0;    // posición en la línea de tiempo
    FramePos duration = 0;
    TimeUs sourceIn = 0;   // instante del medio que se ve/oye en `start`
    double speed = 1.0;    // velocidad de reproducción del medio (2 = el doble de rápido)
    bool enabled = true;

    Transform transform;
    Animated volume{1.0};  // ganancia lineal del audio del clip
    FramePos fadeIn = 0;   // fundido de opacidad al entrar/salir (fotogramas)
    FramePos fadeOut = 0;
    FramePos audioFadeIn = 0;
    FramePos audioFadeOut = 0;
    bool audioDetached = false; // el audio de este clip de video se separó a otra pista
    double denoise = 0.0;       // reducción de ruido del audio (0 = ninguna, 1 = máxima)
    std::optional<TitleStyle> title;
    TransitionIn transitionIn;
    AnimatedGrade color;
    ChromaKey chromaKey;
    std::vector<Mask> masks; // se combinan en orden (ver MaskMode)
    AiMatte matte;           // recorte por IA (se multiplica con las máscaras)
    BlendMode blend = BlendMode::Normal;

    FramePos end() const { return start + duration; }
    bool isCompound() const { return sequenceId != kNoId; }
    bool isGenerated() const { return title.has_value() || isCompound(); }
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
    double volume = 1.0;   // ganancia de la pista (se multiplica por la de cada clip)
    std::vector<Clip> clips;
    bool operator==(const Track&) const = default;
};

// Marcador de la línea de tiempo (p. ej. los pulsos detectados en la música).
struct Marker {
    FramePos frame = 0;
    std::string label;
    std::uint32_t color = 0xFFFFC266; // ARGB
    bool operator==(const Marker&) const = default;
};

// Secuencia guardada: el contenido de un clip compuesto o, mientras se edita uno, la principal.
struct Sequence {
    Id id = kNoId;
    std::string name;
    std::vector<Track> videoTracks;
    std::vector<Track> audioTracks;
    std::vector<Marker> markers; // ordenados por fotograma, sin repetir
    bool operator==(const Sequence&) const = default;
};

struct Project {
    ProjectSettings settings;
    std::vector<MediaItem> media;
    // Pistas de video: índice 0 = V1, la capa inferior; las siguientes se componen encima.
    std::vector<Track> videoTracks;
    // Pistas de audio: A1, A2...
    std::vector<Track> audioTracks;
    std::vector<Marker> markers; // de la secuencia activa, ordenados por fotograma
    // Las pistas de arriba son las de la secuencia que se está editando (`activeSequence`); las
    // demás se guardan en `sequences`. La principal (`mainSequence`) es la que se exporta.
    std::vector<Sequence> sequences;
    Id mainSequence = kNoId;
    Id activeSequence = kNoId;
    std::string activeName;       // nombre de la secuencia activa
    std::vector<Id> sequencePath; // secuencias abiertas antes de la activa (para volver)
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

    const Sequence* findSequence(Id id) const;
    // Pistas de una secuencia: las activas si es la que se edita; nullptr si no existe.
    const std::vector<Track>* videoTracksOf(Id sequence) const;
    const std::vector<Track>* audioTracksOf(Id sequence) const;
    FramePos sequenceDuration(Id sequence) const;
    std::string sequenceName(Id sequence) const;
    // Todas las pistas: las activas y las de las secuencias guardadas.
    template <typename F>
    void forEachTrack(F&& f)
    {
        for (auto* list : {&videoTracks, &audioTracks})
            for (Track& t : *list)
                f(t);
        for (Sequence& seq : sequences)
            for (auto* list : {&seq.videoTracks, &seq.audioTracks})
                for (Track& t : *list)
                    f(t);
    }

    FramePos duration() const; // final del último clip
    TimeUs durationTime() const { return settings.frameToTime(duration()); }

    // Comprueba los invariantes (orden, solapes, referencias, ids únicos). Para pruebas y carga.
    bool validate(std::string* why = nullptr) const;

    bool operator==(const Project&) const = default;
};

std::string trackKindName(TrackKind kind);

// Duración que se considera disponible en imágenes fijas y títulos (se pueden alargar "sin límite").
inline constexpr TimeUs kUnlimitedDuration = 24LL * 3600 * kUsPerSecond;

// Instante del medio que muestra el clip en el fotograma `frame` de la línea de tiempo
// (puede quedar fuera del clip: lo usan las transiciones).
TimeUs sourceTimeAt(const ProjectSettings& s, const Clip& c, FramePos frame);
// Lo mismo para un instante de la línea de tiempo (audio, exacto al microsegundo).
TimeUs sourceTimeAtTime(const ProjectSettings& s, const Clip& c, TimeUs t);

} // namespace movix::core
