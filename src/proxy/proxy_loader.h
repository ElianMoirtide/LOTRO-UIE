#pragma once

#include "common/config.h"

#include <Windows.h>
#include <filesystem>

namespace lotro_uie {

enum class ProxyKind {
    D3D11,
    DXGI,
};

struct ProxyRuntime {
    HMODULE selfModule = nullptr;
    ProxyKind kind = ProxyKind::D3D11;
    std::filesystem::path selfPath;
    std::filesystem::path selfDir;
    Config config;
    HMODULE targetModule = nullptr;
};

ProxyRuntime& GetProxyRuntime();
const Config& ReloadProxyConfig();

// Called once from DllMain (lightweight metadata only, no LoadLibrary).
void InitProxyRuntimeMetadata(HMODULE hModule, ProxyKind kind);

// Safe lazy loader (uses InitOnceExecuteOnce, outside DllMain loader lock).
extern "C" void EnsureProxyLoaded();

// Shutdown cleanup on DLL_PROCESS_DETACH.
void ShutdownProxyRuntime(bool processTerminating);

} // namespace lotro_uie
