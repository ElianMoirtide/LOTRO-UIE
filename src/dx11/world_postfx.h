#pragma once

#include "common/config.h"

#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace lotro_uie {

struct WorldPostFxStats {
    bool initialized = false;
    bool activeThisFrame = false;
    uint64_t totalPassesApplied = 0;
};

class WorldPostFx {
public:
    static WorldPostFx& Instance();

    void InitializeFromConfig(const Config& cfg);
    void ReloadFromConfig(const Config& cfg);
    void ToggleEnabled();
    bool IsEnabled() const noexcept;

    void OnFrameBegin();
    void OnBeforeResizeBuffers();

    // Resolves/copies the current swapchain backbuffer (supporting MSAA > 1) and applies
    // 3D-world CAS + Debanding + Clarity + Vibrance strictly before UI draws begin.
    // Restores all DX11 pipeline state so caller can execute the pending UI draw untouched.
    bool ApplyToBackbuffer(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        IDXGISwapChain* swapChain
    );

    WorldPostFxStats GetStats() const;
    void Shutdown();

private:
    bool EnsureShaders(ID3D11Device* device);
    bool EnsureRenderResources(ID3D11Device* device, ID3D11Texture2D* backbuffer);
    void ReleaseRenderResources();

    mutable std::mutex mutex_;
    ID3D11Device* device_ = nullptr;
    bool shadersReady_ = false;
    bool initFailed_ = false;

    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11Buffer* paramCb_ = nullptr;
    ID3D11SamplerState* pointSampler_ = nullptr;
    ID3D11SamplerState* linearSampler_ = nullptr;
    ID3D11BlendState* opaqueBlend_ = nullptr;
    ID3D11DepthStencilState* noDepthState_ = nullptr;
    ID3D11RasterizerState* rastState_ = nullptr;

    ID3D11Texture2D* cachedBackbuffer_ = nullptr;
    ID3D11RenderTargetView* bbRtv_ = nullptr;
    ID3D11Texture2D* resolvedTex_ = nullptr;
    ID3D11ShaderResourceView* resolvedSrv_ = nullptr;
    uint32_t resW_ = 0;
    uint32_t resH_ = 0;
    DXGI_FORMAT resFmt_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT resolveFmt_ = DXGI_FORMAT_UNKNOWN;

    uint64_t totalPassesApplied_ = 0;
    bool appliedThisFrame_ = false;
    std::atomic<bool> initializedFromConfig_{false};
    std::atomic<bool> enabled_{true};
    float casStrength_ = 0.45f;
    float clarity_ = 0.14f;
    float vibrance_ = 0.12f;
    float dehaze_ = 0.18f;
    float antiGlare_ = 0.25f;
    bool deband_ = true;
};

} // namespace lotro_uie
