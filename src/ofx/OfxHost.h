#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Anfitrión de efectos OpenFX (plugins .ofx de terceros, API C de la Academy Software Foundation).
// Se buscan en las carpetas estándar, se describen una vez y cada clip con un efecto tiene su propia
// instancia. Se renderiza en la CPU, el fotograma completo, en 8 bits o en coma flotante según lo
// que admita el plugin; sin interfaces propias (overlays) ni animación de parámetros.
namespace movix::ofx {

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
};

struct PluginInfo {
    std::string id;
    int versionMajor = 0, versionMinor = 0;
    std::string label, group, description;
    std::string binary;  // ruta del .ofx
    std::string context; // "Filter" o "General"
    bool floatImages = false; // si no, 8 bits
    std::vector<ParamDef> params; // en el orden en que los define el plugin
};

// Plugins encontrados. Carpetas: las de OFX_PLUGIN_PATH, %CommonProgramFiles%\OFX\Plugins y las extra
// (por ejemplo, la del usuario). Se buscan la primera vez que se piden.
class Registry {
public:
    static Registry& instance();
    void setExtraDirs(std::vector<std::string> dirs);
    void rescan();
    std::vector<PluginInfo> plugins();
    std::shared_ptr<const PluginInfo> find(const std::string& id);
    std::vector<std::string> errors(); // plugins que no se pudieron cargar o describir
    void ensureScanned();

private:
    Registry() = default;
};

using ParamValues = std::map<std::string, std::vector<double>>;

// Valores de los parámetros con los que faltan rellenos por defecto.
ParamValues withDefaults(const PluginInfo& plugin, const ParamValues& values);

// Instancia de un efecto: el plugin guarda en ella su estado entre fotogramas.
class Effect {
public:
    static std::unique_ptr<Effect> create(const std::string& pluginId, int width, int height, double frameRate, std::string* error = nullptr);
    ~Effect();
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;

    const PluginInfo& plugin() const;
    // Aplica el efecto a una imagen RGBA de 8 bits sin premultiplicar (filas de arriba abajo) del
    // tamaño con que se creó. `time`: fotograma (dentro del clip).
    bool render(const ParamValues& values, double time, const uint8_t* in, uint8_t* out, std::string* error = nullptr);
    int width() const;
    int height() const;

    struct Impl;

private:
    explicit Effect(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace movix::ofx
