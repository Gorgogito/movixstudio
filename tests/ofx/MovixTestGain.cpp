// Plugin OpenFX de prueba (con la biblioteca de apoyo oficial, ofxs*): multiplica el color por una
// ganancia y un tinte, opcionalmente solo en un canal, y puede invertirlo. Sirve para comprobar el
// anfitrión de MovixStudio con un plugin compilado aparte, como los de terceros.
#include "ofxsImageEffect.h"

#include <algorithm>
#include <memory>

namespace {

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
    static GainFactory factory;
    ids.push_back(&factory);
}
} // namespace OFX::Plugin
