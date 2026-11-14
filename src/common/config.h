#pragma once

#include "common/logger.h"

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lotro_uie {

constexpr UINT kHotkeyModCtrl  = 0x0100u;
constexpr UINT kHotkeyModAlt   = 0x0200u;
constexpr UINT kHotkeyModShift = 0x0400u;
constexpr UINT kHotkeyVkMask   = 0x00FFu;

struct ElementScaleRule {
    std::string name;
    uint64_t texHash = 0;       // 0 = wildcard (match by texWidth/texHeight or quad size)
    uint32_t texWidth = 0;      // 0 = any width
    uint32_t texHeight = 0;     // 0 = any height
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetPxX = 0.0f;
    float offsetPxY = 0.0f;
    float minQuadWidthPx = 0.0f;
    float maxQuadWidthPx = 16384.0f;
    float minQuadHeightPx = 0.0f;
    float maxQuadHeightPx = 16384.0f;
};

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;
    std::wstring chainDll;
    bool showOverlay = true;

    // UI Enhancement (v0.5 / v0.6)
    bool enhanceUi = true;
    float uiSharpness = 0.70f;
    float uiEdgeAniso = 0.85f;
    bool uiEnhanceNameplates = true;
    bool uiEnhanceDepthTested = false;
    bool uiMapMode = true;
    float uiMapSharpnessBoost = 1.20f;
    bool uiSplitScreen = false;

    // UI Modernization: Font Outline/Weight, Multi-Profile Dark Theme, Eye Comfort, Hover Focus/Magnifier, OLED Auto-Dim
    float uiFontOutlineBoost = 0.35f;
    float uiFontWeightBoost = 0.25f;
    float uiThemeDarkSlate = 0.00f;
    uint32_t uiThemeProfile = 0;      // 0 = Obsidian Slate, 1 = Elven Bronze, 2 = Dwarven Iron
    uint32_t uiEyeComfortMode = 0;    // 0 = Off, 1 = Anti-Glare, 2 = Warm 5000K, 3 = Night 4300K
    bool uiHoverFocusBoost = true;
    bool uiOledAutoDim = false;
    float uiOledDimOpacity = 0.72f;
    bool uiHoverMagnifier = true;
    UINT hoverMagnifierKey = VK_MENU; // Hold ALT to magnify hovered element
    float hoverMagnifierScale = 1.25f;

    // HD Texture Replacement (lotro_uie_overrides/<0xHash>.png)
    bool enableTextureOverrides = true;
    std::wstring textureOverrideDir = L"lotro_uie_overrides";

    bool cnnEnabled = true;
    bool cnnUpscaleUiRenderTargets = true;
    uint32_t cnnCacheMaxMb = 192;
    uint32_t cnnMaxNewPerFrame = 4;
    uint32_t cnnMaxDynamicPerFrame = 4;
    UINT hotkeyToggleCnn = kHotkeyModCtrl | VK_F4;

    bool worldPostFxEnabled = true;
    float worldCasStrength = 0.45f;
    bool worldDeband = true;
    float worldClarity = 0.14f;
    float worldVibrance = 0.12f;
    float worldDehaze = 0.18f;
    float worldAntiGlare = 0.25f;
    UINT hotkeyToggleWorldFx = kHotkeyModCtrl | VK_F5;

    uint32_t samplerForceAniso = 16;
    float samplerMipLodBias = -0.35f;
    UINT hotkeyToggleSplit = kHotkeyModCtrl | VK_F3;
    UINT hotkeyToggleEnhance = VK_F8;
    UINT hotkeyCycleSharpness = VK_F7;
    UINT hotkeyReloadConfig = kHotkeyModCtrl | VK_F6;
    UINT hotkeyCopyHover = kHotkeyModCtrl | VK_F2;
    UINT hotkeyScaleUp = kHotkeyModCtrl | VK_PRIOR;   // Ctrl+PageUp (+5%)
    UINT hotkeyScaleDown = kHotkeyModCtrl | VK_NEXT;  // Ctrl+PageDown (-5%)
    std::vector<uint64_t> extraUiPsHashes;
    std::vector<ElementScaleRule> elementScaleRules;

    UINT hotkeyToggleOverlay = VK_F9;
    UINT hotkeyToggleMenu = kHotkeyModCtrl | VK_F9;
    UINT hotkeyCaptureFrame = kHotkeyModCtrl | VK_F10;
    UINT hotkeyCaptureAndDump = kHotkeyModCtrl | VK_F11;

    uint64_t autoCaptureFrame = 0;
    bool autoDumpAssets = false;
    bool dumpAllFrameTextures = false;
    bool dumpOnlyUiTextures = true;
    std::string dumpFormat = "png";
    std::wstring outputDir = L"lotro_uie_captures";

    std::filesystem::path baseDir;
    std::filesystem::path iniPath;
    std::filesystem::path resolvedOutputDir;
    std::filesystem::path resolvedOverrideDir;

    static Config Load(const std::filesystem::path& moduleDir);
    bool Save() const;
};

std::string VirtualKeyName(UINT vk);

} // namespace lotro_uie
