#pragma once

#include "common/config.h"
#include "dx11/asset_dumper.h"
#include "dx11/context_state.h"
#include "dx11/resource_tracker.h"

#include <d3d11.h>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotro_uie {

struct SwapChainInfo {
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    UINT sampleCount = 1;
    BOOL windowed = TRUE;
    uintptr_t swapChainPtr = 0;
    uintptr_t hwnd = 0;
};

enum class DrawKind {
    Draw,
    DrawIndexed,
    DrawInstanced,
    DrawIndexedInstanced,
    DrawAuto,
    DrawInstancedIndirect,
    DrawIndexedInstancedIndirect,
};

struct CapturedGpuEvent {
    uint32_t eventIndex = 0;
    uint32_t afterDrawIndex = 0;
    uint32_t threadId = 0;
    std::string type;
    std::string details;
};

struct CapturedBoundTex {
    UINT slot = 0;
    uint32_t texId = 0;
    UINT width = 0;
    UINT height = 0;
    UINT mipLevels = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    uint32_t dynamicUpdatesInFrame = 0;
    uint32_t dynamicUpdatesTotal = 0;
};

struct CapturedDraw {
    uint32_t drawIndex = 0;
    DrawKind kind = DrawKind::DrawIndexed;
    UINT count = 0;
    UINT instanceCount = 1;
    UINT startLocation = 0;
    INT baseVertex = 0;

    uint32_t rtvId = 0;
    UINT rtvWidth = 0;
    UINT rtvHeight = 0;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    bool isBackbuffer = false;
    bool hasDsv = false;
    uint8_t numRtvs = 0;
    bool hasRtv0Ptr = false;
    bool lazyRtv = false;
    uint32_t dsvId = 0;
    UINT dsvWidth = 0;
    UINT dsvHeight = 0;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;

    uint32_t vsId = 0;
    uint64_t vsHash = 0;
    uint32_t psId = 0;
    uint64_t psHash = 0;

    bool depthEnable = true;
    bool depthWrite = true;
    bool blendEnable = false;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT vbStride0 = 0;
    D3D11_VIEWPORT viewport0{};
    bool hasViewport0 = false;
    D3D11_RECT scissor0{};
    bool hasScissor0 = false;

    bool hasSampler0 = false;
    D3D11_FILTER sampler0Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;

    std::vector<CapturedBoundTex> psTextures;
    bool isUiCandidate = false;
    std::string tags;
};

struct InspectorLiveStats {
    uint64_t frameIndex = 0;
    uint32_t lastFrameDraws = 0;
    uint32_t lastFrameUiCandidates = 0;
    std::string lastCaptureStatus;
};

class Inspector {
public:
    static Inspector& Instance();

    void RequestCapture(bool dumpAssets);
    bool IsCapturing() const;

    void OnFrameBegin(const Config& config);
    void RecordDrawCall(const ContextState& state, DrawKind kind,
                        UINT count, UINT instanceCount, UINT startLoc, INT baseVertex);
    void RecordDispatch(const ContextState& state, UINT groupX, UINT groupY, UINT groupZ, bool indirect);
    void RecordGpuEvent(const std::string& type, const std::string& details);
    void RetainResourceForCapture(ID3D11Resource* res);
    void SetRuntimeDiagnostics(std::vector<std::string> lines);
    void OnFrameEnd(ID3D11Device* device, ID3D11DeviceContext* immediateCtx,
                    const SwapChainInfo& swapInfo, const Config& config);

    InspectorLiveStats GetLiveStats() const;

private:
    Inspector() = default;

    struct RetainedTex {
        TextureMeta meta;
        ID3D11Texture2D* tex = nullptr; // AddRef'd during capture frame only
    };

    void ReleaseRetainedResources();

    mutable std::mutex mutex_;
    uint64_t frameIndex_ = 0;
    bool captureArmed_ = false;
    bool armedDumpAssets_ = false;
    bool capturingThisFrame_ = false;
    bool dumpAssetsForThisFrame_ = false;
    bool autoCaptureTriggered_ = false;

    uint32_t liveDraws_ = 0;
    uint32_t liveUiCandidates_ = 0;
    uint32_t lastFrameDraws_ = 0;
    uint32_t lastFrameUiCandidates_ = 0;
    std::string lastCaptureStatus_;

    std::vector<CapturedDraw> draws_;
    std::vector<CapturedGpuEvent> events_;
    std::vector<std::string> runtimeDiagnostics_;
    std::unordered_map<uint32_t, RetainedTex> retainedTextures_;
    std::unordered_map<uint64_t, std::shared_ptr<const ShaderMeta>> capturedShaders_;
};

} // namespace lotro_uie
