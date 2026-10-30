// Plugin OpenFX de prueba escrito sobre la API en C (la biblioteca de apoyo se compila sin OpenGL):
// multiplica por «gain» la mitad de abajo de la imagen (en coordenadas de OFX), con OpenGL si el
// anfitrión lo ofrece y si no en la CPU. Cuenta sus renders por OpenGL para las pruebas.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GL/gl.h>

#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxOpenGLRender.h>
#include <ofxParam.h>
#include <ofxProperty.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace {

OfxHost* g_host = nullptr;
const OfxPropertySuiteV1* g_prop = nullptr;
const OfxImageEffectSuiteV1* g_effect = nullptr;
const OfxParameterSuiteV1* g_param = nullptr;
const OfxImageEffectOpenGLRenderSuiteV1* g_gl = nullptr;
std::atomic<int> g_glRenders{0}, g_cpuRenders{0}, g_attached{0};

OfxStatus describe(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle props = nullptr;
    g_effect->getPropertySet(effect, &props);
    g_prop->propSetString(props, kOfxPropLabel, 0, "Ganancia GL de prueba");
    g_prop->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "MovixStudio");
    g_prop->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    g_prop->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    g_prop->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);
    g_prop->propSetString(props, kOfxImageEffectPropOpenGLRenderSupported, 0, "true");
    return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect)
{
    OfxPropertySetHandle clip = nullptr;
    g_effect->clipDefine(effect, kOfxImageEffectOutputClipName, &clip);
    g_prop->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    g_effect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clip);
    g_prop->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    OfxParamSetHandle params = nullptr;
    g_effect->getParamSet(effect, &params);
    OfxPropertySetHandle gain = nullptr;
    g_param->paramDefine(params, kOfxParamTypeDouble, "gain", &gain);
    g_prop->propSetString(gain, kOfxPropLabel, 0, "Ganancia");
    g_prop->propSetDouble(gain, kOfxParamPropDefault, 0, 1.0);
    g_prop->propSetDouble(gain, kOfxParamPropDisplayMin, 0, 0.0);
    g_prop->propSetDouble(gain, kOfxParamPropDisplayMax, 0, 1.0);
    return kOfxStatOK;
}

double gainAt(OfxImageEffectHandle effect, OfxTime time)
{
    OfxParamSetHandle params = nullptr;
    g_effect->getParamSet(effect, &params);
    OfxParamHandle gain = nullptr;
    double v = 1.0;
    if (g_param->paramGetHandle(params, "gain", &gain, nullptr) == kOfxStatOK)
        g_param->paramGetValueAtTime(gain, time, &v);
    return std::clamp(v, 0.0, 1.0);
}

OfxStatus renderGl(OfxImageEffectHandle effect, OfxTime time, double gain)
{
    OfxImageClipHandle src = nullptr, dst = nullptr;
    g_effect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &src, nullptr);
    g_effect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &dst, nullptr);
    OfxPropertySetHandle srcTexture = nullptr, dstTexture = nullptr;
    if (g_gl->clipLoadTexture(src, time, nullptr, nullptr, &srcTexture) != kOfxStatOK)
        return kOfxStatGLRenderFailed;
    if (g_gl->clipLoadTexture(dst, time, nullptr, nullptr, &dstTexture) != kOfxStatOK) {
        g_gl->clipFreeTexture(srcTexture);
        return kOfxStatGLRenderFailed;
    }
    int index = 0, target = 0, bounds[4] = {};
    g_prop->propGetInt(srcTexture, kOfxImageEffectPropOpenGLTextureIndex, 0, &index);
    g_prop->propGetInt(srcTexture, kOfxImageEffectPropOpenGLTextureTarget, 0, &target);
    g_prop->propGetIntN(dstTexture, kOfxImagePropBounds, 4, bounds);
    // El destino ya está enlazado: un rectángulo con la textura, la mitad de abajo multiplicada.
    glViewport(0, 0, bounds[2] - bounds[0], bounds[3] - bounds[1]);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);
    glEnable(GLenum(target));
    glBindTexture(GLenum(target), GLuint(index));
    glTexParameteri(GLenum(target), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GLenum(target), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBegin(GL_QUADS);
    glColor4d(gain, gain, gain, 1.0);
    glTexCoord2d(0, 0), glVertex2d(0, 0);
    glTexCoord2d(1, 0), glVertex2d(1, 0);
    glTexCoord2d(1, 0.5), glVertex2d(1, 0.5);
    glTexCoord2d(0, 0.5), glVertex2d(0, 0.5);
    glColor4d(1.0, 1.0, 1.0, 1.0);
    glTexCoord2d(0, 0.5), glVertex2d(0, 0.5);
    glTexCoord2d(1, 0.5), glVertex2d(1, 0.5);
    glTexCoord2d(1, 1), glVertex2d(1, 1);
    glTexCoord2d(0, 1), glVertex2d(0, 1);
    glEnd();
    glBindTexture(GLenum(target), 0);
    glDisable(GLenum(target));
    g_gl->clipFreeTexture(srcTexture);
    g_gl->clipFreeTexture(dstTexture);
    ++g_glRenders;
    return kOfxStatOK;
}

OfxStatus renderCpu(OfxImageEffectHandle effect, OfxTime time, double gain)
{
    OfxImageClipHandle src = nullptr, dst = nullptr;
    g_effect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &src, nullptr);
    g_effect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &dst, nullptr);
    OfxPropertySetHandle srcImage = nullptr, dstImage = nullptr;
    if (g_effect->clipGetImage(src, time, nullptr, &srcImage) != kOfxStatOK)
        return kOfxStatFailed;
    if (g_effect->clipGetImage(dst, time, nullptr, &dstImage) != kOfxStatOK) {
        g_effect->clipReleaseImage(srcImage);
        return kOfxStatFailed;
    }
    void *in = nullptr, *out = nullptr;
    int inRow = 0, outRow = 0, bounds[4] = {};
    char* depth = nullptr;
    g_prop->propGetPointer(srcImage, kOfxImagePropData, 0, &in);
    g_prop->propGetPointer(dstImage, kOfxImagePropData, 0, &out);
    g_prop->propGetInt(srcImage, kOfxImagePropRowBytes, 0, &inRow);
    g_prop->propGetInt(dstImage, kOfxImagePropRowBytes, 0, &outRow);
    g_prop->propGetIntN(dstImage, kOfxImagePropBounds, 4, bounds);
    g_prop->propGetString(dstImage, kOfxImageEffectPropPixelDepth, 0, &depth);
    const bool isFloat = depth && std::strcmp(depth, kOfxBitDepthFloat) == 0;
    const int w = bounds[2] - bounds[0], h = bounds[3] - bounds[1];
    for (int y = 0; y < h; ++y) {
        const double k = y < h / 2 ? gain : 1.0;
        const auto* s = static_cast<const char*>(in) + std::ptrdiff_t(y) * inRow;
        auto* d = static_cast<char*>(out) + std::ptrdiff_t(y) * outRow;
        for (int i = 0; i < w * 4; ++i) {
            const double f = i % 4 == 3 ? 1.0 : k;
            if (isFloat)
                reinterpret_cast<float*>(d)[i] = float(reinterpret_cast<const float*>(s)[i] * f);
            else
                reinterpret_cast<unsigned char*>(d)[i] = (unsigned char)(reinterpret_cast<const unsigned char*>(s)[i] * f + 0.5);
        }
    }
    g_effect->clipReleaseImage(srcImage);
    g_effect->clipReleaseImage(dstImage);
    ++g_cpuRenders;
    return kOfxStatOK;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    OfxTime time = 0;
    int openGL = 0;
    g_prop->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    g_prop->propGetInt(inArgs, kOfxImageEffectPropOpenGLEnabled, 0, &openGL);
    const double gain = gainAt(effect, time);
    return openGL && g_gl ? renderGl(effect, time, gain) : renderCpu(effect, time, gain);
}

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle)
{
    auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (std::strcmp(action, kOfxActionLoad) == 0) {
        g_prop = static_cast<const OfxPropertySuiteV1*>(g_host->fetchSuite(g_host->host, kOfxPropertySuite, 1));
        g_effect = static_cast<const OfxImageEffectSuiteV1*>(g_host->fetchSuite(g_host->host, kOfxImageEffectSuite, 1));
        g_param = static_cast<const OfxParameterSuiteV1*>(g_host->fetchSuite(g_host->host, kOfxParameterSuite, 1));
        g_gl = static_cast<const OfxImageEffectOpenGLRenderSuiteV1*>(g_host->fetchSuite(g_host->host, kOfxOpenGLRenderSuite, 1));
        return g_prop && g_effect && g_param ? kOfxStatOK : kOfxStatErrMissingHostFeature;
    }
    if (std::strcmp(action, kOfxActionDescribe) == 0)
        return describe(effect);
    if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
        return describeInContext(effect);
    if (std::strcmp(action, kOfxImageEffectActionRender) == 0)
        return render(effect, inArgs);
    if (std::strcmp(action, kOfxActionOpenGLContextAttached) == 0) {
        ++g_attached;
        return kOfxStatOK;
    }
    if (std::strcmp(action, kOfxActionOpenGLContextDetached) == 0) {
        --g_attached;
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

void setHost(OfxHost* host)
{
    g_host = host;
}

OfxPlugin g_plugin = {kOfxImageEffectPluginApi, 1, "io.github.gorgogito.movixstudio.test.GlGain", 1, 0, setHost, mainEntry};

} // namespace

extern "C" {
__declspec(dllexport) int OfxGetNumberOfPlugins()
{
    return 1;
}

__declspec(dllexport) OfxPlugin* OfxGetPlugin(int nth)
{
    return nth == 0 ? &g_plugin : nullptr;
}

// Para las pruebas: renders por OpenGL y en la CPU, y contextos OpenGL de los que sabe.
__declspec(dllexport) int MovixTestGlRenders()
{
    return g_glRenders;
}

__declspec(dllexport) int MovixTestCpuRenders()
{
    return g_cpuRenders;
}

__declspec(dllexport) int MovixTestGlAttached()
{
    return g_attached;
}
}
