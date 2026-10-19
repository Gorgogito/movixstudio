#pragma once

#include "core/Project.h"

#include <optional>
#include <vector>

// Operaciones de edición de la línea de tiempo. Todas mantienen los invariantes del proyecto
// (clips ordenados y sin solaparse, medios compatibles con la pista, transiciones solo entre
// clips contiguos, fundidos dentro del clip) y devuelven si hubo cambio.
namespace movix::core::edit {

// --- Medios ---
Id addMedia(Project& p, MediaItem item);         // asigna el id
bool removeMedia(Project& p, Id mediaId);        // elimina también sus clips

// --- Ajustes ---
// Cambia la velocidad del proyecto convirtiendo las posiciones de los clips para que conserven
// su instante (redondeado al fotograma más cercano de la nueva velocidad).
bool setFrameRate(Project& p, Rational rate);
bool setResolution(Project& p, int width, int height);
// Velocidad de fotogramas exacta para un valor decimal (29,97 → 30000/1001...).
Rational guessFrameRate(double fps);
// Si la línea de tiempo está vacía, ajusta resolución y velocidad del proyecto a las del medio.
bool matchSettingsToMedia(Project& p, const MediaItem& media);

// --- Pistas ---
// Inserta una pista; `position` es el índice dentro de su tipo (-1 = al final, encima en video).
Id addTrack(Project& p, TrackKind kind, int position = -1);
bool removeTrack(Project& p, Id trackId);
bool setTrackMuted(Project& p, Id trackId, bool muted);
bool setTrackHidden(Project& p, Id trackId, bool hidden);
bool setTrackLocked(Project& p, Id trackId, bool locked);
bool setTrackVolume(Project& p, Id trackId, double volume);

// --- Clips ---
// Fotogramas disponibles en el medio desde `sourceIn` a la velocidad `speed` (sin límite en imágenes).
FramePos availableFrames(const Project& p, const MediaItem& media, TimeUs sourceIn, double speed = 1.0);
// Fotogramas que el clip puede ocupar como máximo desde su inicio actual.
FramePos maxClipFrames(const Project& p, const Clip& c);

enum class PlaceMode {
    Overwrite, // lo que ocupaba el destino se recorta o se divide
    Insert,    // los clips posteriores de la pista se desplazan para hacer sitio
};

// Coloca un clip del medio en la pista. duration = 0 → todo lo que queda del medio (5 s en imágenes).
// Devuelve el id del clip o kNoId si no es posible (pista bloqueada, medio incompatible...).
Id placeClip(Project& p, Id trackId, Id mediaId, FramePos start, TimeUs sourceIn = 0, FramePos duration = 0,
             PlaceMode mode = PlaceMode::Overwrite);
Id placeTitle(Project& p, Id trackId, FramePos start, FramePos duration, const TitleStyle& style,
              PlaceMode mode = PlaceMode::Overwrite);

// Desplaza clips `delta` fotogramas y `trackOffset` pistas (dentro del mismo tipo).
// Si algún clip quedaría antes de 0, el desplazamiento se limita para todos.
bool moveClips(Project& p, const std::vector<Id>& clipIds, FramePos delta, int trackOffset = 0);

enum class Edge { Start, End };
// Recorta un borde hasta `position`, limitado por los clips vecinos y por el medio disponible.
// Devuelve la posición aplicada (que puede diferir de la pedida) o nullopt si no se pudo.
std::optional<FramePos> trimClip(Project& p, Id clipId, Edge edge, FramePos position);

// Divide un clip en `at` (estrictamente dentro). Devuelve el id de la parte derecha.
Id splitClip(Project& p, Id clipId, FramePos at);
// Divide en `at` los clips indicados (o todos los de pistas no bloqueadas si la lista está vacía).
std::vector<Id> splitAt(Project& p, FramePos at, const std::vector<Id>& clipIds = {});

// Elimina clips. Con `ripple`, los clips posteriores de la misma pista se desplazan para cerrar el hueco.
bool deleteClips(Project& p, const std::vector<Id>& clipIds, bool ripple = false);

bool setClipEnabled(Project& p, Id clipId, bool enabled);

// --- Propiedades animables ---
enum class Property { PositionX, PositionY, Scale, Rotation, Opacity, Volume };
Animated* property(Clip& c, Property prop);
const Animated* property(const Clip& c, Property prop);
// Fija el valor de la propiedad en el fotograma `rel` (relativo al clip): actualiza o crea el
// keyframe si la propiedad está animada; si no, cambia su valor constante.
bool setProperty(Project& p, Id clipId, Property prop, FramePos rel, double value);
// Crea un keyframe en `rel` con el valor actual o, si ya existe, lo quita.
bool toggleKeyframe(Project& p, Id clipId, Property prop, FramePos rel);
bool setKeyframeInterpolation(Project& p, Id clipId, Property prop, FramePos rel, Interpolation interp);
// Devuelve la propiedad a su valor por defecto y sin keyframes.
bool resetProperty(Project& p, Id clipId, Property prop);

// --- Velocidad, fundidos, transiciones, títulos ---
// Cambia la velocidad conservando el inicio; la duración se ajusta (limitada por el medio y el clip siguiente).
bool setClipSpeed(Project& p, Id clipId, double speed);
bool setFades(Project& p, Id clipId, FramePos fadeIn, FramePos fadeOut);
bool setAudioFades(Project& p, Id clipId, FramePos fadeIn, FramePos fadeOut);
// Transición desde el clip anterior contiguo. TransitionType::None la quita.
bool setTransition(Project& p, Id clipId, TransitionType type, FramePos duration);
bool setTitleStyle(Project& p, Id clipId, const TitleStyle& style);

// Separa el audio de un clip de video a una pista de audio libre (o nueva). Devuelve el clip de audio.
Id detachAudio(Project& p, Id clipId);

// --- Portapapeles ---
struct Clipboard {
    struct Item {
        Clip clip;
        TrackKind kind = TrackKind::Video;
        int trackIndex = 0;
    };
    std::vector<Item> items;
    FramePos origin = 0; // inicio del primer clip copiado
    bool empty() const { return items.empty(); }
};
Clipboard copyClips(const Project& p, const std::vector<Id>& clipIds);
// Pega en `at` conservando las distancias y las pistas (crea las que falten). Devuelve los nuevos ids.
std::vector<Id> pasteClips(Project& p, const Clipboard& clipboard, FramePos at);

// Clip de la pista que ocupa el fotograma `f`, o nullptr.
const Clip* clipAt(const Track& track, FramePos f);

// Restablece los invariantes derivados (transiciones y fundidos) tras cambios de posición.
void normalize(Project& p);

} // namespace movix::core::edit
