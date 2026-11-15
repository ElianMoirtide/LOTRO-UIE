#pragma once

#include "common/config.h"
#include "dx11/context_state.h"

#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotro_uie {

// Observed LOTRO UI / Font / Map Pixel Shader hashes (v0.1.1 real captures).
inline constexpr uint64_t kHashUiColorOpacityPs = 0x6B957ABB3F5938DBULL;
inline constexpr uint64_t kHashUiColorOpacityTexMatPs = 0x43AA4E3C051DFEB6ULL;

struct UiElementScaleGpuParams {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetNdcX = 0.0f;
    float offsetNdcY = 0.0f;
    float minWidthNdc = 0.0f;
    float maxWidthNdc = 10.0f;
    float minHeightNdc = 0.0f;
    float maxHeightNdc = 10.0f;
    float cursorNdcX = -10.0f;
    float cursorNdcY = -10.0f;
    float requireHover = 0.0f;
    float drawSlotTag = 0.0f;
};
static_assert(sizeof(UiElementScaleGpuParams) == 48, "UiElementScaleGpuParams must be 48 bytes");

struct UiDrawOverride {
    UiElementScaleGpuParams scaleParams{};
    ID3D11PixelShader* origPs = nullptr;
    ID3D11PixelShader* replacementPs = nullptr;
    ID3D11GeometryShader* replacementGs = nullptr;
    ID3D11ShaderResourceView* replacementSrv0 = nullptr;
    ID3D11ShaderResourceView* backdropSrv = nullptr;
    ID3D11SamplerState* linearSampler = nullptr;
    ID3D11Buffer* paramCb = nullptr;
    ID3D11Buffer* scaleCb = nullptr;
    ID3D11Buffer* soBuffer = nullptr;
    uint8_t cbMask = 0;
    bool isHoverFocus = false;
    bool active = false;
    bool isUiCandidateDraw = false;
    bool isBackbufferTarget = false;
    bool srApplied = false;
    bool isMapDraw = false;
    bool scaleApplied = false;
    bool gsHoverProbe = false;
    bool resetSoOffset = false;
};

struct UiEnhancerStats {
    bool ready = false;
    bool enabled = true;
    bool mapMode = true;
    bool splitScreen = false;
    bool hoverMagnifier = true;
    bool hoverFocusBoost = true;
    bool oledAutoDim = false;
    bool textureOverridesEnabled = true;
    float sharpness = 0.70f;
    float edgeAniso = 0.85f;
    float fontOutlineBoost = 0.35f;
    float fontWeightBoost = 0.25f;
    float themeDarkSlate = 0.00f;
    float glassmorphism = 0.55f;
    float iconPolish = 0.40f;
    uint32_t themeProfile = 0;
    uint32_t eyeComfortMode = 0;
    uint32_t lastFrameEnhancedDraws = 0;
    uint32_t lastFrameSrDraws = 0;
    uint32_t lastFrameMapDraws = 0;
    uint32_t lastFrameScaledDraws = 0;
    uint32_t elementScaleRules = 0;
    uint32_t loadedOverrides = 0;
    uint32_t hoveredTexId = 0;
    uint64_t hoveredTexHash = 0;
    uint32_t hoveredTexW = 0;
    uint32_t hoveredTexH = 0;
    uint32_t registeredTargetShaders = 0;
    std::string hudNotification;
};

class UiEnhancer {
public:
    static UiEnhancer& Instance();

    void InitializeFromConfig(const Config& cfg);
    void ReloadFromConfig(const Config& cfg);
    void ToggleEnabled();
    void ToggleSplitScreen();
    void ToggleMapMode();
    void ToggleHoverMagnifier();
    void ToggleHoverFocusBoost();
    void ToggleOledAutoDim();
    void CycleSharpnessPreset();
    void CycleFontOutlineBoost();
    void CycleFontWeightBoost();
    void CycleThemeDarkSlate();
    void CycleEyeComfortMode();
    void CycleGlassmorphism();
    void CycleIconPolish();
    float GetGlassmorphism() const noexcept;
    uint32_t GetEyeComfortMode() const noexcept;
    bool CopyHoveredElementToClipboard(std::string* outRuleLine = nullptr);
    bool AdjustHoveredElementScale(float deltaScale, Config& ioConfig);
    void SetHudNotification(const std::string& msg, uint32_t durationFrames = 180);
    void ExportCurrentStateToConfig(Config& ioConfig) const;

    bool IsEnabled() const noexcept;
    bool IsSplitScreenEnabled() const noexcept;
    void SetSwapChainViewportInfo(uint32_t width, uint32_t height, uintptr_t hwnd) noexcept;

    void OnDeviceCreated(ID3D11Device* device);
    void OnPixelShaderCreated(ID3D11Device* device, ID3D11PixelShader* ps, uint64_t hash);
    void OnPixelShaderDestroyed(ID3D11PixelShader* ps);

    bool PrepareDraw(ID3D11DeviceContext* ctx, const ContextState& st, UiDrawOverride& out, UINT drawCount = 0);
    void AttachBackdropBlurIfNeeded(UiDrawOverride& ov);
    void SyncGpuConstants(ID3D11DeviceContext* immediateCtx);
    void OnFrameEnd(ID3D11DeviceContext* immediateCtx = nullptr, bool freezeHoverSelection = false);

    UiEnhancerStats GetStats() const;

private:
    UiEnhancer() = default;

    struct RegisteredPs {
        ID3D11Device* device = nullptr;
        uint8_t variant = 0; // 0 = ps_76 layout (cb0[10].x), 1 = ps_3471 layout (cb0[14].x)
    };

    struct alignas(16) GpuParams {
        float sharpness;
        float anisoEdge;
        float wrapU;
        float wrapV;
        float isMap;
        float srActive;
        float mapBoost;
        float splitMinX;
        float fontOutlineBoost;
        float themeDarkSlate;
        float oledDimStrength;
        float screenHeight;
        float fontWeightBoost;
        float themeProfile;
        float eyeComfortMode;
        float hoverFocusBoost;
        float glassmorphism;
        float iconPolish;
        float screenWidth;
        float hasBackdropBlur;
    };
    static_assert(sizeof(GpuParams) == 80, "GpuParams must be 80 bytes");

    void ApplyConfigLocked(const Config& cfg);
    void ReleaseDeviceResources();
    void ClearOverrideCacheLocked();
    ID3D11ShaderResourceView* GetOrLoadOverrideSrvLocked(ID3D11Device* dev, uint64_t texHash, bool srgb);
    void RefreshFastPointersLocked();
    int MatchTargetVariant(uint64_t hash) const;

    mutable std::mutex mutex_;
    std::atomic<bool> initializedFromConfig_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> enhanceNameplates_{true};
    std::atomic<bool> enhanceDepthTested_{false};
    std::atomic<bool> mapMode_{true};
    std::atomic<bool> splitScreen_{false};
    std::atomic<bool> hoverMagnifier_{true};
    std::atomic<bool> hoverFocusBoost_{true};
    std::atomic<bool> oledAutoDim_{false};
    std::atomic<bool> cursorInCenter3d_{false};
    std::atomic<bool> enableTextureOverrides_{true};
    std::atomic<bool> cbDirty_{false};
    std::atomic<uint32_t> themeProfile_{0};
    std::atomic<uint32_t> eyeComfortMode_{0};
    std::atomic<float> glassmorphismAtomic_{0.55f};
    std::atomic<uint32_t> swapWidth_{3840};
    std::atomic<uint32_t> swapHeight_{2160};
    std::atomic<uintptr_t> swapHwnd_{0};
    std::atomic<int32_t> cursorX_{-1};
    std::atomic<int32_t> cursorY_{-1};

    // Fast-path lockless pointer hints for common 1-shader-per-variant case
    std::atomic<ID3D11PixelShader*> fastOrigA_{nullptr};
    std::atomic<ID3D11PixelShader*> fastOrigB_{nullptr};
    std::atomic<bool> fastPointersComplete_{false};
    std::atomic<uint32_t> registeredCount_{0};

    float sharpness_ = 0.70f;
    float edgeAniso_ = 0.85f;
    float mapSharpnessBoost_ = 1.20f;
    float fontOutlineBoost_ = 0.35f;
    float fontWeightBoost_ = 0.25f;
    float themeDarkSlate_ = 0.00f;
    float glassmorphism_ = 0.55f;
    float iconPolish_ = 0.40f;
    float oledDimOpacity_ = 0.72f;
    UINT hoverMagnifierKey_ = VK_MENU;
    float hoverMagnifierScale_ = 1.25f;
    std::filesystem::path overrideDir_;
    std::unordered_map<uint64_t, ID3D11ShaderResourceView*> overrideSrvCache_;
    uint32_t loadedOverrideCount_ = 0;
    std::vector<uint64_t> extraHashes_;
    std::vector<ElementScaleRule> elementScaleRules_;

    std::string hudNotification_;
    uint32_t hudNotificationFrames_ = 0;

    ID3D11Device* device_ = nullptr;
    bool ready_ = false;
    bool initFailed_ = false;
    ID3D11PixelShader* enhancedPsA_ = nullptr;
    ID3D11PixelShader* enhancedPsB_ = nullptr;
    ID3D11GeometryShader* scaleGs_ = nullptr;
    ID3D11Buffer* scaleCb_ = nullptr;
    ID3D11Buffer* soBuffer_ = nullptr;
    ID3D11Buffer* soStagingBuffer_ = nullptr;
    bool scaleGsHasSo_ = false;
    uint32_t hoverFrameTag_ = 1;
    struct HoverCandidateDraw {
        uint32_t texId = 0;
        uint64_t texHash = 0;
        uint32_t texW = 0;
        uint32_t texH = 0;
    };
    static constexpr size_t kMaxHoverCandidates = 512;
    static constexpr size_t kMaxSoVertices = 4096;
    std::vector<HoverCandidateDraw> hoverCandidates_;
    ID3D11SamplerState* linearSamplers_[4]{}; // bit 0: wrapU, bit 1: wrapV
    ID3D11Buffer* paramCbs_[32]{};            // bit0: wrapU, bit1: wrapV, bit2: isMap, bit3: srActive, bit4: hasBackdropBlur (hoverFocus=0)
    ID3D11Buffer* hoveredParamCbs_[32]{};     // bit0: wrapU, bit1: wrapV, bit2: isMap, bit3: srActive, bit4: hasBackdropBlur (hoverFocus=1)

    std::unordered_map<ID3D11PixelShader*, RegisteredPs> targetShaders_;

    std::atomic<uint32_t> liveEnhancedDraws_{0};
    std::atomic<uint32_t> liveSrDraws_{0};
    std::atomic<uint32_t> liveMapDraws_{0};
    std::atomic<uint32_t> liveScaledDraws_{0};
    std::atomic<uint32_t> liveHoveredTexId_{0};
    std::atomic<uint64_t> liveHoveredTexHash_{0};
    std::atomic<uint32_t> liveHoveredTexW_{0};
    std::atomic<uint32_t> liveHoveredTexH_{0};
    uint32_t lastFrameEnhancedDraws_ = 0;
    uint32_t lastFrameSrDraws_ = 0;
    uint32_t lastFrameMapDraws_ = 0;
    uint32_t lastFrameScaledDraws_ = 0;
    uint32_t lastHoveredTexId_ = 0;
    uint64_t lastHoveredTexHash_ = 0;
    uint32_t lastHoveredTexW_ = 0;
    uint32_t lastHoveredTexH_ = 0;
};

} // namespace lotro_uie
