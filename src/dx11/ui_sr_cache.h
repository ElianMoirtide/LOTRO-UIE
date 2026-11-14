#pragma once

#include "common/config.h"

#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lotro_uie {

struct UiSrStats {
    bool initialized = false;
    bool active = false;
    uint32_t entryCount = 0;
    uint64_t cacheBytesUsed = 0;
    uint64_t hitsInFrame = 0;
    uint64_t missesInFrame = 0;
    uint64_t dynamicRebuildsInFrame = 0;
    uint64_t totalUpscales = 0;
};

class UiSrCache {
public:
    static UiSrCache& Instance();

    void InitializeFromConfig(const Config& cfg);
    void ReloadFromConfig(const Config& cfg);
    void ToggleEnabled();
    bool IsEnabled() const noexcept;

    void OnFrameBegin();

    // Must be called when CPU/GPU overwrites a resource (UpdateSubresource / Map(WRITE*) / Copy / Resolve / Offscreen RTV).
    void MarkResourceDirty(ID3D11Resource* res);
    void MarkRenderTargetViewDirty(ID3D11RenderTargetView* rtv);

    // Called internally by COM private-data sentinel when a cached ID3D11Texture2D is destroyed.
    void OnSourceTextureDestroyed(ID3D11Texture2D* tex, uint64_t generation);

    // Returns a cache-owned SRV (NOT AddRef'd for caller convenience; valid through current frame unless Shutdown)
    // when texture is eligible and upscaled. Returns nullptr cleanly on skip/fallback.
    ID3D11ShaderResourceView* GetOrUpscale(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* srcSrv,
        bool wrapU,
        bool wrapV
    );

    UiSrStats GetStats() const;
    void SetShuttingDown(bool shuttingDown) noexcept {
        shuttingDown_.store(shuttingDown, std::memory_order_release);
    }
    void Shutdown();

private:
    struct CacheKey {
        ID3D11Texture2D* tex = nullptr;
        bool wrapU = false;
        bool wrapV = false;
        bool operator==(const CacheKey& o) const noexcept {
            return tex == o.tex && wrapU == o.wrapU && wrapV == o.wrapV;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const noexcept {
            size_t h = reinterpret_cast<size_t>(k.tex);
            h ^= (static_cast<size_t>(k.wrapU ? 1 : 0) << 1);
            h ^= (static_cast<size_t>(k.wrapV ? 1 : 0) << 2);
            return h * 11400714819323198485ull;
        }
    };

    struct CacheEntry {
        ID3D11Texture2D* outTex = nullptr;
        ID3D11ShaderResourceView* outSrv = nullptr;
        ID3D11UnorderedAccessView* outUav = nullptr;
        uint32_t srcWidth = 0;
        uint32_t srcHeight = 0;
        DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN;
        uint64_t byteSize = 0;
        uint64_t generation = 0;
        uint64_t builtDirtyEpoch = 0;
        uint64_t lastUsedFrame = 0;
        uint64_t lastDirtyFrame = 0;
        uint32_t consecutiveDirtyFrames = 0;
        uint32_t cooldownFrames = 0;
    };

    struct RetiredTex {
        ID3D11Texture2D* tex = nullptr;
        uint64_t generation = 0;
    };

    bool EnsureInitialized(ID3D11Device* device);
    bool EnsureScratch(ID3D11Device* device, uint32_t w, uint32_t h);
    void ReleaseScratch();
    void DrainRetiredLocked();
    void EvictToBudgetLocked(uint64_t maxBytes);
    bool RunCnnPass(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSrv,
                    uint32_t w, uint32_t h, bool wrapU, bool wrapV,
                    ID3D11UnorderedAccessView* outUav);
    void ReleaseEntryResources(CacheEntry& e);

    mutable std::mutex mutex_;
    ID3D11Device* device_ = nullptr;
    bool initialized_ = false;
    bool initFailed_ = false;

    ID3D11ComputeShader* csConv1_ = nullptr;
    ID3D11ComputeShader* csConv2_ = nullptr;
    ID3D11ComputeShader* csConv3_ = nullptr;
    ID3D11ComputeShader* csConv4_ = nullptr;
    ID3D11Buffer* weightsCb_ = nullptr;
    ID3D11Buffer* dimsCb_ = nullptr;

    // Reusable R16G16B16A16_FLOAT[4] scratch arrays (sized monotonically up to max seen UI tex <= 1536)
    uint32_t scratchW_ = 0;
    uint32_t scratchH_ = 0;
    ID3D11Texture2D* featTex_[3]{};
    ID3D11ShaderResourceView* featSrv_[3]{};
    ID3D11UnorderedAccessView* featUav_[3]{};

    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> entries_;
    std::unordered_map<ID3D11Texture2D*, uint64_t> dirtyEpochs_;
    std::unordered_map<ID3D11Texture2D*, uint64_t> texGenerations_;
    std::vector<RetiredTex> retiredQueue_;

    uint64_t nextGeneration_ = 1;
    uint64_t totalBytes_ = 0;
    uint64_t frameIndex_ = 0;
    uint32_t newUpscalesInFrame_ = 0;
    uint32_t dynRebuildsInFrame_ = 0;
    uint64_t hitsInFrame_ = 0;
    uint64_t missesInFrame_ = 0;
    uint64_t totalUpscales_ = 0;
    std::atomic<bool> initializedFromConfig_{false};
    std::atomic<bool> enabled_{true};
    bool upscaleUiRts_ = true;
    uint32_t cacheMaxMb_ = 192;
    uint32_t maxNewPerFrame_ = 4;
    uint32_t maxDynamicPerFrame_ = 4;
    std::atomic<bool> shuttingDown_{false};
};

} // namespace lotro_uie
