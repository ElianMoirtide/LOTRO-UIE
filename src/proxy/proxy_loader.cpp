#include "proxy/proxy_loader.h"
#include "common/logger.h"

#include <mutex>

namespace lotro_uie {

namespace {

ProxyRuntime g_runtime;
INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

struct ExportBinding {
    const char* name;
    void** targetPtr;
};

std::filesystem::path GetFallbackLogPath() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buf) / L"LOTRO-UIE" / L"lotro_uie.log";
    }
    const DWORD tempLen = GetTempPathW(MAX_PATH, buf);
    if (tempLen > 0 && tempLen < MAX_PATH) {
        return std::filesystem::path(buf) / L"LOTRO-UIE" / L"lotro_uie.log";
    }
    return std::filesystem::current_path() / L"LOTRO-UIE" / L"lotro_uie.log";
}

std::wstring GetHostProcessPath() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(buf, len);
    }
    return L"<unknown>";
}

bool IsAuxiliaryHelperProcess(const std::wstring& hostPath) {
    const std::filesystem::path p(hostPath);
    std::wstring exe = p.filename().wstring();
    for (wchar_t& ch : exe) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        }
    }
    return exe == L"lotrobrowserhelper64.exe" ||
           exe == L"lotrobrowserhelper.exe" ||
           exe == L"awesomium_process.exe" ||
           exe == L"steamwebhelper.exe";
}

} // namespace

// Each target (d3d11 vs dxgi) provides its own ResolveTargetExports implementation.
void ResolveTargetExports(HMODULE targetDll);

ProxyRuntime& GetProxyRuntime() {
    return g_runtime;
}

void InitProxyRuntimeMetadata(HMODULE hModule, ProxyKind kind) {
    g_runtime.selfModule = hModule;
    g_runtime.kind = kind;

    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(hModule, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        g_runtime.selfPath = std::filesystem::path(buf);
        g_runtime.selfDir = g_runtime.selfPath.parent_path();
    } else {
        g_runtime.selfDir = std::filesystem::current_path();
        g_runtime.selfPath = g_runtime.selfDir / (kind == ProxyKind::D3D11 ? L"d3d11.dll" : L"dxgi.dll");
    }
}

static BOOL CALLBACK InitOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring hostPath = GetHostProcessPath();
    const bool helperPassthrough = IsAuxiliaryHelperProcess(hostPath);

    g_runtime.config = Config::Load(g_runtime.selfDir);
    if (helperPassthrough) {
        g_runtime.config.enabled = false;
    } else {
        const std::filesystem::path primaryLog = g_runtime.selfDir / L"lotro_uie.log";
        const bool primaryOpened = Logger::Init(primaryLog, g_runtime.config.logLevel);
        bool fallbackOpened = false;
        if (!primaryOpened) {
            fallbackOpened = Logger::Init(GetFallbackLogPath(), g_runtime.config.logLevel);
        }

        UIE_LOG_INFO(
            "LOTRO-UIE v0.4 initialized (mode=%s, enabled=%s)",
            g_runtime.kind == ProxyKind::D3D11 ? "d3d11.dll" : "dxgi.dll",
            g_runtime.config.enabled ? "true" : "false"
        );
        UIE_LOG_INFO("Host process: %ls", hostPath.c_str());
        UIE_LOG_INFO("Module dir: %ls", g_runtime.selfDir.c_str());
        if (fallbackOpened) {
            UIE_LOG_WARN("Primary log path '%ls' was not writable; using fallback '%ls'",
                         primaryLog.c_str(), Logger::GetLogPath().c_str());
        }
        UIE_LOG_INFO("Output dir: %ls", g_runtime.config.resolvedOutputDir.c_str());
        UIE_LOG_INFO(
            "Hotkeys: Overlay=%s, Capture=%s, Capture+Dump=%s",
            VirtualKeyName(g_runtime.config.hotkeyToggleOverlay).c_str(),
            VirtualKeyName(g_runtime.config.hotkeyCaptureFrame).c_str(),
            VirtualKeyName(g_runtime.config.hotkeyCaptureAndDump).c_str()
        );
    }

    if (g_runtime.config.enabled) {
        // Games/overlays often probe DX11 with LoadLibrary("d3d11.dll") + FreeLibrary() while
        // keeping DXGI factories or vtable hooks alive; pin our DLL so hooks never become dangling.
        HMODULE pinnedModule = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&EnsureProxyLoaded),
                &pinnedModule)) {
            UIE_LOG_INFO("Pinned proxy module in memory (%p)", static_cast<void*>(pinnedModule));
        } else {
            UIE_LOG_WARN("Failed to pin proxy module (err=%lu)", GetLastError());
        }
    }

    const wchar_t* defaultDllName = (g_runtime.kind == ProxyKind::D3D11) ? L"d3d11.dll" : L"dxgi.dll";

    HMODULE target = nullptr;
    if (!g_runtime.config.chainDll.empty()) {
        std::filesystem::path chainPath(g_runtime.config.chainDll);
        if (chainPath.is_relative()) {
            chainPath = g_runtime.selfDir / chainPath;
        }
        std::error_code ec;
        if (!std::filesystem::equivalent(chainPath, g_runtime.selfPath, ec)) {
            target = LoadLibraryW(chainPath.c_str());
            if (target) {
                UIE_LOG_INFO("Chained to DLL: %ls", chainPath.c_str());
            } else {
                UIE_LOG_WARN("Failed to load ChainDll '%ls' (err=%lu), falling back to System32",
                             chainPath.c_str(), GetLastError());
            }
        } else {
            UIE_LOG_WARN("ChainDll points to self ('%ls'), ignoring and loading System32", chainPath.c_str());
        }
    }

    if (!target) {
        wchar_t sysDir[MAX_PATH]{};
        UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
        std::filesystem::path sysPath = (sysLen > 0 && sysLen < MAX_PATH)
            ? (std::filesystem::path(sysDir) / defaultDllName)
            : std::filesystem::path(defaultDllName);
        target = LoadLibraryW(sysPath.c_str());
        if (target) {
            UIE_LOG_INFO("Loaded system DLL: %ls", sysPath.c_str());
        } else {
            UIE_LOG_ERROR("Failed to load system DLL '%ls' (err=%lu)", sysPath.c_str(), GetLastError());
        }
    }

    g_runtime.targetModule = target;
    if (target) {
        ResolveTargetExports(target);
    }

    return TRUE;
}

extern "C" void EnsureProxyLoaded() {
    InitOnceExecuteOnce(&g_initOnce, InitOnceCallback, nullptr, nullptr);
}

const Config& ReloadProxyConfig() {
    const std::filesystem::path dir = g_runtime.config.baseDir.empty() ? g_runtime.selfDir : g_runtime.config.baseDir;
    g_runtime.config = Config::Load(dir);
    Logger::SetLevel(g_runtime.config.logLevel);
    UIE_LOG_INFO("Reloaded lotro_uie.ini (enabled=%s, sharpness=%.2f, scaleRules=%zu, dumpFormat=%s)",
                 g_runtime.config.enabled ? "true" : "false",
                 g_runtime.config.uiSharpness,
                 g_runtime.config.elementScaleRules.size(),
                 g_runtime.config.dumpFormat.c_str());
    return g_runtime.config;
}

void ShutdownProxyRuntime(bool processTerminating) {
    UIE_LOG_INFO("LOTRO-UIE shutting down (processTerminating=%s)", processTerminating ? "true" : "false");
    // Avoid FreeLibrary() inside DllMain(DLL_PROCESS_DETACH) under the OS loader lock.
    g_runtime.targetModule = nullptr;
    Logger::Shutdown();
}

} // namespace lotro_uie
