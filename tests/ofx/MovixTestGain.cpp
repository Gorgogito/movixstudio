// Plugins OpenFX de prueba (con la biblioteca de apoyo oficial, ofxs*), compilados aparte como los de
// terceros, para comprobar el anfitrión de MovixStudio:
// - Ganancia (filtro): multiplica el color por una ganancia y un tinte, opcionalmente solo en un
//   canal, y puede invertirlo.
// - Rectángulo (generador y filtro): un rectángulo de color centrado; como generador, sobre fondo
//   transparente; como filtro, encima del clip.
// - Barrido (transición): la imagen de destino entra desde la izquierda, o por mezcla.
#include "ofxsImageEffect.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

// Valor del canal c del píxel (0..1), en 8 bits o en coma flotante.
double channel(const void* p, int c, bool isFloat)
{
    return isFloat ? static_cast<const float*>(p)[c] : static_cast<const unsigned char*>(p)[c] / 255.0;
}

void store(void* p, int c, double v, bool isFloat)
{
    v = std::clamp(v, 0.0, 1.0);
    if (isFloat)
        static_cast<float*>(p)[c] = float(v);
    else
        static_cast<unsigned char*>(p)[c] = static_cast<unsigned char>(v * 255.0 + 0.5);
}

void describeCommon(OFX::ImageEffectDescriptor& desc)
{
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setSupportsTiles(false);
    desc.setSupportsMultiResolution(false);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
}

// ---------------------------------------------------------------- Rectángulo

const char* const kRectId = "io.github.gorgogito.movixstudio.test.Rectangle";

class RectPlugin : public OFX::ImageEffect {
public:
    explicit RectPlugin(OfxImageEffectHandle handle, OFX::ContextEnum context) : OFX::ImageEffect(handle)
    {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        if (context == OFX::eContextFilter)
            src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        color_ = fetchRGBAParam("color");
        size_ = fetchDoubleParam("size");
    }

    void render(const OFX::RenderArguments& args) override
    {
        std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
        std::unique_ptr<OFX::Image> src(src_ ? src_->fetchImage(args.time) : nullptr);
        if (!dst || (src_ && !src))
            OFX::throwSuiteStatusException(kOfxStatFailed);
        double color[4];
        color_->getValueAtTime(args.time, color[0], color[1], color[2], color[3]);
        const double size = size_->getValueAtTime(args.time);
        const bool isFloat = dst->getPixelDepth() == OFX::eBitDepthFloat;
        const OfxRectI b = dst->getBounds();
        const double w = b.x2 - b.x1, h = b.y2 - b.y1;
        const double x0 = b.x1 + w * (1 - size) / 2, x1 = b.x1 + w * (1 + size) / 2;
        const double y0 = b.y1 + h * (1 - size) / 2, y1 = b.y1 + h * (1 + size) / 2;
        const OfxRectI win = args.renderWindow;
        for (int y = win.y1; y < win.y2; ++y)
            for (int x = win.x1; x < win.x2; ++x) {
                void* d = dst->getPixelAddress(x, y);
                if (!d)
                    continue;
                const bool inside = x + 0.5 >= x0 && x + 0.5 < x1 && y + 0.5 >= y0 && y + 0.5 < y1;
                const void* s = src ? src->getPixelAddress(x, y) : nullptr;
                for (int c = 0; c < 4; ++c)
                    store(d, c, inside ? color[c] : s ? channel(s, c, isFloat) : 0.0, isFloat);
            }
    }

private:
    OFX::Clip* dst_ = nullptr;
    OFX::Clip* src_ = nullptr;
    OFX::RGBAParam* color_ = nullptr;
    OFX::DoubleParam* size_ = nullptr;
};

class RectFactory : public OFX::PluginFactoryHelper<RectFactory> {
public:
    RectFactory() : OFX::PluginFactoryHelper<RectFactory>(kRectId, 1, 0) {}

    void describe(OFX::ImageEffectDescriptor& desc) override
    {
        desc.setLabels("Rectángulo de prueba", "Rectángulo", "Rectángulo de prueba");
        desc.setPluginGrouping("MovixStudio/Pruebas");
        desc.addSupportedContext(OFX::eContextGenerator);
        desc.addSupportedContext(OFX::eContextFilter);
        describeCommon(desc);
    }

    void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) override
    {
        if (context == OFX::eContextFilter)
            desc.defineClip(kOfxImageEffectSimpleSourceClipName)->addSupportedComponent(OFX::ePixelComponentRGBA);
        desc.defineClip(kOfxImageEffectOutputClipName)->addSupportedComponent(OFX::ePixelComponentRGBA);
        OFX::RGBAParamDescriptor* color = desc.defineRGBAParam("color");
        color->setLabels("Color", "Color", "Color");
        color->setDefault(1.0, 0.0, 0.0, 1.0);
        OFX::DoubleParamDescriptor* size = desc.defineDoubleParam("size");
        size->setLabels("Tamaño", "Tamaño", "Tamaño");
        size->setDefault(0.5);
        size->setRange(0.0, 1.0);
        size->setDisplayRange(0.0, 1.0);
        // Solo como generador: un parámetro más (cada contexto describe los suyos).
        if (context == OFX::eContextGenerator) {
            OFX::BooleanParamDescriptor* unused = desc.defineBooleanParam("generatorOnly");
            unused->setLabels("Solo del generador", "Solo del generador", "Solo del generador");
        }
    }

    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override { return new RectPlugin(handle, context); }
};

// ---------------------------------------------------------------- Barrido

const char* const kWipeId = "io.github.gorgogito.movixstudio.test.Wipe";

class WipePlugin : public OFX::ImageEffect {
public:
    explicit WipePlugin(OfxImageEffectHandle handle) : OFX::ImageEffect(handle)
    {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        from_ = fetchClip(kOfxImageEffectTransitionSourceFromClipName);
        to_ = fetchClip(kOfxImageEffectTransitionSourceToClipName);
        transition_ = fetchDoubleParam(kOfxImageEffectTransitionParamName);
        mix_ = fetchBooleanParam("mix");
    }

    void render(const OFX::RenderArguments& args) override
    {
        std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
        std::unique_ptr<OFX::Image> from(from_->fetchImage(args.time));
        std::unique_ptr<OFX::Image> to(to_->fetchImage(args.time));
        if (!dst || !from || !to)
            OFX::throwSuiteStatusException(kOfxStatFailed);
        const double t = transition_->getValueAtTime(args.time);
        const bool mix = mix_->getValueAtTime(args.time);
        const bool isFloat = dst->getPixelDepth() == OFX::eBitDepthFloat;
        const OfxRectI b = dst->getBounds();
        const double edge = b.x1 + (b.x2 - b.x1) * t;
        const OfxRectI win = args.renderWindow;
        for (int y = win.y1; y < win.y2; ++y)
            for (int x = win.x1; x < win.x2; ++x) {
                void* d = dst->getPixelAddress(x, y);
                const void* f = from->getPixelAddress(x, y);
                const void* g = to->getPixelAddress(x, y);
                if (!d || !f || !g)
                    continue;
                for (int c = 0; c < 4; ++c) {
                    const double a = channel(f, c, isFloat), z = channel(g, c, isFloat);
                    store(d, c, mix ? a + (z - a) * t : x + 0.5 < edge ? z : a, isFloat);
                }
            }
    }

private:
    OFX::Clip* dst_ = nullptr;
    OFX::Clip* from_ = nullptr;
    OFX::Clip* to_ = nullptr;
    OFX::DoubleParam* transition_ = nullptr;
    OFX::BooleanParam* mix_ = nullptr;
};

class WipeFactory : public OFX::PluginFactoryHelper<WipeFactory> {
public:
    WipeFactory() : OFX::PluginFactoryHelper<WipeFactory>(kWipeId, 1, 0) {}

    void describe(OFX::ImageEffectDescriptor& desc) override
    {
        desc.setLabels("Barrido de prueba", "Barrido", "Barrido de prueba");
        desc.setPluginGrouping("MovixStudio/Pruebas");
        desc.addSupportedContext(OFX::eContextTransition);
        describeCommon(desc);
    }

    void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum) override
    {
        desc.defineClip(kOfxImageEffectTransitionSourceFromClipName)->addSupportedComponent(OFX::ePixelComponentRGBA);
        desc.defineClip(kOfxImageEffectTransitionSourceToClipName)->addSupportedComponent(OFX::ePixelComponentRGBA);
        desc.defineClip(kOfxImageEffectOutputClipName)->addSupportedComponent(OFX::ePixelComponentRGBA);
        OFX::DoubleParamDescriptor* t = desc.defineDoubleParam(kOfxImageEffectTransitionParamName);
        t->setLabels("Transición", "Transición", "Transición");
        t->setDefault(0.0);
        t->setRange(0.0, 1.0);
        OFX::BooleanParamDescriptor* mix = desc.defineBooleanParam("mix");
        mix->setLabels("Mezclar", "Mezclar", "Mezclar");
        mix->setDefault(false);
    }

    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) override { return new WipePlugin(handle); }
};

// ---------------------------------------------------------------- Ganancia

const char* const kId = "io.github.gorgogito.movixstudio.test.Gain";

class GainPlugin : public OFX::ImageEffect {
public:
    explicit GainPlugin(OfxImageEffectHandle handle) : OFX::ImageEffect(handle)
    {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        gain_ = fetchDoubleParam("gain");
        invert_ = fetchBooleanParam("invert");
        channel_ = fetchChoiceParam("channel");
        tint_ = fetchRGBParam("tint");
    }

    void render(const OFX::RenderArguments& args) override
    {
        std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
        std::unique_ptr<OFX::Image> src(src_->fetchImage(args.time));
        if (!dst || !src)
            OFX::throwSuiteStatusException(kOfxStatFailed);
        const double gain = gain_->getValueAtTime(args.time);
        const bool invert = invert_->getValueAtTime(args.time);
        int channel = 0;
        channel_->getValueAtTime(args.time, channel);
        double tint[3];
        tint_->getValueAtTime(args.time, tint[0], tint[1], tint[2]);
        const bool isFloat = dst->getPixelDepth() == OFX::eBitDepthFloat;
        const OfxRectI w = args.renderWindow;
        for (int y = w.y1; y < w.y2; ++y) {
            for (int x = w.x1; x < w.x2; ++x) {
                void* d = dst->getPixelAddress(x, y);
                void* s = src->getPixelAddress(x, y);
                if (!d || !s)
                    continue;
                for (int c = 0; c < 4; ++c) {
                    double v = isFloat ? static_cast<float*>(s)[c] : static_cast<unsigned char*>(s)[c] / 255.0;
                    if (c < 3 && (channel == 0 || channel == c + 1)) {
                        v *= gain * tint[c];
                        if (invert)
                            v = 1.0 - v;
                    }
                    v = std::clamp(v, 0.0, 1.0);
                    if (isFloat)
                        static_cast<float*>(d)[c] = float(v);
                    else
                        static_cast<unsigned char*>(d)[c] = static_cast<unsigned char>(v * 255.0 + 0.5);
                }
            }
        }
    }

private:
    OFX::Clip* dst_ = nullptr;
    OFX::Clip* src_ = nullptr;
    OFX::DoubleParam* gain_ = nullptr;
    OFX::BooleanParam* invert_ = nullptr;
    OFX::ChoiceParam* channel_ = nullptr;
    OFX::RGBParam* tint_ = nullptr;
};

class GainFactory : public OFX::PluginFactoryHelper<GainFactory> {
public:
    GainFactory() : OFX::PluginFactoryHelper<GainFactory>(kId, 1, 2) {}

    void describe(OFX::ImageEffectDescriptor& desc) override
    {
        desc.setLabels("Ganancia de prueba", "Ganancia", "Ganancia de prueba");
        desc.setPluginGrouping("MovixStudio/Pruebas");
        desc.setPluginDescription("Multiplica el color (plugin de las pruebas de MovixStudio).");
        desc.addSupportedContext(OFX::eContextFilter);
        desc.addSupportedBitDepth(OFX::eBitDepthUByte);
        desc.addSupportedBitDepth(OFX::eBitDepthFloat);
        desc.setSingleInstance(false);
        desc.setSupportsTiles(false);
        desc.setSupportsMultiResolution(false);
        desc.setRenderThreadSafety(OFX::eRenderFullySafe);
    }

    void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum) override
    {
        OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        src->addSupportedComponent(OFX::ePixelComponentRGBA);
        OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
        dst->addSupportedComponent(OFX::ePixelComponentRGBA);

        OFX::DoubleParamDescriptor* gain = desc.defineDoubleParam("gain");
        gain->setLabels("Ganancia", "Ganancia", "Ganancia");
        gain->setDefault(1.0);
        gain->setRange(0.0, 10.0);
        gain->setDisplayRange(0.0, 4.0);
        OFX::BooleanParamDescriptor* invert = desc.defineBooleanParam("invert");
        invert->setLabels("Invertir", "Invertir", "Invertir");
        invert->setDefault(false);
        OFX::ChoiceParamDescriptor* channel = desc.defineChoiceParam("channel");
        channel->setLabels("Canal", "Canal", "Canal");
        channel->appendOption("Todos");
        channel->appendOption("Rojo");
        channel->appendOption("Verde");
        channel->appendOption("Azul");
        channel->setDefault(0);
        OFX::RGBParamDescriptor* tint = desc.defineRGBParam("tint");
        tint->setLabels("Tinte", "Tinte", "Tinte");
        tint->setDefault(1.0, 1.0, 1.0);
        OFX::PageParamDescriptor* page = desc.definePageParam("Controles");
        page->addChild(*gain);
        page->addChild(*invert);
        page->addChild(*channel);
        page->addChild(*tint);
    }

    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) override { return new GainPlugin(handle); }
};

} // namespace

namespace OFX::Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static GainFactory gain;
    static RectFactory rect;
    static WipeFactory wipe;
    ids.push_back(&gain);
    ids.push_back(&rect);
    ids.push_back(&wipe);
}
} // namespace OFX::Plugin
