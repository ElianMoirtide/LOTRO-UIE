#include "dx11/resource_tracker.h"
#include "dx11/dxgi_format_names.h"
#include "common/hash.h"

#include <algorithm>
#include <cstring>

namespace lotro_uie {

namespace {

// {9B8C207F-73C1-4B8A-8D11-5B90A07C1E42}
const GUID kTagGuid = {
    0x9b8c207f, 0x73c1, 0x4b8a, {0x8d, 0x11, 0x5b, 0x90, 0xa0, 0x7c, 0x1e, 0x42}
};

bool HasVisibleNonZeroPixels(const void* data, size_t size, DXGI_FORMAT format) {
    if (!data || size == 0) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);

    const bool isRgba8 =
        format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    if (isRgba8 && size >= 4) {
        // In RGBA8/BGRA8 UI textures, byte 3 of every 32-bit pixel is Alpha.
        // If every pixel has Alpha == 0, the texture is 100% transparent/invisible.
        size_t i = 0;
        for (; i + sizeof(uint64_t) <= size; i += sizeof(uint64_t)) {
            uint64_t w = 0;
            std::memcpy(&w, bytes + i, sizeof(uint64_t));
            if ((w & 0xFF000000FF000000ULL) != 0) {
                return true;
            }
        }
        for (; i + 4 <= size; i += 4) {
            if (bytes[i + 3] != 0) {
                return true;
            }
        }
        return false;
    }

    size_t i = 0;
    for (; i + sizeof(uint64_t) <= size; i += sizeof(uint64_t)) {
        uint64_t w = 0;
        std::memcpy(&w, bytes + i, sizeof(uint64_t));
        if (w != 0) {
            return true;
        }
    }
    for (; i < size; ++i) {
        if (bytes[i] != 0) {
            return true;
        }
    }
    return false;
}

uint64_t HashSubresourceData(const void* data, size_t pitch, size_t effectiveRows, DXGI_FORMAT format) {
    if (!data || pitch == 0 || effectiveRows <= 1) {
        return 0;
    }
    const size_t totalSafeBytes = pitch * (effectiveRows - 1);
    if (totalSafeBytes == 0) {
        return 0;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (totalSafeBytes <= 65536) {
        if (!HasVisibleNonZeroPixels(bytes, totalSafeBytes, format)) {
            return 0;
        }
        return HashBytes(bytes, totalSafeBytes);
    }

    // For buffers larger than 64 KB, sample 64 evenly-spaced 1024-byte windows across the full height.
    constexpr size_t kNumWindows = 64;
    constexpr size_t kWindowBytes = 1024;
    const size_t maxStart = totalSafeBytes - kWindowBytes;
    bool anyVisible = false;
    uint64_t h = 14695981039346656037ULL;
    for (size_t wIdx = 0; wIdx < kNumWindows; ++wIdx) {
        const size_t offset = ((maxStart * wIdx) / (kNumWindows - 1)) & ~static_cast<size_t>(3);
        const uint8_t* winPtr = bytes + offset;
        if (!anyVisible && HasVisibleNonZeroPixels(winPtr, kWindowBytes, format)) {
            anyVisible = true;
        }
        for (size_t b = 0; b < kWindowBytes; ++b) {
            h ^= static_cast<uint64_t>(winPtr[b]);
            h *= 1099511628211ULL;
        }
    }
    return anyVisible ? h : 0;
}

uint64_t HashInitialSubresource(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initialData) {
    if (!desc || !initialData || !initialData[0].pSysMem) {
        return 0;
    }
    if (desc->Width == 0 || desc->Height == 0) {
        return 0;
    }
    const size_t pitch = static_cast<size_t>(initialData[0].SysMemPitch);
    if (pitch == 0) {
        return 0;
    }
    const size_t height = static_cast<size_t>(desc->Height);
    const size_t effectiveRows = IsBlockCompressedFormat(desc->Format)
        ? std::max<size_t>(1, (height + 3) / 4)
        : height;
    return HashSubresourceData(initialData[0].pSysMem, pitch, effectiveRows, desc->Format);
}

} // namespace

class DestructionTag final : public IUnknown {
public:
    DestructionTag(ResourceTracker* tracker, ResourceTracker::ObjKind kind, void* key) noexcept
        : tracker_(tracker), kind_(kind), key_(key) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppvObject = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rc = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rc == 0) {
            if (tracker_ && !tracker_->IsShuttingDown()) {
                tracker_->OnObjectDestroyed(kind_, key_);
            }
            delete this;
        }
        return rc;
    }

private:
    std::atomic<ULONG> refCount_{1};
    ResourceTracker* tracker_;
    ResourceTracker::ObjKind kind_;
    void* key_;
};

ResourceTracker& ResourceTracker::Instance() {
    // Intentionally leaked to avoid static destruction order issues during ExitProcess.
    static auto* instance = new ResourceTracker();
    return *instance;
}

void ResourceTracker::SetShuttingDown(bool shuttingDown) noexcept {
    shuttingDown_.store(shuttingDown, std::memory_order_release);
}

bool ResourceTracker::IsShuttingDown() const noexcept {
    return shuttingDown_.load(std::memory_order_acquire);
}

void ResourceTracker::AttachDestructionHook(ID3D11DeviceChild* child, ObjKind kind, void* key) {
    if (!child || IsShuttingDown()) return;
    auto* tag = new DestructionTag(this, kind, key);
    child->SetPrivateDataInterface(kTagGuid, tag);
    tag->Release();
}

void ResourceTracker::OnObjectDestroyed(ObjKind kind, void* key) {
    if (IsShuttingDown()) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    switch (kind) {
        case ObjKind::Texture2D:    textures_.erase(key); break;
        case ObjKind::Srv:           srvs_.erase(key); break;
        case ObjKind::Rtv:           rtvs_.erase(key); break;
        case ObjKind::Dsv:           dsvs_.erase(key); break;
        case ObjKind::VertexShader:  vs_.erase(key); break;
        case ObjKind::PixelShader:   ps_.erase(key); break;
        case ObjKind::ComputeShader: cs_.erase(key); break;
        case ObjKind::Sampler:      samplers_.erase(key); break;
        case ObjKind::Blend:        blends_.erase(key); break;
        case ObjKind::DepthStencil: depthStencils_.erase(key); break;
        case ObjKind::Rasterizer:   rasterizers_.erase(key); break;
    }
}

void ResourceTracker::TrackTexture2D(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC* desc,
                                     const D3D11_SUBRESOURCE_DATA* initialData, bool isBackbuffer) {
    if (!tex || !desc || IsShuttingDown()) return;

    TextureMeta meta;
    meta.width = desc->Width;
    meta.height = desc->Height;
    meta.mipLevels = desc->MipLevels;
    meta.arraySize = desc->ArraySize;
    meta.format = desc->Format;
    meta.usage = desc->Usage;
    meta.bindFlags = desc->BindFlags;
    meta.cpuAccessFlags = desc->CPUAccessFlags;
    meta.sampleCount = desc->SampleDesc.Count;
    meta.initialHash = HashInitialSubresource(desc, initialData);
    meta.isBackbuffer = isBackbuffer;

    bool isNew = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = textures_.find(tex);
        if (it == textures_.end()) {
            meta.id = nextTexId_.fetch_add(1, std::memory_order_relaxed);
            textures_.emplace(tex, meta);
            isNew = true;
        } else {
            if (isBackbuffer) it->second.isBackbuffer = true;
        }
    }
    if (isNew) {
        AttachDestructionHook(tex, ObjKind::Texture2D, tex);
    }
}

void ResourceTracker::MarkBackbufferTexture(ID3D11Texture2D* backbufferTex) {
    if (!backbufferTex || IsShuttingDown()) return;
    D3D11_TEXTURE2D_DESC desc{};
    backbufferTex->GetDesc(&desc);
    TrackTexture2D(backbufferTex, &desc, nullptr, true);
}

void ResourceTracker::TrackSrv(ID3D11ShaderResourceView* srv, ID3D11Resource* resource,
                               const D3D11_SHADER_RESOURCE_VIEW_DESC* desc) {
    if (!srv || !resource || IsShuttingDown()) return;

    // Ensure underlying Texture2D is tracked if it wasn't created through our CreateTexture2D hook.
    {
        bool texKnown = false;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            texKnown = (textures_.find(resource) != textures_.end());
        }
        if (!texKnown) {
            ID3D11Texture2D* tex2d = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) && tex2d) {
                D3D11_TEXTURE2D_DESC texDesc{};
                tex2d->GetDesc(&texDesc);
                TrackTexture2D(tex2d, &texDesc, nullptr, false);
                tex2d->Release();
            }
        }
    }

    SrvMeta meta;
    meta.resource = resource;
    if (desc) {
        meta.format = desc->Format;
        if (desc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            meta.mipLevels = desc->Texture2D.MipLevels;
        }
    }

    bool isNew = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = srvs_.try_emplace(srv);
        if (inserted) {
            meta.id = nextSrvId_.fetch_add(1, std::memory_order_relaxed);
            it->second = meta;
            isNew = true;
        }
    }
    if (isNew) {
        AttachDestructionHook(srv, ObjKind::Srv, srv);
    }
}

namespace {

bool ResolveTexture2DFromResource(ResourceTracker& tracker, ID3D11Resource* resource,
                                  UINT& outW, UINT& outH, DXGI_FORMAT& inOutFmt,
                                  bool& inOutBackbuffer) {
    if (!resource) return false;
    ID3D11Texture2D* tex2d = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) || !tex2d) {
        return false;
    }
    D3D11_TEXTURE2D_DESC texDesc{};
    tex2d->GetDesc(&texDesc);
    outW = texDesc.Width;
    outH = texDesc.Height;
    if (inOutFmt == DXGI_FORMAT_UNKNOWN) inOutFmt = texDesc.Format;
    if (auto existing = tracker.GetTexture(tex2d); existing && existing->isBackbuffer) {
        inOutBackbuffer = true;
    } else {
        tracker.TrackTexture2D(tex2d, &texDesc, nullptr, inOutBackbuffer);
    }
    tex2d->Release();
    return true;
}

} // namespace

void ResourceTracker::TrackRtv(ID3D11RenderTargetView* rtv, ID3D11Resource* resource,
                               const D3D11_RENDER_TARGET_VIEW_DESC* desc, bool isBackbuffer) {
    if (!rtv || !resource || IsShuttingDown()) return;

    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = desc ? desc->Format : DXGI_FORMAT_UNKNOWN;
    bool texBackbuffer = isBackbuffer;
    ResolveTexture2DFromResource(*this, resource, w, h, fmt, texBackbuffer);

    RtvMeta meta;
    meta.resource = resource;
    meta.width = w;
    meta.height = h;
    meta.format = fmt;
    meta.isBackbuffer = texBackbuffer;

    bool isNew = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = rtvs_.try_emplace(rtv);
        if (inserted) {
            meta.id = nextRtvId_.fetch_add(1, std::memory_order_relaxed);
            it->second = meta;
            isNew = true;
        } else if (texBackbuffer) {
            it->second.isBackbuffer = true;
        }
    }
    if (isNew) {
        AttachDestructionHook(rtv, ObjKind::Rtv, rtv);
    }
}

void ResourceTracker::TrackDsv(ID3D11DepthStencilView* dsv, ID3D11Resource* resource,
                               const D3D11_DEPTH_STENCIL_VIEW_DESC* desc) {
    if (!dsv || !resource || IsShuttingDown()) return;

    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = desc ? desc->Format : DXGI_FORMAT_UNKNOWN;
    bool ignoredBackbuffer = false;
    ResolveTexture2DFromResource(*this, resource, w, h, fmt, ignoredBackbuffer);

    DsvMeta meta;
    meta.resource = resource;
    meta.width = w;
    meta.height = h;
    meta.format = fmt;

    bool isNew = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = dsvs_.try_emplace(dsv);
        if (inserted) {
            meta.id = nextDsvId_.fetch_add(1, std::memory_order_relaxed);
            it->second = meta;
            isNew = true;
        }
    }
    if (isNew) {
        AttachDestructionHook(dsv, ObjKind::Dsv, dsv);
    }
}

template <typename MapT, typename DescT, typename ComT>
void ResourceTracker::TrackStateDesc(MapT& map, std::atomic<uint32_t>& nextId,
                                     ObjKind kind, ComT* obj, const DescT* desc) {
    if (!obj || !desc || IsShuttingDown()) return;
    bool isNew = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = map.try_emplace(obj);
        if (inserted) {
            it->second.id = nextId.fetch_add(1, std::memory_order_relaxed);
            it->second.desc = *desc;
            isNew = true;
        }
    }
    if (isNew) {
        AttachDestructionHook(obj, kind, obj);
    }
}

uint64_t ResourceTracker::TrackShader(std::unordered_map<void*, std::shared_ptr<const ShaderMeta>>& map,
                                      std::atomic<uint32_t>& nextId, ObjKind kind, bool isPixelShader,
                                      ID3D11DeviceChild* shader, void* key, const void* bytecode, size_t size) {
    if (!shader || !key || IsShuttingDown()) return 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = map.find(key);
        if (it != map.end() && it->second) return it->second->hash;
    }

    auto meta = std::make_shared<ShaderMeta>();
    meta->isPixelShader = isPixelShader;
    meta->isComputeShader = (kind == ObjKind::ComputeShader);
    const uint64_t computedHash = HashBytes(bytecode, size);
    meta->hash = computedHash;
    if (bytecode && size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(bytecode);
        meta->bytecode.assign(bytes, bytes + size);
    }

    bool isNew = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = map.try_emplace(key);
        if (inserted) {
            meta->id = nextId.fetch_add(1, std::memory_order_relaxed);
            it->second = std::move(meta);
            isNew = true;
        }
    }
    if (isNew) {
        AttachDestructionHook(shader, kind, key);
    }
    return computedHash;
}

void ResourceTracker::TrackVertexShader(ID3D11VertexShader* vs, const void* bytecode, size_t size) {
    TrackShader(vs_, nextVsId_, ObjKind::VertexShader, false, vs, vs, bytecode, size);
}

uint64_t ResourceTracker::TrackPixelShader(ID3D11PixelShader* ps, const void* bytecode, size_t size) {
    return TrackShader(ps_, nextPsId_, ObjKind::PixelShader, true, ps, ps, bytecode, size);
}

void ResourceTracker::TrackComputeShader(ID3D11ComputeShader* cs, const void* bytecode, size_t size) {
    TrackShader(cs_, nextCsId_, ObjKind::ComputeShader, false, cs, cs, bytecode, size);
}

void ResourceTracker::TrackSampler(ID3D11SamplerState* sampler, const D3D11_SAMPLER_DESC* desc) {
    TrackStateDesc(samplers_, nextSamplerId_, ObjKind::Sampler, sampler, desc);
}

void ResourceTracker::TrackBlendState(ID3D11BlendState* state, const D3D11_BLEND_DESC* desc) {
    TrackStateDesc(blends_, nextBlendId_, ObjKind::Blend, state, desc);
}

void ResourceTracker::TrackDepthStencilState(ID3D11DepthStencilState* state, const D3D11_DEPTH_STENCIL_DESC* desc) {
    TrackStateDesc(depthStencils_, nextDepthId_, ObjKind::DepthStencil, state, desc);
}

void ResourceTracker::TrackRasterizerState(ID3D11RasterizerState* state, const D3D11_RASTERIZER_DESC* desc) {
    TrackStateDesc(rasterizers_, nextRastId_, ObjKind::Rasterizer, state, desc);
}

void ResourceTracker::RecordTextureDynamicUpdate(ID3D11Resource* resource) {
    if (!resource || IsShuttingDown()) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = textures_.find(resource);
    if (it != textures_.end()) {
        ++it->second.dynamicUpdatesTotal;
        ++it->second.dynamicUpdatesInFrame;
    }
}

void ResourceTracker::RecordTextureSubresourceUpdate(
    ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box,
    const void* srcData, UINT srcRowPitch
) {
    if (!resource || IsShuttingDown()) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = textures_.find(resource);
    if (it == textures_.end()) return;
    ++it->second.dynamicUpdatesTotal;
    ++it->second.dynamicUpdatesInFrame;

    if (subresource == 0 && srcData && srcRowPitch > 0) {
        const bool isFullUpdate = (box == nullptr) ||
            (box->left == 0 && box->top == 0 &&
             box->right >= it->second.width && box->bottom >= it->second.height);
        if (isFullUpdate) {
            const size_t height = static_cast<size_t>(it->second.height);
            const size_t effectiveRows = IsBlockCompressedFormat(it->second.format)
                ? std::max<size_t>(1, (height + 3) / 4)
                : height;
            it->second.initialHash = HashSubresourceData(
                srcData, static_cast<size_t>(srcRowPitch), effectiveRows, it->second.format);
        } else if (it->second.initialHash == 0 && box &&
                   box->right > box->left && box->bottom > box->top) {
            const size_t boxH = static_cast<size_t>(box->bottom - box->top);
            const size_t effectiveRows = IsBlockCompressedFormat(it->second.format)
                ? std::max<size_t>(1, (boxH + 3) / 4)
                : boxH;
            const uint64_t subHash = HashSubresourceData(
                srcData, static_cast<size_t>(srcRowPitch), effectiveRows, it->second.format);
            if (subHash != 0) {
                uint64_t mixed = subHash;
                mixed ^= (static_cast<uint64_t>(it->second.width) << 32) | it->second.height;
                mixed *= 1099511628211ULL;
                mixed ^= (static_cast<uint64_t>(box->left) << 16) | box->top;
                mixed *= 1099511628211ULL;
                it->second.initialHash = mixed;
            }
        }
    }
}

void ResourceTracker::PropagateTextureHashOnCopy(ID3D11Resource* dstResource, ID3D11Resource* srcResource) {
    if (!dstResource || !srcResource || dstResource == srcResource || IsShuttingDown()) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto dstIt = textures_.find(dstResource);
    auto srcIt = textures_.find(srcResource);
    if (dstIt != textures_.end() && srcIt != textures_.end()) {
        if (dstIt->second.initialHash == 0 && srcIt->second.initialHash != 0) {
            dstIt->second.initialHash = srcIt->second.initialHash;
        }
    }
}

void ResourceTracker::ResetPerFrameCounters() {
    if (IsShuttingDown()) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& [k, tex] : textures_) {
        tex.dynamicUpdatesInFrame = 0;
    }
}

std::optional<TextureMeta> ResourceTracker::GetTexture(ID3D11Resource* resource) const {
    if (!resource || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = textures_.find(resource);
    if (it == textures_.end()) return std::nullopt;
    return it->second;
}

bool ResourceTracker::IsBackbufferTexture(ID3D11Resource* resource) const {
    if (!resource || IsShuttingDown()) return false;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = textures_.find(resource);
    return (it != textures_.end() && it->second.isBackbuffer);
}

std::optional<TextureMeta> ResourceTracker::GetTextureForSrv(ID3D11ShaderResourceView* srv, ID3D11Resource** outResource) {
    if (outResource) *outResource = nullptr;
    if (!srv || IsShuttingDown()) return std::nullopt;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto srvIt = srvs_.find(srv);
        if (srvIt != srvs_.end() && srvIt->second.resource) {
            auto texIt = textures_.find(srvIt->second.resource);
            if (texIt != textures_.end()) {
                if (outResource) *outResource = srvIt->second.resource;
                return texIt->second;
            }
        }
    }
    // Lazy-track SRV & underlying Texture2D (e.g., if created prior to hook attach in dxgi.dll mode).
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return std::nullopt;
    ID3D11Texture2D* tex2d = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) && tex2d) {
        if (!GetTexture(tex2d)) {
            D3D11_TEXTURE2D_DESC td{};
            tex2d->GetDesc(&td);
            TrackTexture2D(tex2d, &td, nullptr, false);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        srv->GetDesc(&sd);
        TrackSrv(srv, tex2d, &sd);
        tex2d->Release();
    }
    res->Release();

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto srvIt = srvs_.find(srv);
    if (srvIt == srvs_.end() || !srvIt->second.resource) return std::nullopt;
    auto texIt = textures_.find(srvIt->second.resource);
    if (texIt == textures_.end()) return std::nullopt;
    if (outResource) *outResource = srvIt->second.resource;
    return texIt->second;
}

std::optional<RtvMeta> ResourceTracker::GetRtv(ID3D11RenderTargetView* rtv) const {
    if (!rtv || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = rtvs_.find(rtv);
    if (it == rtvs_.end()) return std::nullopt;
    return it->second;
}

std::optional<RtvMeta> ResourceTracker::GetOrTrackRtv(ID3D11RenderTargetView* rtv, bool* outLazy) {
    if (outLazy) *outLazy = false;
    if (!rtv || IsShuttingDown()) return std::nullopt;
    if (auto existing = GetRtv(rtv)) {
        return existing;
    }
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (!res) return std::nullopt;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtv->GetDesc(&rtvDesc);
    TrackRtv(rtv, res, &rtvDesc, false);
    res->Release();
    if (outLazy) *outLazy = true;
    return GetRtv(rtv);
}

std::optional<DsvMeta> ResourceTracker::GetDsv(ID3D11DepthStencilView* dsv) const {
    if (!dsv || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = dsvs_.find(dsv);
    if (it == dsvs_.end()) return std::nullopt;
    return it->second;
}

std::shared_ptr<const ShaderMeta> ResourceTracker::GetVertexShader(ID3D11VertexShader* vs) const {
    if (!vs || IsShuttingDown()) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = vs_.find(vs);
    if (it == vs_.end()) return nullptr;
    return it->second;
}

std::shared_ptr<const ShaderMeta> ResourceTracker::GetPixelShader(ID3D11PixelShader* ps) const {
    if (!ps || IsShuttingDown()) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = ps_.find(ps);
    if (it == ps_.end()) return nullptr;
    return it->second;
}

std::vector<std::pair<ID3D11PixelShader*, uint64_t>> ResourceTracker::GetTrackedPixelShaderHashes() const {
    std::vector<std::pair<ID3D11PixelShader*, uint64_t>> out;
    if (IsShuttingDown()) return out;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    out.reserve(ps_.size());
    for (const auto& [ptr, meta] : ps_) {
        if (ptr && meta && meta->hash != 0) {
            out.emplace_back(static_cast<ID3D11PixelShader*>(ptr), meta->hash);
        }
    }
    return out;
}

std::shared_ptr<const ShaderMeta> ResourceTracker::GetComputeShader(ID3D11ComputeShader* cs) const {
    if (!cs || IsShuttingDown()) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = cs_.find(cs);
    if (it == cs_.end()) return nullptr;
    return it->second;
}

std::optional<SamplerMeta> ResourceTracker::GetSampler(ID3D11SamplerState* sampler) const {
    if (!sampler || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = samplers_.find(sampler);
    if (it == samplers_.end()) return std::nullopt;
    return it->second;
}

std::optional<BlendMeta> ResourceTracker::GetBlendState(ID3D11BlendState* state) const {
    if (!state || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = blends_.find(state);
    if (it == blends_.end()) return std::nullopt;
    return it->second;
}

std::optional<DepthStencilMeta> ResourceTracker::GetDepthStencilState(ID3D11DepthStencilState* state) const {
    if (!state || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = depthStencils_.find(state);
    if (it == depthStencils_.end()) return std::nullopt;
    return it->second;
}

std::optional<RasterizerMeta> ResourceTracker::GetRasterizerState(ID3D11RasterizerState* state) const {
    if (!state || IsShuttingDown()) return std::nullopt;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = rasterizers_.find(state);
    if (it == rasterizers_.end()) return std::nullopt;
    return it->second;
}

} // namespace lotro_uie
