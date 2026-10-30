#pragma once

#include "core/Animated.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Anfitrión de efectos OpenFX (plugins .ofx de terceros, API C de la Academy Software Foundation).
// Se buscan en las carpetas estándar, se describen una vez en cada papel que admiten (filtro,
// generador o transición) y cada uso tiene su propia instancia. Se renderiza en la CPU, el
// fotograma completo, en 8 bits o en coma flotante según lo que admita el plugin; los parámetros
// pueden tener keyframes (el plugin los lee en cualquier instante). La interfaz propia del plugin
// sobre el visor (overlay) la dibuja el plugin con OpenGL en una imagen aparte (ver Overlay).
namespace movix::ofx {

// Papel de un plugin: filtro de un clip (contexto Filter o General), generador de imagen
// (Generator) o transición entre dos clips (Transition). Un plugin puede tener varios.
enum class Role { Filter, Generator, Transition };

enum class ParamType { Double, Int, Bool, Choice, Rgb, Rgba, Double2D, Int2D, Double3D, Int3D, String, Group, Page, PushButton, Other };

struct ParamDef {
    std::string name;  // identificador estable (el que se guarda en el proyecto)
    std::string label; // visible
    std::string hint;
    std::string parent; // grupo
    ParamType type = ParamType::Other;
    int dimension = 1;
    std::vector<double> defaults; // `dimension` valores (bool, choice e int como números)
    double min = -1e300, max = 1e300; // intervalo para la interfaz (de cada componente)
    std::vector<std::string> options; // choice
    std::string defaultText;          // string
    bool secret = false;
    bool normalised = false; // fracciones del tamaño del proyecto (se convierten al leerlas)
    bool animates = false;   // admite keyframes
};

struct PluginInfo {
    std::string id;
    int versionMajor = 0, versionMinor = 0;
    std::string label, group, description;
    std::string binary;  // ruta del .ofx
    Role role = Role::Filter;
    std::string context; // contexto OFX: "Filter", "General", "Generator" o "Transition"
    bool floatImages = false; // si no, 8 bits
    bool hasOverlay = false;  // tiene interfaz sobre el visor
    // En el orden en que los define el plugin (pueden cambiar con el papel). El de progreso de las
    // transiciones lo pone el anfitrión: va como `secret`.
    std::vector<ParamDef> params;
};

// Plugins encontrados. Carpetas: las de OFX_PLUGIN_PATH, %CommonProgramFiles%\OFX\Plugins y las extra
// (por ejemplo, la del usuario). Se buscan la primera vez que se piden.
class Registry {
public:
    static Registry& instance();
    void setExtraDirs(std::vector<std::string> dirs);
    void rescan();
    std::vector<PluginInfo> plugins(Role role = Role::Filter);
    std::shared_ptr<const PluginInfo> find(const std::string& id, Role role = Role::Filter);
    std::vector<std::string> errors(); // plugins que no se pudieron cargar o describir
    void ensureScanned();

private:
    Registry() = default;
};

// Valor de cada parámetro: un valor animable por componente, en fotogramas del clip.
using ParamValues = std::map<std::string, std::vector<core::Animated>>;

// Valores de los parámetros con los que faltan rellenos por defecto.
ParamValues withDefaults(const PluginInfo& plugin, const ParamValues& values);

// Instancia de un efecto: el plugin guarda en ella su estado entre fotogramas.
class Effect {
public:
    static std::unique_ptr<Effect> create(const std::string& pluginId, int width, int height, double frameRate, std::string* error = nullptr,
                                          Role role = Role::Filter);
    ~Effect();
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;

    const PluginInfo& plugin() const;
    // Aplica el efecto a una imagen RGBA de 8 bits sin premultiplicar (filas de arriba abajo) del
    // tamaño con que se creó. `time`: fotograma (dentro del clip).
    bool render(const ParamValues& values, double time, const uint8_t* in, uint8_t* out, std::string* error = nullptr);
    // Igual con RGBA en coma flotante (0..1 en la señal de la imagen; 10 bits o HDR). Si el plugin
    // solo admite 8 bits, la imagen pasa por 8 bits.
    bool render(const ParamValues& values, double time, const float* in, float* out, std::string* error = nullptr);
    // Generador: la imagen sale de la nada.
    bool generate(const ParamValues& values, double time, uint8_t* out, std::string* error = nullptr);
    bool generate(const ParamValues& values, double time, float* out, std::string* error = nullptr);
    // Transición de `from` a `to` con progreso 0..1 (el plugin lo lee en su parámetro «Transition»).
    bool transition(const ParamValues& values, double time, double progress, const uint8_t* from, const uint8_t* to, uint8_t* out,
                    std::string* error = nullptr);
    bool transition(const ParamValues& values, double time, double progress, const float* from, const float* to, float* out,
                    std::string* error = nullptr);
    int width() const;
    int height() const;
    // Valores actuales de los parámetros de la instancia (los que dejó el último render o los que
    // cambió el plugin desde su interfaz).
    ParamValues values() const;

    struct Impl;

private:
    friend class Overlay;
    // `inputs`: una imagen por clip de entrada del papel (ninguna, Source o SourceFrom y SourceTo).
    bool renderImpl(const ParamValues& values, double time, std::vector<const void*> inputs, void* out, bool floatIO, std::string* error);
    explicit Effect(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

// Interfaz del plugin sobre el visor (interact de OFX) para una instancia. El plugin dibuja con
// OpenGL: se le da un contexto propio fuera de pantalla y se lee la imagen. Las coordenadas que
// recibe son las de la imagen del efecto (píxeles desde abajo a la izquierda). Se usa desde un solo
// hilo. Por ahora solo en Windows (WGL).
class Overlay {
public:
    // nullptr si el plugin no tiene interfaz o no se pudo crear. `effect` debe sobrevivirla.
    static std::unique_ptr<Overlay> create(Effect& effect, std::string* error = nullptr);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Dibuja la interfaz con `values` en el instante `time` sobre una vista de viewWidth × viewHeight
    // que cubre la imagen del efecto entera. `rgba`: RGBA premultiplicado, filas de arriba abajo.
    bool draw(const ParamValues& values, double time, int viewWidth, int viewHeight, std::vector<uint8_t>& rgba);
    enum class Pen { Down, Motion, Up };
    // Evento del ratón en (x, y), píxeles de la vista con origen arriba a la izquierda. Devuelve si el
    // plugin lo usó; `values` recibe los valores de los parámetros después (los que cambió, incluidos).
    bool pen(Pen kind, double x, double y, double time, int viewWidth, int viewHeight, ParamValues& values);
    enum class Key { Down, Up, Repeat };
    // Tecla pulsada con el visor enfocado: `qtKey` es un Qt::Key y `text`, lo que escribe (UTF-8).
    // Devuelve si el plugin la usó; `values`, como en pen().
    bool key(Key kind, int qtKey, const std::string& text, double time, int viewWidth, int viewHeight, ParamValues& values);
    // El plugin pidió redibujar desde la última vez que se preguntó.
    bool takeRedraw();

    struct Impl;

private:
    explicit Overlay(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace movix::ofx
