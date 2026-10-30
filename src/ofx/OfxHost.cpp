#include "ofx/OfxHost.h"

#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxInteract.h>
#include <ofxKeySyms.h>
#include <ofxOld.h> // kOfxInteractPropViewportSize: retirada en 1.4, la leen plugins anteriores
#include <ofxMemory.h>
#include <ofxMessage.h>
#include <ofxMultiThread.h>
#include <ofxOpenGLRender.h>
#include <ofxParam.h>
#include <ofxProperty.h>

#include <QDir>
#include <QDirIterator>
#include <QLibrary>
#include <QString>

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#include <excpt.h>
#endif

namespace movix::ofx {

namespace {

// ===================================================================== Propiedades

struct Prop {
    char type = 's'; // 's' cadena, 'd' doble, 'i' entero, 'p' puntero
    std::vector<std::string> s;
    std::vector<double> d;
    std::vector<int> i;
    std::vector<void*> p;
    int size() const
    {
        switch (type) {
        case 's': return int(s.size());
        case 'd': return int(d.size());
        case 'i': return int(i.size());
        default: return int(p.size());
        }
    }
};

// Base de todo lo que el plugin ve como un conjunto de propiedades (efectos, clips, parámetros,
// imágenes): el puntero del objeto es el del conjunto.
struct PropertySet {
    virtual ~PropertySet() = default;
    std::map<std::string, Prop, std::less<>> props;

    void setS(const char* name, std::vector<std::string> v)
    {
        Prop& p = props[name];
        p = Prop{};
        p.type = 's';
        p.s = std::move(v);
    }
    void setD(const char* name, std::vector<double> v)
    {
        Prop& p = props[name];
        p = Prop{};
        p.type = 'd';
        p.d = std::move(v);
    }
    void setI(const char* name, std::vector<int> v)
    {
        Prop& p = props[name];
        p = Prop{};
        p.type = 'i';
        p.i = std::move(v);
    }
    void setP(const char* name, void* v)
    {
        Prop& p = props[name];
        p = Prop{};
        p.type = 'p';
        p.p = {v};
    }
    const Prop* find(const char* name) const
    {
        auto it = props.find(std::string_view(name));
        return it == props.end() ? nullptr : &it->second;
    }
    std::string str(const char* name, int index = 0, std::string fallback = {}) const
    {
        const Prop* p = find(name);
        return p && p->type == 's' && index < int(p->s.size()) ? p->s[size_t(index)] : fallback;
    }
    std::vector<std::string> strs(const char* name) const
    {
        const Prop* p = find(name);
        return p && p->type == 's' ? p->s : std::vector<std::string>{};
    }
    double num(const char* name, int index = 0, double fallback = 0.0) const
    {
        const Prop* p = find(name);
        if (!p)
            return fallback;
        if (p->type == 'd' && index < int(p->d.size()))
            return p->d[size_t(index)];
        if (p->type == 'i' && index < int(p->i.size()))
            return double(p->i[size_t(index)]);
        return fallback;
    }
    void* ptr(const char* name) const
    {
        const Prop* p = find(name);
        return p && p->type == 'p' && !p->p.empty() ? p->p[0] : nullptr;
    }
    std::vector<double> nums(const char* name) const
    {
        const Prop* p = find(name);
        if (!p)
            return {};
        if (p->type == 'd')
            return p->d;
        if (p->type == 'i')
            return std::vector<double>(p->i.begin(), p->i.end());
        return {};
    }
};

PropertySet* asSet(OfxPropertySetHandle h)
{
    return reinterpret_cast<PropertySet*>(h);
}

OfxPropertySetHandle asHandle(PropertySet* s)
{
    return reinterpret_cast<OfxPropertySetHandle>(s);
}

// Las escrituras crean la propiedad si no existe: algunos plugins fijan propiedades que el
// anfitrión no conoce, y negarse les hace fallar al describirse.
Prop* propForWrite(OfxPropertySetHandle h, const char* name, char type, int index)
{
    if (!h || !name || index < 0)
        return nullptr;
    Prop& p = asSet(h)->props[name];
    if (p.size() == 0)
        p.type = type;
    if (p.type != type) {
        if ((p.type == 'd' && type == 'i') || (p.type == 'i' && type == 'd')) {
            // se conserva el tipo existente (conversión numérica)
        } else {
            p = Prop{};
            p.type = type;
        }
    }
    const size_t need = size_t(index) + 1;
    switch (p.type) {
    case 's': if (p.s.size() < need) p.s.resize(need); break;
    case 'd': if (p.d.size() < need) p.d.resize(need); break;
    case 'i': if (p.i.size() < need) p.i.resize(need); break;
    default: if (p.p.size() < need) p.p.resize(need); break;
    }
    return &p;
}

const Prop* propForRead(OfxPropertySetHandle h, const char* name, int index, OfxStatus& status)
{
    status = kOfxStatOK;
    if (!h || !name) {
        status = kOfxStatErrBadHandle;
        return nullptr;
    }
    const Prop* p = asSet(h)->find(name);
    if (!p) {
        status = kOfxStatErrUnknown;
        return nullptr;
    }
    if (index < 0 || index >= p->size()) {
        status = kOfxStatErrBadIndex;
        return nullptr;
    }
    return p;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char* name, int index, void* value)
{
    Prop* p = propForWrite(h, name, 'p', index);
    if (!p)
        return kOfxStatErrBadHandle;
    if (p->type != 'p')
        return kOfxStatErrValue;
    p->p[size_t(index)] = value;
    return kOfxStatOK;
}
OfxStatus propSetString(OfxPropertySetHandle h, const char* name, int index, const char* value)
{
    Prop* p = propForWrite(h, name, 's', index);
    if (!p)
        return kOfxStatErrBadHandle;
    if (p->type != 's')
        return kOfxStatErrValue;
    p->s[size_t(index)] = value ? value : "";
    return kOfxStatOK;
}
OfxStatus propSetDouble(OfxPropertySetHandle h, const char* name, int index, double value)
{
    Prop* p = propForWrite(h, name, 'd', index);
    if (!p)
        return kOfxStatErrBadHandle;
    if (p->type == 'd')
        p->d[size_t(index)] = value;
    else if (p->type == 'i')
        p->i[size_t(index)] = int(value);
    else
        return kOfxStatErrValue;
    return kOfxStatOK;
}
OfxStatus propSetInt(OfxPropertySetHandle h, const char* name, int index, int value)
{
    Prop* p = propForWrite(h, name, 'i', index);
    if (!p)
        return kOfxStatErrBadHandle;
    if (p->type == 'i')
        p->i[size_t(index)] = value;
    else if (p->type == 'd')
        p->d[size_t(index)] = value;
    else
        return kOfxStatErrValue;
    return kOfxStatOK;
}
OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* name, int count, void* const* value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propSetPointer(h, name, i, value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char* name, int count, const char* const* value)
{
    if (h && name && count == 0) { // lista vacía
        asSet(h)->setS(name, {});
        return kOfxStatOK;
    }
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propSetString(h, name, i, value[i]); st != kOfxStatOK)
            return st;
    if (Prop* p = h && name ? &asSet(h)->props[name] : nullptr; p && p->type == 's')
        p->s.resize(size_t(count));
    return kOfxStatOK;
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* name, int count, const double* value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propSetDouble(h, name, i, value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char* name, int count, const int* value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propSetInt(h, name, i, value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propGetPointer(OfxPropertySetHandle h, const char* name, int index, void** value)
{
    OfxStatus st;
    const Prop* p = propForRead(h, name, index, st);
    if (!p)
        return st;
    if (p->type != 'p')
        return kOfxStatErrValue;
    *value = p->p[size_t(index)];
    return kOfxStatOK;
}
OfxStatus propGetString(OfxPropertySetHandle h, const char* name, int index, char** value)
{
    OfxStatus st;
    const Prop* p = propForRead(h, name, index, st);
    if (!p)
        return st;
    if (p->type != 's')
        return kOfxStatErrValue;
    *value = const_cast<char*>(p->s[size_t(index)].c_str()); // vive mientras el conjunto
    return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle h, const char* name, int index, double* value)
{
    OfxStatus st;
    const Prop* p = propForRead(h, name, index, st);
    if (!p)
        return st;
    if (p->type == 'd')
        *value = p->d[size_t(index)];
    else if (p->type == 'i')
        *value = p->i[size_t(index)];
    else
        return kOfxStatErrValue;
    return kOfxStatOK;
}
OfxStatus propGetInt(OfxPropertySetHandle h, const char* name, int index, int* value)
{
    OfxStatus st;
    const Prop* p = propForRead(h, name, index, st);
    if (!p)
        return st;
    if (p->type == 'i')
        *value = p->i[size_t(index)];
    else if (p->type == 'd')
        *value = int(p->d[size_t(index)]);
    else
        return kOfxStatErrValue;
    return kOfxStatOK;
}
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* name, int count, void** value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propGetPointer(h, name, i, &value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propGetStringN(OfxPropertySetHandle h, const char* name, int count, char** value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propGetString(h, name, i, &value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* name, int count, double* value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propGetDouble(h, name, i, &value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propGetIntN(OfxPropertySetHandle h, const char* name, int count, int* value)
{
    for (int i = 0; i < count; ++i)
        if (OfxStatus st = propGetInt(h, name, i, &value[i]); st != kOfxStatOK)
            return st;
    return kOfxStatOK;
}
OfxStatus propReset(OfxPropertySetHandle h, const char* name)
{
    return h && name && asSet(h)->find(name) ? kOfxStatOK : kOfxStatErrUnknown;
}
OfxStatus propGetDimension(OfxPropertySetHandle h, const char* name, int* count)
{
    if (!h || !name)
        return kOfxStatErrBadHandle;
    const Prop* p = asSet(h)->find(name);
    if (!p)
        return kOfxStatErrUnknown;
    *count = p->size();
    return kOfxStatOK;
}

const OfxPropertySuiteV1 kPropertySuite = {
    propSetPointer,  propSetString,  propSetDouble,  propSetInt,     propSetPointerN, propSetStringN,   propSetDoubleN,
    propSetIntN,     propGetPointer, propGetString,  propGetDouble,  propGetInt,      propGetPointerN,  propGetStringN,
    propGetDoubleN,  propGetIntN,    propReset,      propGetDimension,
};

// ===================================================================== Efectos, clips y parámetros

struct EffectObj;

struct ParamObj : PropertySet {
    std::string name;
    std::string type;
    std::vector<core::Animated> curve; // un valor animable por componente (en fotogramas del clip)
    std::string text;
    EffectObj* owner = nullptr;
};

struct ParamSetObj {
    EffectObj* owner = nullptr;
    std::vector<std::unique_ptr<ParamObj>> list;
    ParamObj* find(const char* name)
    {
        for (auto& p : list)
            if (p->name == name)
                return p.get();
        return nullptr;
    }
};

struct ClipObj : PropertySet {
    std::string name;
    EffectObj* owner = nullptr;
};

struct ImageObj : PropertySet {};

struct LoadedPlugin;

struct EffectObj : PropertySet {
    bool instance = false;
    LoadedPlugin* plugin = nullptr;
    ParamSetObj params;
    std::vector<std::unique_ptr<ClipObj>> clips;
    // Estado del render en curso: la imagen de cada clip conectado (por su nombre).
    int width = 0, height = 0;
    bool floatImages = false;
    std::vector<std::pair<std::string, void*>> images;
    int rowBytes = 0;
    // Render por OpenGL en curso: la textura de cada clip (la de salida, ya enlazada como destino).
    std::vector<std::pair<std::string, unsigned>> textures;
    double time = 0; // instante del render (el de paramGetValue)
    std::string lastMessage;

    ClipObj* clip(const char* name)
    {
        for (auto& c : clips)
            if (c->name == name)
                return c.get();
        return nullptr;
    }
};

EffectObj* asEffect(OfxImageEffectHandle h)
{
    return reinterpret_cast<EffectObj*>(h);
}

// Un plugin en uno de sus papeles (cada papel se describe en su contexto).
struct LoadedPlugin {
    OfxPlugin* api = nullptr;
    std::shared_ptr<PluginInfo> info;
    std::shared_ptr<EffectObj> descriptor; // compartido entre los papeles del plugin
    std::unique_ptr<EffectObj> contextDescriptor;
    std::string context; // kOfxImageEffectContext...
    bool unsafe = false;   // el plugin pide un único render a la vez (en todos sus papeles)
    std::shared_ptr<std::mutex> renderMutex = std::make_shared<std::mutex>();
};

// Clips de entrada de cada papel, en el orden en que Effect::renderImpl recibe sus imágenes.
std::vector<const char*> inputClips(Role role)
{
    switch (role) {
    case Role::Filter: return {kOfxImageEffectSimpleSourceClipName};
    case Role::Generator: return {};
    case Role::Transition: return {kOfxImageEffectTransitionSourceFromClipName, kOfxImageEffectTransitionSourceToClipName};
    }
    return {};
}

// --------------------------------------------------------------------- Suite de efectos

OfxStatus getPropertySet(OfxImageEffectHandle effect, OfxPropertySetHandle* props)
{
    if (!effect)
        return kOfxStatErrBadHandle;
    *props = asHandle(asEffect(effect));
    return kOfxStatOK;
}

OfxStatus getParamSet(OfxImageEffectHandle effect, OfxParamSetHandle* paramSet)
{
    if (!effect)
        return kOfxStatErrBadHandle;
    *paramSet = reinterpret_cast<OfxParamSetHandle>(&asEffect(effect)->params);
    return kOfxStatOK;
}

OfxStatus clipDefine(OfxImageEffectHandle effect, const char* name, OfxPropertySetHandle* props)
{
    if (!effect || !name)
        return kOfxStatErrBadHandle;
    EffectObj* e = asEffect(effect);
    ClipObj* c = e->clip(name);
    if (!c) {
        auto clip = std::make_unique<ClipObj>();
        clip->name = name;
        clip->owner = e;
        clip->setS(kOfxPropType, {kOfxTypeClip});
        clip->setS(kOfxPropName, {name});
        clip->setS(kOfxPropLabel, {name});
        clip->setS(kOfxPropShortLabel, {name});
        clip->setS(kOfxPropLongLabel, {name});
        clip->setS(kOfxImageEffectPropSupportedComponents, {});
        clip->setI(kOfxImageEffectPropTemporalClipAccess, {0});
        clip->setI(kOfxImageClipPropOptional, {0});
        clip->setI(kOfxImageClipPropIsMask, {0});
        clip->setS(kOfxImageClipPropFieldExtraction, {kOfxImageFieldDoubled});
        clip->setI(kOfxImageEffectPropSupportsTiles, {1});
        c = clip.get();
        e->clips.push_back(std::move(clip));
    }
    *props = asHandle(c);
    return kOfxStatOK;
}

OfxStatus clipGetHandle(OfxImageEffectHandle effect, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props)
{
    if (!effect || !name)
        return kOfxStatErrBadHandle;
    ClipObj* c = asEffect(effect)->clip(name);
    if (!c)
        return kOfxStatErrUnknown;
    *clip = reinterpret_cast<OfxImageClipHandle>(c);
    if (props)
        *props = asHandle(c);
    return kOfxStatOK;
}

OfxStatus clipGetPropertySet(OfxImageClipHandle clip, OfxPropertySetHandle* props)
{
    if (!clip)
        return kOfxStatErrBadHandle;
    *props = asHandle(reinterpret_cast<ClipObj*>(clip));
    return kOfxStatOK;
}

OfxStatus clipGetImage(OfxImageClipHandle clip, OfxTime time, const OfxRectD*, OfxPropertySetHandle* image)
{
    if (!clip)
        return kOfxStatErrBadHandle;
    ClipObj* c = reinterpret_cast<ClipObj*>(clip);
    EffectObj* e = c->owner;
    void* data = nullptr;
    for (const auto& [name, pixels] : e->images)
        if (name == c->name)
            data = pixels;
    if (!data)
        return kOfxStatFailed; // clips sin conectar (máscaras opcionales...)
    auto* img = new ImageObj;
    img->setS(kOfxPropType, {kOfxTypeImage});
    img->setS(kOfxImageEffectPropPixelDepth, {e->floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte});
    img->setS(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
    img->setS(kOfxImageEffectPropPreMultiplication, {kOfxImageUnPreMultiplied});
    img->setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    img->setD(kOfxImagePropPixelAspectRatio, {1.0});
    img->setP(kOfxImagePropData, data);
    img->setI(kOfxImagePropBounds, {0, 0, e->width, e->height});
    img->setI(kOfxImagePropRegionOfDefinition, {0, 0, e->width, e->height});
    img->setI(kOfxImagePropRowBytes, {e->rowBytes});
    img->setS(kOfxImagePropField, {kOfxImageFieldNone});
    char id[64];
    std::snprintf(id, sizeof id, "movix-%p-%g", data, time);
    img->setS(kOfxImagePropUniqueIdentifier, {id});
    *image = asHandle(img);
    return kOfxStatOK;
}

// ---- Render por OpenGL: las imágenes de los clips como texturas.

OfxStatus clipLoadTexture(OfxImageClipHandle clip, OfxTime time, const char*, const OfxRectD*, OfxPropertySetHandle* textureHandle)
{
    if (!clip || !textureHandle)
        return kOfxStatErrBadHandle;
    ClipObj* c = reinterpret_cast<ClipObj*>(clip);
    EffectObj* e = c->owner;
    unsigned texture = 0;
    for (const auto& [name, t] : e->textures)
        if (name == c->name)
            texture = t;
    if (!texture)
        return kOfxStatFailed;
    auto* img = new ImageObj;
    img->setS(kOfxPropType, {kOfxTypeImage});
    img->setI(kOfxImageEffectPropOpenGLTextureIndex, {int(texture)});
    img->setI(kOfxImageEffectPropOpenGLTextureTarget, {0x0DE1}); // GL_TEXTURE_2D
    img->setS(kOfxImageEffectPropPixelDepth, {e->floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte});
    img->setS(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
    img->setS(kOfxImageEffectPropPreMultiplication, {kOfxImageUnPreMultiplied});
    img->setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    img->setD(kOfxImagePropPixelAspectRatio, {1.0});
    img->setI(kOfxImagePropBounds, {0, 0, e->width, e->height});
    img->setI(kOfxImagePropRegionOfDefinition, {0, 0, e->width, e->height});
    img->setI(kOfxImagePropRowBytes, {e->width * 4 * (e->floatImages ? 4 : 1)});
    img->setS(kOfxImagePropField, {kOfxImageFieldNone});
    char id[64];
    std::snprintf(id, sizeof id, "movix-gl-%u-%g", texture, time);
    img->setS(kOfxImagePropUniqueIdentifier, {id});
    *textureHandle = asHandle(img);
    return kOfxStatOK;
}

OfxStatus clipFreeTexture(OfxPropertySetHandle textureHandle)
{
    if (!textureHandle)
        return kOfxStatErrBadHandle;
    delete static_cast<ImageObj*>(asSet(textureHandle)); // las texturas son del anfitrión
    return kOfxStatOK;
}

OfxStatus flushResources()
{
    return kOfxStatOK;
}

const OfxImageEffectOpenGLRenderSuiteV1 kOpenGLRenderSuite = {clipLoadTexture, clipFreeTexture, flushResources};

OfxStatus clipReleaseImage(OfxPropertySetHandle image)
{
    if (!image)
        return kOfxStatErrBadHandle;
    delete static_cast<ImageObj*>(asSet(image));
    return kOfxStatOK;
}

OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle clip, OfxTime, OfxRectD* bounds)
{
    if (!clip)
        return kOfxStatErrBadHandle;
    const EffectObj* e = reinterpret_cast<ClipObj*>(clip)->owner;
    *bounds = {0.0, 0.0, double(e->width), double(e->height)};
    return kOfxStatOK;
}

int abortRender(OfxImageEffectHandle)
{
    return 0;
}

struct MemoryObj {
    void* data = nullptr;
};

OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t bytes, OfxImageMemoryHandle* handle)
{
    auto* m = new MemoryObj;
    m->data = std::malloc(std::max<size_t>(bytes, 1)); // alineada a 16 bytes en Windows de 64 bits
    if (!m->data) {
        delete m;
        return kOfxStatErrMemory;
    }
    *handle = reinterpret_cast<OfxImageMemoryHandle>(m);
    return kOfxStatOK;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle handle)
{
    auto* m = reinterpret_cast<MemoryObj*>(handle);
    if (!m)
        return kOfxStatErrBadHandle;
    std::free(m->data);
    delete m;
    return kOfxStatOK;
}
OfxStatus imageMemoryLock(OfxImageMemoryHandle handle, void** ptr)
{
    auto* m = reinterpret_cast<MemoryObj*>(handle);
    if (!m)
        return kOfxStatErrBadHandle;
    *ptr = m->data;
    return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle handle)
{
    return handle ? kOfxStatOK : kOfxStatErrBadHandle;
}

const OfxImageEffectSuiteV1 kImageEffectSuite = {
    getPropertySet, getParamSet,     clipDefine,      clipGetHandle,   clipGetPropertySet, clipGetImage,      clipReleaseImage,
    clipGetRegionOfDefinition, abortRender, imageMemoryAlloc, imageMemoryFree, imageMemoryLock, imageMemoryUnlock,
};

// --------------------------------------------------------------------- Suite de parámetros

int dimensionOf(const std::string& type)
{
    if (type == kOfxParamTypeRGB || type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D)
        return 3;
    if (type == kOfxParamTypeRGBA)
        return 4;
    if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D)
        return 2;
    return 1;
}

bool isIntegerType(const std::string& type)
{
    return type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice
        || type == kOfxParamTypeInteger2D || type == kOfxParamTypeInteger3D;
}

bool isStringType(const std::string& type)
{
    return type == kOfxParamTypeString || type == kOfxParamTypeCustom;
}

OfxStatus paramDefine(OfxParamSetHandle paramSet, const char* type, const char* name, OfxPropertySetHandle* props)
{
    auto* set = reinterpret_cast<ParamSetObj*>(paramSet);
    if (!set || !type || !name)
        return kOfxStatErrBadHandle;
    if (set->find(name))
        return kOfxStatErrExists;
    auto p = std::make_unique<ParamObj>();
    p->name = name;
    p->type = type;
    p->owner = set->owner;
    const int dim = dimensionOf(type);
    p->setS(kOfxPropType, {kOfxTypeParameter});
    p->setS(kOfxParamPropType, {type});
    p->setS(kOfxPropName, {name});
    p->setS(kOfxPropLabel, {name});
    p->setS(kOfxPropShortLabel, {name});
    p->setS(kOfxPropLongLabel, {name});
    p->setS(kOfxParamPropHint, {""});
    p->setS(kOfxParamPropScriptName, {name});
    p->setS(kOfxParamPropParent, {""});
    p->setI(kOfxParamPropSecret, {0});
    p->setI(kOfxParamPropEnabled, {1});
    p->setI(kOfxParamPropCanUndo, {1});
    p->setI(kOfxParamPropEvaluateOnChange, {1});
    p->setI(kOfxParamPropPersistant, {1});
    p->setI(kOfxParamPropAnimates, {isStringType(type) ? 0 : 1});
    p->setI(kOfxParamPropIsAnimating, {0});
    p->setI(kOfxParamPropIsAutoKeying, {0});
    p->setI(kOfxParamPropPluginMayWrite, {0});
    p->setP(kOfxParamPropDataPtr, nullptr);
    p->setS(kOfxParamPropCacheInvalidation, {kOfxParamInvalidateValueChange});
    if (isStringType(type)) {
        p->setS(kOfxParamPropDefault, {""});
        p->setS(kOfxParamPropStringMode, {kOfxParamStringIsSingleLine});
    } else if (isIntegerType(type)) {
        p->setI(kOfxParamPropDefault, std::vector<int>(size_t(dim), 0));
        p->setI(kOfxParamPropMin, std::vector<int>(size_t(dim), INT32_MIN));
        p->setI(kOfxParamPropMax, std::vector<int>(size_t(dim), INT32_MAX));
        p->setI(kOfxParamPropDisplayMin, std::vector<int>(size_t(dim), INT32_MIN));
        p->setI(kOfxParamPropDisplayMax, std::vector<int>(size_t(dim), INT32_MAX));
        if (std::string_view(type) == kOfxParamTypeChoice)
            p->setS(kOfxParamPropChoiceOption, {});
    } else if (dim >= 1 && std::string_view(type) != kOfxParamTypeGroup && std::string_view(type) != kOfxParamTypePage
               && std::string_view(type) != kOfxParamTypePushButton) {
        const bool color = std::string_view(type) == kOfxParamTypeRGB || std::string_view(type) == kOfxParamTypeRGBA;
        p->setD(kOfxParamPropDefault, std::vector<double>(size_t(dim), 0.0));
        p->setD(kOfxParamPropMin, std::vector<double>(size_t(dim), color ? 0.0 : -1e300));
        p->setD(kOfxParamPropMax, std::vector<double>(size_t(dim), color ? 1.0 : 1e300));
        p->setD(kOfxParamPropDisplayMin, std::vector<double>(size_t(dim), color ? 0.0 : -1e300));
        p->setD(kOfxParamPropDisplayMax, std::vector<double>(size_t(dim), color ? 1.0 : 1e300));
        p->setD(kOfxParamPropIncrement, {1.0});
        p->setI(kOfxParamPropDigits, {2});
        p->setS(kOfxParamPropDoubleType, {kOfxParamDoubleTypePlain});
    }
    if (std::string_view(type) == kOfxParamTypePage)
        p->setS(kOfxParamPropPageChild, {});
    *props = asHandle(p.get());
    set->list.push_back(std::move(p));
    return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle paramSet, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props)
{
    auto* set = reinterpret_cast<ParamSetObj*>(paramSet);
    if (!set || !name)
        return kOfxStatErrBadHandle;
    ParamObj* p = set->find(name);
    if (!p)
        return kOfxStatErrUnknown;
    *param = reinterpret_cast<OfxParamHandle>(p);
    if (props)
        *props = asHandle(p);
    return kOfxStatOK;
}

OfxStatus paramSetGetPropertySet(OfxParamSetHandle paramSet, OfxPropertySetHandle* props)
{
    auto* set = reinterpret_cast<ParamSetObj*>(paramSet);
    if (!set)
        return kOfxStatErrBadHandle;
    *props = asHandle(set->owner); // el mismo que el del efecto
    return kOfxStatOK;
}

OfxStatus paramGetPropertySet(OfxParamHandle param, OfxPropertySetHandle* props)
{
    if (!param)
        return kOfxStatErrBadHandle;
    *props = asHandle(reinterpret_cast<ParamObj*>(param));
    return kOfxStatOK;
}

// Escala de cada componente de un parámetro en coordenadas normalizadas (al leerlo).
double normalScale(const ParamObj& p, int component)
{
    const std::string dt = p.str(kOfxParamPropDoubleType);
    const std::string coords = p.str(kOfxParamPropDefaultCoordinateSystem);
    const bool legacy = dt.find("Normalised") != std::string::npos;
    const bool normalised = legacy || coords == kOfxParamCoordinatesNormalised;
    if (!normalised || !p.owner)
        return 1.0;
    const bool x = dt.find('X') != std::string::npos, y = dt.find('Y') != std::string::npos;
    if (x && y)
        return component == 0 ? p.owner->width : p.owner->height;
    if (x)
        return p.owner->width;
    if (y)
        return p.owner->height;
    return 1.0;
}

// Valor de un componente en un instante cualquiera: los keyframes están en fotogramas enteros; entre
// dos fotogramas (desenfoque de movimiento, campos) se interpola linealmente.
double sampleAt(const core::Animated& a, double time)
{
    if (!a.isAnimated())
        return a.value;
    const double f = std::floor(time);
    const double v0 = a.at(std::int64_t(f));
    if (time == f)
        return v0;
    return v0 + (a.at(std::int64_t(f) + 1) - v0) * (time - f);
}

double componentAt(const ParamObj& p, int i, double time)
{
    return size_t(i) < p.curve.size() ? sampleAt(p.curve[size_t(i)], time) : 0.0;
}

// Fotogramas con keyframe del parámetro (los mismos en todos sus componentes), en orden.
std::vector<double> keyTimes(const ParamObj& p)
{
    std::vector<double> times;
    for (const core::Animated& a : p.curve)
        for (const core::Keyframe& k : a.keys)
            times.push_back(double(k.frame));
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

OfxStatus getValueV(ParamObj* p, double time, va_list ap)
{
    if (!p)
        return kOfxStatErrBadHandle;
    if (isStringType(p->type)) {
        char** out = va_arg(ap, char**);
        *out = const_cast<char*>(p->text.c_str());
        return kOfxStatOK;
    }
    const int dim = dimensionOf(p->type);
    for (int i = 0; i < dim; ++i) {
        if (isIntegerType(p->type)) {
            int* out = va_arg(ap, int*);
            *out = int(std::lround(componentAt(*p, i, time)));
        } else {
            double* out = va_arg(ap, double*);
            *out = componentAt(*p, i, time) * normalScale(*p, i);
        }
    }
    return kOfxStatOK;
}

// Lo que escribe el plugin: un keyframe si el parámetro está animado; si no, el valor fijo.
OfxStatus setValueV(ParamObj* p, double time, va_list ap)
{
    if (!p)
        return kOfxStatErrBadHandle;
    if (isStringType(p->type)) {
        const char* v = va_arg(ap, const char*);
        p->text = v ? v : "";
        return kOfxStatOK;
    }
    const int dim = dimensionOf(p->type);
    p->curve.resize(size_t(dim));
    for (int i = 0; i < dim; ++i) {
        const double v = isIntegerType(p->type) ? double(va_arg(ap, int)) : va_arg(ap, double) / normalScale(*p, i);
        p->curve[size_t(i)].set(std::llround(time), v);
    }
    return kOfxStatOK;
}

double currentTime(const ParamObj* p)
{
    return p && p->owner ? p->owner->time : 0.0;
}

OfxStatus paramGetValue(OfxParamHandle param, ...)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    va_list ap;
    va_start(ap, param);
    const OfxStatus st = getValueV(p, currentTime(p), ap);
    va_end(ap);
    return st;
}
OfxStatus paramSetValue(OfxParamHandle param, ...)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    va_list ap;
    va_start(ap, param);
    const OfxStatus st = setValueV(p, currentTime(p), ap);
    va_end(ap);
    return st;
}
OfxStatus paramGetNumKeys(OfxParamHandle param, unsigned int* n)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p || !n)
        return kOfxStatErrBadHandle;
    *n = unsigned(keyTimes(*p).size());
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle param, unsigned int nthKey, OfxTime* time)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p || !time)
        return kOfxStatErrBadHandle;
    const auto times = keyTimes(*p);
    if (nthKey >= times.size())
        return kOfxStatErrBadIndex;
    *time = times[nthKey];
    return kOfxStatOK;
}
// direction 0: keyframe en `time`; −1: el anterior; 1: el siguiente.
OfxStatus paramGetKeyIndex(OfxParamHandle param, OfxTime time, int direction, int* index)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p || !index)
        return kOfxStatErrBadHandle;
    const auto times = keyTimes(*p);
    int found = -1;
    for (int i = 0; i < int(times.size()); ++i) {
        const double t = times[size_t(i)];
        if (direction == 0 ? std::abs(t - time) < 1e-6 : direction < 0 ? t < time - 1e-6 : t > time + 1e-6) {
            found = i;
            if (direction >= 0)
                break; // el primero que cumple; hacia atrás, el último
        }
    }
    if (found < 0)
        return kOfxStatFailed;
    *index = found;
    return kOfxStatOK;
}
OfxStatus paramDeleteKey(OfxParamHandle param, OfxTime time)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p)
        return kOfxStatErrBadHandle;
    bool removed = false;
    for (core::Animated& a : p->curve)
        removed = a.removeKey(std::llround(time)) || removed;
    return removed ? kOfxStatOK : kOfxStatErrBadIndex;
}
OfxStatus paramDeleteAllKeys(OfxParamHandle param)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p)
        return kOfxStatErrBadHandle;
    for (core::Animated& a : p->curve) // queda fijo en el valor del instante actual
        a = core::Animated(sampleAt(a, currentTime(p)));
    return kOfxStatOK;
}
OfxStatus paramCopy(OfxParamHandle to, OfxParamHandle from, OfxTime offset, const OfxRangeD* range)
{
    auto* t = reinterpret_cast<ParamObj*>(to);
    auto* f = reinterpret_cast<ParamObj*>(from);
    if (!t || !f || t->type != f->type)
        return kOfxStatErrBadHandle;
    t->text = f->text;
    t->curve = f->curve;
    const auto shift = std::llround(offset);
    for (core::Animated& a : t->curve) {
        std::vector<core::Keyframe> keys;
        for (core::Keyframe k : a.keys)
            if (!range || (k.frame >= range->min && k.frame <= range->max)) {
                k.frame += shift;
                keys.push_back(k);
            }
        a.keys = std::move(keys);
    }
    return kOfxStatOK;
}
OfxStatus paramEditBegin(OfxParamSetHandle, const char*)
{
    return kOfxStatOK;
}
OfxStatus paramEditEnd(OfxParamSetHandle)
{
    return kOfxStatOK;
}

OfxStatus paramGetValueAtTimeImpl(OfxParamHandle param, OfxTime time, ...)
{
    va_list ap;
    va_start(ap, time);
    const OfxStatus st = getValueV(reinterpret_cast<ParamObj*>(param), time, ap);
    va_end(ap);
    return st;
}
// Derivada por fotograma (diferencia centrada de medio fotograma a cada lado).
OfxStatus paramGetDerivativeImpl(OfxParamHandle param, OfxTime time, ...)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p)
        return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    for (int i = 0; i < dimensionOf(p->type); ++i)
        *va_arg(ap, double*) = (componentAt(*p, i, time + 0.5) - componentAt(*p, i, time - 0.5)) * normalScale(*p, i);
    va_end(ap);
    return kOfxStatOK;
}
// Integral entre t1 y t2 por trapecios (el valor es lineal a trozos entre fotogramas, salvo los
// tramos suaves: con pasos de un fotograma como mucho el error es despreciable).
OfxStatus paramGetIntegralImpl(OfxParamHandle param, OfxTime t1, OfxTime t2, ...)
{
    auto* p = reinterpret_cast<ParamObj*>(param);
    if (!p)
        return kOfxStatErrBadHandle;
    const int steps = std::clamp(int(std::ceil(std::abs(t2 - t1))), 1, 100000);
    const double h = (t2 - t1) / steps;
    va_list ap;
    va_start(ap, t2);
    for (int i = 0; i < dimensionOf(p->type); ++i) {
        double sum = 0.0;
        for (int s = 0; s < steps; ++s)
            sum += (componentAt(*p, i, t1 + s * h) + componentAt(*p, i, t1 + (s + 1) * h)) * 0.5 * h;
        *va_arg(ap, double*) = sum * normalScale(*p, i);
    }
    va_end(ap);
    return kOfxStatOK;
}
OfxStatus paramSetValueAtTimeImpl(OfxParamHandle param, OfxTime time, ...)
{
    va_list ap;
    va_start(ap, time);
    const OfxStatus st = setValueV(reinterpret_cast<ParamObj*>(param), time, ap);
    va_end(ap);
    return st;
}

const OfxParameterSuiteV1 kParameterSuite = {
    paramDefine,       paramGetHandle,        paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
    paramGetValueAtTimeImpl, paramGetDerivativeImpl, paramGetIntegralImpl, paramSetValue, paramSetValueAtTimeImpl,
    paramGetNumKeys,   paramGetKeyTime,       paramGetKeyIndex,       paramDeleteKey,      paramDeleteAllKeys,
    paramCopy,         paramEditBegin,        paramEditEnd,
};

// --------------------------------------------------------------------- Memoria, hilos y mensajes

OfxStatus memoryAlloc(void*, size_t bytes, void** data)
{
    *data = std::malloc(std::max<size_t>(bytes, 1));
    return *data ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFree(void* data)
{
    std::free(data);
    return kOfxStatOK;
}
const OfxMemorySuiteV1 kMemorySuite = {memoryAlloc, memoryFree};

thread_local unsigned int t_threadIndex = 0;
thread_local bool t_spawned = false;

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int nThreads, void* arg)
{
    if (!func)
        return kOfxStatFailed;
    const unsigned int cpus = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int n = std::clamp(nThreads, 1u, cpus);
    if (n == 1) {
        func(0, 1, arg);
        return kOfxStatOK;
    }
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < n; ++i)
        threads.emplace_back([=] {
            t_threadIndex = i;
            t_spawned = true;
            func(i, n, arg);
        });
    for (auto& t : threads)
        t.join();
    return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int* n)
{
    *n = std::max(1u, std::thread::hardware_concurrency());
    return kOfxStatOK;
}
OfxStatus multiThreadIndex(unsigned int* index)
{
    *index = t_threadIndex;
    return kOfxStatOK;
}
int multiThreadIsSpawnedThread()
{
    return t_spawned ? 1 : 0;
}
OfxStatus mutexCreate(OfxMutexHandle* mutex, int lockCount)
{
    auto* m = new std::recursive_mutex;
    for (int i = 0; i < lockCount; ++i)
        m->lock();
    *mutex = reinterpret_cast<OfxMutexHandle>(m);
    return kOfxStatOK;
}
OfxStatus mutexDestroy(const OfxMutexHandle mutex)
{
    delete reinterpret_cast<std::recursive_mutex*>(mutex);
    return kOfxStatOK;
}
OfxStatus mutexLock(const OfxMutexHandle mutex)
{
    reinterpret_cast<std::recursive_mutex*>(mutex)->lock();
    return kOfxStatOK;
}
OfxStatus mutexUnLock(const OfxMutexHandle mutex)
{
    reinterpret_cast<std::recursive_mutex*>(mutex)->unlock();
    return kOfxStatOK;
}
OfxStatus mutexTryLock(const OfxMutexHandle mutex)
{
    return reinterpret_cast<std::recursive_mutex*>(mutex)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}
const OfxMultiThreadSuiteV1 kMultiThreadSuite = {multiThread, multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread,
                                                 mutexCreate, mutexDestroy,       mutexLock,        mutexUnLock,
                                                 mutexTryLock};

std::mutex g_messageMutex;
std::string g_lastMessage;

OfxStatus messageV(void* handle, const char* type, const char* format, va_list ap)
{
    char text[1024] = {};
    if (format)
        std::vsnprintf(text, sizeof text, format, ap);
    {
        std::lock_guard lock(g_messageMutex);
        g_lastMessage = text;
    }
    // Los mensajes a un efecto (errores al renderizar) se muestran al usuario como error del efecto.
    if (handle && type && (std::strcmp(type, kOfxMessageError) == 0 || std::strcmp(type, kOfxMessageFatal) == 0))
        reinterpret_cast<EffectObj*>(handle)->lastMessage = text;
    return kOfxStatReplyDefault; // sin interfaz para preguntas: la respuesta por defecto
}
OfxStatus message(void* handle, const char* type, const char*, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const OfxStatus st = messageV(handle, type, format, ap);
    va_end(ap);
    return st;
}
OfxStatus setPersistentMessage(void* handle, const char* type, const char*, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    messageV(handle, type, format, ap);
    va_end(ap);
    return kOfxStatOK;
}
OfxStatus clearPersistentMessage(void* handle)
{
    if (handle)
        reinterpret_cast<EffectObj*>(handle)->lastMessage.clear();
    return kOfxStatOK;
}
const OfxMessageSuiteV1 kMessageSuiteV1 = {message};
const OfxMessageSuiteV2 kMessageSuiteV2 = {message, setPersistentMessage, clearPersistentMessage};

// --------------------------------------------------------------------- Suite de interacción

struct InteractObj : PropertySet {
    bool redraw = false; // el plugin pidió redibujar
};

OfxStatus interactSwapBuffers(OfxInteractHandle h)
{
    return h ? kOfxStatOK : kOfxStatErrBadHandle;
}
OfxStatus interactRedraw(OfxInteractHandle h)
{
    if (!h)
        return kOfxStatErrBadHandle;
    reinterpret_cast<InteractObj*>(h)->redraw = true;
    return kOfxStatOK;
}
OfxStatus interactGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle* props)
{
    if (!h || !props)
        return kOfxStatErrBadHandle;
    *props = asHandle(reinterpret_cast<InteractObj*>(h));
    return kOfxStatOK;
}
const OfxInteractSuiteV1 kInteractSuite = {interactSwapBuffers, interactRedraw, interactGetPropertySet};

// --------------------------------------------------------------------- Anfitrión

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version)
{
    if (!name)
        return nullptr;
    const std::string_view n(name);
    if (n == kOfxPropertySuite && version == 1)
        return &kPropertySuite;
    if (n == kOfxImageEffectSuite && version == 1)
        return &kImageEffectSuite;
    if (n == kOfxOpenGLRenderSuite && version == 1)
        return &kOpenGLRenderSuite;
    if (n == kOfxParameterSuite && version == 1)
        return &kParameterSuite;
    if (n == kOfxMemorySuite && version == 1)
        return &kMemorySuite;
    if (n == kOfxMultiThreadSuite && version == 1)
        return &kMultiThreadSuite;
    if (n == kOfxInteractSuite && version == 1)
        return &kInteractSuite;
    if (n == kOfxMessageSuite && version == 1)
        return &kMessageSuiteV1;
    if (n == kOfxMessageSuite && version == 2)
        return &kMessageSuiteV2;
    return nullptr; // timeline, parametric, OpenGL...: no disponibles
}

PropertySet& hostProps()
{
    static PropertySet props = [] {
        PropertySet h;
        h.setS(kOfxPropType, {kOfxTypeImageEffectHost});
        h.setS(kOfxPropName, {"io.github.gorgogito.movixstudio"});
        h.setS(kOfxPropLabel, {"MovixStudio"});
        h.setI(kOfxPropAPIVersion, {1, 4});
        h.setI(kOfxPropVersion, {MOVIX_VERSION_MAJOR, MOVIX_VERSION_MINOR, MOVIX_VERSION_PATCH});
        h.setS(kOfxPropVersionLabel, {MOVIX_VERSION});
        h.setI(kOfxImageEffectHostPropIsBackground, {0});
        h.setI(kOfxImageEffectPropSupportsOverlays, {1});
        h.setS(kOfxImageEffectPropOpenGLRenderSupported, {"true"});
        h.setI(kOfxImageEffectPropSupportsMultiResolution, {0});
        h.setI(kOfxImageEffectPropSupportsTiles, {0});
        h.setI(kOfxImageEffectPropTemporalClipAccess, {0});
        h.setS(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA});
        h.setS(kOfxImageEffectPropSupportedContexts, {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral,
                                                      kOfxImageEffectContextGenerator, kOfxImageEffectContextTransition});
        h.setS(kOfxImageEffectPropSupportedPixelDepths, {kOfxBitDepthByte, kOfxBitDepthFloat});
        h.setI(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
        h.setI(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
        h.setI(kOfxImageEffectPropSetableFrameRate, {0});
        h.setI(kOfxImageEffectPropSetableFielding, {0});
        h.setI(kOfxImageEffectInstancePropSequentialRender, {0});
        h.setI(kOfxParamHostPropSupportsCustomInteract, {0});
        h.setI(kOfxParamHostPropSupportsStringAnimation, {0});
        h.setI(kOfxParamHostPropSupportsCustomAnimation, {0});
        h.setI(kOfxParamHostPropSupportsBooleanAnimation, {0});
        h.setI(kOfxParamHostPropSupportsChoiceAnimation, {0});
        h.setI(kOfxParamHostPropMaxParameters, {-1});
        h.setI(kOfxParamHostPropMaxPages, {0});
        h.setI(kOfxParamHostPropPageRowColumnCount, {0, 0});
        h.setI(kOfxImageEffectPropRenderQualityDraft, {0});
        h.setS(kOfxImageEffectHostPropNativeOrigin, {kOfxHostNativeOriginBottomLeft});
        h.setP(kOfxPropHostOSHandle, nullptr);
        return h;
    }();
    return props;
}

OfxHost& host()
{
    static OfxHost h{asHandle(&hostProps()), fetchSuite};
    return h;
}

// Llama a la acción protegiendo al editor de un fallo dentro del plugin.
#ifdef _MSC_VER
OfxStatus guardedEntry(OfxPluginEntryPoint* entry, const char* action, const void* handle, OfxPropertySetHandle in,
                       OfxPropertySetHandle out)
{
    __try {
        return entry(action, handle, in, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kOfxStatErrFatal;
    }
}
#else
OfxStatus guardedEntry(OfxPluginEntryPoint* entry, const char* action, const void* handle, OfxPropertySetHandle in,
                       OfxPropertySetHandle out)
{
    return entry(action, handle, in, out);
}
#endif

OfxStatus callAction(LoadedPlugin& p, const char* action, EffectObj* effect, PropertySet* in = nullptr, PropertySet* out = nullptr)
{
    return guardedEntry(p.api->mainEntry, action, effect, asHandle(in), asHandle(out));
}

bool ok(OfxStatus st)
{
    return st == kOfxStatOK || st == kOfxStatReplyDefault;
}

ParamType paramTypeOf(const std::string& t)
{
    if (t == kOfxParamTypeDouble) return ParamType::Double;
    if (t == kOfxParamTypeInteger) return ParamType::Int;
    if (t == kOfxParamTypeBoolean) return ParamType::Bool;
    if (t == kOfxParamTypeChoice) return ParamType::Choice;
    if (t == kOfxParamTypeRGB) return ParamType::Rgb;
    if (t == kOfxParamTypeRGBA) return ParamType::Rgba;
    if (t == kOfxParamTypeDouble2D) return ParamType::Double2D;
    if (t == kOfxParamTypeInteger2D) return ParamType::Int2D;
    if (t == kOfxParamTypeDouble3D) return ParamType::Double3D;
    if (t == kOfxParamTypeInteger3D) return ParamType::Int3D;
    if (t == kOfxParamTypeString) return ParamType::String;
    if (t == kOfxParamTypeGroup) return ParamType::Group;
    if (t == kOfxParamTypePage) return ParamType::Page;
    if (t == kOfxParamTypePushButton) return ParamType::PushButton;
    return ParamType::Other;
}

ParamDef describeParam(const ParamObj& p)
{
    ParamDef d;
    d.name = p.name;
    d.label = p.str(kOfxPropLabel, 0, p.name);
    d.hint = p.str(kOfxParamPropHint);
    d.parent = p.str(kOfxParamPropParent);
    d.type = paramTypeOf(p.type);
    d.dimension = dimensionOf(p.type);
    d.secret = p.num(kOfxParamPropSecret) != 0;
    d.animates = !isStringType(p.type) && p.num(kOfxParamPropAnimates, 0, 1) != 0;
    if (isStringType(p.type)) {
        d.defaultText = p.str(kOfxParamPropDefault);
    } else {
        d.defaults = p.nums(kOfxParamPropDefault);
        d.defaults.resize(size_t(d.dimension), 0.0);
        // Intervalo de la interfaz: el de pantalla, limitado al válido.
        const double lo = std::max(p.num(kOfxParamPropDisplayMin, 0, -1e300), p.num(kOfxParamPropMin, 0, -1e300));
        const double hi = std::min(p.num(kOfxParamPropDisplayMax, 0, 1e300), p.num(kOfxParamPropMax, 0, 1e300));
        d.min = lo;
        d.max = hi;
    }
    d.options = p.strs(kOfxParamPropChoiceOption);
    if (d.type == ParamType::Choice)
        d.min = 0, d.max = std::max<double>(0, double(d.options.size()) - 1);
    if (d.type == ParamType::Bool)
        d.min = 0, d.max = 1;
    const std::string dt = p.str(kOfxParamPropDoubleType);
    d.normalised = dt.find("Normalised") != std::string::npos
                || (p.str(kOfxParamPropDefaultCoordinateSystem) == kOfxParamCoordinatesNormalised && dt != kOfxParamDoubleTypePlain);
    return d;
}

// ===================================================================== Registro

struct RegistryState {
    std::mutex mutex;
    bool scanned = false;
    std::vector<std::string> extraDirs;
    std::vector<std::unique_ptr<LoadedPlugin>> plugins;
    std::vector<std::string> errors;
};

RegistryState& state()
{
    static auto* s = new RegistryState; // nunca se destruye: descargar plugins al salir puede colgarse
    return *s;
}

std::vector<QString> pluginDirs(const std::vector<std::string>& extra)
{
    std::vector<QString> dirs;
    for (const QString& d : qEnvironmentVariable("OFX_PLUGIN_PATH").split(QLatin1Char(';'), Qt::SkipEmptyParts))
        dirs.push_back(d);
    const QString common = qEnvironmentVariable("CommonProgramFiles");
    if (!common.isEmpty())
        dirs.push_back(common + QStringLiteral("/OFX/Plugins"));
    for (const std::string& d : extra)
        dirs.push_back(QString::fromStdString(d));
    return dirs;
}

// Describe el plugin en el contexto de un papel y lo registra (o sustituye a una versión anterior).
// Si no sirve en ese papel, devuelve false con el motivo en `why`.
bool describeRole(OfxPlugin* api, const std::shared_ptr<EffectObj>& descriptor, const std::shared_ptr<std::mutex>& renderMutex,
                  Role role, const char* context, const QString& binaryPath, RegistryState& s, std::string& why)
{
    const std::string id = api->pluginIdentifier ? api->pluginIdentifier : "";
    const char* roleName = role == Role::Filter ? "filtro" : role == Role::Generator ? "generador" : "transición";
    auto fail = [&](const std::string& reason) {
        why = id + " (" + roleName + "): " + reason;
        return false;
    };
    auto p = std::make_unique<LoadedPlugin>();
    p->api = api;
    p->descriptor = descriptor;
    p->renderMutex = renderMutex;
    p->context = context;
    p->contextDescriptor = std::make_unique<EffectObj>();
    EffectObj& c = *p->contextDescriptor;
    static_cast<PropertySet&>(c) = static_cast<const PropertySet&>(*descriptor);
    c.plugin = p.get();
    c.params.owner = &c;
    c.setS(kOfxImageEffectPropContext, {p->context});
    PropertySet inArgs;
    inArgs.setS(kOfxImageEffectPropContext, {p->context});
    if (!ok(callAction(*p, kOfxImageEffectActionDescribeInContext, &c, &inArgs)))
        return fail("no se pudo describir");
    auto rgba = [](const ClipObj* clip) {
        const auto comps = clip->strs(kOfxImageEffectPropSupportedComponents);
        return comps.empty() || std::find(comps.begin(), comps.end(), kOfxImageComponentRGBA) != comps.end();
    };
    std::vector<const char*> needed = inputClips(role);
    needed.push_back(kOfxImageEffectOutputClipName);
    for (const char* name : needed) {
        const ClipObj* clip = c.clip(name);
        if (!clip)
            return fail(std::string("le falta el clip ") + name);
        if (!rgba(clip))
            return fail("no admite imágenes RGBA");
    }

    auto info = std::make_shared<PluginInfo>();
    info->id = id;
    info->versionMajor = int(api->pluginVersionMajor);
    info->versionMinor = int(api->pluginVersionMinor);
    info->label = c.str(kOfxPropLabel, 0, id);
    info->group = c.str(kOfxImageEffectPluginPropGrouping);
    info->description = c.str(kOfxPropPluginDescription);
    info->binary = binaryPath.toStdString();
    info->role = role;
    info->context = std::string(context).substr(std::strlen("OfxImageEffectContext"));
    const auto depths = c.strs(kOfxImageEffectPropSupportedPixelDepths);
    info->floatImages = std::find(depths.begin(), depths.end(), kOfxBitDepthFloat) != depths.end();
    info->byteImages = depths.empty() || std::find(depths.begin(), depths.end(), kOfxBitDepthByte) != depths.end();
    const std::string gl = c.str(kOfxImageEffectPropOpenGLRenderSupported);
    info->openGL = gl == "true" || gl == "needed";
    info->needsOpenGL = gl == "needed";
    if (!info->floatImages && !info->byteImages)
        return fail("no admite imágenes de 8 bits ni en coma flotante");
    for (const auto& param : c.params.list) {
        ParamDef def = describeParam(*param);
        if (role == Role::Transition && def.name == kOfxImageEffectTransitionParamName)
            def.secret = true; // el progreso lo pone el anfitrión
        info->params.push_back(std::move(def));
    }
    info->hasOverlay = c.ptr(kOfxImageEffectPluginPropOverlayInteractV1) != nullptr;
    p->unsafe = c.str(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe;
    p->info = std::move(info);

    // Otra versión del mismo plugin en el mismo papel: se queda la más reciente.
    auto same = std::find_if(s.plugins.begin(), s.plugins.end(), [&](const auto& x) { return x->info->id == id && x->info->role == role; });
    if (same != s.plugins.end()) {
        const auto& old = *(*same)->info;
        if (std::tie(old.versionMajor, old.versionMinor) >= std::tie(p->info->versionMajor, p->info->versionMinor))
            return true;
        *same = std::move(p);
        return true;
    }
    s.plugins.push_back(std::move(p));
    return true;
}

void describePlugin(OfxPlugin* api, const QString& binaryPath, RegistryState& s)
{
    LoadedPlugin base;
    base.api = api;
    const std::string id = api->pluginIdentifier ? api->pluginIdentifier : "";
    auto fail = [&](const char* why) { s.errors.push_back(id + ": " + why); };
    api->setHost(&host());
    if (!ok(callAction(base, kOfxActionLoad, nullptr)))
        return fail("no se pudo cargar");

    auto descriptor = std::make_shared<EffectObj>();
    EffectObj& d = *descriptor;
    d.plugin = &base;
    d.params.owner = &d;
    d.setS(kOfxPropType, {kOfxTypeImageEffect});
    d.setS(kOfxPropLabel, {id});
    d.setS(kOfxImageEffectPluginPropGrouping, {""});
    d.setS(kOfxImageEffectPropSupportedContexts, {});
    d.setS(kOfxImageEffectPropSupportedPixelDepths, {});
    d.setI(kOfxImageEffectPluginPropSingleInstance, {0});
    d.setS(kOfxImageEffectPluginRenderThreadSafety, {kOfxImageEffectRenderInstanceSafe});
    d.setI(kOfxImageEffectPluginPropHostFrameThreading, {0});
    d.setI(kOfxImageEffectPropSupportsMultiResolution, {1});
    d.setI(kOfxImageEffectPropSupportsTiles, {1});
    d.setI(kOfxImageEffectPropTemporalClipAccess, {0});
    d.setI(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
    d.setI(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
    d.setI(kOfxImageEffectPluginPropFieldRenderTwiceAlways, {1});
    d.setS(kOfxPropPluginDescription, {""});
    d.setP(kOfxImageEffectPluginPropOverlayInteractV1, nullptr);
    if (!ok(callAction(base, kOfxActionDescribe, &d)))
        return fail("no se pudo describir");

    const auto contexts = d.strs(kOfxImageEffectPropSupportedContexts);
    auto has = [&](const char* c) { return std::find(contexts.begin(), contexts.end(), c) != contexts.end(); };
    std::vector<std::pair<Role, const char*>> roles;
    if (has(kOfxImageEffectContextFilter) || has(kOfxImageEffectContextGeneral))
        roles.emplace_back(Role::Filter, has(kOfxImageEffectContextFilter) ? kOfxImageEffectContextFilter : kOfxImageEffectContextGeneral);
    if (has(kOfxImageEffectContextGenerator))
        roles.emplace_back(Role::Generator, kOfxImageEffectContextGenerator);
    if (has(kOfxImageEffectContextTransition))
        roles.emplace_back(Role::Transition, kOfxImageEffectContextTransition);
    if (roles.empty())
        return fail("no es un filtro, un generador ni una transición");
    // Un papel que no encaja (p. ej. un contexto General sin clip Source) solo es un error si el
    // plugin no sirve para ningún otro.
    const auto renderMutex = std::make_shared<std::mutex>();
    std::vector<std::string> why(roles.size());
    bool any = false;
    for (size_t i = 0; i < roles.size(); ++i)
        any = describeRole(api, descriptor, renderMutex, roles[i].first, roles[i].second, binaryPath, s, why[i]) || any;
    if (!any)
        s.errors.insert(s.errors.end(), why.begin(), why.end());
}

void scanLocked(RegistryState& s)
{
    s.scanned = true;
    for (const QString& dir : pluginDirs(s.extraDirs)) {
        QDirIterator it(dir, {QStringLiteral("*.ofx")}, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            const QString path = it.next();
            if (!path.contains(QStringLiteral("/Contents/Win64/"), Qt::CaseInsensitive))
                continue; // solo los paquetes de 64 bits para Windows
            auto lib = std::make_unique<QLibrary>(path);
            if (!lib->load()) {
                s.errors.push_back(path.toStdString() + ": " + lib->errorString().toStdString());
                continue;
            }
            using CountFn = int (*)();
            using GetFn = OfxPlugin* (*)(int);
            using SetHostFn = OfxStatus (*)(const OfxHost*);
            auto count = reinterpret_cast<CountFn>(lib->resolve("OfxGetNumberOfPlugins"));
            auto get = reinterpret_cast<GetFn>(lib->resolve("OfxGetPlugin"));
            if (!count || !get) {
                s.errors.push_back(path.toStdString() + ": no es un plugin OpenFX");
                continue;
            }
            if (auto setHost = reinterpret_cast<SetHostFn>(lib->resolve("OfxSetHost")))
                setHost(&host());
            const size_t before = s.plugins.size(); // (un plugin sustituido no cuenta: su biblioteca sigue cargada)
            for (int i = 0, n = count(); i < n; ++i) {
                OfxPlugin* api = get(i);
                if (!api || !api->pluginApi || std::strcmp(api->pluginApi, kOfxImageEffectPluginApi) != 0 || api->apiVersion != 1)
                    continue;
                describePlugin(api, path, s);
            }
            if (s.plugins.size() > before)
                lib.release(); // no se descarga nunca: el plugin guarda punteros dentro de ella
        }
    }
}

} // namespace

// ===================================================================== API pública

Registry& Registry::instance()
{
    static Registry r;
    return r;
}

void Registry::setExtraDirs(std::vector<std::string> dirs)
{
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    s.extraDirs = std::move(dirs);
}

void Registry::ensureScanned()
{
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.scanned)
        scanLocked(s);
}

void Registry::rescan()
{
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    // Los ya cargados se conservan (sus instancias siguen vivas); solo se añaden los nuevos.
    s.errors.clear();
    scanLocked(s);
}

std::vector<PluginInfo> Registry::plugins(Role role)
{
    ensureScanned();
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    std::vector<PluginInfo> out;
    for (const auto& p : s.plugins)
        if (p->info->role == role)
            out.push_back(*p->info);
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) {
        return std::tie(a.group, a.label) < std::tie(b.group, b.label);
    });
    return out;
}

std::shared_ptr<const PluginInfo> Registry::find(const std::string& id, Role role)
{
    ensureScanned();
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    for (const auto& p : s.plugins)
        if (p->info->id == id && p->info->role == role)
            return p->info;
    return nullptr;
}

std::vector<std::string> Registry::errors()
{
    ensureScanned();
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    return s.errors;
}

ParamValues withDefaults(const PluginInfo& plugin, const ParamValues& values)
{
    ParamValues out;
    for (const ParamDef& d : plugin.params) {
        if (d.type == ParamType::Group || d.type == ParamType::Page || d.type == ParamType::PushButton || d.type == ParamType::String
            || d.type == ParamType::Other)
            continue;
        auto it = values.find(d.name);
        std::vector<core::Animated> v;
        if (it != values.end())
            v = it->second;
        for (size_t i = v.size(); i < size_t(d.dimension); ++i) // los componentes que faltan, por defecto
            v.emplace_back(i < d.defaults.size() ? d.defaults[i] : 0.0);
        v.resize(size_t(d.dimension));
        out[d.name] = std::move(v);
    }
    return out;
}

// Pone en la instancia los valores de los parámetros (los que no vienen se quedan como estaban).
void applyValues(EffectObj& e, const ParamValues& values)
{
    for (auto& p : e.params.list) {
        auto it = values.find(p->name);
        if (it != values.end() && !isStringType(p->type)) {
            p->curve = it->second;
            p->curve.resize(size_t(dimensionOf(p->type)));
        }
        if (!isStringType(p->type))
            p->setI(kOfxParamPropIsAnimating, {!keyTimes(*p).empty() ? 1 : 0});
    }
}

class GlCanvas;

struct Effect::Impl {
    LoadedPlugin* plugin = nullptr;
    std::unique_ptr<EffectObj> obj;
    std::vector<std::vector<float>> inF; // copias de las entradas, por clip
    std::vector<std::vector<uint8_t>> inB;
    std::vector<float> outF;
    std::vector<uint8_t> outB;
    bool created = false;
    GlCanvas* gl = nullptr; // contexto OpenGL del que el plugin sabe (kOfxActionOpenGLContextAttached)
    std::vector<uint8_t> readback;

    ~Impl();
};

namespace {
std::atomic<bool> g_openGLRender{true};

// Render por OpenGL (más abajo, junto al contexto): 1 si salió, 0 si hay que hacerlo en la CPU,
// −1 si el plugin falló.
int renderWithOpenGL(Effect::Impl& d, const std::vector<const void*>& inputs, void* out, bool floatIO, double time, std::string* error);
// Si hay un contexto OpenGL en este hilo, el plugin deja de usarlo (al destruir la instancia).
void detachOpenGL(Effect::Impl& d);
} // namespace

void setOpenGLRender(bool enabled)
{
    g_openGLRender = enabled;
}

bool openGLRender()
{
    return g_openGLRender;
}

Effect::Impl::~Impl()
{
    if (created && plugin) {
        detachOpenGL(*this);
        callAction(*plugin, kOfxActionDestroyInstance, obj.get());
    }
}

Effect::Effect(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}

Effect::~Effect() = default;

const PluginInfo& Effect::plugin() const
{
    return *d->plugin->info;
}

int Effect::width() const
{
    return d->obj->width;
}

int Effect::height() const
{
    return d->obj->height;
}

ParamValues Effect::values() const
{
    ParamValues out;
    for (const auto& p : d->obj->params.list)
        if (!isStringType(p->type) && !p->curve.empty())
            out[p->name] = p->curve;
    return out;
}

std::unique_ptr<Effect> Effect::create(const std::string& pluginId, int width, int height, double frameRate, std::string* error, Role role,
                                       bool preferFloat)
{
    Registry::instance().ensureScanned();
    RegistryState& s = state();
    LoadedPlugin* plugin = nullptr;
    {
        std::lock_guard lock(s.mutex);
        for (const auto& p : s.plugins)
            if (p->info->id == pluginId && p->info->role == role)
                plugin = p.get();
    }
    if (!plugin) {
        if (error)
            *error = "El efecto " + pluginId + " no está instalado.";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->plugin = plugin;
    impl->obj = std::make_unique<EffectObj>();
    EffectObj& e = *impl->obj;
    const EffectObj& desc = *plugin->contextDescriptor;
    static_cast<PropertySet&>(e) = static_cast<const PropertySet&>(desc);
    e.instance = true;
    e.plugin = plugin;
    e.params.owner = &e;
    e.width = width;
    e.height = height;
    e.floatImages = plugin->info->floatImages && (preferFloat || !plugin->info->byteImages);
    e.setS(kOfxPropType, {kOfxTypeImageEffectInstance});
    e.setS(kOfxImageEffectPropContext, {plugin->context});
    e.setP(kOfxPropInstanceData, nullptr);
    e.setD(kOfxImageEffectPropProjectSize, {double(width), double(height)});
    e.setD(kOfxImageEffectPropProjectOffset, {0.0, 0.0});
    e.setD(kOfxImageEffectPropProjectExtent, {double(width), double(height)});
    e.setD(kOfxImageEffectPropProjectPixelAspectRatio, {1.0});
    e.setD(kOfxImageEffectInstancePropEffectDuration, {1e6});
    e.setI(kOfxImageEffectInstancePropSequentialRender, {0});
    e.setD(kOfxImageEffectPropFrameRate, {frameRate});
    e.setI(kOfxPropIsInteractive, {0});
    for (const auto& dp : desc.params.list) {
        auto p = std::make_unique<ParamObj>(*dp);
        p->owner = &e;
        if (isStringType(p->type))
            p->text = p->str(kOfxParamPropDefault);
        else
            for (double v : p->nums(kOfxParamPropDefault))
                p->curve.emplace_back(v);
        e.params.list.push_back(std::move(p));
    }
    const char* depth = e.floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte;
    for (const auto& dc : desc.clips) {
        auto c = std::make_unique<ClipObj>(*dc);
        c->owner = &e;
        const auto inputs = inputClips(role);
        const bool connected = c->name == kOfxImageEffectOutputClipName
                               || std::any_of(inputs.begin(), inputs.end(), [&](const char* n) { return c->name == n; });
        c->setS(kOfxImageEffectPropPixelDepth, {depth});
        c->setS(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
        c->setS(kOfxImageClipPropUnmappedPixelDepth, {depth});
        c->setS(kOfxImageClipPropUnmappedComponents, {kOfxImageComponentRGBA});
        c->setS(kOfxImageEffectPropPreMultiplication, {kOfxImageUnPreMultiplied});
        c->setD(kOfxImagePropPixelAspectRatio, {1.0});
        c->setD(kOfxImageEffectPropFrameRate, {frameRate});
        c->setD(kOfxImageEffectPropUnmappedFrameRate, {frameRate});
        c->setD(kOfxImageEffectPropFrameRange, {0.0, 1e6});
        c->setD(kOfxImageEffectPropUnmappedFrameRange, {0.0, 1e6});
        c->setS(kOfxImageClipPropFieldOrder, {kOfxImageFieldNone});
        c->setI(kOfxImageClipPropConnected, {connected ? 1 : 0});
        c->setI(kOfxImageClipPropContinuousSamples, {0});
        e.clips.push_back(std::move(c));
    }
    std::unique_lock renderLock(*plugin->renderMutex, std::defer_lock);
    if (plugin->unsafe)
        renderLock.lock();
    if (!ok(callAction(*plugin, kOfxActionCreateInstance, &e))) {
        if (error)
            *error = "El efecto " + plugin->info->label + " no se pudo iniciar." + (e.lastMessage.empty() ? "" : " " + e.lastMessage);
        return nullptr;
    }
    impl->created = true;
    return std::unique_ptr<Effect>(new Effect(std::move(impl)));
}

bool Effect::floatImages() const
{
    return d->obj->floatImages;
}

bool Effect::render(const ParamValues& values, double time, const uint8_t* in, uint8_t* out, std::string* error)
{
    return renderImpl(values, time, {in}, out, false, error);
}

bool Effect::render(const ParamValues& values, double time, const float* in, float* out, std::string* error)
{
    return renderImpl(values, time, {in}, out, true, error);
}

bool Effect::generate(const ParamValues& values, double time, uint8_t* out, std::string* error)
{
    return renderImpl(values, time, {}, out, false, error);
}

bool Effect::generate(const ParamValues& values, double time, float* out, std::string* error)
{
    return renderImpl(values, time, {}, out, true, error);
}

namespace {
ParamValues withProgress(const ParamValues& values, double progress)
{
    ParamValues v = values;
    v[kOfxImageEffectTransitionParamName] = {core::Animated(std::clamp(progress, 0.0, 1.0))};
    return v;
}
} // namespace

bool Effect::transition(const ParamValues& values, double time, double progress, const uint8_t* from, const uint8_t* to, uint8_t* out,
                        std::string* error)
{
    return renderImpl(withProgress(values, progress), time, {from, to}, out, false, error);
}

bool Effect::transition(const ParamValues& values, double time, double progress, const float* from, const float* to, float* out,
                        std::string* error)
{
    return renderImpl(withProgress(values, progress), time, {from, to}, out, true, error);
}

namespace {
uint8_t toByte(float v)
{
    return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Las acciones de un render de la imagen entera en `time` (con las imágenes o texturas ya puestas).
OfxStatus renderActions(LoadedPlugin& plugin, EffectObj& e, double time, bool openGL)
{
    std::unique_lock renderLock(*plugin.renderMutex, std::defer_lock);
    if (plugin.unsafe)
        renderLock.lock();
    PropertySet seq;
    seq.setD(kOfxImageEffectPropFrameRange, {time, time});
    seq.setD(kOfxImageEffectPropFrameStep, {1.0});
    seq.setI(kOfxPropIsInteractive, {0});
    seq.setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    seq.setI(kOfxImageEffectPropSequentialRenderStatus, {0});
    seq.setI(kOfxImageEffectPropInteractiveRenderStatus, {0});
    seq.setI(kOfxImageEffectPropOpenGLEnabled, {openGL ? 1 : 0});
    callAction(plugin, kOfxImageEffectActionBeginSequenceRender, &e, &seq);
    PropertySet args;
    args.setD(kOfxPropTime, {time});
    args.setS(kOfxImageEffectPropFieldToRender, {kOfxImageFieldNone});
    args.setI(kOfxImageEffectPropRenderWindow, {0, 0, e.width, e.height});
    args.setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    args.setI(kOfxImageEffectPropSequentialRenderStatus, {0});
    args.setI(kOfxImageEffectPropInteractiveRenderStatus, {0});
    args.setI(kOfxImageEffectPropRenderQualityDraft, {0});
    args.setI(kOfxImageEffectPropOpenGLEnabled, {openGL ? 1 : 0});
    const OfxStatus st = callAction(plugin, kOfxImageEffectActionRender, &e, &args);
    callAction(plugin, kOfxImageEffectActionEndSequenceRender, &e, &seq);
    return st;
}

std::string renderError(const LoadedPlugin& plugin, const EffectObj& e, OfxStatus st)
{
    return st == kOfxStatErrFatal ? "El efecto " + plugin.info->label + " falló al renderizar."
                                  : "El efecto " + plugin.info->label + " no pudo renderizar." + (e.lastMessage.empty() ? "" : " " + e.lastMessage);
}
} // namespace

bool Effect::renderImpl(const ParamValues& values, double time, std::vector<const void*> inputs, void* out, bool floatIO, std::string* error)
{
    EffectObj& e = *d->obj;
    LoadedPlugin& plugin = *d->plugin;
    const auto names = inputClips(plugin.info->role);
    if (inputs.size() != names.size()) {
        if (error)
            *error = "El efecto " + plugin.info->label + " no recibió sus imágenes.";
        return false;
    }
    const int w = e.width, h = e.height;
    e.time = time;
    applyValues(e, values);

    // En la GPU si el plugin sabe; si no sale (sin OpenGL o falla con él), en la CPU.
    if (plugin.info->openGL && (g_openGLRender || plugin.info->needsOpenGL)) {
        const int gl = renderWithOpenGL(*d, inputs, out, floatIO, time, error);
        if (gl != 0)
            return gl > 0;
        if (plugin.info->needsOpenGL) {
            if (error)
                *error = "El efecto " + plugin.info->label + " necesita OpenGL y no está disponible.";
            return false;
        }
    }

    // Imágenes de abajo arriba (como espera OFX): el puntero a la fila de abajo y un paso negativo.
    // Si la profundidad del plugin es la de las imágenes, lee y escribe las del llamador; si no, se
    // convierten a la suya.
    const size_t values4 = size_t(w) * size_t(h) * 4;
    const bool direct = e.floatImages == floatIO;
    e.images.clear();
    d->inF.resize(inputs.size());
    d->inB.resize(inputs.size());
    const int stride = w * 4 * int(e.floatImages ? sizeof(float) : 1);
    auto bottomRow = [&](const void* top) { return static_cast<uint8_t*>(const_cast<void*>(top)) + ptrdiff_t(h - 1) * stride; };
    for (size_t k = 0; k < inputs.size(); ++k) {
        const void* image = inputs[k];
        if (!direct && e.floatImages) {
            std::vector<float>& copy = d->inF[k];
            copy.resize(values4);
            for (size_t i = 0; i < values4; ++i)
                copy[i] = float(static_cast<const uint8_t*>(inputs[k])[i]) / 255.0f;
            image = copy.data();
        } else if (!direct) {
            std::vector<uint8_t>& copy = d->inB[k];
            copy.resize(values4);
            for (size_t i = 0; i < values4; ++i)
                copy[i] = toByte(static_cast<const float*>(inputs[k])[i]);
            image = copy.data();
        }
        e.images.emplace_back(names[k], bottomRow(image));
    }
    void* target = out;
    if (!direct && e.floatImages) {
        d->outF.assign(values4, 0.0f);
        target = d->outF.data();
    } else if (!direct) {
        d->outB.assign(values4, 0);
        target = d->outB.data();
    }
    e.rowBytes = -stride;
    e.images.emplace_back(kOfxImageEffectOutputClipName, bottomRow(target));

    const OfxStatus st = renderActions(plugin, e, time, false);
    e.images.clear();
    if (!ok(st)) {
        if (error)
            *error = renderError(plugin, e, st);
        return false;
    }
    if (direct)
        return true;
    if (e.floatImages)
        for (size_t i = 0; i < values4; ++i)
            static_cast<uint8_t*>(out)[i] = toByte(d->outF[i]);
    else
        for (size_t i = 0; i < values4; ++i)
            static_cast<float*>(out)[i] = float(d->outB[i]) / 255.0f;
    return true;
}

} // namespace movix::ofx

// ===================================================================== Interfaz sobre el visor

namespace movix::ofx {

// Contexto OpenGL propio (WGL, compatible con las llamadas clásicas que usan los plugins) sobre una
// ventana oculta, con un framebuffer del tamaño de la vista. No depende de la plataforma de Qt.
class GlCanvas {
public:
    bool create()
    {
#ifdef _WIN32
        static const ATOM windowClass = [] {
            WNDCLASSW wc{};
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MovixOfxOverlay";
            return RegisterClassW(&wc);
        }();
        if (!windowClass)
            return false;
        m_window = CreateWindowExW(0, L"MovixOfxOverlay", L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        m_dc = m_window ? GetDC(m_window) : nullptr;
        if (!m_dc)
            return false;
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        const int format = ChoosePixelFormat(m_dc, &pfd);
        if (!format || !SetPixelFormat(m_dc, format, &pfd))
            return false;
        m_context = wglCreateContext(m_dc);
        if (!m_context || !makeCurrent())
            return false;
        auto load = [](const char* name) { return reinterpret_cast<void*>(wglGetProcAddress(name)); };
        genFramebuffers = reinterpret_cast<decltype(genFramebuffers)>(load("glGenFramebuffers"));
        deleteFramebuffers = reinterpret_cast<decltype(deleteFramebuffers)>(load("glDeleteFramebuffers"));
        bindFramebuffer = reinterpret_cast<decltype(bindFramebuffer)>(load("glBindFramebuffer"));
        framebufferTexture2D = reinterpret_cast<decltype(framebufferTexture2D)>(load("glFramebufferTexture2D"));
        checkFramebufferStatus = reinterpret_cast<decltype(checkFramebufferStatus)>(load("glCheckFramebufferStatus"));
        const bool ok = genFramebuffers && deleteFramebuffers && bindFramebuffer && framebufferTexture2D && checkFramebufferStatus;
        doneCurrent();
        return ok; // sin framebuffers (OpenGL 1.1 por software): sin interfaz
#else
        return false;
#endif
    }

    ~GlCanvas()
    {
#ifdef _WIN32
        if (m_context && makeCurrent()) {
            if (m_fbo)
                deleteFramebuffers(1, &m_fbo);
            if (m_texture)
                glDeleteTextures(1, &m_texture);
            for (const Texture& t : m_inputs)
                glDeleteTextures(1, &t.id);
            doneCurrent();
        }
        if (m_context)
            wglDeleteContext(m_context);
        if (m_dc)
            ReleaseDC(m_window, m_dc);
        if (m_window)
            DestroyWindow(m_window);
#endif
    }

    bool makeCurrent()
    {
#ifdef _WIN32
        return m_context && wglMakeCurrent(m_dc, m_context);
#else
        return false;
#endif
    }

    void doneCurrent()
    {
#ifdef _WIN32
        wglMakeCurrent(nullptr, nullptr);
#endif
    }

    // Activa el contexto con un framebuffer de w × h (en coma flotante si `floatTarget`); el de la
    // ventana deja de usarse.
    bool bind(int w, int h, bool floatTarget = false)
    {
        if (!makeCurrent())
            return false;
        constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kComplete = 0x8CD5;
        if (!m_fbo || m_width != w || m_height != h || m_float != floatTarget) {
            if (!m_texture)
                glGenTextures(1, &m_texture);
            glBindTexture(GL_TEXTURE_2D, m_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, floatTarget ? kRgba32f : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);
            if (!m_fbo)
                genFramebuffers(1, &m_fbo);
            bindFramebuffer(kFramebuffer, m_fbo);
            framebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, m_texture, 0);
            if (checkFramebufferStatus(kFramebuffer) != kComplete)
                return false;
            m_width = w;
            m_height = h;
            m_float = floatTarget;
        }
        bindFramebuffer(kFramebuffer, m_fbo);
        return true;
    }

    // La textura del framebuffer (el destino del render).
    GLuint texture() const { return m_texture; }
    void bindTarget() { bindFramebuffer(0x8D40, m_fbo); }

    // Textura de entrada `index` de w × h, con el contenido sin definir (se reutiliza entre renders).
    GLuint inputTexture(size_t index, int w, int h, bool floatTexture)
    {
        if (m_inputs.size() <= index)
            m_inputs.resize(index + 1);
        Texture& t = m_inputs[index];
        if (!t.id)
            glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        if (t.width != w || t.height != h || t.isFloat != floatTexture) {
            glTexImage2D(GL_TEXTURE_2D, 0, floatTexture ? kRgba32f : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            t.width = w;
            t.height = h;
            t.isFloat = floatTexture;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return t.id;
    }

    void release()
    {
        bindFramebuffer(0x8D40, 0);
        doneCurrent();
    }

private:
#ifdef _WIN32
    HWND m_window = nullptr;
    HDC m_dc = nullptr;
    HGLRC m_context = nullptr;
#endif
    static constexpr GLint kRgba32f = 0x8814;
    struct Texture {
        GLuint id = 0;
        int width = 0, height = 0;
        bool isFloat = false;
    };
    GLuint m_fbo = 0, m_texture = 0;
    int m_width = 0, m_height = 0;
    bool m_float = false;
    std::vector<Texture> m_inputs;
    void(APIENTRY* genFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindFramebuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum(APIENTRY* checkFramebufferStatus)(GLenum) = nullptr;
};

namespace {

// Contexto OpenGL de cada hilo para los plugins que renderizan con OpenGL (un contexto solo puede
// estar activo en un hilo). Nulo si no hay OpenGL con framebuffers.
std::unique_ptr<GlCanvas>& threadCanvasSlot()
{
    thread_local std::unique_ptr<GlCanvas> canvas;
    return canvas;
}

GlCanvas* threadCanvas()
{
    thread_local bool tried = false;
    if (!tried) {
        tried = true;
        auto canvas = std::make_unique<GlCanvas>();
        if (canvas->create())
            threadCanvasSlot() = std::move(canvas);
    }
    return threadCanvasSlot().get();
}

int renderWithOpenGL(Effect::Impl& d, const std::vector<const void*>& inputs, void* out, bool floatIO, double time, std::string* error)
{
    EffectObj& e = *d.obj;
    LoadedPlugin& plugin = *d.plugin;
    GlCanvas* gl = threadCanvas();
    const int w = e.width, h = e.height;
    if (!gl || !gl->bind(w, h, e.floatImages))
        return 0;
    glPushAttrib(GL_ALL_ATTRIB_BITS); // lo que el plugin cambie no pasa al siguiente
    // Las texturas van de abajo arriba, como las imágenes de OFX: las filas se suben al revés. OpenGL
    // convierte entre 8 bits y coma flotante si la instancia trabaja en el otro tipo.
    const GLenum type = floatIO ? GL_FLOAT : GL_UNSIGNED_BYTE;
    const size_t rowBytes = size_t(w) * 4 * (floatIO ? sizeof(float) : 1);
    const auto names = inputClips(plugin.info->role);
    e.textures.clear();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (size_t k = 0; k < inputs.size() && k < names.size(); ++k) {
        const GLuint texture = gl->inputTexture(k, w, h, e.floatImages);
        for (int y = 0; y < h; ++y)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1, GL_RGBA, type, static_cast<const uint8_t*>(inputs[k]) + size_t(h - 1 - y) * rowBytes);
        e.textures.emplace_back(names[k], texture);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    e.textures.emplace_back(kOfxImageEffectOutputClipName, gl->texture());
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (d.gl != gl) {
        callAction(plugin, kOfxActionOpenGLContextAttached, &e);
        d.gl = gl;
    }
    const OfxStatus st = renderActions(plugin, e, time, true);
    e.textures.clear();
    glPopAttrib();
    if (st == kOfxStatGLRenderFailed || st == kOfxStatGLOutOfMemory || st == kOfxStatErrMemory) {
        gl->release(); // se repite en la CPU
        return 0;
    }
    if (!ok(st)) {
        gl->release();
        if (error)
            *error = renderError(plugin, e, st);
        return -1;
    }
    gl->bindTarget(); // por si el plugin dejó enlazado otro framebuffer
    while (glGetError() != GL_NO_ERROR) {
    } // los errores del plugin no cuentan como de la lectura
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    d.readback.resize(rowBytes * size_t(h));
    glReadPixels(0, 0, w, h, GL_RGBA, type, d.readback.data());
    const bool failed = glGetError() != GL_NO_ERROR;
    gl->release();
    if (failed)
        return 0;
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(out) + size_t(h - 1 - y) * rowBytes, d.readback.data() + size_t(y) * rowBytes, rowBytes);
    return 1;
}

void detachOpenGL(Effect::Impl& d)
{
    // Solo si el contexto es el de este hilo (el de otro hilo no se puede activar aquí).
    GlCanvas* gl = threadCanvasSlot().get();
    if (!d.gl || d.gl != gl || !gl->makeCurrent())
        return;
    callAction(*d.plugin, kOfxActionOpenGLContextDetached, d.obj.get());
    gl->doneCurrent();
    d.gl = nullptr;
}

} // namespace

struct Overlay::Impl {
    Effect* effect = nullptr;
    OfxPluginEntryPoint* entry = nullptr;
    InteractObj descriptor;
    InteractObj instance;
    bool created = false;
    GlCanvas gl;

    OfxStatus call(const char* action, InteractObj* handle, PropertySet* in = nullptr)
    {
        return guardedEntry(entry, action, handle, asHandle(in), nullptr);
    }

    // Argumentos comunes de las acciones de dibujo y del ratón.
    PropertySet args(double time, int viewWidth, int viewHeight)
    {
        EffectObj& e = *effect->d->obj;
        PropertySet a;
        a.setP(kOfxPropEffectInstance, &e);
        a.setD(kOfxPropTime, {time});
        a.setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        a.setD(kOfxInteractPropBackgroundColour, {0.0, 0.0, 0.0});
        a.setI(kOfxInteractPropViewportSize, {viewWidth, viewHeight});
        a.setD(kOfxInteractPropPixelScale, {double(e.width) / std::max(1, viewWidth), double(e.height) / std::max(1, viewHeight)});
        instance.setI(kOfxInteractPropViewportSize, {viewWidth, viewHeight});
        instance.setD(kOfxInteractPropPixelScale, a.nums(kOfxInteractPropPixelScale));
        return a;
    }

    // Framebuffer de la vista, proyección en coordenadas de la imagen y todo transparente.
    bool begin(int viewWidth, int viewHeight)
    {
        if (!gl.bind(viewWidth, viewHeight))
            return false;
        const EffectObj& e = *effect->d->obj;
        glViewport(0, 0, viewWidth, viewHeight);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, double(e.width), 0.0, double(e.height), -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        return true;
    }

    ~Impl()
    {
        if (created) {
            gl.makeCurrent();
            call(kOfxActionDestroyInstance, &instance);
            gl.doneCurrent();
        }
    }
};

Overlay::Overlay(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}

Overlay::~Overlay() = default;

std::unique_ptr<Overlay> Overlay::create(Effect& effect, std::string* error)
{
    LoadedPlugin& plugin = *effect.d->plugin;
    auto* entry = reinterpret_cast<OfxPluginEntryPoint*>(plugin.contextDescriptor->ptr(kOfxImageEffectPluginPropOverlayInteractV1));
    if (!entry)
        return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->effect = &effect;
    impl->entry = entry;
    if (!impl->gl.create()) {
        if (error)
            *error = "No se pudo crear el contexto OpenGL para la interfaz de " + plugin.info->label + ".";
        return nullptr;
    }
    for (InteractObj* o : {&impl->descriptor, &impl->instance}) {
        o->setI(kOfxInteractPropHasAlpha, {1});
        o->setI(kOfxInteractPropBitDepth, {8});
    }
    if (!ok(impl->call(kOfxActionDescribe, &impl->descriptor))) {
        if (error)
            *error = "La interfaz de " + plugin.info->label + " no se pudo describir.";
        return nullptr;
    }
    InteractObj& i = impl->instance;
    static_cast<PropertySet&>(i) = static_cast<const PropertySet&>(impl->descriptor);
    i.setP(kOfxPropEffectInstance, effect.d->obj.get());
    i.setP(kOfxPropInstanceData, nullptr);
    i.setD(kOfxInteractPropPixelScale, {1.0, 1.0});
    i.setD(kOfxInteractPropBackgroundColour, {0.0, 0.0, 0.0});
    i.setD(kOfxInteractPropSuggestedColour, {1.0, 1.0, 1.0});
    i.setI(kOfxInteractPropViewportSize, {effect.width(), effect.height()});
    i.setS(kOfxInteractPropSlaveToParam, {});
    // Algunos plugins consultan OpenGL al crearse: con el contexto ya activo.
    impl->gl.makeCurrent();
    const bool created = ok(impl->call(kOfxActionCreateInstance, &i));
    impl->gl.doneCurrent();
    if (!created) {
        if (error)
            *error = "La interfaz de " + plugin.info->label + " no se pudo iniciar.";
        return nullptr;
    }
    impl->created = true;
    return std::unique_ptr<Overlay>(new Overlay(std::move(impl)));
}

bool Overlay::draw(const ParamValues& values, double time, int viewWidth, int viewHeight, std::vector<uint8_t>& rgba)
{
    if (viewWidth <= 0 || viewHeight <= 0)
        return false;
    EffectObj& e = *d->effect->d->obj;
    e.time = time;
    applyValues(e, values);
    if (!d->begin(viewWidth, viewHeight)) {
        d->gl.doneCurrent();
        return false;
    }
    PropertySet a = d->args(time, viewWidth, viewHeight);
    d->call(kOfxInteractActionDraw, &d->instance, &a);
    // El plugin puede haber dejado estados cambiados: se lee con los de lectura por defecto.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<uint8_t> bottomUp(size_t(viewWidth) * size_t(viewHeight) * 4);
    glReadPixels(0, 0, viewWidth, viewHeight, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    d->gl.release();
    const size_t row = size_t(viewWidth) * 4;
    rgba.resize(bottomUp.size());
    for (int y = 0; y < viewHeight; ++y)
        std::memcpy(rgba.data() + size_t(y) * row, bottomUp.data() + size_t(viewHeight - 1 - y) * row, row);
    return true;
}

bool Overlay::pen(Pen kind, double x, double y, double time, int viewWidth, int viewHeight, ParamValues& values)
{
    EffectObj& e = *d->effect->d->obj;
    e.time = time;
    applyValues(e, values);
    PropertySet a = d->args(time, viewWidth, viewHeight);
    const double sx = double(e.width) / std::max(1, viewWidth), sy = double(e.height) / std::max(1, viewHeight);
    a.setD(kOfxInteractPropPenPosition, {x * sx, (viewHeight - y) * sy});
    a.setI(kOfxInteractPropPenViewportPosition, {int(std::lround(x)), int(std::lround(viewHeight - y))});
    a.setD(kOfxInteractPropPenPressure, {kind == Pen::Up ? 0.0 : 1.0});
    const char* action = kind == Pen::Down ? kOfxInteractActionPenDown : kind == Pen::Up ? kOfxInteractActionPenUp : kOfxInteractActionPenMotion;
    d->gl.makeCurrent();
    const OfxStatus st = d->call(action, &d->instance, &a);
    d->gl.doneCurrent();
    values = d->effect->values();
    return st == kOfxStatOK;
}

namespace {

// Símbolo de OFX (los de X11) de una tecla de Qt; los imprimibles coinciden con su carácter.
int keySym(int qtKey, const std::string& text)
{
    static const std::map<int, int> special = {
        {Qt::Key_Left, kOfxKey_Left},         {Qt::Key_Right, kOfxKey_Right},     {Qt::Key_Up, kOfxKey_Up},
        {Qt::Key_Down, kOfxKey_Down},         {Qt::Key_Home, kOfxKey_Home},       {Qt::Key_End, kOfxKey_End},
        {Qt::Key_PageUp, kOfxKey_Page_Up},    {Qt::Key_PageDown, kOfxKey_Page_Down},
        {Qt::Key_Return, kOfxKey_Return},     {Qt::Key_Enter, kOfxKey_KP_Enter},  {Qt::Key_Escape, kOfxKey_Escape},
        {Qt::Key_Tab, kOfxKey_Tab},           {Qt::Key_Backspace, kOfxKey_BackSpace},
        {Qt::Key_Delete, kOfxKey_Delete},     {Qt::Key_Insert, kOfxKey_Insert},   {Qt::Key_Shift, kOfxKey_Shift_L},
        {Qt::Key_Control, kOfxKey_Control_L}, {Qt::Key_Alt, kOfxKey_Alt_L},       {Qt::Key_Meta, kOfxKey_Super_L},
        {Qt::Key_Space, kOfxKey_space},
    };
    if (const auto it = special.find(qtKey); it != special.end())
        return it->second;
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12)
        return kOfxKey_F1 + (qtKey - Qt::Key_F1);
    // Imprimibles: el carácter que escribe (minúscula o mayúscula según la tecla de mayúsculas).
    const QString s = QString::fromStdString(text);
    if (s.size() == 1 && s[0].unicode() >= 0x20 && s[0].unicode() <= 0xff)
        return s[0].unicode();
    return qtKey >= 0x20 && qtKey <= 0xff ? QChar(qtKey).toLower().unicode() : 0;
}

} // namespace

bool Overlay::key(Key kind, int qtKey, const std::string& text, double time, int viewWidth, int viewHeight, ParamValues& values)
{
    const int sym = keySym(qtKey, text); // kOfxKey_Unknown (0) con texto: un carácter sin símbolo
    if (sym == kOfxKey_Unknown && text.empty())
        return false;
    EffectObj& e = *d->effect->d->obj;
    e.time = time;
    applyValues(e, values);
    PropertySet a = d->args(time, viewWidth, viewHeight);
    a.setI(kOfxPropKeySym, {sym});
    a.setS(kOfxPropKeyString, {text});
    const char* action = kind == Key::Down ? kOfxInteractActionKeyDown : kind == Key::Up ? kOfxInteractActionKeyUp : kOfxInteractActionKeyRepeat;
    d->gl.makeCurrent();
    const OfxStatus st = d->call(action, &d->instance, &a);
    d->gl.doneCurrent();
    values = d->effect->values();
    return st == kOfxStatOK;
}

bool Overlay::takeRedraw()
{
    return std::exchange(d->instance.redraw, false);
}

} // namespace movix::ofx
