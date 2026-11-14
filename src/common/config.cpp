#include "common/config.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace lotro_uie {

namespace {

std::filesystem::path GetFallbackDataDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buf) / L"LOTRO-UIE";
    }
    const DWORD tempLen = GetTempPathW(MAX_PATH, buf);
    if (tempLen > 0 && tempLen < MAX_PATH) {
        return std::filesystem::path(buf) / L"LOTRO-UIE";
    }
    return std::filesystem::current_path() / L"LOTRO-UIE";
}

bool IsDirectoryWritable(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return false;
    }
    const std::filesystem::path probePath =
        dir / (L".lotro_uie_write_probe_" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE h = CreateFileW(
        probePath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

std::string Trim(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool ParseBool(const std::string& val, bool fallback) {
    const std::string v = ToLower(Trim(val));
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return fallback;
}

float ParseFloatOrBoolPreset(const std::string& val, float trueDefault, float minVal, float maxVal, float fallback) {
    const std::string v = ToLower(Trim(val));
    if (v == "true" || v == "yes" || v == "on") return trueDefault;
    if (v == "false" || v == "no" || v == "off") return 0.0f;
    try {
        return (std::clamp)(std::stof(v), minVal, maxVal);
    } catch (...) {
        return fallback;
    }
}

uint32_t ParseThemeProfile(const std::string& val, uint32_t fallback) {
    const std::string v = ToLower(Trim(val));
    if (v == "obsidian" || v == "slate" || v == "0") return 0u;
    if (v == "elven" || v == "bronze" || v == "elvenbronze" || v == "1") return 1u;
    if (v == "dwarven" || v == "iron" || v == "dwarveniron" || v == "2") return 2u;
    try {
        return (std::clamp)(static_cast<uint32_t>(std::stoul(v)), 0u, 2u);
    } catch (...) {
        return fallback;
    }
}

uint32_t ParseEyeComfortMode(const std::string& val, uint32_t fallback) {
    const std::string v = ToLower(Trim(val));
    if (v == "off" || v == "false" || v == "0") return 0u;
    if (v == "antiglare" || v == "anti-glare" || v == "1") return 1u;
    if (v == "warm" || v == "5000k" || v == "warm5000k" || v == "2") return 2u;
    if (v == "night" || v == "4300k" || v == "night4300k" || v == "3") return 3u;
    try {
        return (std::clamp)(static_cast<uint32_t>(std::stoul(v)), 0u, 3u);
    } catch (...) {
        return fallback;
    }
}

LogLevel ParseLogLevel(const std::string& val) {
    const std::string v = ToLower(Trim(val));
    if (v == "debug") return LogLevel::Debug;
    if (v == "warn" || v == "warning") return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    return LogLevel::Info;
}

UINT ParseSingleKeyToken(const std::string& v) {
    if (v.empty()) return 0;
    if (v.size() >= 2 && v[0] == 'f') {
        try {
            int fnum = std::stoi(v.substr(1));
            if (fnum >= 1 && fnum <= 24) {
                return static_cast<UINT>(VK_F1 + (fnum - 1));
            }
        } catch (...) {}
    }
    if (v == "insert" || v == "ins") return VK_INSERT;
    if (v == "delete" || v == "del") return VK_DELETE;
    if (v == "home") return VK_HOME;
    if (v == "end") return VK_END;
    if (v == "pageup" || v == "pgup" || v == "prior") return VK_PRIOR;
    if (v == "pagedown" || v == "pgdn" || v == "next") return VK_NEXT;
    if (v == "pause") return VK_PAUSE;
    if (v == "scrolllock") return VK_SCROLL;
    if (v == "tab") return VK_TAB;
    if (v == "backspace" || v == "bksp") return VK_BACK;
    if (v == "alt" || v == "menu") return VK_MENU;
    if (v == "ctrl" || v == "control") return VK_CONTROL;
    if (v == "shift") return VK_SHIFT;
    if (v == "plus" || v == "add") return VK_ADD;
    if (v == "minus" || v == "subtract") return VK_SUBTRACT;
    if (v.rfind("0x", 0) == 0) {
        try {
            return static_cast<UINT>(std::stoul(v, nullptr, 16));
        } catch (...) {}
    }
    if (v.size() == 1 && std::isalnum(static_cast<unsigned char>(v[0]))) {
        return static_cast<UINT>(std::toupper(static_cast<unsigned char>(v[0])));
    }
    return 0;
}

UINT ParseVirtualKey(const std::string& val, UINT fallback) {
    const std::string full = ToLower(Trim(val));
    if (full.empty()) return fallback;
    if (full == "none" || full == "off" || full == "disabled" || full == "0") return 0;

    UINT mods = 0;
    UINT vk = 0;
    std::stringstream ss(full);
    std::string part;
    while (std::getline(ss, part, '+')) {
        const std::string tok = Trim(part);
        if (tok.empty()) continue;
        if (tok == "ctrl" || tok == "control" || tok == "strg") {
            mods |= kHotkeyModCtrl;
        } else if (tok == "alt" && full.find('+') != std::string::npos) {
            mods |= kHotkeyModAlt;
        } else if (tok == "shift" || tok == "umschalt") {
            mods |= kHotkeyModShift;
        } else {
            const UINT parsed = ParseSingleKeyToken(tok);
            if (parsed != 0) vk = parsed;
        }
    }
    if (vk == 0) return fallback;
    return mods | (vk & kHotkeyVkMask);
}

bool ParseScalePair(const std::string& raw, float& outScaleX, float& outScaleY) {
    std::string s = ToLower(Trim(raw));
    if (s.empty()) return false;
    try {
        if (s.back() == '%') {
            const bool isDelta = (s.front() == '+' || s.front() == '-');
            const float pct = std::stof(s.substr(0, s.size() - 1));
            const float mult = isDelta ? (1.0f + pct * 0.01f) : (pct * 0.01f);
            outScaleX = outScaleY = (std::clamp)(mult, 0.25f, 4.0f);
            return true;
        }
        const auto xPos = s.find('x');
        if (xPos != std::string::npos && xPos > 0 && s.rfind("0x", 0) != 0) {
            const float sx = std::stof(Trim(s.substr(0, xPos)));
            const float sy = std::stof(Trim(s.substr(xPos + 1)));
            outScaleX = (std::clamp)(sx, 0.25f, 4.0f);
            outScaleY = (std::clamp)(sy, 0.25f, 4.0f);
            return true;
        }
        const float m = std::stof(s);
        outScaleX = outScaleY = (std::clamp)(m, 0.25f, 4.0f);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseElementScaleRule(const std::string& name, const std::string& val, ElementScaleRule& outRule) {
    std::vector<std::string> fields;
    std::stringstream ss(val);
    std::string item;
    while (std::getline(ss, item, ',')) {
        fields.push_back(Trim(item));
    }
    if (fields.size() < 2) return false;

    outRule = ElementScaleRule{};
    outRule.name = name;

    const std::string target = ToLower(fields[0]);
    if (target == "*" || target == "any" || target == "all") {
        outRule.texHash = 0;
    } else if (target.rfind("0x", 0) == 0) {
        try {
            outRule.texHash = std::stoull(target, nullptr, 16);
        } catch (...) {
            return false;
        }
    } else {
        const auto xPos = target.find('x');
        if (xPos != std::string::npos) {
            try {
                outRule.texWidth = static_cast<uint32_t>(std::stoul(Trim(target.substr(0, xPos))));
                outRule.texHeight = static_cast<uint32_t>(std::stoul(Trim(target.substr(xPos + 1))));
            } catch (...) {
                return false;
            }
        } else {
            try {
                outRule.texHash = std::stoull(target, nullptr, 16);
            } catch (...) {
                return false;
            }
        }
    }

    if (!ParseScalePair(fields[1], outRule.scaleX, outRule.scaleY)) {
        return false;
    }
    // Ignore legacy FNV-1a hashes of 100% zero-filled empty UI container buffers
    if (outRule.texHash == 0xF8A2D2F0E38D8F25ULL || outRule.texHash == 0xEB05052EA5B62325ULL) {
        return false;
    }
    if (fields.size() >= 3 && !fields[2].empty()) {
        try { outRule.offsetPxX = (std::clamp)(std::stof(fields[2]), -1024.0f, 1024.0f); } catch (...) {}
    }
    if (fields.size() >= 4 && !fields[3].empty()) {
        try { outRule.offsetPxY = (std::clamp)(std::stof(fields[3]), -1024.0f, 1024.0f); } catch (...) {}
    }
    if (fields.size() >= 5 && !fields[4].empty()) {
        try { outRule.maxQuadWidthPx = (std::clamp)(std::stof(fields[4]), 1.0f, 16384.0f); } catch (...) {}
    }
    if (fields.size() >= 6 && !fields[5].empty()) {
        try { outRule.maxQuadHeightPx = (std::clamp)(std::stof(fields[5]), 1.0f, 16384.0f); } catch (...) {}
    }
    return true;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

} // namespace

Config Config::Load(const std::filesystem::path& moduleDir) {
    Config cfg;
    cfg.baseDir = moduleDir;
    cfg.iniPath = moduleDir / L"lotro_uie.ini";

    std::ifstream in(cfg.iniPath);
    if (in.is_open()) {
        std::string section;
        std::string line;
        while (std::getline(in, line)) {
            const auto commentPos = line.find_first_of(";#");
            if (commentPos != std::string::npos) {
                line.erase(commentPos);
            }
            line = Trim(line);
            if (line.empty()) continue;

            if (line.front() == '[' && line.back() == ']') {
                section = ToLower(Trim(line.substr(1, line.size() - 2)));
                continue;
            }

            const auto eqPos = line.find('=');
            if (eqPos == std::string::npos) continue;

            const std::string key = ToLower(Trim(line.substr(0, eqPos)));
            const std::string val = Trim(line.substr(eqPos + 1));

            if (section == "general") {
                if (key == "enabled") cfg.enabled = ParseBool(val, cfg.enabled);
                else if (key == "loglevel") cfg.logLevel = ParseLogLevel(val);
                else if (key == "chaindll") cfg.chainDll = Utf8ToWide(val);
                else if (key == "showoverlay") cfg.showOverlay = ParseBool(val, cfg.showOverlay);
                else if (key == "enhanceui") cfg.enhanceUi = ParseBool(val, cfg.enhanceUi);
            } else if (section == "enhancer") {
                if (key == "enabled" || key == "enhanceui") {
                    cfg.enhanceUi = ParseBool(val, cfg.enhanceUi);
                } else if (key == "sharpness") {
                    try {
                        cfg.uiSharpness = (std::clamp)(std::stof(val), 0.0f, 1.5f);
                    } catch (...) {}
                } else if (key == "edgeaniso" || key == "edgereconstruction") {
                    try {
                        cfg.uiEdgeAniso = (std::clamp)(std::stof(val), 0.0f, 1.0f);
                    } catch (...) {}
                } else if (key == "enhancenameplates") {
                    cfg.uiEnhanceNameplates = ParseBool(val, cfg.uiEnhanceNameplates);
                } else if (key == "enhancedepthtested") {
                    cfg.uiEnhanceDepthTested = ParseBool(val, cfg.uiEnhanceDepthTested);
                } else if (key == "mapmode") {
                    cfg.uiMapMode = ParseBool(val, cfg.uiMapMode);
                } else if (key == "mapsharpnessboost") {
                    try {
                        cfg.uiMapSharpnessBoost = (std::clamp)(std::stof(val), 0.5f, 1.8f);
                    } catch (...) {}
                } else if (key == "splitscreen") {
                    cfg.uiSplitScreen = ParseBool(val, cfg.uiSplitScreen);
                } else if (key == "fontoutlineboost" || key == "fonthalo") {
                    cfg.uiFontOutlineBoost = ParseFloatOrBoolPreset(val, 0.35f, 0.0f, 1.0f, cfg.uiFontOutlineBoost);
                } else if (key == "fontweightboost" || key == "fontstemboost") {
                    cfg.uiFontWeightBoost = ParseFloatOrBoolPreset(val, 0.25f, 0.0f, 1.0f, cfg.uiFontWeightBoost);
                } else if (key == "themedarkslate" || key == "darkmode") {
                    cfg.uiThemeDarkSlate = ParseFloatOrBoolPreset(val, 0.75f, 0.0f, 1.0f, cfg.uiThemeDarkSlate);
                } else if (key == "themeprofile") {
                    cfg.uiThemeProfile = ParseThemeProfile(val, cfg.uiThemeProfile);
                } else if (key == "eyecomfortmode" || key == "eyecare") {
                    cfg.uiEyeComfortMode = ParseEyeComfortMode(val, cfg.uiEyeComfortMode);
                } else if (key == "hoverfocusboost") {
                    cfg.uiHoverFocusBoost = ParseBool(val, cfg.uiHoverFocusBoost);
                } else if (key == "oledautodim") {
                    cfg.uiOledAutoDim = ParseBool(val, cfg.uiOledAutoDim);
                } else if (key == "oleddimopacity") {
                    try { cfg.uiOledDimOpacity = (std::clamp)(std::stof(val), 0.25f, 1.0f); } catch (...) {}
                } else if (key == "hovermagnifier") {
                    cfg.uiHoverMagnifier = ParseBool(val, cfg.uiHoverMagnifier);
                } else if (key == "hovermagnifierkey") {
                    cfg.hoverMagnifierKey = ParseVirtualKey(val, cfg.hoverMagnifierKey);
                } else if (key == "hovermagnifierscale") {
                    try { cfg.hoverMagnifierScale = (std::clamp)(std::stof(val), 1.05f, 2.50f); } catch (...) {}
                } else if (key == "enabletextureoverrides") {
                    cfg.enableTextureOverrides = ParseBool(val, cfg.enableTextureOverrides);
                } else if (key == "textureoverridedir" && !val.empty()) {
                    cfg.textureOverrideDir = Utf8ToWide(val);
                } else if (key == "hotkeytogglesplit" || key == "hotkeysplitscreen") {
                    cfg.hotkeyToggleSplit = ParseVirtualKey(val, cfg.hotkeyToggleSplit);
                } else if (key == "hotkeycopyhover") {
                    cfg.hotkeyCopyHover = ParseVirtualKey(val, cfg.hotkeyCopyHover);
                } else if (key == "hotkeyscaleup") {
                    cfg.hotkeyScaleUp = ParseVirtualKey(val, cfg.hotkeyScaleUp);
                } else if (key == "hotkeyscaledown") {
                    cfg.hotkeyScaleDown = ParseVirtualKey(val, cfg.hotkeyScaleDown);
                } else if (key == "hotkeytoggle") {
                    cfg.hotkeyToggleEnhance = ParseVirtualKey(val, cfg.hotkeyToggleEnhance);
                } else if (key == "hotkeycyclesharpness") {
                    cfg.hotkeyCycleSharpness = ParseVirtualKey(val, cfg.hotkeyCycleSharpness);
                } else if (key == "hotkeyreloadconfig") {
                    cfg.hotkeyReloadConfig = ParseVirtualKey(val, cfg.hotkeyReloadConfig);
                } else if (key == "extrapshashes" || key == "targetpshash") {
                    std::stringstream ss(val);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        const std::string t = ToLower(Trim(item));
                        if (t.empty()) continue;
                        try {
                            const uint64_t h = std::stoull(t, nullptr, 16);
                            if (h != 0) cfg.extraUiPsHashes.push_back(h);
                        } catch (...) {}
                    }
                }
            } else if (section == "uitheme" || section == "theme") {
                if (key == "fontoutlineboost" || key == "fonthalo") {
                    cfg.uiFontOutlineBoost = ParseFloatOrBoolPreset(val, 0.35f, 0.0f, 1.0f, cfg.uiFontOutlineBoost);
                } else if (key == "fontweightboost" || key == "fontstemboost") {
                    cfg.uiFontWeightBoost = ParseFloatOrBoolPreset(val, 0.25f, 0.0f, 1.0f, cfg.uiFontWeightBoost);
                } else if (key == "themedarkslate" || key == "darkslate" || key == "darkmode") {
                    cfg.uiThemeDarkSlate = ParseFloatOrBoolPreset(val, 0.75f, 0.0f, 1.0f, cfg.uiThemeDarkSlate);
                } else if (key == "themeprofile") {
                    cfg.uiThemeProfile = ParseThemeProfile(val, cfg.uiThemeProfile);
                } else if (key == "eyecomfortmode" || key == "eyecare") {
                    cfg.uiEyeComfortMode = ParseEyeComfortMode(val, cfg.uiEyeComfortMode);
                } else if (key == "hoverfocusboost") {
                    cfg.uiHoverFocusBoost = ParseBool(val, cfg.uiHoverFocusBoost);
                } else if (key == "oledautodim") {
                    cfg.uiOledAutoDim = ParseBool(val, cfg.uiOledAutoDim);
                } else if (key == "oleddimopacity") {
                    try { cfg.uiOledDimOpacity = (std::clamp)(std::stof(val), 0.25f, 1.0f); } catch (...) {}
                } else if (key == "hovermagnifier") {
                    cfg.uiHoverMagnifier = ParseBool(val, cfg.uiHoverMagnifier);
                } else if (key == "hovermagnifierkey") {
                    cfg.hoverMagnifierKey = ParseVirtualKey(val, cfg.hoverMagnifierKey);
                } else if (key == "hovermagnifierscale") {
                    try { cfg.hoverMagnifierScale = (std::clamp)(std::stof(val), 1.05f, 2.50f); } catch (...) {}
                }
            } else if (section == "overrides" || section == "textureoverrides") {
                if (key == "enabled" || key == "enabletextureoverrides") {
                    cfg.enableTextureOverrides = ParseBool(val, cfg.enableTextureOverrides);
                } else if ((key == "overridedir" || key == "textureoverridedir") && !val.empty()) {
                    cfg.textureOverrideDir = Utf8ToWide(val);
                }
            } else if (section == "cnn") {
                if (key == "enabled") cfg.cnnEnabled = ParseBool(val, cfg.cnnEnabled);
                else if (key == "upscaleuirendertargets" || key == "upscalert") {
                    cfg.cnnUpscaleUiRenderTargets = ParseBool(val, cfg.cnnUpscaleUiRenderTargets);
                } else if (key == "cachemaxmb") {
                    try { cfg.cnnCacheMaxMb = (std::clamp)(static_cast<uint32_t>(std::stoul(val)), 16u, 1024u); } catch (...) {}
                } else if (key == "maxnewperframe") {
                    try { cfg.cnnMaxNewPerFrame = (std::clamp)(static_cast<uint32_t>(std::stoul(val)), 1u, 64u); } catch (...) {}
                } else if (key == "maxdynamicperframe") {
                    try { cfg.cnnMaxDynamicPerFrame = (std::clamp)(static_cast<uint32_t>(std::stoul(val)), 0u, 32u); } catch (...) {}
                } else if (key == "hotkeytoggle") {
                    cfg.hotkeyToggleCnn = ParseVirtualKey(val, cfg.hotkeyToggleCnn);
                }
            } else if (section == "worldpostfx") {
                if (key == "enabled") cfg.worldPostFxEnabled = ParseBool(val, cfg.worldPostFxEnabled);
                else if (key == "casstrength") {
                    try { cfg.worldCasStrength = (std::clamp)(std::stof(val), 0.0f, 1.0f); } catch (...) {}
                } else if (key == "deband") {
                    cfg.worldDeband = ParseBool(val, cfg.worldDeband);
                } else if (key == "clarity") {
                    try { cfg.worldClarity = (std::clamp)(std::stof(val), 0.0f, 0.6f); } catch (...) {}
                } else if (key == "vibrance") {
                    try { cfg.worldVibrance = (std::clamp)(std::stof(val), 0.0f, 0.6f); } catch (...) {}
                } else if (key == "dehaze") {
                    try { cfg.worldDehaze = (std::clamp)(std::stof(val), 0.0f, 0.6f); } catch (...) {}
                } else if (key == "antiglare") {
                    try { cfg.worldAntiGlare = (std::clamp)(std::stof(val), 0.0f, 1.0f); } catch (...) {}
                } else if (key == "hotkeytoggle") {
                    cfg.hotkeyToggleWorldFx = ParseVirtualKey(val, cfg.hotkeyToggleWorldFx);
                }
            } else if (section == "samplers") {
                if (key == "forceanisotropy") {
                    try { cfg.samplerForceAniso = (std::clamp)(static_cast<uint32_t>(std::stoul(val)), 0u, 16u); } catch (...) {}
                } else if (key == "miplodbias") {
                    try { cfg.samplerMipLodBias = (std::clamp)(std::stof(val), -1.5f, 1.5f); } catch (...) {}
                }
            } else if (section == "elementscale" || section == "elementrules") {
                if (key == "hotkeycopyhover") {
                    cfg.hotkeyCopyHover = ParseVirtualKey(val, cfg.hotkeyCopyHover);
                } else if (key == "hotkeyscaleup") {
                    cfg.hotkeyScaleUp = ParseVirtualKey(val, cfg.hotkeyScaleUp);
                } else if (key == "hotkeyscaledown") {
                    cfg.hotkeyScaleDown = ParseVirtualKey(val, cfg.hotkeyScaleDown);
                } else if (key == "hovermagnifier") {
                    cfg.uiHoverMagnifier = ParseBool(val, cfg.uiHoverMagnifier);
                } else if (key == "hovermagnifierkey") {
                    cfg.hoverMagnifierKey = ParseVirtualKey(val, cfg.hoverMagnifierKey);
                } else if (key == "hovermagnifierscale") {
                    try { cfg.hoverMagnifierScale = (std::clamp)(std::stof(val), 1.05f, 2.50f); } catch (...) {}
                } else {
                    ElementScaleRule rule{};
                    if (ParseElementScaleRule(key, val, rule)) {
                        cfg.elementScaleRules.push_back(std::move(rule));
                    }
                }
            } else if (section == "inspector") {
                if (key == "hotkeytoggleoverlay") cfg.hotkeyToggleOverlay = ParseVirtualKey(val, cfg.hotkeyToggleOverlay);
                else if (key == "hotkeytogglemenu") cfg.hotkeyToggleMenu = ParseVirtualKey(val, cfg.hotkeyToggleMenu);
                else if (key == "hotkeycaptureframe") cfg.hotkeyCaptureFrame = ParseVirtualKey(val, cfg.hotkeyCaptureFrame);
                else if (key == "hotkeycaptureanddump") cfg.hotkeyCaptureAndDump = ParseVirtualKey(val, cfg.hotkeyCaptureAndDump);
                else if (key == "autocaptureframe") {
                    try { cfg.autoCaptureFrame = std::stoull(val); } catch (...) {}
                } else if (key == "autodumpassets") cfg.autoDumpAssets = ParseBool(val, cfg.autoDumpAssets);
                else if (key == "dumpallframetextures") cfg.dumpAllFrameTextures = ParseBool(val, cfg.dumpAllFrameTextures);
                else if (key == "dumponlyuitextures") cfg.dumpOnlyUiTextures = ParseBool(val, cfg.dumpOnlyUiTextures);
                else if (key == "dumpformat" && !val.empty()) cfg.dumpFormat = ToLower(Trim(val));
                else if (key == "outputdir" && !val.empty()) cfg.outputDir = Utf8ToWide(val);
            }
        }
    }

    const std::filesystem::path outPath(cfg.outputDir);
    if (outPath.is_absolute()) {
        cfg.resolvedOutputDir = outPath;
    } else if (IsDirectoryWritable(moduleDir)) {
        cfg.resolvedOutputDir = moduleDir / outPath;
    } else {
        cfg.resolvedOutputDir = GetFallbackDataDir() / outPath;
    }

    const std::filesystem::path ovrPath(cfg.textureOverrideDir);
    if (ovrPath.is_absolute()) {
        cfg.resolvedOverrideDir = ovrPath;
    } else {
        cfg.resolvedOverrideDir = moduleDir / ovrPath;
    }
    return cfg;
}

bool Config::Save() const {
    if (iniPath.empty()) return false;
    std::ofstream out(iniPath, std::ios::trunc);
    if (!out.is_open()) return false;

    auto wideToUtf8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string s(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    };
    auto logLvlStr = [](LogLevel l) -> const char* {
        switch (l) {
            case LogLevel::Debug: return "debug";
            case LogLevel::Warn:  return "warn";
            case LogLevel::Error: return "error";
            default:              return "info";
        }
    };

    out << "[General]\n"
        << "Enabled = " << (enabled ? "true" : "false") << "\n"
        << "LogLevel = " << logLvlStr(logLevel) << "\n"
        << "ChainDll = " << wideToUtf8(chainDll) << "\n"
        << "ShowOverlay = " << (showOverlay ? "true" : "false") << "\n\n";

    out << "[Inspector]\n"
        << "HotkeyToggleOverlay = " << VirtualKeyName(hotkeyToggleOverlay) << "\n"
        << "HotkeyToggleMenu = " << VirtualKeyName(hotkeyToggleMenu) << "\n"
        << "HotkeyCaptureFrame = " << VirtualKeyName(hotkeyCaptureFrame) << "\n"
        << "HotkeyCaptureAndDump = " << VirtualKeyName(hotkeyCaptureAndDump) << "\n"
        << "AutoCaptureFrame = " << autoCaptureFrame << "\n"
        << "AutoDumpAssets = " << (autoDumpAssets ? "true" : "false") << "\n"
        << "DumpAllFrameTextures = " << (dumpAllFrameTextures ? "true" : "false") << "\n"
        << "DumpOnlyUiTextures = " << (dumpOnlyUiTextures ? "true" : "false") << "\n"
        << "DumpFormat = " << dumpFormat << "\n"
        << "OutputDir = " << wideToUtf8(outputDir) << "\n\n";

    char fbuf[256];
    std::snprintf(fbuf, sizeof(fbuf),
                  "Sharpness = %.2f\nEdgeAniso = %.2f\nMapSharpnessBoost = %.2f\n",
                  uiSharpness, uiEdgeAniso, uiMapSharpnessBoost);
    out << "[Enhancer]\n"
        << "Enabled = " << (enhanceUi ? "true" : "false") << "\n"
        << fbuf
        << "EnhanceNameplates = " << (uiEnhanceNameplates ? "true" : "false") << "\n"
        << "EnhanceDepthTested = " << (uiEnhanceDepthTested ? "true" : "false") << "\n"
        << "MapMode = " << (uiMapMode ? "true" : "false") << "\n"
        << "SplitScreen = " << (uiSplitScreen ? "true" : "false") << "\n";
    out << "ExtraPsHashes = ";
    for (size_t i = 0; i < extraUiPsHashes.size(); ++i) {
        char hbuf[32];
        std::snprintf(hbuf, sizeof(hbuf), "%s0x%016llX", (i > 0 ? ", " : ""),
                      static_cast<unsigned long long>(extraUiPsHashes[i]));
        out << hbuf;
    }
    out << "\n"
        << "HotkeyToggle = " << VirtualKeyName(hotkeyToggleEnhance) << "\n"
        << "HotkeyCycleSharpness = " << VirtualKeyName(hotkeyCycleSharpness) << "\n"
        << "HotkeyReloadConfig = " << VirtualKeyName(hotkeyReloadConfig) << "\n"
        << "HotkeyToggleSplit = " << VirtualKeyName(hotkeyToggleSplit) << "\n\n";

    std::snprintf(fbuf, sizeof(fbuf),
                  "FontOutlineBoost = %.2f\nFontWeightBoost = %.2f\nThemeDarkSlate = %.2f\nThemeProfile = %u\nEyeComfortMode = %u\nOledDimOpacity = %.2f\nHoverMagnifierScale = %.2f\n",
                  uiFontOutlineBoost, uiFontWeightBoost, uiThemeDarkSlate, uiThemeProfile, uiEyeComfortMode, uiOledDimOpacity, hoverMagnifierScale);
    out << "[UiTheme]\n"
        << fbuf
        << "HoverFocusBoost = " << (uiHoverFocusBoost ? "true" : "false") << "\n"
        << "OledAutoDim = " << (uiOledAutoDim ? "true" : "false") << "\n"
        << "HoverMagnifier = " << (uiHoverMagnifier ? "true" : "false") << "\n"
        << "HoverMagnifierKey = " << VirtualKeyName(hoverMagnifierKey) << "\n\n";

    out << "[Overrides]\n"
        << "EnableTextureOverrides = " << (enableTextureOverrides ? "true" : "false") << "\n"
        << "OverrideDir = " << wideToUtf8(textureOverrideDir) << "\n\n";

    out << "[ElementScale]\n"
        << "HotkeyCopyHover = " << VirtualKeyName(hotkeyCopyHover) << "\n"
        << "HotkeyScaleUp = " << VirtualKeyName(hotkeyScaleUp) << "\n"
        << "HotkeyScaleDown = " << VirtualKeyName(hotkeyScaleDown) << "\n";
    for (const auto& r : elementScaleRules) {
        char rbuf[256];
        char targetBuf[64];
        if (r.texHash != 0) {
            std::snprintf(targetBuf, sizeof(targetBuf), "0x%016llX", static_cast<unsigned long long>(r.texHash));
        } else if (r.texWidth != 0 && r.texHeight != 0) {
            std::snprintf(targetBuf, sizeof(targetBuf), "%ux%u", r.texWidth, r.texHeight);
        } else {
            std::snprintf(targetBuf, sizeof(targetBuf), "*");
        }
        std::snprintf(rbuf, sizeof(rbuf), "%s = %s, %.2fx%.2f, %.0f, %.0f, %.0f, %.0f\n",
                      r.name.empty() ? "element" : r.name.c_str(),
                      targetBuf, r.scaleX, r.scaleY,
                      r.offsetPxX, r.offsetPxY, r.maxQuadWidthPx, r.maxQuadHeightPx);
        out << rbuf;
    }
    out << "\n";

    out << "[CNN]\n"
        << "Enabled = " << (cnnEnabled ? "true" : "false") << "\n"
        << "UpscaleUiRenderTargets = " << (cnnUpscaleUiRenderTargets ? "true" : "false") << "\n"
        << "CacheMaxMb = " << cnnCacheMaxMb << "\n"
        << "MaxNewPerFrame = " << cnnMaxNewPerFrame << "\n"
        << "MaxDynamicPerFrame = " << cnnMaxDynamicPerFrame << "\n"
        << "HotkeyToggle = " << VirtualKeyName(hotkeyToggleCnn) << "\n\n";

    std::snprintf(fbuf, sizeof(fbuf),
                  "CasStrength = %.2f\nClarity = %.2f\nVibrance = %.2f\nDehaze = %.2f\nAntiGlare = %.2f\n",
                  worldCasStrength, worldClarity, worldVibrance, worldDehaze, worldAntiGlare);
    out << "[WorldPostFX]\n"
        << "Enabled = " << (worldPostFxEnabled ? "true" : "false") << "\n"
        << fbuf
        << "Deband = " << (worldDeband ? "true" : "false") << "\n"
        << "HotkeyToggle = " << VirtualKeyName(hotkeyToggleWorldFx) << "\n\n";

    std::snprintf(fbuf, sizeof(fbuf), "MipLodBias = %.2f\n", samplerMipLodBias);
    out << "[Samplers]\n"
        << "ForceAnisotropy = " << samplerForceAniso << "\n"
        << fbuf;
    return true;
}

std::string VirtualKeyName(UINT hk) {
    if (hk == 0) return "OFF";
    std::string prefix;
    if (hk & kHotkeyModCtrl)  prefix += "Ctrl+";
    if (hk & kHotkeyModAlt)   prefix += "Alt+";
    if (hk & kHotkeyModShift) prefix += "Shift+";

    const UINT vk = hk & kHotkeyVkMask;
    if (vk >= VK_F1 && vk <= VK_F24) {
        return prefix + "F" + std::to_string(1 + (vk - VK_F1));
    }
    switch (vk) {
        case VK_INSERT:   return prefix + "INS";
        case VK_DELETE:   return prefix + "DEL";
        case VK_HOME:     return prefix + "HOME";
        case VK_END:      return prefix + "END";
        case VK_PRIOR:    return prefix + "PGUP";
        case VK_NEXT:     return prefix + "PGDN";
        case VK_PAUSE:    return prefix + "PAUSE";
        case VK_SCROLL:   return prefix + "SCROLL";
        case VK_MENU:     return prefix + "ALT";
        case VK_CONTROL:  return prefix + "CTRL";
        case VK_SHIFT:    return prefix + "SHIFT";
        case VK_ADD:      return prefix + "PLUS";
        case VK_SUBTRACT: return prefix + "MINUS";
        default: break;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return prefix + std::string(1, static_cast<char>(vk));
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", vk);
    return prefix + buf;
}

} // namespace lotro_uie
