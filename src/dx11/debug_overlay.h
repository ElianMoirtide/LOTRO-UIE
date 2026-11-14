#pragma once

#include "common/config.h"
#include "dx11/inspector.h"
#include "dx11/ui_enhancer.h"

#include <d3d11.h>
#include <dxgi.h>
#include <atomic>

namespace lotro_uie {

class DebugOverlay {
public:
    static DebugOverlay& Instance();

    void ToggleMenu();
    bool IsMenuOpen() const noexcept;

    void OnBeforeResizeBuffers();
    void Shutdown();

    void Render(
        IDXGISwapChain* swapChain,
        ID3D11Device* device,
        ID3D11DeviceContext* immediateCtx,
        const SwapChainInfo& swapInfo,
        const InspectorLiveStats& stats,
        const UiEnhancerStats& uieStats,
        Config& config
    );

private:
    DebugOverlay() = default;

    bool EnsureInitialized(ID3D11Device* device);

    bool initialized_ = false;
    bool initFailed_ = false;
    std::atomic<bool> menuOpen_{false};
    bool prevLButton_ = false;

    ID3D11Device* device_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11InputLayout* inputLayout_ = nullptr;
    ID3D11Buffer* vb_ = nullptr;
    ID3D11Texture2D* fontTex_ = nullptr;
    ID3D11ShaderResourceView* fontSrv_ = nullptr;
    ID3D11SamplerState* pointSampler_ = nullptr;
    ID3D11BlendState* blendState_ = nullptr;
    ID3D11DepthStencilState* depthState_ = nullptr;
    ID3D11RasterizerState* rastState_ = nullptr;
};

} // namespace lotro_uie
