#pragma once

#include "core/Project.h"

#include <optional>
#include <vector>

// Operaciones de edición de la línea de tiempo. Todas mantienen los invariantes del proyecto
// (clips ordenados y sin solaparse, medios compatibles con la pista) y devuelven si hubo cambio.
// La colocación de clips sigue el modo "sobrescribir": lo que ocupaba el destino se recorta o se divide.
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

// --- Clips ---
// Fotogramas disponibles en el medio desde `sourceIn` hasta su final.
FramePos availableFrames(const Project& p, const MediaItem& media, TimeUs sourceIn);

// Coloca un clip del medio en la pista. duration = 0 → todo lo que queda del medio.
// Devuelve el id del clip o kNoId si no es posible (pista bloqueada, medio incompatible...).
Id placeClip(Project& p, Id trackId, Id mediaId, FramePos start, TimeUs sourceIn = 0, FramePos duration = 0);

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

bool setClipGain(Project& p, Id clipId, float gain);
bool setClipEnabled(Project& p, Id clipId, bool enabled);

// Clip de la pista que ocupa el fotograma `f`, o nullptr.
const Clip* clipAt(const Track& track, FramePos f);

} // namespace movix::core::edit
