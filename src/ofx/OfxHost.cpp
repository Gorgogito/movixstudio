#include "ofx/OfxHost.h"

#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxMemory.h>
#include <ofxMessage.h>
#include <ofxMultiThread.h>
#include <ofxParam.h>
#include <ofxProperty.h>

#include <QDir>
#include <QDirIterator>
#include <QLibrary>
#include <QString>

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
    // Estado del render en curso.
    int width = 0, height = 0;
    bool floatImages = false;
    void* inData = nullptr;
    void* outData = nullptr;
    int rowBytes = 0;
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

struct LoadedPlugin {
    OfxPlugin* api = nullptr;
    std::shared_ptr<PluginInfo> info;
    std::unique_ptr<EffectObj> descriptor;
    std::unique_ptr<EffectObj> contextDescriptor;
    std::string context; // kOfxImageEffectContextFilter o General
    bool unsafe = false;   // el plugin pide un único render a la vez
    std::mutex renderMutex;
};

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
    const bool output = c->name == kOfxImageEffectOutputClipName;
    const bool source = c->name == kOfxImageEffectSimpleSourceClipName;
    void* data = output ? e->outData : source ? e->inData : nullptr;
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
    if (n == kOfxParameterSuite && version == 1)
        return &kParameterSuite;
    if (n == kOfxMemorySuite && version == 1)
        return &kMemorySuite;
    if (n == kOfxMultiThreadSuite && version == 1)
        return &kMultiThreadSuite;
    if (n == kOfxMessageSuite && version == 1)
        return &kMessageSuiteV1;
    if (n == kOfxMessageSuite && version == 2)
        return &kMessageSuiteV2;
    return nullptr; // interact, timeline, parametric, OpenGL...: no disponibles
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
        h.setI(kOfxImageEffectPropSupportsOverlays, {0});
        h.setI(kOfxImageEffectPropSupportsMultiResolution, {0});
        h.setI(kOfxImageEffectPropSupportsTiles, {0});
        h.setI(kOfxImageEffectPropTemporalClipAccess, {0});
        h.setS(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA});
        h.setS(kOfxImageEffectPropSupportedContexts, {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral});
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

void describePlugin(OfxPlugin* api, const QString& binaryPath, RegistryState& s)
{
    auto p = std::make_unique<LoadedPlugin>();
    p->api = api;
    const std::string id = api->pluginIdentifier ? api->pluginIdentifier : "";
    auto fail = [&](const char* why) { s.errors.push_back(id + ": " + why); };
    api->setHost(&host());
    if (!ok(callAction(*p, kOfxActionLoad, nullptr)))
        return fail("no se pudo cargar");

    p->descriptor = std::make_unique<EffectObj>();
    EffectObj& d = *p->descriptor;
    d.plugin = p.get();
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
    if (!ok(callAction(*p, kOfxActionDescribe, &d)))
        return fail("no se pudo describir");

    const auto contexts = d.strs(kOfxImageEffectPropSupportedContexts);
    auto has = [&](const char* c) { return std::find(contexts.begin(), contexts.end(), c) != contexts.end(); };
    p->context = has(kOfxImageEffectContextFilter) ? kOfxImageEffectContextFilter
               : has(kOfxImageEffectContextGeneral) ? kOfxImageEffectContextGeneral
                                                    : std::string();
    if (p->context.empty())
        return fail("no es un filtro (contextos admitidos: generadores, transiciones...)");

    p->contextDescriptor = std::make_unique<EffectObj>();
    EffectObj& c = *p->contextDescriptor;
    static_cast<PropertySet&>(c) = static_cast<const PropertySet&>(d);
    c.plugin = p.get();
    c.params.owner = &c;
    c.setS(kOfxImageEffectPropContext, {p->context});
    PropertySet inArgs;
    inArgs.setS(kOfxImageEffectPropContext, {p->context});
    if (!ok(callAction(*p, kOfxImageEffectActionDescribeInContext, &c, &inArgs)))
        return fail("no se pudo describir como filtro");
    ClipObj* output = c.clip(kOfxImageEffectOutputClipName);
    ClipObj* source = c.clip(kOfxImageEffectSimpleSourceClipName);
    auto rgba = [](const ClipObj* clip) {
        const auto comps = clip->strs(kOfxImageEffectPropSupportedComponents);
        return comps.empty() || std::find(comps.begin(), comps.end(), kOfxImageComponentRGBA) != comps.end();
    };
    if (!output || !source)
        return fail("le faltan los clips Source u Output");
    if (!rgba(output) || !rgba(source))
        return fail("no admite imágenes RGBA");

    auto info = std::make_shared<PluginInfo>();
    info->id = id;
    info->versionMajor = int(api->pluginVersionMajor);
    info->versionMinor = int(api->pluginVersionMinor);
    info->label = c.str(kOfxPropLabel, 0, id);
    info->group = c.str(kOfxImageEffectPluginPropGrouping);
    info->description = c.str(kOfxPropPluginDescription);
    info->binary = binaryPath.toStdString();
    info->context = p->context == kOfxImageEffectContextFilter ? "Filter" : "General";
    const auto depths = c.strs(kOfxImageEffectPropSupportedPixelDepths);
    info->floatImages = std::find(depths.begin(), depths.end(), kOfxBitDepthFloat) != depths.end();
    if (!info->floatImages && std::find(depths.begin(), depths.end(), kOfxBitDepthByte) == depths.end() && !depths.empty())
        return fail("no admite imágenes de 8 bits ni en coma flotante");
    for (const auto& param : c.params.list)
        info->params.push_back(describeParam(*param));
    p->unsafe = c.str(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe;
    p->info = std::move(info);

    // Otra versión del mismo plugin: se queda la más reciente.
    auto same = std::find_if(s.plugins.begin(), s.plugins.end(), [&](const auto& x) { return x->info->id == id; });
    if (same != s.plugins.end()) {
        const auto& old = *(*same)->info;
        if (std::tie(old.versionMajor, old.versionMinor) >= std::tie(p->info->versionMajor, p->info->versionMinor))
            return;
        *same = std::move(p);
        return;
    }
    s.plugins.push_back(std::move(p));
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

std::vector<PluginInfo> Registry::plugins()
{
    ensureScanned();
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    std::vector<PluginInfo> out;
    for (const auto& p : s.plugins)
        out.push_back(*p->info);
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) {
        return std::tie(a.group, a.label) < std::tie(b.group, b.label);
    });
    return out;
}

std::shared_ptr<const PluginInfo> Registry::find(const std::string& id)
{
    ensureScanned();
    RegistryState& s = state();
    std::lock_guard lock(s.mutex);
    for (const auto& p : s.plugins)
        if (p->info->id == id)
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

struct Effect::Impl {
    LoadedPlugin* plugin = nullptr;
    std::unique_ptr<EffectObj> obj;
    std::vector<float> inF, outF;
    std::vector<uint8_t> outB;
    bool created = false;

    ~Impl()
    {
        if (created && plugin)
            callAction(*plugin, kOfxActionDestroyInstance, obj.get());
    }
};

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

std::unique_ptr<Effect> Effect::create(const std::string& pluginId, int width, int height, double frameRate, std::string* error)
{
    Registry::instance().ensureScanned();
    RegistryState& s = state();
    LoadedPlugin* plugin = nullptr;
    {
        std::lock_guard lock(s.mutex);
        for (const auto& p : s.plugins)
            if (p->info->id == pluginId)
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
    e.floatImages = plugin->info->floatImages;
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
        const bool connected = c->name == kOfxImageEffectOutputClipName || c->name == kOfxImageEffectSimpleSourceClipName;
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
    std::unique_lock renderLock(plugin->renderMutex, std::defer_lock);
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

bool Effect::render(const ParamValues& values, double time, const uint8_t* in, uint8_t* out, std::string* error)
{
    return renderImpl(values, time, in, out, false, error);
}

bool Effect::render(const ParamValues& values, double time, const float* in, float* out, std::string* error)
{
    return renderImpl(values, time, in, out, true, error);
}

bool Effect::renderImpl(const ParamValues& values, double time, const void* in, void* out, bool floatIO, std::string* error)
{
    EffectObj& e = *d->obj;
    LoadedPlugin& plugin = *d->plugin;
    const int w = e.width, h = e.height;
    e.time = time;
    for (auto& p : e.params.list) {
        auto it = values.find(p->name);
        if (it != values.end() && !isStringType(p->type)) {
            p->curve = it->second;
            p->curve.resize(size_t(dimensionOf(p->type)));
        }
        if (!isStringType(p->type))
            p->setI(kOfxParamPropIsAnimating, {!keyTimes(*p).empty() ? 1 : 0});
    }

    // Imágenes de abajo arriba (como espera OFX): el puntero a la fila de abajo y un paso negativo.
    // Si la profundidad del plugin no es la de las imágenes, se convierte a la suya.
    const size_t pixels = size_t(w) * size_t(h);
    const size_t values4 = pixels * 4;
    const auto* in8 = static_cast<const uint8_t*>(in);
    const auto* inF = static_cast<const float*>(in);
    std::vector<uint8_t> inCopy;
    if (e.floatImages) {
        d->inF.resize(values4);
        d->outF.assign(values4, 0.0f);
        if (floatIO)
            std::copy(inF, inF + values4, d->inF.begin());
        else
            for (size_t i = 0; i < values4; ++i)
                d->inF[i] = float(in8[i]) / 255.0f;
        const int stride = w * 4 * int(sizeof(float));
        e.rowBytes = -stride;
        e.inData = reinterpret_cast<uint8_t*>(d->inF.data()) + ptrdiff_t(h - 1) * stride;
        e.outData = reinterpret_cast<uint8_t*>(d->outF.data()) + ptrdiff_t(h - 1) * stride;
    } else {
        // El plugin no debería escribir la entrada, pero por si acaso trabaja sobre una copia.
        if (floatIO) {
            inCopy.resize(values4);
            for (size_t i = 0; i < values4; ++i)
                inCopy[i] = uint8_t(std::clamp(std::lround(inF[i] * 255.0f), 0L, 255L));
        } else {
            inCopy.assign(in8, in8 + values4);
        }
        d->outB.assign(values4, 0);
        const int stride = w * 4;
        e.rowBytes = -stride;
        e.inData = inCopy.data() + ptrdiff_t(h - 1) * stride;
        e.outData = d->outB.data() + ptrdiff_t(h - 1) * stride;
    }

    std::unique_lock renderLock(plugin.renderMutex, std::defer_lock);
    if (plugin.unsafe)
        renderLock.lock();
    PropertySet seq;
    seq.setD(kOfxImageEffectPropFrameRange, {time, time});
    seq.setD(kOfxImageEffectPropFrameStep, {1.0});
    seq.setI(kOfxPropIsInteractive, {0});
    seq.setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    seq.setI(kOfxImageEffectPropSequentialRenderStatus, {0});
    seq.setI(kOfxImageEffectPropInteractiveRenderStatus, {0});
    callAction(plugin, kOfxImageEffectActionBeginSequenceRender, &e, &seq);
    PropertySet args;
    args.setD(kOfxPropTime, {time});
    args.setS(kOfxImageEffectPropFieldToRender, {kOfxImageFieldNone});
    args.setI(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
    args.setD(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    args.setI(kOfxImageEffectPropSequentialRenderStatus, {0});
    args.setI(kOfxImageEffectPropInteractiveRenderStatus, {0});
    args.setI(kOfxImageEffectPropRenderQualityDraft, {0});
    const OfxStatus st = callAction(plugin, kOfxImageEffectActionRender, &e, &args);
    callAction(plugin, kOfxImageEffectActionEndSequenceRender, &e, &seq);
    e.inData = e.outData = nullptr;
    if (!ok(st)) {
        if (error)
            *error = st == kOfxStatErrFatal ? "El efecto " + plugin.info->label + " falló al renderizar."
                                             : "El efecto " + plugin.info->label + " no pudo renderizar." + (e.lastMessage.empty() ? "" : " " + e.lastMessage);
        return false;
    }
    if (e.floatImages && floatIO)
        std::copy(d->outF.begin(), d->outF.end(), static_cast<float*>(out));
    else if (e.floatImages)
        for (size_t i = 0; i < values4; ++i)
            static_cast<uint8_t*>(out)[i] = uint8_t(std::clamp(std::lround(d->outF[i] * 255.0f), 0L, 255L));
    else if (floatIO)
        for (size_t i = 0; i < values4; ++i)
            static_cast<float*>(out)[i] = float(d->outB[i]) / 255.0f;
    else
        std::copy(d->outB.begin(), d->outB.end(), static_cast<uint8_t*>(out));
    return true;
}

} // namespace movix::ofx
