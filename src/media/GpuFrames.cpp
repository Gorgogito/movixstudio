#include "media/GpuFrames.h"

#include <cstring>

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

std::shared_ptr<VideoFrame> downloadGpuFrame(const VideoFrame& frame)
{
    auto* tex = static_cast<ID3D11Texture2D*>(frame.gpuTexture);
    if (!tex || frame.width <= 0 || frame.height <= 0)
        return nullptr;
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    std::shared_ptr<VideoFrame> out;
    if (SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &staging))) {
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            // NV12/P010 en memoria: el plano Y y, a continuación (RowPitch × alto), el UV intercalado.
            const bool deep = frame.format == PixelFormat::P016;
            const size_t sample = deep ? 2 : 1;
            out = std::make_shared<VideoFrame>(frame);
            out->gpuTexture = nullptr;
            out->gpuHold.reset();
            const size_t yRow = size_t(frame.width) * sample, uvRow = size_t(frame.chromaWidth()) * 2 * sample;
            out->y.resize(yRow * size_t(frame.height));
            out->uv.resize(uvRow * size_t(frame.chromaHeight()));
            const auto* base = static_cast<const uint8_t*>(map.pData);
            for (int r = 0; r < frame.height; ++r)
                std::memcpy(out->y.data() + yRow * size_t(r), base + size_t(map.RowPitch) * size_t(r), yRow);
            const uint8_t* uv = base + size_t(map.RowPitch) * desc.Height;
            for (int r = 0; r < frame.chromaHeight(); ++r)
                std::memcpy(out->uv.data() + uvRow * size_t(r), uv + size_t(map.RowPitch) * size_t(r), uvRow);
            ctx->Unmap(staging, 0);
        }
        staging->Release();
    }
    ctx->Release();
    dev->Release();
    return out;
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
std::shared_ptr<VideoFrame> downloadGpuFrame(const VideoFrame&)
{
    return nullptr;
}

#endif

} // namespace movix::media
