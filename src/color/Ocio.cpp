#include "color/Ocio.h"

#include <OpenColorIO/OpenColorIO.h>

#include <map>
#include <mutex>

namespace OCIO = OCIO_NAMESPACE;

namespace movix::color {

namespace {

constexpr const char* kBuiltinConfig = "ocio://studio-config-latest";

struct State {
    std::mutex mutex;
    bool loaded = false;
    OCIO::ConstConfigRcPtr config;
    std::string error;
    std::vector<InputSpace> inputs;
    std::map<std::pair<std::string, int>, OCIO::ConstCPUProcessorRcPtr> processors;
};

State& state()
{
    static State s;
    return s;
}

// Debe llamarse con el mutex tomado.
void ensureLoaded(State& s)
{
    if (s.loaded)
        return;
    s.loaded = true;
    try {
        s.config = OCIO::Config::CreateFromFile(kBuiltinConfig);
        const int n = s.config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_SCENE, OCIO::COLORSPACE_ACTIVE);
        for (int i = 0; i < n; ++i) {
            const char* name = s.config->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_SCENE, OCIO::COLORSPACE_ACTIVE, i);
            OCIO::ConstColorSpaceRcPtr cs = s.config->getColorSpace(name);
            const std::string family = cs && cs->getFamily() ? cs->getFamily() : "";
            if (family.rfind("Input", 0) == 0) // espacios de cámara
                s.inputs.push_back({name, family});
        }
    } catch (const OCIO::Exception& e) {
        s.error = e.what();
        s.config.reset();
    }
}

// Pantalla del config que corresponde al modo del proyecto.
std::string displayFor(const OCIO::ConstConfigRcPtr& config, core::ColorMode mode)
{
    const char* wanted = mode == core::ColorMode::PQ ? "Rec.2100-PQ" : mode == core::ColorMode::HLG ? "Rec.2100-HLG" : "Rec.1886";
    for (int i = 0; i < config->getNumDisplays(); ++i) {
        const std::string d = config->getDisplay(i);
        if (d.find(wanted) != std::string::npos)
            return d;
    }
    return config->getDefaultDisplay();
}

} // namespace

const std::vector<InputSpace>& ocioInputSpaces()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    ensureLoaded(s);
    return s.inputs;
}

std::string ocioDescription()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    ensureLoaded(s);
    std::string desc = std::string("OpenColorIO ") + OCIO::GetVersion();
    if (s.config)
        desc += std::string(" · ") + s.config->getName();
    else
        desc += " · " + s.error;
    return desc;
}

RgbTransform ocioInputTransform(const std::string& space, core::ColorMode mode, std::string* error)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    ensureLoaded(s);
    if (!s.config) {
        if (error)
            *error = s.error;
        return {};
    }
    const auto key = std::make_pair(space, int(mode));
    auto it = s.processors.find(key);
    if (it == s.processors.end()) {
        try {
            const std::string display = displayFor(s.config, mode);
            auto dvt = OCIO::DisplayViewTransform::Create();
            dvt->setSrc(space.c_str());
            dvt->setDisplay(display.c_str());
            dvt->setView(s.config->getDefaultView(display.c_str()));
            auto processor = s.config->getProcessor(dvt);
            it = s.processors.emplace(key, processor->getDefaultCPUProcessor()).first;
        } catch (const OCIO::Exception& e) {
            if (error)
                *error = e.what();
            return {};
        }
    }
    OCIO::ConstCPUProcessorRcPtr cpu = it->second;
    return [cpu](Rgb c) {
        float px[3] = {c.r, c.g, c.b};
        cpu->applyRGB(px); // los procesadores de CPU de OCIO se pueden usar desde varios hilos
        return Rgb{px[0], px[1], px[2]};
    };
}

} // namespace movix::color
