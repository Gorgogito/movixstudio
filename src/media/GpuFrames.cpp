#include "media/GpuFrames.h"

#ifdef _WIN32
#include <d3d11.h>
#endif

namespace movix::media {

#ifdef _WIN32

struct GpuTexturePool::Shared {
    std::mutex mutex;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    std::vector<ID3D11Texture2D*> free;
    size_t total = 0;
    ~Shared()
    {
        for (ID3D11Texture2D* t : free)
            t->Release();
        if (context)
            context->Release();
        if (device)
            device->Release();
    }
};

GpuTexturePool::GpuTexturePool(void* device, int width, int height, bool deep)
    : m_shared(std::make_shared<Shared>()), m_width(width), m_height(height), m_deep(deep)
{
    m_shared->device = static_cast<ID3D11Device*>(device);
    if (m_shared->device) {
        m_shared->device->AddRef();
        m_shared->device->GetImmediateContext(&m_shared->context);
    }
}

GpuTexturePool::~GpuTexturePool() = default;

void* GpuTexturePool::copyFrom(void* source, int index, std::shared_ptr<void>& hold)
{
    Shared& s = *m_shared;
    if (!s.device || !s.context || !source)
        return nullptr;
    ID3D11Texture2D* tex = nullptr;
    {
        std::lock_guard lock(s.mutex);
        if (!s.free.empty()) {
            tex = s.free.back();
            s.free.pop_back();
        }
    }
    if (!tex) {
        if (s.total >= 64) // algo retiene demasiados fotogramas: mejor el camino por memoria
            return nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(m_width);
        desc.Height = UINT(m_height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = m_deep ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(s.device->CreateTexture2D(&desc, nullptr, &tex)))
            return nullptr;
        ++s.total;
    }
    // La copia queda en la cola de la GPU antes que el dibujo que la use (mismo contexto).
    const D3D11_BOX box{0, 0, 0, UINT(m_width), UINT(m_height), 1};
    s.context->CopySubresourceRegion(tex, 0, 0, 0, 0, static_cast<ID3D11Texture2D*>(source), UINT(index), &box);
    std::weak_ptr<Shared> pool = m_shared;
    hold = std::shared_ptr<void>(tex, [pool](void* p) {
        auto* t = static_cast<ID3D11Texture2D*>(p);
        if (auto shared = pool.lock()) {
            std::lock_guard lock(shared->mutex);
            shared->free.push_back(t);
        } else {
            t->Release();
        }
    });
    return tex;
}

bool enableMultithreadProtection(void* device)
{
    auto* dev = static_cast<ID3D11Device*>(device);
    if (!dev)
        return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    ID3D10Multithread* mt = nullptr;
    const bool ok = ctx && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)));
    if (ok) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    if (ctx)
        ctx->Release();
    return ok;
}

#else

struct GpuTexturePool::Shared {};
GpuTexturePool::GpuTexturePool(void*, int width, int height, bool deep) : m_width(width), m_height(height), m_deep(deep) {}
GpuTexturePool::~GpuTexturePool() = default;
void* GpuTexturePool::copyFrom(void*, int, std::shared_ptr<void>&)
{
    return nullptr;
}
bool enableMultithreadProtection(void*)
{
    return false;
}

#endif

} // namespace movix::media
