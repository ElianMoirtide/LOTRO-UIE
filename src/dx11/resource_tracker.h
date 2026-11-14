#pragma once

#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace lotro_uie {

struct TextureMeta {
    uint32_t id = 0;
    UINT width = 0;
    UINT height = 0;
    UINT mipLevels = 0;
    UINT arraySize = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    UINT bindFlags = 0;
    UINT cpuAccessFlags = 0;
    UINT sampleCount = 1;
    uint64_t initialHash = 0;
    uint32_t dynamicUpdatesTotal = 0;
    uint32_t dynamicUpdatesInFrame = 0;
    bool isBackbuffer = false;
};

struct SrvMeta {
    uint32_t id = 0;
    ID3D11Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT mipLevels = 0;
};

struct RtvMeta {
    uint32_t id = 0;
    ID3D11Resource* resource = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool isBackbuffer = false;
};

struct DsvMeta {
    uint32_t id = 0;
    ID3D11Resource* resource = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct ShaderMeta {
    uint32_t id = 0;
    bool isPixelShader = false;
    bool isComputeShader = false;
    uint64_t hash = 0;
    std::vector<uint8_t> bytecode;
};

struct SamplerMeta {
    uint32_t id = 0;
    D3D11_SAMPLER_DESC desc{};
};

struct BlendMeta {
    uint32_t id = 0;
    D3D11_BLEND_DESC desc{};
};

struct DepthStencilMeta {
    uint32_t id = 0;
    D3D11_DEPTH_STENCIL_DESC desc{};
};

struct RasterizerMeta {
    uint32_t id = 0;
    D3D11_RASTERIZER_DESC desc{};
};

class ResourceTracker {
public:
    static ResourceTracker& Instance();

    void SetShuttingDown(bool shuttingDown) noexcept;
    bool IsShuttingDown() const noexcept;

    void TrackTexture2D(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC* desc,
                        const D3D11_SUBRESOURCE_DATA* initialData, bool isBackbuffer = false);
    void TrackSrv(ID3D11ShaderResourceView* srv, ID3D11Resource* resource,
                  const D3D11_SHADER_RESOURCE_VIEW_DESC* desc);
    void TrackRtv(ID3D11RenderTargetView* rtv, ID3D11Resource* resource,
                  const D3D11_RENDER_TARGET_VIEW_DESC* desc, bool isBackbuffer = false);
    void TrackDsv(ID3D11DepthStencilView* dsv, ID3D11Resource* resource,
                  const D3D11_DEPTH_STENCIL_VIEW_DESC* desc);
    void TrackVertexShader(ID3D11VertexShader* vs, const void* bytecode, size_t size);
    uint64_t TrackPixelShader(ID3D11PixelShader* ps, const void* bytecode, size_t size);
    void TrackComputeShader(ID3D11ComputeShader* cs, const void* bytecode, size_t size);
    void TrackSampler(ID3D11SamplerState* sampler, const D3D11_SAMPLER_DESC* desc);
    void TrackBlendState(ID3D11BlendState* state, const D3D11_BLEND_DESC* desc);
    void TrackDepthStencilState(ID3D11DepthStencilState* state, const D3D11_DEPTH_STENCIL_DESC* desc);
    void TrackRasterizerState(ID3D11RasterizerState* state, const D3D11_RASTERIZER_DESC* desc);

    void RecordTextureDynamicUpdate(ID3D11Resource* resource);
    void RecordTextureSubresourceUpdate(ID3D11Resource* resource, UINT subresource,
                                        const D3D11_BOX* box, const void* srcData, UINT srcRowPitch);
    void PropagateTextureHashOnCopy(ID3D11Resource* dstResource, ID3D11Resource* srcResource);
    void ResetPerFrameCounters();
    void MarkBackbufferTexture(ID3D11Texture2D* backbufferTex);

    std::optional<TextureMeta> GetTexture(ID3D11Resource* resource) const;
    bool IsBackbufferTexture(ID3D11Resource* resource) const;
    std::optional<TextureMeta> GetTextureForSrv(ID3D11ShaderResourceView* srv, ID3D11Resource** outResource = nullptr);
    std::optional<RtvMeta> GetRtv(ID3D11RenderTargetView* rtv) const;
    std::optional<RtvMeta> GetOrTrackRtv(ID3D11RenderTargetView* rtv, bool* outLazy = nullptr);
    std::optional<DsvMeta> GetDsv(ID3D11DepthStencilView* dsv) const;
    std::shared_ptr<const ShaderMeta> GetVertexShader(ID3D11VertexShader* vs) const;
    std::shared_ptr<const ShaderMeta> GetPixelShader(ID3D11PixelShader* ps) const;
    std::vector<std::pair<ID3D11PixelShader*, uint64_t>> GetTrackedPixelShaderHashes() const;
    std::shared_ptr<const ShaderMeta> GetComputeShader(ID3D11ComputeShader* cs) const;
    std::optional<SamplerMeta> GetSampler(ID3D11SamplerState* sampler) const;
    std::optional<BlendMeta> GetBlendState(ID3D11BlendState* state) const;
    std::optional<DepthStencilMeta> GetDepthStencilState(ID3D11DepthStencilState* state) const;
    std::optional<RasterizerMeta> GetRasterizerState(ID3D11RasterizerState* state) const;

private:
    ResourceTracker() = default;

    enum class ObjKind : uint8_t {
        Texture2D,
        Srv,
        Rtv,
        Dsv,
        VertexShader,
        PixelShader,
        ComputeShader,
        Sampler,
        Blend,
        DepthStencil,
        Rasterizer,
    };

    void AttachDestructionHook(ID3D11DeviceChild* child, ObjKind kind, void* key);
    void OnObjectDestroyed(ObjKind kind, void* key);

    template <typename MapT, typename DescT, typename ComT>
    void TrackStateDesc(MapT& map, std::atomic<uint32_t>& nextId, ObjKind kind, ComT* obj, const DescT* desc);

    uint64_t TrackShader(std::unordered_map<void*, std::shared_ptr<const ShaderMeta>>& map,
                         std::atomic<uint32_t>& nextId, ObjKind kind, bool isPixelShader,
                         ID3D11DeviceChild* shader, void* key, const void* bytecode, size_t size);

    friend class DestructionTag;

    mutable std::shared_mutex mutex_;
    std::atomic<bool> shuttingDown_{false};
    std::atomic<uint32_t> nextTexId_{1};
    std::atomic<uint32_t> nextSrvId_{1};
    std::atomic<uint32_t> nextRtvId_{1};
    std::atomic<uint32_t> nextDsvId_{1};
    std::atomic<uint32_t> nextVsId_{1};
    std::atomic<uint32_t> nextPsId_{1};
    std::atomic<uint32_t> nextCsId_{1};
    std::atomic<uint32_t> nextSamplerId_{1};
    std::atomic<uint32_t> nextBlendId_{1};
    std::atomic<uint32_t> nextDepthId_{1};
    std::atomic<uint32_t> nextRastId_{1};

    std::unordered_map<void*, TextureMeta> textures_;
    std::unordered_map<void*, SrvMeta> srvs_;
    std::unordered_map<void*, RtvMeta> rtvs_;
    std::unordered_map<void*, DsvMeta> dsvs_;
    std::unordered_map<void*, std::shared_ptr<const ShaderMeta>> vs_;
    std::unordered_map<void*, std::shared_ptr<const ShaderMeta>> ps_;
    std::unordered_map<void*, std::shared_ptr<const ShaderMeta>> cs_;
    std::unordered_map<void*, SamplerMeta> samplers_;
    std::unordered_map<void*, BlendMeta> blends_;
    std::unordered_map<void*, DepthStencilMeta> depthStencils_;
    std::unordered_map<void*, RasterizerMeta> rasterizers_;
};

} // namespace lotro_uie
