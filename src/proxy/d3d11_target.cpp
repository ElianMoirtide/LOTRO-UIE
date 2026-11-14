#include "common/logger.h"
#include "dx11/dx11_hooks.h"
#include "proxy/proxy_loader.h"

#include <Windows.h>
#include <d3d11.h>

#define D3D11_FORWARD_LIST(X) \
    X(CreateDirect3D11DeviceFromDXGIDevice) \
    X(CreateDirect3D11SurfaceFromDXGISurface) \
    X(D3D11CoreCreateDevice) \
    X(D3D11CoreCreateLayeredDevice) \
    X(D3D11CoreGetLayeredDeviceSize) \
    X(D3D11CoreRegisterLayers) \
    X(D3D11CreateDeviceForD3D12) \
    X(D3D11On12CreateDevice) \
    X(D3DKMTCloseAdapter) \
    X(D3DKMTCreateAllocation) \
    X(D3DKMTCreateContext) \
    X(D3DKMTCreateDevice) \
    X(D3DKMTCreateSynchronizationObject) \
    X(D3DKMTDestroyAllocation) \
    X(D3DKMTDestroyContext) \
    X(D3DKMTDestroyDevice) \
    X(D3DKMTDestroySynchronizationObject) \
    X(D3DKMTEscape) \
    X(D3DKMTGetContextSchedulingPriority) \
    X(D3DKMTGetDeviceState) \
    X(D3DKMTGetDisplayModeList) \
    X(D3DKMTGetMultisampleMethodList) \
    X(D3DKMTGetRuntimeData) \
    X(D3DKMTGetSharedPrimaryHandle) \
    X(D3DKMTLock) \
    X(D3DKMTOpenAdapterFromHdc) \
    X(D3DKMTOpenResource) \
    X(D3DKMTPresent) \
    X(D3DKMTQueryAdapterInfo) \
    X(D3DKMTQueryAllocationResidency) \
    X(D3DKMTQueryResourceInfo) \
    X(D3DKMTRender) \
    X(D3DKMTSetAllocationPriority) \
    X(D3DKMTSetContextSchedulingPriority) \
    X(D3DKMTSetDisplayMode) \
    X(D3DKMTSetDisplayPrivateDriverFormat) \
    X(D3DKMTSetGammaRamp) \
    X(D3DKMTSetVidPnSourceOwner) \
    X(D3DKMTSignalSynchronizationObject) \
    X(D3DKMTUnlock) \
    X(D3DKMTWaitForSynchronizationObject) \
    X(D3DKMTWaitForVerticalBlankEvent) \
    X(D3DPerformance_BeginEvent) \
    X(D3DPerformance_EndEvent) \
    X(D3DPerformance_GetStatus) \
    X(D3DPerformance_SetMarker) \
    X(EnableFeatureLevelUpgrade) \
    X(OpenAdapter10) \
    X(OpenAdapter10_2)

extern "C" {
#define DECLARE_ORIG(name) extern void* orig_##name;
D3D11_FORWARD_LIST(DECLARE_ORIG)
#undef DECLARE_ORIG
}

namespace {

using PfnD3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
);

using PfnD3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
);

PfnD3D11CreateDevice g_origCreateDevice = nullptr;
PfnD3D11CreateDeviceAndSwapChain g_origCreateDeviceAndSwapChain = nullptr;

} // namespace

namespace lotro_uie {

void ResolveTargetExports(HMODULE targetDll) {
    g_origCreateDevice = reinterpret_cast<PfnD3D11CreateDevice>(
        GetProcAddress(targetDll, "D3D11CreateDevice"));
    g_origCreateDeviceAndSwapChain = reinterpret_cast<PfnD3D11CreateDeviceAndSwapChain>(
        GetProcAddress(targetDll, "D3D11CreateDeviceAndSwapChain"));

#define RESOLVE_ORIG(name) orig_##name = reinterpret_cast<void*>(GetProcAddress(targetDll, #name));
    D3D11_FORWARD_LIST(RESOLVE_ORIG)
#undef RESOLVE_ORIG
}

} // namespace lotro_uie

extern "C" HRESULT WINAPI Proxy_D3D11CreateDevice(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
) {
    lotro_uie::EnsureProxyLoaded();
    if (!g_origCreateDevice) {
        UIE_LOG_ERROR("D3D11CreateDevice called but original export is null");
        return E_FAIL;
    }

    UIE_LOG_INFO(
        "D3D11CreateDevice(DriverType=%d, Flags=0x%X, FeatureLevels=%u, SDKVersion=%u, pAdapter=%p, wantDevice=%d)",
        static_cast<int>(DriverType),
        Flags,
        FeatureLevels,
        SDKVersion,
        static_cast<const void*>(pAdapter),
        ppDevice ? 1 : 0
    );

    const HRESULT hr = g_origCreateDevice(
        pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
        ppDevice, pFeatureLevel, ppImmediateContext
    );

    UIE_LOG_INFO(
        "D3D11CreateDevice -> hr=0x%08X (device=%p, ctx=%p)",
        static_cast<unsigned>(hr),
        (SUCCEEDED(hr) && ppDevice) ? static_cast<void*>(*ppDevice) : nullptr,
        (SUCCEEDED(hr) && ppImmediateContext) ? static_cast<void*>(*ppImmediateContext) : nullptr
    );

    if (SUCCEEDED(hr) && lotro_uie::GetProxyRuntime().config.enabled) {
        if (ppDevice && *ppDevice) {
            lotro_uie::OnDeviceCreated(*ppDevice, ppImmediateContext ? *ppImmediateContext : nullptr);
        }
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
) {
    lotro_uie::EnsureProxyLoaded();
    if (!g_origCreateDeviceAndSwapChain) {
        UIE_LOG_ERROR("D3D11CreateDeviceAndSwapChain called but original export is null");
        return E_FAIL;
    }

    UIE_LOG_INFO(
        "D3D11CreateDeviceAndSwapChain(DriverType=%d, Flags=0x%X, FeatureLevels=%u, pAdapter=%p, wantSwap=%d, Size=%ux%u, Windowed=%d, HWND=%p)",
        static_cast<int>(DriverType),
        Flags,
        FeatureLevels,
        static_cast<const void*>(pAdapter),
        ppSwapChain ? 1 : 0,
        pSwapChainDesc ? pSwapChainDesc->BufferDesc.Width : 0u,
        pSwapChainDesc ? pSwapChainDesc->BufferDesc.Height : 0u,
        pSwapChainDesc ? (pSwapChainDesc->Windowed ? 1 : 0) : -1,
        pSwapChainDesc ? static_cast<const void*>(pSwapChainDesc->OutputWindow) : nullptr
    );

    const HRESULT hr = g_origCreateDeviceAndSwapChain(
        pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
        pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext
    );

    UIE_LOG_INFO(
        "D3D11CreateDeviceAndSwapChain -> hr=0x%08X (swapChain=%p, device=%p, ctx=%p)",
        static_cast<unsigned>(hr),
        (SUCCEEDED(hr) && ppSwapChain) ? static_cast<void*>(*ppSwapChain) : nullptr,
        (SUCCEEDED(hr) && ppDevice) ? static_cast<void*>(*ppDevice) : nullptr,
        (SUCCEEDED(hr) && ppImmediateContext) ? static_cast<void*>(*ppImmediateContext) : nullptr
    );

    if (SUCCEEDED(hr) && lotro_uie::GetProxyRuntime().config.enabled) {
        if (ppDevice && *ppDevice) {
            lotro_uie::OnDeviceCreated(*ppDevice, ppImmediateContext ? *ppImmediateContext : nullptr);
        }
        if (ppSwapChain && *ppSwapChain) {
            lotro_uie::OnSwapChainCreated(*ppSwapChain);
        }
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        lotro_uie::InitProxyRuntimeMetadata(hModule, lotro_uie::ProxyKind::D3D11);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        const bool terminating = (lpReserved != nullptr);
        lotro_uie::ShutdownDx11Hooks(terminating);
        lotro_uie::ShutdownProxyRuntime(terminating);
    }
    return TRUE;
}
