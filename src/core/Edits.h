#pragma once

#include "core/Project.h"
#include "core/Transcript.h"

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
bool setColorMode(Project& p, ColorMode mode);
bool setLinearLight(Project& p, bool on);
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
// Clip generado por un plugin OpenFX (generador) con sus parámetros por defecto.
Id placeGenerator(Project& p, Id trackId, FramePos start, FramePos duration, const std::string& pluginId,
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
enum class Property {
    PositionX, PositionY, Scale, Rotation, Opacity, Volume, Speed,
    CropLeft, CropTop, CropRight, CropBottom,
    MaskX, MaskY, MaskWidth, MaskHeight, MaskRotation, MaskFeather,
};
// Las propiedades de máscara se refieren a la máscara `maskIndex` del clip (nullptr si no existe).
Animated* property(Clip& c, Property prop, int maskIndex = 0);
const Animated* property(const Clip& c, Property prop, int maskIndex = 0);
// Fija el valor de la propiedad en el fotograma `rel` (relativo al clip): actualiza o crea el
// keyframe si la propiedad está animada; si no, cambia su valor constante.
bool setProperty(Project& p, Id clipId, Property prop, FramePos rel, double value, int maskIndex = 0);
// Crea un keyframe en `rel` con el valor actual o, si ya existe, lo quita.
bool toggleKeyframe(Project& p, Id clipId, Property prop, FramePos rel, int maskIndex = 0);
bool setKeyframeInterpolation(Project& p, Id clipId, Property prop, FramePos rel, Interpolation interp, int maskIndex = 0);
// Devuelve la propiedad a su valor por defecto y sin keyframes.
bool resetProperty(Project& p, Id clipId, Property prop, int maskIndex = 0);

// --- Velocidad, fundidos, transiciones, títulos ---
// Cambia la velocidad conservando el inicio; la duración se ajusta (limitada por el medio y el clip siguiente).
bool setClipSpeed(Project& p, Id clipId, double speed);
// Reproducir el tramo del medio al revés (clips de medio con video o audio, no compuestos).
bool setReverse(Project& p, Id clipId, bool reverse);
bool setFades(Project& p, Id clipId, FramePos fadeIn, FramePos fadeOut);
bool setAudioFades(Project& p, Id clipId, FramePos fadeIn, FramePos fadeOut);
bool setDenoise(Project& p, Id clipId, double amount); // 0..1; solo clips con audio
// Transición desde el clip anterior contiguo. TransitionType::None la quita; Ofx necesita el plugin
// (si es el que ya tenía, conserva sus parámetros).
bool setTransition(Project& p, Id clipId, TransitionType type, FramePos duration, const std::string& ofxPlugin = {});
bool setTitleStyle(Project& p, Id clipId, const TitleStyle& style);

// --- Efectos de imagen (solo clips de video) ---
// Valores fuera de rango se limitan; las curvas diagonales se vacían.
ColorGrade sanitized(ColorGrade grade);
// Con keyframes de color, cambia (o crea) el de `rel`; sin ellos, la corrección de todo el clip.
bool setColorGrade(Project& p, Id clipId, const ColorGrade& grade, FramePos rel = 0);
bool toggleColorKeyframe(Project& p, Id clipId, FramePos rel);
bool setColorKeyInterpolation(Project& p, Id clipId, FramePos rel, Interpolation interp);
bool setAnimatedGrade(Project& p, Id clipId, const AnimatedGrade& grade); // pegar la corrección completa
bool setChromaKey(Project& p, Id clipId, const ChromaKey& key);
// Máscaras (hasta kMaxMasks; sus valores animables se editan como propiedades con su índice).
int addMask(Project& p, Id clipId, MaskShape shape); // devuelve el índice o -1
bool removeMask(Project& p, Id clipId, int index);
bool setMaskShape(Project& p, Id clipId, int index, MaskShape shape);
bool setMaskOptions(Project& p, Id clipId, int index, MaskMode mode, bool invert);
// Con keyframes de forma, cambia (o crea) el del fotograma `rel`; añadir o quitar un punto lo hace en
// todos los keyframes.
bool setMaskPoints(Project& p, Id clipId, int index, std::vector<PathPoint> points, FramePos rel = 0);
// Añade un keyframe con la forma en `rel` o lo quita si ya existe.
bool toggleMaskPathKey(Project& p, Id clipId, int index, FramePos rel);
bool setMaskPathKeyInterpolation(Project& p, Id clipId, int index, FramePos rel, Interpolation interp);
bool setBlendMode(Project& p, Id clipId, BlendMode mode);
// Sustituye el medio de un clip conservando su posición y duración (p. ej. por una versión
// procesada que empieza en `sourceIn`). Falla si el medio nuevo no alcanza para la duración.
bool replaceClipMedia(Project& p, Id clipId, Id mediaId, TimeUs sourceIn);
// Mate de IA del clip (solo clips de video con medio). Un AiMatte vacío lo quita.
bool setMatte(Project& p, Id clipId, const AiMatte& matte);

// Efectos OpenFX de los clips de video. Devuelven si hubo cambio. En los parámetros, `index` es el
// del efecto o kOfxGenerator (el del clip generador) o kOfxTransition (su transición de entrada,
// cuyos keyframes cuentan desde el primer fotograma de la transición).
inline constexpr int kOfxGenerator = -1;
inline constexpr int kOfxTransition = -2;
int addOfxEffect(Project& p, Id clipId, const std::string& pluginId); // índice o −1
bool removeOfxEffect(Project& p, Id clipId, int index);
bool setOfxEffectEnabled(Project& p, Id clipId, int index, bool enabled);
// Fija el valor del parámetro en `rel`: si está animado, en su keyframe (que se crea si no existe).
bool setOfxParam(Project& p, Id clipId, int index, const std::string& name, std::vector<double> value, FramePos rel = 0);
// Crea un keyframe del parámetro en `rel` con `current` (su valor actual, con los que faltan por
// defecto) o, si ya existe, lo quita. Al quitar el último, el parámetro queda fijo en ese valor.
bool toggleOfxKey(Project& p, Id clipId, int index, const std::string& name, FramePos rel, const std::vector<double>& current,
                  Interpolation interp = Interpolation::Linear);
bool setOfxKeyInterpolation(Project& p, Id clipId, int index, const std::string& name, FramePos rel, Interpolation interp);
bool moveOfxEffect(Project& p, Id clipId, int from, int to);
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

// --- Clips compuestos ---
// Mueve los clips a una secuencia nueva y los sustituye por un clip compuesto (en la pista de video
// más baja de la selección o, si solo hay audio, en la de audio; en una pista nueva si su hueco no
// queda libre). Devuelve el id del compuesto.
Id createCompound(Project& p, const std::vector<Id>& clipIds, const std::string& name);
// Devuelve el contenido visible de un compuesto a la línea de tiempo tal como se ve: con su velocidad
// (también variable) y al revés si lo estaba, aplicadas a cada clip. Las pistas de video siguen
// juntas y en orden encima de la del compuesto; donde otra pista tiene algo en ese tramo se intercala
// una nueva (nada se sobrescribe).
std::vector<Id> breakCompound(Project& p, Id clipId);
// La secuencia pasa a ser la línea de tiempo activa (la actual se guarda); closeSequence vuelve a
// la anterior del camino.
bool openSequence(Project& p, Id sequence);
bool closeSequence(Project& p);
bool renameSequence(Project& p, Id sequence, const std::string& name);
// ¿Contiene `sequence` (directa o indirectamente) a `target`?
bool sequenceContains(const Project& p, Id sequence, Id target);

// --- Multicámara ---
struct MulticamSource {
    Id mediaId = kNoId;
    TimeUs offset = 0; // instante en que empieza el medio respecto de los demás (sincronía)
    double speed = 1.0; // corrige la deriva de su reloj respecto de la referencia
};
// Crea una secuencia multicámara (un ángulo por medio con imagen, en ese orden, con su audio; los
// medios solo de audio, en pistas de audio) y coloca su clip en la pista de video `trackId` desde
// `at`, sobrescribiendo. Se ve el primer ángulo; suena el primer audio aparte si lo hay y, si no,
// el del ángulo que se ve. Devuelve el id del clip.
Id createMulticam(Project& p, const std::vector<MulticamSource>& sources, const std::string& name, Id trackId, FramePos at);
int multicamAngles(const Project& p, const Clip& c);       // ángulos con imagen (0 si no es multicámara)
int multicamAudioSources(const Project& p, const Clip& c); // ángulos + pistas de audio
bool setMulticamAngle(Project& p, Id clipId, int angle);
bool setMulticamAudio(Project& p, Id clipId, int audioAngle); // −1 = el del ángulo que se ve
// Pasa al ángulo desde `at` (dividiendo el clip si `at` no es su inicio). Devuelve el clip que lo
// muestra o kNoId.
Id switchAngle(Project& p, Id clipId, FramePos at, int angle);

// --- Transcripción y subtítulos ---
bool setTranscript(Project& p, Id mediaId, TranscriptRef transcript); // nullptr la quita
// Crea una pista de video encima de todas con un título por subtítulo (redondeados a fotogramas,
// sin solaparse) colocados en el tercio inferior. Devuelve el id de la pista.
Id addSubtitleTrack(Project& p, const std::vector<Cue>& cues, const TitleStyle& style, const std::string& name);

// Elimina tramos de la línea de tiempo en todas las pistas desbloqueadas y cierra los huecos (lo
// posterior se adelanta, de modo que video y audio siguen sincronizados). Los tramos se ordenan
// y se fusionan. Devuelve los fotogramas eliminados (0 = sin cambios).
FramePos removeRanges(Project& p, std::vector<FrameRange> ranges);

// --- Marcadores (de la secuencia activa) ---
// Añade marcadores en los fotogramas (no repite los que ya existen). Devuelve cuántos añadió.
int addMarkers(Project& p, const std::vector<FramePos>& frames, const std::string& label, std::uint32_t color = 0xFFFFC266);
bool removeMarker(Project& p, FramePos frame);
// Quita los marcadores con esa etiqueta (todos si está vacía).
bool clearMarkers(Project& p, const std::string& label = {});

// Clip de la pista que ocupa el fotograma `f`, o nullptr.
const Clip* clipAt(const Track& track, FramePos f);

// Restablece los invariantes derivados (transiciones y fundidos) tras cambios de posición.
void normalize(Project& p);

} // namespace movix::core::edit
