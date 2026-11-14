#include "common/logger.h"
#include "dx11/dx11_hooks.h"
#include "proxy/proxy_loader.h"

#include <Windows.h>
#include <dxgi1_3.h>

#define DXGI_FORWARD_LIST(X) \
    X(ApplyCompatResolutionQuirking) \
    X(CompatString) \
    X(CompatValue) \
    X(DXGID3D10CreateDevice) \
    X(DXGID3D10CreateLayeredDevice) \
    X(DXGID3D10GetLayeredDeviceSize) \
    X(DXGID3D10RegisterLayers) \
    X(DXGIDeclareAdapterRemovalSupport) \
    X(DXGIDisableVBlankVirtualization) \
    X(DXGIDumpJournal) \
    X(DXGIGetDebugInterface1) \
    X(DXGIReportAdapterConfiguration) \
    X(PIXBeginCapture) \
    X(PIXEndCapture) \
    X(PIXGetCaptureState) \
    X(SetAppCompatStringPointer) \
    X(UpdateHMDEmulationStatus)

extern "C" {
#define DECLARE_ORIG(name) extern void* orig_##name;
DXGI_FORWARD_LIST(DECLARE_ORIG)
#undef DECLARE_ORIG
}

namespace {

using PfnCreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PfnCreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

PfnCreateDXGIFactory g_origCreateFactory = nullptr;
PfnCreateDXGIFactory1 g_origCreateFactory1 = nullptr;
PfnCreateDXGIFactory2 g_origCreateFactory2 = nullptr;

} // namespace

namespace lotro_uie {

void ResolveTargetExports(HMODULE targetDll) {
    g_origCreateFactory = reinterpret_cast<PfnCreateDXGIFactory>(
        GetProcAddress(targetDll, "CreateDXGIFactory"));
    g_origCreateFactory1 = reinterpret_cast<PfnCreateDXGIFactory1>(
        GetProcAddress(targetDll, "CreateDXGIFactory1"));
    g_origCreateFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(
        GetProcAddress(targetDll, "CreateDXGIFactory2"));

#define RESOLVE_ORIG(name) orig_##name = reinterpret_cast<void*>(GetProcAddress(targetDll, #name));
    DXGI_FORWARD_LIST(RESOLVE_ORIG)
#undef RESOLVE_ORIG
}

} // namespace lotro_uie

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    lotro_uie::EnsureProxyLoaded();
    if (!g_origCreateFactory) {
        return E_FAIL;
    }
    const HRESULT hr = g_origCreateFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && lotro_uie::GetProxyRuntime().config.enabled && ppFactory && *ppFactory) {
        lotro_uie::OnFactoryCreated(static_cast<IUnknown*>(*ppFactory));
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    lotro_uie::EnsureProxyLoaded();
    if (!g_origCreateFactory1) {
        return E_FAIL;
    }
    const HRESULT hr = g_origCreateFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && lotro_uie::GetProxyRuntime().config.enabled && ppFactory && *ppFactory) {
        lotro_uie::OnFactoryCreated(static_cast<IUnknown*>(*ppFactory));
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    lotro_uie::EnsureProxyLoaded();
    if (!g_origCreateFactory2) {
        return E_FAIL;
    }
    const HRESULT hr = g_origCreateFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && lotro_uie::GetProxyRuntime().config.enabled && ppFactory && *ppFactory) {
        lotro_uie::OnFactoryCreated(static_cast<IUnknown*>(*ppFactory));
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        lotro_uie::InitProxyRuntimeMetadata(hModule, lotro_uie::ProxyKind::DXGI);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        const bool terminating = (lpReserved != nullptr);
        lotro_uie::ShutdownDx11Hooks(terminating);
        lotro_uie::ShutdownProxyRuntime(terminating);
    }
    return TRUE;
}
