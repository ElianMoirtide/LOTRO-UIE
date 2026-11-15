#include "dx11/dx11_hooks.h"
#include "common/logger.h"
#include "dx11/asset_dumper.h"
#include "dx11/context_state.h"
#include "dx11/debug_overlay.h"
#include "dx11/dxgi_format_names.h"
#include "dx11/inspector.h"
#include "dx11/resource_tracker.h"
#include "dx11/ui_enhancer.h"
#include "dx11/ui_sr_cache.h"
#include "dx11/vtable_hook.h"
#include "dx11/vtable_slots.h"
#include "dx11/world_postfx.h"
#include "proxy/proxy_loader.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace lotro_uie {

namespace {

thread_local int g_hookBypassDepth = 0;
thread_local int g_presentDepth = 0;

struct ScopedHookBypass {
    ScopedHookBypass() { ++g_hookBypassDepth; }
    ~ScopedHookBypass() { --g_hookBypassDepth; }
};

bool IsBypassed() noexcept {
    return g_hookBypassDepth > 0;
}

// Context state management: fast dedicated slot for immediate context + map for deferred contexts.
std::atomic<ID3D11DeviceContext*> g_immediateCtx{nullptr};
std::atomic<ID3D11DeviceContext*> g_immediateCtx1Alias{nullptr};
std::mutex g_immediateStateMutex;
ContextState g_immediateState;

std::mutex g_deferredStatesMutex;
std::unordered_map<ID3D11DeviceContext*, ContextState> g_deferredStates;

template <typename Fn>
void WithContextState(ID3D11DeviceContext* ctx, Fn&& fn) {
    if (!ctx) return;
    if (ctx == g_immediateCtx.load(std::memory_order_acquire) ||
        ctx == g_immediateCtx1Alias.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_immediateStateMutex);
        fn(g_immediateState);
        return;
    }
    std::lock_guard<std::mutex> lock(g_deferredStatesMutex);
    fn(g_deferredStates[ctx]);
}

struct SeenSwapChain {
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    UINT sampleCount = 1;
    BOOL windowed = TRUE;
    HWND hwnd = nullptr;
    uint64_t presentCount = 0;
    uint64_t present1Count = 0;
};

std::mutex g_swapInfoMutex;
SwapChainInfo g_swapInfo{};
std::unordered_map<IDXGISwapChain*, SeenSwapChain> g_seenSwapChains;
IDXGISwapChain* g_lastActiveSwapChain = nullptr;
std::atomic<uint32_t> g_vtableResetLogCount{0};
std::atomic<bool> g_loggedPostPresentReset{false};
std::mutex g_contextPatchMutex;
std::atomic<bool> g_overlayVisible{true};
std::atomic<bool> g_overlayInitializedFromConfig{false};
bool g_prevF2 = false;
bool g_prevF3 = false;
bool g_prevF4 = false;
bool g_prevF5 = false;
bool g_prevF6 = false;
bool g_prevF7 = false;
bool g_prevF8 = false;
bool g_prevF9 = false;
bool g_prevCtrlF9 = false;
bool g_prevF10 = false;
bool g_prevF11 = false;
bool g_prevPgUp = false;
bool g_prevPgDn = false;
std::atomic<uint32_t> g_upgradedSamplersCount{0};

std::mutex g_hookedSetMutex;
void* g_lastLoggedDevice = nullptr;
void* g_lastLoggedDevice1 = nullptr;
void* g_lastLoggedCtx = nullptr;
void* g_lastLoggedCtx1 = nullptr;
void* g_lastLoggedSwapChain = nullptr;
void* g_lastLoggedFactory = nullptr;

std::string ModuleNameForAddress(const void* addr) {
    if (!addr) return "null";
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) || !mod) {
        return "unknown";
    }
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
    if (len == 0) return "unknown";
    const wchar_t* base = path;
    for (DWORD i = 0; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    }
    char narrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, narrow, MAX_PATH, nullptr, nullptr);
    const auto& rt = GetProxyRuntime();
    if (mod == rt.selfModule) return std::string("PROXY:") + narrow;
    if (mod == rt.targetModule) return std::string("SYS:") + narrow;
    return narrow;
}

std::string DescribeResourceShort(ID3D11Resource* res) {
    if (!res) return "res=null";
    auto& tracker = ResourceTracker::Instance();
    if (auto meta = tracker.GetTexture(res)) {
        std::ostringstream oss;
        oss << "tex_" << meta->id << "(" << meta->width << "x" << meta->height
            << " " << DxgiFormatName(meta->format)
            << (meta->isBackbuffer ? " BB" : "") << ")";
        return oss.str();
    }
    ID3D11Texture2D* tex2d = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) && tex2d) {
        D3D11_TEXTURE2D_DESC desc{};
        tex2d->GetDesc(&desc);
        tracker.TrackTexture2D(tex2d, &desc, nullptr, false);
        auto meta = tracker.GetTexture(tex2d);
        tex2d->Release();
        if (meta) {
            std::ostringstream oss;
            oss << "tex_" << meta->id << "*(" << meta->width << "x" << meta->height
                << " " << DxgiFormatName(meta->format)
                << (meta->isBackbuffer ? " BB" : "") << ")";
            return oss.str();
        }
        std::ostringstream oss;
        oss << "tex2d(" << desc.Width << "x" << desc.Height << " " << DxgiFormatName(desc.Format) << ")";
        return oss.str();
    }
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    return std::string("res(dim=") + std::to_string(static_cast<int>(dim)) + ")";
}

std::string DescribeRtvShort(ID3D11RenderTargetView* rtv) {
    if (!rtv) return "rtv=null";
    auto& tracker = ResourceTracker::Instance();
    auto meta = tracker.GetRtv(rtv);
    bool lazy = false;
    if (!meta) {
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (res) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            rtv->GetDesc(&desc);
            tracker.TrackRtv(rtv, res, &desc, false);
            res->Release();
            meta = tracker.GetRtv(rtv);
            lazy = true;
        }
    }
    if (!meta) return "rtv=untracked";
    std::ostringstream oss;
    oss << "rtv=#" << meta->id << "(" << meta->width << "x" << meta->height
        << " " << DxgiFormatName(meta->format)
        << (meta->isBackbuffer ? " BB" : "") << (lazy ? " LAZY" : "") << ")";
    return oss.str();
}

std::string DescribeDsvShort(ID3D11DepthStencilView* dsv) {
    if (!dsv) return "dsv=null";
    auto& tracker = ResourceTracker::Instance();
    auto meta = tracker.GetDsv(dsv);
    if (!meta) {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res) {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            dsv->GetDesc(&desc);
            tracker.TrackDsv(dsv, res, &desc);
            res->Release();
            meta = tracker.GetDsv(dsv);
        }
    }
    if (!meta) return "dsv=untracked";
    std::ostringstream oss;
    oss << "dsv=#" << meta->id << "(" << meta->width << "x" << meta->height
        << " " << DxgiFormatName(meta->format) << ")";
    return oss.str();
}

bool IsKeyJustPressed(UINT hk, bool& prevState) {
    const UINT vk = hk & kHotkeyVkMask;
    if (vk == 0) {
        prevState = false;
        return false;
    }
    const bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    const bool shiftDown = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
    const bool wantCtrl  = (hk & kHotkeyModCtrl)  != 0;
    const bool wantAlt   = (hk & kHotkeyModAlt)   != 0;
    const bool wantShift = (hk & kHotkeyModShift) != 0;
    const bool modsMatch = (ctrlDown == wantCtrl) && (altDown == wantAlt) && (shiftDown == wantShift);
    const bool keyDown   = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    const bool down      = keyDown && modsMatch;
    const bool justPressed = down && !prevState;
    prevState = down;
    return justPressed;
}

bool IsProcessForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ============================================================================
// Function pointer typedefs and VtableDispatch tables
// ============================================================================

// Device
using PfnCreateTexture2D = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                       const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PfnCreateShaderResourceView = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                                const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                                ID3D11ShaderResourceView**);
using PfnCreateRenderTargetView = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                              const D3D11_RENDER_TARGET_VIEW_DESC*,
                                                              ID3D11RenderTargetView**);
using PfnCreateVertexShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                          ID3D11ClassLinkage*, ID3D11VertexShader**);
using PfnCreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                         ID3D11ClassLinkage*, ID3D11PixelShader**);
using PfnCreateComputeShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                           ID3D11ClassLinkage*, ID3D11ComputeShader**);
using PfnCreateDepthStencilView = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                                const D3D11_DEPTH_STENCIL_VIEW_DESC*,
                                                                ID3D11DepthStencilView**);
using PfnCreateBlendState = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BLEND_DESC*, ID3D11BlendState**);
using PfnCreateDepthStencilState = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_DEPTH_STENCIL_DESC*,
                                                               ID3D11DepthStencilState**);
using PfnCreateRasterizerState = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_RASTERIZER_DESC*,
                                                             ID3D11RasterizerState**);
using PfnCreateSamplerState = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*,
                                                          ID3D11SamplerState**);
using PfnCreateDeferredContext = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using PfnGetImmediateContext = void(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11DeviceContext**);
using PfnGetImmediateContext1 = void(STDMETHODCALLTYPE*)(ID3D11Device1*, ID3D11DeviceContext1**);
using PfnCreateDeferredContext1 = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device1*, UINT, ID3D11DeviceContext1**);
using PfnCreateDeviceContextState = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device1*, UINT, const D3D_FEATURE_LEVEL*,
                                                                 UINT, UINT, REFIID, D3D_FEATURE_LEVEL*,
                                                                 ID3DDeviceContextState**);

VtableDispatch<PfnCreateTexture2D> g_dispCreateTexture2D;
VtableDispatch<PfnCreateShaderResourceView> g_dispCreateSRV;
VtableDispatch<PfnCreateRenderTargetView> g_dispCreateRTV;
VtableDispatch<PfnCreateDepthStencilView> g_dispCreateDSV;
VtableDispatch<PfnCreateVertexShader> g_dispCreateVS;
VtableDispatch<PfnCreatePixelShader> g_dispCreatePS;
VtableDispatch<PfnCreateComputeShader> g_dispCreateCS;
VtableDispatch<PfnCreateBlendState> g_dispCreateBlend;
VtableDispatch<PfnCreateDepthStencilState> g_dispCreateDepth;
VtableDispatch<PfnCreateRasterizerState> g_dispCreateRast;
VtableDispatch<PfnCreateSamplerState> g_dispCreateSampler;
VtableDispatch<PfnCreateDeferredContext> g_dispCreateDeferredCtx;
VtableDispatch<PfnGetImmediateContext> g_dispGetImmCtx;
VtableDispatch<PfnGetImmediateContext1> g_dispGetImmCtx1;
VtableDispatch<PfnCreateDeferredContext1> g_dispCreateDeferredCtx1;
VtableDispatch<PfnCreateDeviceContextState> g_dispCreateDeviceCtxState;

// Context
using PfnPSSetShaderResources = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                         ID3D11ShaderResourceView* const*);
using PfnPSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*,
                                                ID3D11ClassInstance* const*, UINT);
using PfnPSSetSamplers = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
using PfnVSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                                ID3D11ClassInstance* const*, UINT);
using PfnCSSetShaderResources = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                         ID3D11ShaderResourceView* const*);
using PfnCSSetUnorderedAccessViews = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                              ID3D11UnorderedAccessView* const*, const UINT*);
using PfnCSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*,
                                                ID3D11ClassInstance* const*, UINT);
using PfnDrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PfnDraw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PfnDrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PfnDrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PfnDrawIndexedInstancedIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PfnDrawInstancedIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PfnDrawAuto = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PfnDispatch = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using PfnDispatchIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PfnMap = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                           D3D11_MAPPED_SUBRESOURCE*);
using PfnUnmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PfnIASetVertexBuffers = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*,
                                                       const UINT*, const UINT*);
using PfnIASetPrimitiveTopology = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
using PfnOMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                       ID3D11DepthStencilView*);
using PfnOMSetRenderTargetsAndUAVs = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                              ID3D11RenderTargetView* const*,
                                                              ID3D11DepthStencilView*, UINT, UINT,
                                                              ID3D11UnorderedAccessView* const*, const UINT*);
using PfnOMSetBlendState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using PfnOMSetDepthStencilState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using PfnRSSetState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
using PfnRSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PfnRSSetScissorRects = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using PfnCopySubresourceRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                          ID3D11Resource*, UINT, const D3D11_BOX*);
using PfnCopyResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PfnUpdateSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                      const void*, UINT, UINT);
using PfnClearRenderTargetView = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using PfnClearDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PfnResolveSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                       ID3D11Resource*, UINT, DXGI_FORMAT);
using PfnExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PfnFinishCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PfnFlush = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PfnClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PfnClearView = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const FLOAT[4], const D3D11_RECT*, UINT);
using PfnDiscardResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*);
using PfnDiscardView = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*);
using PfnDiscardView1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT);
using PfnSwapDeviceContextState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*,
                                                           ID3DDeviceContextState**);

VtableDispatch<PfnPSSetShaderResources> g_dispPSSetSRVs;
VtableDispatch<PfnPSSetShader> g_dispPSSetShader;
VtableDispatch<PfnPSSetSamplers> g_dispPSSetSamplers;
VtableDispatch<PfnVSSetShader> g_dispVSSetShader;
VtableDispatch<PfnCSSetShaderResources> g_dispCSSetSRVs;
VtableDispatch<PfnCSSetUnorderedAccessViews> g_dispCSSetUAVs;
VtableDispatch<PfnCSSetShader> g_dispCSSetShader;
VtableDispatch<PfnDrawIndexed> g_dispDrawIndexed;
VtableDispatch<PfnDraw> g_dispDraw;
VtableDispatch<PfnDrawIndexedInstanced> g_dispDrawIndexedInstanced;
VtableDispatch<PfnDrawInstanced> g_dispDrawInstanced;
VtableDispatch<PfnDrawIndexedInstancedIndirect> g_dispDrawIndexedInstancedIndirect;
VtableDispatch<PfnDrawInstancedIndirect> g_dispDrawInstancedIndirect;
VtableDispatch<PfnDrawAuto> g_dispDrawAuto;
VtableDispatch<PfnDispatch> g_dispDispatch;
VtableDispatch<PfnDispatchIndirect> g_dispDispatchIndirect;
VtableDispatch<PfnMap> g_dispMap;
VtableDispatch<PfnUnmap> g_dispUnmap;
VtableDispatch<PfnIASetVertexBuffers> g_dispIASetVBs;
VtableDispatch<PfnIASetPrimitiveTopology> g_dispIASetTopo;
VtableDispatch<PfnOMSetRenderTargets> g_dispOMSetRTs;
VtableDispatch<PfnOMSetRenderTargetsAndUAVs> g_dispOMSetRTsUAVs;
VtableDispatch<PfnOMSetBlendState> g_dispOMSetBlend;
VtableDispatch<PfnOMSetDepthStencilState> g_dispOMSetDepth;
VtableDispatch<PfnRSSetState> g_dispRSSetState;
VtableDispatch<PfnRSSetViewports> g_dispRSSetViewports;
VtableDispatch<PfnRSSetScissorRects> g_dispRSSetScissors;
VtableDispatch<PfnCopySubresourceRegion> g_dispCopySubresourceRegion;
VtableDispatch<PfnCopyResource> g_dispCopyResource;
VtableDispatch<PfnUpdateSubresource> g_dispUpdateSubresource;
VtableDispatch<PfnClearRenderTargetView> g_dispClearRTV;
VtableDispatch<PfnClearDepthStencilView> g_dispClearDSV;
VtableDispatch<PfnResolveSubresource> g_dispResolveSubresource;
VtableDispatch<PfnExecuteCommandList> g_dispExecuteCommandList;
VtableDispatch<PfnFinishCommandList> g_dispFinishCommandList;
VtableDispatch<PfnFlush> g_dispFlush;
VtableDispatch<PfnClearState> g_dispClearState;
VtableDispatch<PfnClearView> g_dispClearView;
VtableDispatch<PfnDiscardResource> g_dispDiscardResource;
VtableDispatch<PfnDiscardView> g_dispDiscardView;
VtableDispatch<PfnDiscardView1> g_dispDiscardView1;
VtableDispatch<PfnSwapDeviceContextState> g_dispSwapDeviceContextState;

// DXGI Factory & SwapChain
using PfnCreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                       IDXGISwapChain**);
using PfnCreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                              const DXGI_SWAP_CHAIN_DESC1*,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                              IDXGIOutput*, IDXGISwapChain1**);
using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PfnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

VtableDispatch<PfnCreateSwapChain> g_dispCreateSwapChain;
VtableDispatch<PfnCreateSwapChainForHwnd> g_dispCreateSwapChainForHwnd;
VtableDispatch<PfnPresent> g_dispPresent;
VtableDispatch<PfnResizeBuffers> g_dispResizeBuffers;
VtableDispatch<PfnPresent1> g_dispPresent1;

void HookDeviceContext(ID3D11DeviceContext* ctx, bool isImmediate);
bool AreCoreContextHooksIntact(ID3D11DeviceContext* ctx) noexcept;
bool VerifyContextHooks(ID3D11DeviceContext* ctx, const char* caller) noexcept;
void EnsureImmediateContextHooks(IDXGISwapChain* swapChain);

// ============================================================================
// Device Hooks
// ============================================================================

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(
    ID3D11Device* This,
    const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D** ppTexture2D
) {
    auto orig = g_dispCreateTexture2D.Resolve(This);
    HRESULT hr = orig(This, pDesc, pInitialData, ppTexture2D);
    if (SUCCEEDED(hr) && !IsBypassed() && ppTexture2D && *ppTexture2D && pDesc) {
        ResourceTracker::Instance().TrackTexture2D(*ppTexture2D, pDesc, pInitialData, false);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSRV(
    ID3D11Device* This,
    ID3D11Resource* pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
    ID3D11ShaderResourceView** ppSRView
) {
    auto orig = g_dispCreateSRV.Resolve(This);
    HRESULT hr = orig(This, pResource, pDesc, ppSRView);
    if (SUCCEEDED(hr) && !IsBypassed() && ppSRView && *ppSRView) {
        ResourceTracker::Instance().TrackSrv(*ppSRView, pResource, pDesc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateRTV(
    ID3D11Device* This,
    ID3D11Resource* pResource,
    const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
    ID3D11RenderTargetView** ppRTView
) {
    auto orig = g_dispCreateRTV.Resolve(This);
    HRESULT hr = orig(This, pResource, pDesc, ppRTView);
    if (SUCCEEDED(hr) && !IsBypassed() && ppRTView && *ppRTView) {
        ResourceTracker::Instance().TrackRtv(*ppRTView, pResource, pDesc, false);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDSV(
    ID3D11Device* This,
    ID3D11Resource* pResource,
    const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
    ID3D11DepthStencilView** ppDepthStencilView
) {
    auto orig = g_dispCreateDSV.Resolve(This);
    HRESULT hr = orig(This, pResource, pDesc, ppDepthStencilView);
    if (SUCCEEDED(hr) && !IsBypassed() && ppDepthStencilView && *ppDepthStencilView) {
        ResourceTracker::Instance().TrackDsv(*ppDepthStencilView, pResource, pDesc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateVS(
    ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    ID3D11VertexShader** ppVertexShader
) {
    auto orig = g_dispCreateVS.Resolve(This);
    HRESULT hr = orig(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
    if (SUCCEEDED(hr) && !IsBypassed() && ppVertexShader && *ppVertexShader) {
        ResourceTracker::Instance().TrackVertexShader(*ppVertexShader, pShaderBytecode, BytecodeLength);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePS(
    ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    ID3D11PixelShader** ppPixelShader
) {
    auto orig = g_dispCreatePS.Resolve(This);
    HRESULT hr = orig(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
    if (SUCCEEDED(hr) && !IsBypassed() && ppPixelShader && *ppPixelShader) {
        const uint64_t psHash = ResourceTracker::Instance().TrackPixelShader(
            *ppPixelShader, pShaderBytecode, BytecodeLength);
        auto& uie = UiEnhancer::Instance();
        uie.InitializeFromConfig(GetProxyRuntime().config);
        uie.OnPixelShaderCreated(This, *ppPixelShader, psHash);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCS(
    ID3D11Device* This,
    const void* pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    ID3D11ComputeShader** ppComputeShader
) {
    auto orig = g_dispCreateCS.Resolve(This);
    HRESULT hr = orig(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader);
    if (SUCCEEDED(hr) && !IsBypassed() && ppComputeShader && *ppComputeShader) {
        ResourceTracker::Instance().TrackComputeShader(*ppComputeShader, pShaderBytecode, BytecodeLength);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateBlendState(
    ID3D11Device* This,
    const D3D11_BLEND_DESC* pBlendStateDesc,
    ID3D11BlendState** ppBlendState
) {
    auto orig = g_dispCreateBlend.Resolve(This);
    HRESULT hr = orig(This, pBlendStateDesc, ppBlendState);
    if (SUCCEEDED(hr) && !IsBypassed() && ppBlendState && *ppBlendState) {
        ResourceTracker::Instance().TrackBlendState(*ppBlendState, pBlendStateDesc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDepthStencilState(
    ID3D11Device* This,
    const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc,
    ID3D11DepthStencilState** ppDepthStencilState
) {
    auto orig = g_dispCreateDepth.Resolve(This);
    HRESULT hr = orig(This, pDepthStencilDesc, ppDepthStencilState);
    if (SUCCEEDED(hr) && !IsBypassed() && ppDepthStencilState && *ppDepthStencilState) {
        ResourceTracker::Instance().TrackDepthStencilState(*ppDepthStencilState, pDepthStencilDesc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateRasterizerState(
    ID3D11Device* This,
    const D3D11_RASTERIZER_DESC* pRasterizerDesc,
    ID3D11RasterizerState** ppRasterizerState
) {
    auto orig = g_dispCreateRast.Resolve(This);
    HRESULT hr = orig(This, pRasterizerDesc, ppRasterizerState);
    if (SUCCEEDED(hr) && !IsBypassed() && ppRasterizerState && *ppRasterizerState) {
        ResourceTracker::Instance().TrackRasterizerState(*ppRasterizerState, pRasterizerDesc);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSamplerState(
    ID3D11Device* This,
    const D3D11_SAMPLER_DESC* pSamplerDesc,
    ID3D11SamplerState** ppSamplerState
) {
    const D3D11_SAMPLER_DESC* descToUse = pSamplerDesc;
    D3D11_SAMPLER_DESC patched{};
    bool upgraded = false;
    if (pSamplerDesc && !IsBypassed()) {
        const auto& cfg = GetProxyRuntime().config;
        const bool isLinearOrAniso =
            pSamplerDesc->Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR ||
            pSamplerDesc->Filter == D3D11_FILTER_ANISOTROPIC;
        const bool isRepeatingOrAniso =
            pSamplerDesc->AddressU == D3D11_TEXTURE_ADDRESS_WRAP ||
            pSamplerDesc->AddressV == D3D11_TEXTURE_ADDRESS_WRAP ||
            pSamplerDesc->AddressU == D3D11_TEXTURE_ADDRESS_MIRROR ||
            pSamplerDesc->AddressV == D3D11_TEXTURE_ADDRESS_MIRROR ||
            pSamplerDesc->Filter == D3D11_FILTER_ANISOTROPIC;
        if (cfg.enabled && cfg.samplerForceAniso > 0 && isLinearOrAniso && isRepeatingOrAniso &&
            pSamplerDesc->MaxLOD >= 1.0f) {
            patched = *pSamplerDesc;
            patched.Filter = D3D11_FILTER_ANISOTROPIC;
            patched.MaxAnisotropy = (std::max)(patched.MaxAnisotropy, cfg.samplerForceAniso);
            patched.MipLODBias = (std::clamp)(patched.MipLODBias + cfg.samplerMipLodBias, -1.5f, 1.5f);
            descToUse = &patched;
            upgraded = true;
        }
    }
    auto orig = g_dispCreateSampler.Resolve(This);
    HRESULT hr = orig(This, descToUse, ppSamplerState);
    if (SUCCEEDED(hr) && !IsBypassed() && ppSamplerState && *ppSamplerState) {
        ResourceTracker::Instance().TrackSampler(*ppSamplerState, descToUse);
        if (upgraded) {
            const uint32_t cnt = g_upgradedSamplersCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cnt <= 5) {
                UIE_LOG_INFO("Sampler upgraded #%u -> %ux Aniso, MipLODBias=%.2f",
                             cnt, patched.MaxAnisotropy, patched.MipLODBias);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(
    ID3D11Device* This,
    UINT ContextFlags,
    ID3D11DeviceContext** ppDeferredContext
) {
    auto orig = g_dispCreateDeferredCtx.Resolve(This);
    HRESULT hr = orig(This, ContextFlags, ppDeferredContext);
    if (SUCCEEDED(hr) && ppDeferredContext && *ppDeferredContext) {
        HookDeviceContext(*ppDeferredContext, false);
    }
    return hr;
}

void STDMETHODCALLTYPE HookGetImmediateContext(
    ID3D11Device* This,
    ID3D11DeviceContext** ppImmediateContext
) {
    auto orig = g_dispGetImmCtx.Resolve(This);
    orig(This, ppImmediateContext);
    if (ppImmediateContext && *ppImmediateContext) {
        HookDeviceContext(*ppImmediateContext, true);
    }
}

void STDMETHODCALLTYPE HookGetImmediateContext1(
    ID3D11Device1* This,
    ID3D11DeviceContext1** ppImmediateContext
) {
    auto orig = g_dispGetImmCtx1.Resolve(This);
    orig(This, ppImmediateContext);
    if (ppImmediateContext && *ppImmediateContext) {
        HookDeviceContext(*ppImmediateContext, true);
    }
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext1(
    ID3D11Device1* This,
    UINT ContextFlags,
    ID3D11DeviceContext1** ppDeferredContext
) {
    auto orig = g_dispCreateDeferredCtx1.Resolve(This);
    HRESULT hr = orig(This, ContextFlags, ppDeferredContext);
    if (SUCCEEDED(hr) && ppDeferredContext && *ppDeferredContext) {
        HookDeviceContext(*ppDeferredContext, false);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDeviceContextState(
    ID3D11Device1* This,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    REFIID EmulatedInterface,
    D3D_FEATURE_LEVEL* pChosenFeatureLevel,
    ID3DDeviceContextState** ppContextState
) {
    auto orig = g_dispCreateDeviceCtxState.Resolve(This);
    HRESULT hr = orig(This, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                      EmulatedInterface, pChosenFeatureLevel, ppContextState);
    if (SUCCEEDED(hr) && !IsBypassed() && Inspector::Instance().IsCapturing()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "flags=0x%X state=%p",
                      Flags, ppContextState ? static_cast<void*>(*ppContextState) : nullptr);
        Inspector::Instance().RecordGpuEvent("CreateDeviceContextState", buf);
    }
    return hr;
}

// ============================================================================
// Context Hooks
// ============================================================================

void STDMETHODCALLTYPE HookPSSetShaderResources(
    ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumViews,
    ID3D11ShaderResourceView* const* ppShaderResourceViews
) {
    g_dispPSSetSRVs.Resolve(This)(This, StartSlot, NumViews, ppShaderResourceViews);
    if (IsBypassed() || StartSlot >= ContextState::kMaxSrvs) return;
    WithContextState(This, [&](ContextState& st) {
        for (UINT i = 0; i < NumViews; ++i) {
            const UINT slot = StartSlot + i;
            if (slot >= ContextState::kMaxSrvs) break;
            st.psSrvs[slot] = ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr;
        }
    });
}

void STDMETHODCALLTYPE HookPSSetShader(
    ID3D11DeviceContext* This,
    ID3D11PixelShader* pPixelShader,
    ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances
) {
    g_dispPSSetShader.Resolve(This)(This, pPixelShader, ppClassInstances, NumClassInstances);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) { st.ps = pPixelShader; });
}

void STDMETHODCALLTYPE HookPSSetSamplers(
    ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumSamplers,
    ID3D11SamplerState* const* ppSamplers
) {
    g_dispPSSetSamplers.Resolve(This)(This, StartSlot, NumSamplers, ppSamplers);
    if (IsBypassed() || StartSlot >= ContextState::kMaxSamplers) return;
    WithContextState(This, [&](ContextState& st) {
        for (UINT i = 0; i < NumSamplers; ++i) {
            const UINT slot = StartSlot + i;
            if (slot >= ContextState::kMaxSamplers) break;
            st.psSamplers[slot] = ppSamplers ? ppSamplers[i] : nullptr;
        }
    });
}

void STDMETHODCALLTYPE HookVSSetShader(
    ID3D11DeviceContext* This,
    ID3D11VertexShader* pVertexShader,
    ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances
) {
    g_dispVSSetShader.Resolve(This)(This, pVertexShader, ppClassInstances, NumClassInstances);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) { st.vs = pVertexShader; });
}

void STDMETHODCALLTYPE HookIASetVertexBuffers(
    ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumBuffers,
    ID3D11Buffer* const* ppVertexBuffers,
    const UINT* pStrides,
    const UINT* pOffsets
) {
    g_dispIASetVBs.Resolve(This)(This, StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
    if (IsBypassed()) return;
    if (StartSlot == 0 && NumBuffers > 0) {
        WithContextState(This, [&](ContextState& st) {
            st.vbStride0 = (ppVertexBuffers && ppVertexBuffers[0] && pStrides) ? pStrides[0] : 0;
        });
    }
}

void STDMETHODCALLTYPE HookIASetPrimitiveTopology(
    ID3D11DeviceContext* This,
    D3D11_PRIMITIVE_TOPOLOGY Topology
) {
    g_dispIASetTopo.Resolve(This)(This, Topology);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) { st.topology = Topology; });
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(
    ID3D11DeviceContext* This,
    UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView
) {
    g_dispOMSetRTs.Resolve(This)(This, NumViews, ppRenderTargetViews, pDepthStencilView);
    if (IsBypassed()) return;
    if (ppRenderTargetViews && NumViews > 0 && ppRenderTargetViews[0]) {
        ScopedHookBypass bypass;
        UiSrCache::Instance().MarkRenderTargetViewDirty(ppRenderTargetViews[0]);
    }
    WithContextState(This, [&](ContextState& st) {
        const UINT clamped = std::min<UINT>(NumViews, static_cast<UINT>(ContextState::kMaxRtvs));
        st.numRtvs = clamped;
        st.rtvs.fill(nullptr);
        if (ppRenderTargetViews) {
            for (UINT i = 0; i < clamped; ++i) {
                st.rtvs[i] = ppRenderTargetViews[i];
            }
        }
        st.dsv = pDepthStencilView;
    });
    VerifyContextHooks(This, "OMSetRenderTargets");
}

void STDMETHODCALLTYPE HookOMSetRenderTargetsAndUAVs(
    ID3D11DeviceContext* This,
    UINT NumRTVs,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot,
    UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts
) {
    g_dispOMSetRTsUAVs.Resolve(This)(This, NumRTVs, ppRenderTargetViews, pDepthStencilView,
                                     UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    if (IsBypassed()) return;
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        WithContextState(This, [&](ContextState& st) {
            const UINT clamped = std::min<UINT>(NumRTVs, static_cast<UINT>(ContextState::kMaxRtvs));
            st.numRtvs = clamped;
            st.rtvs.fill(nullptr);
            if (ppRenderTargetViews) {
                for (UINT i = 0; i < clamped; ++i) {
                    st.rtvs[i] = ppRenderTargetViews[i];
                }
            }
            st.dsv = pDepthStencilView;
        });
    }
    VerifyContextHooks(This, "OMSetRenderTargetsAndUAVs");
}

void STDMETHODCALLTYPE HookOMSetBlendState(
    ID3D11DeviceContext* This,
    ID3D11BlendState* pBlendState,
    const FLOAT BlendFactor[4],
    UINT SampleMask
) {
    g_dispOMSetBlend.Resolve(This)(This, pBlendState, BlendFactor, SampleMask);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) { st.blendState = pBlendState; });
}

void STDMETHODCALLTYPE HookOMSetDepthStencilState(
    ID3D11DeviceContext* This,
    ID3D11DepthStencilState* pDepthStencilState,
    UINT StencilRef
) {
    g_dispOMSetDepth.Resolve(This)(This, pDepthStencilState, StencilRef);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) {
        st.depthStencilState = pDepthStencilState;
        st.stencilRef = StencilRef;
    });
}

void STDMETHODCALLTYPE HookRSSetState(
    ID3D11DeviceContext* This,
    ID3D11RasterizerState* pRasterizerState
) {
    g_dispRSSetState.Resolve(This)(This, pRasterizerState);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) { st.rasterizerState = pRasterizerState; });
}

void STDMETHODCALLTYPE HookRSSetViewports(
    ID3D11DeviceContext* This,
    UINT NumViewports,
    const D3D11_VIEWPORT* pViewports
) {
    g_dispRSSetViewports.Resolve(This)(This, NumViewports, pViewports);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) {
        if (NumViewports > 0 && pViewports) {
            st.hasViewport0 = true;
            st.viewport0 = pViewports[0];
        } else {
            st.hasViewport0 = false;
        }
    });
}

void STDMETHODCALLTYPE HookRSSetScissorRects(
    ID3D11DeviceContext* This,
    UINT NumRects,
    const D3D11_RECT* pRects
) {
    g_dispRSSetScissors.Resolve(This)(This, NumRects, pRects);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) {
        if (NumRects > 0 && pRects) {
            st.hasScissor0 = true;
            st.scissor0 = pRects[0];
        } else {
            st.hasScissor0 = false;
        }
    });
}

HRESULT STDMETHODCALLTYPE HookMap(
    ID3D11DeviceContext* This,
    ID3D11Resource* pResource,
    UINT Subresource,
    D3D11_MAP MapType,
    UINT MapFlags,
    D3D11_MAPPED_SUBRESOURCE* pMappedResource
) {
    HRESULT hr = g_dispMap.Resolve(This)(This, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (SUCCEEDED(hr) && !IsBypassed() && pResource &&
        (MapType == D3D11_MAP_WRITE || MapType == D3D11_MAP_WRITE_DISCARD || MapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        ResourceTracker::Instance().RecordTextureDynamicUpdate(pResource);
        UiSrCache::Instance().MarkResourceDirty(pResource);
    }
    return hr;
}

void STDMETHODCALLTYPE HookUpdateSubresource(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    const D3D11_BOX* pDstBox,
    const void* pSrcData,
    UINT SrcRowPitch,
    UINT SrcDepthPitch
) {
    g_dispUpdateSubresource.Resolve(This)(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    if (!IsBypassed() && pDstResource) {
        ResourceTracker::Instance().RecordTextureSubresourceUpdate(
            pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch);
        UiSrCache::Instance().MarkResourceDirty(pDstResource);
    }
}

void STDMETHODCALLTYPE HookClearState(ID3D11DeviceContext* This) {
    g_dispClearState.Resolve(This)(This);
    if (IsBypassed()) return;
    if (Inspector::Instance().IsCapturing()) {
        Inspector::Instance().RecordGpuEvent("ClearState", "context reset");
    }
    VerifyContextHooks(This, "ClearState");
    WithContextState(This, [](ContextState& st) { st.Clear(); });
}

class ScopedUiOverride {
public:
    ScopedUiOverride(ID3D11DeviceContext* ctx, const UiDrawOverride& ov)
        : ctx_(ctx), ov_(ov) {
        if (!ctx_ || !ov_.active) return;
        ScopedHookBypass bypass;
        if (ov_.replacementSrv0) {
            ctx_->PSGetShaderResources(0, 1, &origSrv0_);
            if (auto psrv = g_dispPSSetSRVs.Resolve(ctx_)) {
                psrv(ctx_, 0, 1, &ov_.replacementSrv0);
            } else {
                ctx_->PSSetShaderResources(0, 1, &ov_.replacementSrv0);
            }
        }
        if (ov_.backdropSrv) {
            ctx_->PSGetShaderResources(14, 1, &origSrv14_);
            if (auto psrv = g_dispPSSetSRVs.Resolve(ctx_)) {
                psrv(ctx_, 14, 1, &ov_.backdropSrv);
            } else {
                ctx_->PSSetShaderResources(14, 1, &ov_.backdropSrv);
            }
        }
        ctx_->PSGetSamplers(15, 1, &origSampler15_);
        ctx_->PSGetConstantBuffers(12, 1, &origCb12_);
        if (auto pss = g_dispPSSetShader.Resolve(ctx_)) {
            pss(ctx_, ov_.replacementPs, nullptr, 0);
        } else {
            ctx_->PSSetShader(ov_.replacementPs, nullptr, 0);
        }
        if (auto psmp = g_dispPSSetSamplers.Resolve(ctx_)) {
            psmp(ctx_, 15, 1, &ov_.linearSampler);
        } else {
            ctx_->PSSetSamplers(15, 1, &ov_.linearSampler);
        }
        ctx_->PSSetConstantBuffers(12, 1, &ov_.paramCb);

        if ((ov_.scaleApplied || ov_.gsHoverProbe) && ov_.replacementGs && ov_.scaleCb) {
            ctx_->GSGetShader(&origGs_, origGsInst_, &origGsInstCount_);
            ctx_->GSGetConstantBuffers(12, 1, &origGsCb12_);
            ctx_->UpdateSubresource(ov_.scaleCb, 0, nullptr, &ov_.scaleParams, 0, 0);
            ctx_->GSSetShader(ov_.replacementGs, nullptr, 0);
            ctx_->GSSetConstantBuffers(12, 1, &ov_.scaleCb);
            gsBound_ = true;
            if (ov_.gsHoverProbe && ov_.soBuffer) {
                const UINT soOffset = ov_.resetSoOffset ? 0u : static_cast<UINT>(-1);
                ctx_->SOSetTargets(1, &ov_.soBuffer, &soOffset);
                soBound_ = true;
            }
        }
    }

    ~ScopedUiOverride() {
        if (!ctx_ || !ov_.active) return;
        ScopedHookBypass bypass;
        if (soBound_) {
            ID3D11Buffer* nullBuf = nullptr;
            const UINT zeroOffset = 0;
            ctx_->SOSetTargets(1, &nullBuf, &zeroOffset);
        }
        if (gsBound_) {
            ctx_->GSSetShader(origGs_, origGsInst_, origGsInstCount_);
            ctx_->GSSetConstantBuffers(12, 1, &origGsCb12_);
            for (UINT i = 0; i < origGsInstCount_; ++i) {
                if (origGsInst_[i]) origGsInst_[i]->Release();
            }
            if (origGs_) origGs_->Release();
            if (origGsCb12_) origGsCb12_->Release();
        }
        if (ov_.replacementSrv0) {
            if (auto psrv = g_dispPSSetSRVs.Resolve(ctx_)) {
                psrv(ctx_, 0, 1, &origSrv0_);
            } else {
                ctx_->PSSetShaderResources(0, 1, &origSrv0_);
            }
            if (origSrv0_) {
                origSrv0_->Release();
                origSrv0_ = nullptr;
            }
        }
        if (ov_.backdropSrv) {
            if (auto psrv = g_dispPSSetSRVs.Resolve(ctx_)) {
                psrv(ctx_, 14, 1, &origSrv14_);
            } else {
                ctx_->PSSetShaderResources(14, 1, &origSrv14_);
            }
            if (origSrv14_) {
                origSrv14_->Release();
                origSrv14_ = nullptr;
            }
        }
        if (auto pss = g_dispPSSetShader.Resolve(ctx_)) {
            pss(ctx_, ov_.origPs, nullptr, 0);
        } else {
            ctx_->PSSetShader(ov_.origPs, nullptr, 0);
        }
        if (auto psmp = g_dispPSSetSamplers.Resolve(ctx_)) {
            psmp(ctx_, 15, 1, &origSampler15_);
        } else {
            ctx_->PSSetSamplers(15, 1, &origSampler15_);
        }
        ctx_->PSSetConstantBuffers(12, 1, &origCb12_);
        if (origSampler15_) origSampler15_->Release();
        if (origCb12_) origCb12_->Release();
    }

private:
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11ShaderResourceView* origSrv0_ = nullptr;
    ID3D11ShaderResourceView* origSrv14_ = nullptr;
    ID3D11SamplerState* origSampler15_ = nullptr;
    ID3D11Buffer* origCb12_ = nullptr;
    ID3D11GeometryShader* origGs_ = nullptr;
    ID3D11ClassInstance* origGsInst_[8]{};
    UINT origGsInstCount_ = 8;
    ID3D11Buffer* origGsCb12_ = nullptr;
    bool gsBound_ = false;
    bool soBound_ = false;
    UiDrawOverride ov_{};
};

void TrackAndPrepareDraw(
    ID3D11DeviceContext* ctx, DrawKind kind, UINT count, UINT instanceCount,
    UINT startLoc, INT baseVertex, UiDrawOverride& ov
) {
    if (IsBypassed()) return;
    WithContextState(ctx, [&](ContextState& st) {
        Inspector::Instance().RecordDrawCall(st, kind, count, instanceCount, startLoc, baseVertex);
        ScopedHookBypass bypass;
        UiEnhancer::Instance().PrepareDraw(ctx, st, ov, count);
    });
    const bool needWorldOrGlass =
        WorldPostFx::Instance().IsEnabled() ||
        (UiEnhancer::Instance().IsEnabled() && UiEnhancer::Instance().GetGlassmorphism() > 0.001f);
    if (ov.isUiCandidateDraw && ov.isBackbufferTarget && needWorldOrGlass &&
        !WorldPostFx::Instance().HasProcessedThisFrame()) {
        IDXGISwapChain* sc = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_swapInfoMutex);
            sc = g_lastActiveSwapChain;
            if (sc) sc->AddRef();
        }
        if (sc) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) {
                ScopedHookBypass bypass;
                WorldPostFx::Instance().ApplyToBackbuffer(dev, ctx, sc);
                dev->Release();
            }
            sc->Release();
        }
    }
    UiEnhancer::Instance().AttachBackdropBlurIfNeeded(ov);
}

void STDMETHODCALLTYPE HookDrawIndexed(
    ID3D11DeviceContext* This,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation
) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::DrawIndexed, IndexCount, 1, StartIndexLocation, BaseVertexLocation, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDrawIndexed.Resolve(This)(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE HookDraw(
    ID3D11DeviceContext* This,
    UINT VertexCount,
    UINT StartVertexLocation
) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::Draw, VertexCount, 1, StartVertexLocation, 0, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDraw.Resolve(This)(This, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(
    ID3D11DeviceContext* This,
    UINT IndexCountPerInstance,
    UINT InstanceCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation,
    UINT StartInstanceLocation
) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::DrawIndexedInstanced, IndexCountPerInstance, InstanceCount,
                        StartIndexLocation, BaseVertexLocation, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDrawIndexedInstanced.Resolve(This)(This, IndexCountPerInstance, InstanceCount,
                                             StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE HookDrawInstanced(
    ID3D11DeviceContext* This,
    UINT VertexCountPerInstance,
    UINT InstanceCount,
    UINT StartVertexLocation,
    UINT StartInstanceLocation
) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::DrawInstanced, VertexCountPerInstance, InstanceCount,
                        StartVertexLocation, 0, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDrawInstanced.Resolve(This)(This, VertexCountPerInstance, InstanceCount,
                                      StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE HookDrawAuto(ID3D11DeviceContext* This) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::DrawAuto, 0, 1, 0, 0, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDrawAuto.Resolve(This)(This);
}

void STDMETHODCALLTYPE HookDrawInstancedIndirect(
    ID3D11DeviceContext* This,
    ID3D11Buffer* pBufferForArgs,
    UINT AlignedByteOffsetForArgs
) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::DrawInstancedIndirect, 0, 1, AlignedByteOffsetForArgs, 0, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDrawInstancedIndirect.Resolve(This)(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirect(
    ID3D11DeviceContext* This,
    ID3D11Buffer* pBufferForArgs,
    UINT AlignedByteOffsetForArgs
) {
    UiDrawOverride ov{};
    TrackAndPrepareDraw(This, DrawKind::DrawIndexedInstancedIndirect, 0, 1, AlignedByteOffsetForArgs, 0, ov);
    ScopedUiOverride uiGuard(This, ov);
    g_dispDrawIndexedInstancedIndirect.Resolve(This)(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE HookCSSetShader(
    ID3D11DeviceContext* This,
    ID3D11ComputeShader* pComputeShader,
    ID3D11ClassInstance* const* ppClassInstances,
    UINT NumClassInstances
) {
    g_dispCSSetShader.Resolve(This)(This, pComputeShader, ppClassInstances, NumClassInstances);
    if (IsBypassed()) return;
    WithContextState(This, [&](ContextState& st) { st.cs = pComputeShader; });
}

void STDMETHODCALLTYPE HookCSSetShaderResources(
    ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumViews,
    ID3D11ShaderResourceView* const* ppShaderResourceViews
) {
    g_dispCSSetSRVs.Resolve(This)(This, StartSlot, NumViews, ppShaderResourceViews);
    if (IsBypassed() || StartSlot >= ContextState::kMaxSrvs) return;
    WithContextState(This, [&](ContextState& st) {
        const UINT count = std::min<UINT>(NumViews, static_cast<UINT>(ContextState::kMaxSrvs) - StartSlot);
        for (UINT i = 0; i < count; ++i) {
            st.csSrvs[StartSlot + i] = ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr;
        }
    });
}

void STDMETHODCALLTYPE HookCSSetUnorderedAccessViews(
    ID3D11DeviceContext* This,
    UINT StartSlot,
    UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts
) {
    g_dispCSSetUAVs.Resolve(This)(This, StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    if (IsBypassed() || StartSlot >= ContextState::kMaxRtvs) return;
    WithContextState(This, [&](ContextState& st) {
        const UINT count = std::min<UINT>(NumUAVs, static_cast<UINT>(ContextState::kMaxRtvs) - StartSlot);
        for (UINT i = 0; i < count; ++i) {
            st.csUavs[StartSlot + i] = ppUnorderedAccessViews ? ppUnorderedAccessViews[i] : nullptr;
        }
    });
}

void STDMETHODCALLTYPE HookDispatch(
    ID3D11DeviceContext* This,
    UINT ThreadGroupCountX,
    UINT ThreadGroupCountY,
    UINT ThreadGroupCountZ
) {
    if (!IsBypassed()) {
        WithContextState(This, [&](ContextState& st) {
            Inspector::Instance().RecordDispatch(st, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ, false);
        });
    }
    g_dispDispatch.Resolve(This)(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE HookDispatchIndirect(
    ID3D11DeviceContext* This,
    ID3D11Buffer* pBufferForArgs,
    UINT AlignedByteOffsetForArgs
) {
    if (!IsBypassed()) {
        WithContextState(This, [&](ContextState& st) {
            Inspector::Instance().RecordDispatch(st, 0, 0, AlignedByteOffsetForArgs, true);
        });
    }
    g_dispDispatchIndirect.Resolve(This)(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE HookUnmap(
    ID3D11DeviceContext* This,
    ID3D11Resource* pResource,
    UINT Subresource
) {
    g_dispUnmap.Resolve(This)(This, pResource, Subresource);
    if (!IsBypassed()) {
        VerifyContextHooks(This, "Unmap");
    }
}

void STDMETHODCALLTYPE HookCopySubresourceRegion(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    UINT DstX,
    UINT DstY,
    UINT DstZ,
    ID3D11Resource* pSrcResource,
    UINT SrcSubresource,
    const D3D11_BOX* pSrcBox
) {
    g_dispCopySubresourceRegion.Resolve(This)(
        This, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
    if (IsBypassed()) return;
    if (DstSubresource == 0 && SrcSubresource == 0) {
        ResourceTracker::Instance().PropagateTextureHashOnCopy(pDstResource, pSrcResource);
    }
    UiSrCache::Instance().MarkResourceDirty(pDstResource);
    if (Inspector::Instance().IsCapturing()) {
        ScopedHookBypass bypass;
        std::ostringstream oss;
        oss << "dst=" << DescribeResourceShort(pDstResource)
            << " <- src=" << DescribeResourceShort(pSrcResource);
        Inspector::Instance().RecordGpuEvent("CopySubresourceRegion", oss.str());
    }
    VerifyContextHooks(This, "CopySubresourceRegion");
}

void STDMETHODCALLTYPE HookCopyResource(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    ID3D11Resource* pSrcResource
) {
    g_dispCopyResource.Resolve(This)(This, pDstResource, pSrcResource);
    if (IsBypassed()) return;
    ResourceTracker::Instance().PropagateTextureHashOnCopy(pDstResource, pSrcResource);
    UiSrCache::Instance().MarkResourceDirty(pDstResource);
    if (Inspector::Instance().IsCapturing()) {
        ScopedHookBypass bypass;
        std::ostringstream oss;
        oss << "dst=" << DescribeResourceShort(pDstResource)
            << " <- src=" << DescribeResourceShort(pSrcResource);
        Inspector::Instance().RecordGpuEvent("CopyResource", oss.str());
        Inspector::Instance().RetainResourceForCapture(pSrcResource);
        Inspector::Instance().RetainResourceForCapture(pDstResource);
    }
    VerifyContextHooks(This, "CopyResource");
}

void STDMETHODCALLTYPE HookResolveSubresource(
    ID3D11DeviceContext* This,
    ID3D11Resource* pDstResource,
    UINT DstSubresource,
    ID3D11Resource* pSrcResource,
    UINT SrcSubresource,
    DXGI_FORMAT Format
) {
    g_dispResolveSubresource.Resolve(This)(This, pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
    if (IsBypassed()) return;
    UiSrCache::Instance().MarkResourceDirty(pDstResource);
    if (Inspector::Instance().IsCapturing()) {
        ScopedHookBypass bypass;
        std::ostringstream oss;
        oss << "dst=" << DescribeResourceShort(pDstResource)
            << " <- src=" << DescribeResourceShort(pSrcResource)
            << " fmt=" << DxgiFormatName(Format);
        Inspector::Instance().RecordGpuEvent("ResolveSubresource", oss.str());
        Inspector::Instance().RetainResourceForCapture(pSrcResource);
        Inspector::Instance().RetainResourceForCapture(pDstResource);
    }
    VerifyContextHooks(This, "ResolveSubresource");
}

void STDMETHODCALLTYPE HookClearRenderTargetView(
    ID3D11DeviceContext* This,
    ID3D11RenderTargetView* pRenderTargetView,
    const FLOAT ColorRGBA[4]
) {
    g_dispClearRTV.Resolve(This)(This, pRenderTargetView, ColorRGBA);
    if (IsBypassed()) return;
    if (pRenderTargetView) {
        ScopedHookBypass bypass;
        UiSrCache::Instance().MarkRenderTargetViewDirty(pRenderTargetView);
    }
    if (Inspector::Instance().IsCapturing()) {
        ScopedHookBypass bypass;
        std::string desc = DescribeRtvShort(pRenderTargetView);
        if (ColorRGBA) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), " rgba=(%.2f,%.2f,%.2f,%.2f)",
                          ColorRGBA[0], ColorRGBA[1], ColorRGBA[2], ColorRGBA[3]);
            desc += buf;
        }
        Inspector::Instance().RecordGpuEvent("ClearRTV", desc);
    }
    VerifyContextHooks(This, "ClearRTV");
}

void STDMETHODCALLTYPE HookClearDepthStencilView(
    ID3D11DeviceContext* This,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT ClearFlags,
    FLOAT Depth,
    UINT8 Stencil
) {
    g_dispClearDSV.Resolve(This)(This, pDepthStencilView, ClearFlags, Depth, Stencil);
    if (IsBypassed()) return;
    if (Inspector::Instance().IsCapturing()) {
        ScopedHookBypass bypass;
        Inspector::Instance().RecordGpuEvent("ClearDSV", DescribeDsvShort(pDepthStencilView));
    }
    VerifyContextHooks(This, "ClearDSV");
}

void STDMETHODCALLTYPE HookExecuteCommandList(
    ID3D11DeviceContext* This,
    ID3D11CommandList* pCommandList,
    BOOL RestoreContextState
) {
    g_dispExecuteCommandList.Resolve(This)(This, pCommandList, RestoreContextState);
    if (IsBypassed()) return;
    if (Inspector::Instance().IsCapturing()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "cmdList=%p restore=%d",
                      static_cast<const void*>(pCommandList), RestoreContextState ? 1 : 0);
        Inspector::Instance().RecordGpuEvent("ExecuteCommandList", buf);
    }
    if (!RestoreContextState) {
        WithContextState(This, [](ContextState& st) { st.Clear(); });
    }
    VerifyContextHooks(This, "ExecuteCommandList");
}

HRESULT STDMETHODCALLTYPE HookFinishCommandList(
    ID3D11DeviceContext* This,
    BOOL RestoreDeferredContextState,
    ID3D11CommandList** ppCommandList
) {
    HRESULT hr = g_dispFinishCommandList.Resolve(This)(This, RestoreDeferredContextState, ppCommandList);
    if (!IsBypassed() && Inspector::Instance().IsCapturing()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ctx=%p cmdList=%p hr=0x%08X",
                      static_cast<const void*>(This),
                      (SUCCEEDED(hr) && ppCommandList) ? static_cast<void*>(*ppCommandList) : nullptr,
                      static_cast<unsigned>(hr));
        Inspector::Instance().RecordGpuEvent("FinishCommandList", buf);
    }
    return hr;
}

void STDMETHODCALLTYPE HookFlush(ID3D11DeviceContext* This) {
    g_dispFlush.Resolve(This)(This);
    if (IsBypassed()) return;
    if (Inspector::Instance().IsCapturing()) {
        Inspector::Instance().RecordGpuEvent("Flush", "context flush");
    }
    VerifyContextHooks(This, "Flush");
}

void STDMETHODCALLTYPE HookClearView(
    ID3D11DeviceContext1* This,
    ID3D11View* pView,
    const FLOAT Color[4],
    const D3D11_RECT* pRect,
    UINT NumRects
) {
    g_dispClearView.Resolve(This)(This, pView, Color, pRect, NumRects);
    if (IsBypassed()) return;
    if (Inspector::Instance().IsCapturing()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "view=%p numRects=%u", static_cast<const void*>(pView), NumRects);
        Inspector::Instance().RecordGpuEvent("ClearView", buf);
    }
    VerifyContextHooks(This, "ClearView");
}

void STDMETHODCALLTYPE HookDiscardResource(ID3D11DeviceContext1* This, ID3D11Resource* pResource) {
    g_dispDiscardResource.Resolve(This)(This, pResource);
    if (!IsBypassed()) VerifyContextHooks(This, "DiscardResource");
}

void STDMETHODCALLTYPE HookDiscardView(ID3D11DeviceContext1* This, ID3D11View* pResourceView) {
    g_dispDiscardView.Resolve(This)(This, pResourceView);
    if (!IsBypassed()) VerifyContextHooks(This, "DiscardView");
}

void STDMETHODCALLTYPE HookDiscardView1(
    ID3D11DeviceContext1* This,
    ID3D11View* pResourceView,
    const D3D11_RECT* pRects,
    UINT NumRects
) {
    g_dispDiscardView1.Resolve(This)(This, pResourceView, pRects, NumRects);
    if (!IsBypassed()) VerifyContextHooks(This, "DiscardView1");
}

void STDMETHODCALLTYPE HookSwapDeviceContextState(
    ID3D11DeviceContext1* This,
    ID3DDeviceContextState* pState,
    ID3DDeviceContextState** ppPreviousState
) {
    g_dispSwapDeviceContextState.Resolve(This)(This, pState, ppPreviousState);
    if (IsBypassed()) return;
    VerifyContextHooks(This, "SwapDeviceContextState");
    if (Inspector::Instance().IsCapturing()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "state=%p prev=%p",
                      static_cast<const void*>(pState),
                      ppPreviousState ? static_cast<void*>(*ppPreviousState) : nullptr);
        Inspector::Instance().RecordGpuEvent("SwapDeviceContextState", buf);
    }
    // Sync active RTVs/VS/PS/Viewport because SwapDeviceContextState replaces pipeline state atomically.
    ScopedHookBypass bypass;
    WithContextState(This, [&](ContextState& st) {
        st.Clear();
        ID3D11RenderTargetView* rtvs[ContextState::kMaxRtvs]{};
        ID3D11DepthStencilView* dsv = nullptr;
        This->OMGetRenderTargets(static_cast<UINT>(ContextState::kMaxRtvs), rtvs, &dsv);
        st.numRtvs = 0;
        for (UINT i = 0; i < static_cast<UINT>(ContextState::kMaxRtvs); ++i) {
            st.rtvs[i] = rtvs[i];
            if (rtvs[i]) {
                st.numRtvs = i + 1;
                rtvs[i]->Release();
            }
        }
        st.dsv = dsv;
        if (dsv) dsv->Release();

        ID3D11VertexShader* vs = nullptr;
        This->VSGetShader(&vs, nullptr, nullptr);
        st.vs = vs;
        if (vs) vs->Release();

        ID3D11PixelShader* ps = nullptr;
        This->PSGetShader(&ps, nullptr, nullptr);
        st.ps = ps;
        if (ps) ps->Release();

        UINT nvp = 1;
        D3D11_VIEWPORT vp{};
        This->RSGetViewports(&nvp, &vp);
        if (nvp > 0) {
            st.hasViewport0 = true;
            st.viewport0 = vp;
        }
    });
}

// ============================================================================
// DXGI Factory & SwapChain Hooks
// ============================================================================

void UpdateSwapChainMetadata(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        std::lock_guard<std::mutex> lock(g_swapInfoMutex);
        g_swapInfo.width = desc.BufferDesc.Width;
        g_swapInfo.height = desc.BufferDesc.Height;
        g_swapInfo.format = desc.BufferDesc.Format;
        g_swapInfo.bufferCount = desc.BufferCount;
        g_swapInfo.sampleCount = desc.SampleDesc.Count;
        g_swapInfo.windowed = desc.Windowed;
    }

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
        ResourceTracker::Instance().MarkBackbufferTexture(bb);
        bb->Release();
    }
}

std::vector<std::string> BuildRuntimeDiagnostics(
    IDXGISwapChain* activeSwapChain,
    ID3D11Device* device,
    ID3D11DeviceContext* immediateCtx
) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_swapInfoMutex);
        for (const auto& [scPtr, sc] : g_seenSwapChains) {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "SwapChain %p%s: %ux%u MSAA=%u Win=%d HWND=%p Present=%llu Present1=%llu",
                          static_cast<const void*>(scPtr),
                          (scPtr == activeSwapChain) ? " [THIS]" : "",
                          sc.width, sc.height, sc.sampleCount, sc.windowed ? 1 : 0,
                          static_cast<const void*>(sc.hwnd),
                          static_cast<unsigned long long>(sc.presentCount),
                          static_cast<unsigned long long>(sc.present1Count));
            lines.emplace_back(buf);
        }
    }

    if (activeSwapChain && *reinterpret_cast<void***>(activeSwapChain)) {
        void** vtbl = *reinterpret_cast<void***>(activeSwapChain);
        void* slotFn = vtbl[vtable_slots::swapchain::Present];
        void* nextFn = reinterpret_cast<void*>(g_dispPresent.Resolve(activeSwapChain));
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Vtbl SwapChain::Present -> %p (%s) | next -> %p (%s)",
                      slotFn, ModuleNameForAddress(slotFn).c_str(),
                      nextFn, ModuleNameForAddress(nextFn).c_str());
        lines.emplace_back(buf);
    }
    if (device && *reinterpret_cast<void***>(device)) {
        void** vtbl = *reinterpret_cast<void***>(device);
        void* slotFn = vtbl[vtable_slots::device::CreateTexture2D];
        void* nextFn = reinterpret_cast<void*>(g_dispCreateTexture2D.Resolve(device));
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Vtbl Device::CreateTex2D -> %p (%s) | next -> %p (%s)",
                      slotFn, ModuleNameForAddress(slotFn).c_str(),
                      nextFn, ModuleNameForAddress(nextFn).c_str());
        lines.emplace_back(buf);
    }
    if (immediateCtx && *reinterpret_cast<void***>(immediateCtx)) {
        void** vtbl = *reinterpret_cast<void***>(immediateCtx);
        void* drawFn = vtbl[vtable_slots::context::Draw];
        void* omFn = vtbl[vtable_slots::context::OMSetRenderTargets];
        void* nextDraw = reinterpret_cast<void*>(g_dispDraw.Resolve(immediateCtx));
        char buf[224];
        std::snprintf(buf, sizeof(buf), "Vtbl ImmCtx(%p, vtbl=%p): Draw=%s(next=%s) OMSetRTs=%s",
                      static_cast<const void*>(immediateCtx), static_cast<const void*>(vtbl),
                      ModuleNameForAddress(drawFn).c_str(),
                      ModuleNameForAddress(nextDraw).c_str(),
                      ModuleNameForAddress(omFn).c_str());
        lines.emplace_back(buf);
    }

    static const wchar_t* kProbeDlls[] = {
        L"d3d11.dll", L"dxgi.dll", L"d3d9.dll", L"d3d10.dll", L"d3d10_1.dll",
        L"d3d12.dll", L"d2d1.dll", L"dwrite.dll", L"opengl32.dll", L"vulkan-1.dll",
        L"gameoverlayrenderer64.dll", L"nvapi64.dll", L"nvwgf2umx.dll",
        L"atidxx64.dll", L"ReShade64.dll", L"bink2w64.dll", L"awesomium.dll",
        L"libcef.dll", L"d3dcompiler_47.dll"
    };
    std::string loadedList;
    for (const wchar_t* dll : kProbeDlls) {
        if (GetModuleHandleW(dll)) {
            if (!loadedList.empty()) loadedList += ", ";
            char narrow[64]{};
            WideCharToMultiByte(CP_UTF8, 0, dll, -1, narrow, 64, nullptr, nullptr);
            loadedList += narrow;
        }
    }
    lines.push_back("Loaded GFX/UI/Overlay DLLs: " + (loadedList.empty() ? std::string("none") : loadedList));
    return lines;
}

void HandleFramePresent(IDXGISwapChain* swapChain, bool isPresent1) {
    auto& rt = GetProxyRuntime();
    auto& cfg = rt.config;
    if (!cfg.enabled) return;

    if (!g_overlayInitializedFromConfig.exchange(true, std::memory_order_acq_rel)) {
        g_overlayVisible.store(cfg.showOverlay, std::memory_order_release);
    }

    ScopedHookBypass bypass;

    SwapChainInfo infoCopy{};
    infoCopy.swapChainPtr = reinterpret_cast<uintptr_t>(swapChain);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        infoCopy.width = desc.BufferDesc.Width;
        infoCopy.height = desc.BufferDesc.Height;
        infoCopy.format = desc.BufferDesc.Format;
        infoCopy.bufferCount = desc.BufferCount;
        infoCopy.sampleCount = desc.SampleDesc.Count;
        infoCopy.windowed = desc.Windowed;
        infoCopy.hwnd = reinterpret_cast<uintptr_t>(desc.OutputWindow);
    } else {
        std::lock_guard<std::mutex> lock(g_swapInfoMutex);
        infoCopy = g_swapInfo;
        infoCopy.swapChainPtr = reinterpret_cast<uintptr_t>(swapChain);
    }

    bool firstSeenForChain = false;
    bool activeChanged = false;
    bool isSecondary = false;
    IDXGISwapChain* prevActive = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_swapInfoMutex);
        auto& entry = g_seenSwapChains[swapChain];
        firstSeenForChain = (entry.presentCount == 0 && entry.present1Count == 0);
        entry.width = infoCopy.width;
        entry.height = infoCopy.height;
        entry.format = infoCopy.format;
        entry.bufferCount = infoCopy.bufferCount;
        entry.sampleCount = infoCopy.sampleCount;
        entry.windowed = infoCopy.windowed;
        entry.hwnd = desc.OutputWindow;
        if (isPresent1) ++entry.present1Count;
        else ++entry.presentCount;

        const uint64_t thisArea = static_cast<uint64_t>(infoCopy.width) * infoCopy.height;
        uint64_t maxArea = thisArea;
        for (const auto& [scPtr, seen] : g_seenSwapChains) {
            const uint64_t a = static_cast<uint64_t>(seen.width) * seen.height;
            if (a > maxArea) maxArea = a;
        }
        isSecondary = (thisArea > 0 && maxArea > thisArea);
        if (!isSecondary) {
            g_swapInfo = infoCopy;
            if (g_lastActiveSwapChain != swapChain) {
                activeChanged = true;
                prevActive = g_lastActiveSwapChain;
                g_lastActiveSwapChain = swapChain;
            }
        }
    }

    if (firstSeenForChain || activeChanged) {
        wchar_t clsNameW[128]{};
        wchar_t wndTitleW[128]{};
        if (desc.OutputWindow) {
            GetClassNameW(desc.OutputWindow, clsNameW, 128);
            GetWindowTextW(desc.OutputWindow, wndTitleW, 128);
        }
        char clsName[256]{};
        char wndTitle[256]{};
        WideCharToMultiByte(CP_UTF8, 0, clsNameW, -1, clsName, static_cast<int>(sizeof(clsName)), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, wndTitleW, -1, wndTitle, static_cast<int>(sizeof(wndTitle)), nullptr, nullptr);

        void** scVtbl = *reinterpret_cast<void***>(swapChain);
        void* presentFn = scVtbl ? scVtbl[isPresent1 ? vtable_slots::swapchain::Present1 : vtable_slots::swapchain::Present] : nullptr;
        void* origFn = isPresent1
            ? reinterpret_cast<void*>(g_dispPresent1.Resolve(swapChain))
            : reinterpret_cast<void*>(g_dispPresent.Resolve(swapChain));
        UIE_LOG_INFO("%s SwapChain %p (%s): %ux%u MSAA=%u Win=%d HWND=%p class=\"%s\" title=\"%s\" slotOwner=%s origOwner=%s%s",
                     isSecondary ? "SecondaryPresent" : "PrimaryPresent",
                     static_cast<void*>(swapChain),
                     isPresent1 ? "Present1" : "Present",
                     infoCopy.width, infoCopy.height, infoCopy.sampleCount, infoCopy.windowed ? 1 : 0,
                     static_cast<void*>(desc.OutputWindow),
                     clsName, wndTitle,
                     ModuleNameForAddress(presentFn).c_str(),
                     ModuleNameForAddress(origFn).c_str(),
                     activeChanged && prevActive ? " [SWITCHED]" : "");
    }

    if (isSecondary) {
        if (Inspector::Instance().IsCapturing()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "sc=%p %ux%u MSAA=%u HWND=%p %s",
                          static_cast<void*>(swapChain), infoCopy.width, infoCopy.height,
                          infoCopy.sampleCount, static_cast<void*>(desc.OutputWindow),
                          isPresent1 ? "Present1" : "Present");
            Inspector::Instance().RecordGpuEvent("PresentSecondary", buf);
        }
        return;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateCtx = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        auto getImm = g_dispGetImmCtx.Resolve(device);
        if (getImm) getImm(device, &immediateCtx);
        else device->GetImmediateContext(&immediateCtx);
    }

    // Check BEFORE OnFrameEnd whether anything overwrote immediate context's vtable mid-frame!
    VerifyContextHooks(immediateCtx, "PrePresentCheck");

    // Refresh swapchain backbuffer marking (handles FLIP_DISCARD buffer rotation cleanly).
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
        ResourceTracker::Instance().MarkBackbufferTexture(bb);
        bb->Release();
    }

    if (Inspector::Instance().IsCapturing()) {
        Inspector::Instance().SetRuntimeDiagnostics(BuildRuntimeDiagnostics(swapChain, device, immediateCtx));
    }

    Inspector::Instance().OnFrameEnd(device, immediateCtx, infoCopy, cfg);
    UiEnhancer::Instance().SetSwapChainViewportInfo(infoCopy.width, infoCopy.height, infoCopy.hwnd);
    UiEnhancer::Instance().InitializeFromConfig(cfg);
    UiSrCache::Instance().InitializeFromConfig(cfg);
    WorldPostFx::Instance().InitializeFromConfig(cfg);
    UiEnhancer::Instance().OnFrameEnd(immediateCtx, DebugOverlay::Instance().IsMenuOpen());

    if (IsProcessForeground()) {
        if (IsKeyJustPressed(cfg.hotkeyCopyHover, g_prevF2)) {
            UiEnhancer::Instance().CopyHoveredElementToClipboard();
        }
        if (IsKeyJustPressed(cfg.hotkeyScaleUp, g_prevPgUp)) {
            UiEnhancer::Instance().AdjustHoveredElementScale(+0.05f, cfg);
        }
        if (IsKeyJustPressed(cfg.hotkeyScaleDown, g_prevPgDn)) {
            UiEnhancer::Instance().AdjustHoveredElementScale(-0.05f, cfg);
        }
        if (IsKeyJustPressed(cfg.hotkeyToggleSplit, g_prevF3)) {
            UiEnhancer::Instance().ToggleSplitScreen();
        }
        if (IsKeyJustPressed(cfg.hotkeyToggleCnn, g_prevF4)) {
            UiSrCache::Instance().ToggleEnabled();
        }
        if (IsKeyJustPressed(cfg.hotkeyToggleWorldFx, g_prevF5)) {
            WorldPostFx::Instance().ToggleEnabled();
        }
        if (IsKeyJustPressed(cfg.hotkeyReloadConfig, g_prevF6)) {
            const Config& reloaded = ReloadProxyConfig();
            UiEnhancer::Instance().ReloadFromConfig(reloaded);
            UiSrCache::Instance().ReloadFromConfig(reloaded);
            WorldPostFx::Instance().ReloadFromConfig(reloaded);
            UiEnhancer::Instance().SetHudNotification("CONFIG & OVERRIDES RELOADED");
        }
        if (IsKeyJustPressed(cfg.hotkeyCycleSharpness, g_prevF7)) {
            UiEnhancer::Instance().CycleSharpnessPreset();
        }
        if (IsKeyJustPressed(cfg.hotkeyToggleEnhance, g_prevF8)) {
            UiEnhancer::Instance().ToggleEnabled();
        }
        if (IsKeyJustPressed(cfg.hotkeyToggleMenu, g_prevCtrlF9)) {
            DebugOverlay::Instance().ToggleMenu();
            if (DebugOverlay::Instance().IsMenuOpen()) {
                g_overlayVisible.store(true, std::memory_order_release);
            }
        }
        if (IsKeyJustPressed(cfg.hotkeyToggleOverlay, g_prevF9)) {
            const bool nextVal = !g_overlayVisible.load(std::memory_order_relaxed);
            g_overlayVisible.store(nextVal, std::memory_order_release);
            UIE_LOG_INFO("Overlay toggled: %s", nextVal ? "ON" : "OFF");
        }
        if (IsKeyJustPressed(cfg.hotkeyCaptureFrame, g_prevF10)) {
            Inspector::Instance().RequestCapture(false);
        }
        if (IsKeyJustPressed(cfg.hotkeyCaptureAndDump, g_prevF11)) {
            Inspector::Instance().RequestCapture(true);
        }
    } else {
        g_prevF2 = g_prevF3 = g_prevF4 = g_prevF5 = g_prevF6 = g_prevF7 = g_prevF8 =
            g_prevF9 = g_prevCtrlF9 = g_prevF10 = g_prevF11 = g_prevPgUp = g_prevPgDn = false;
    }

    if (immediateCtx) {
        UiEnhancer::Instance().SyncGpuConstants(immediateCtx);
    }

    const auto stats = Inspector::Instance().GetLiveStats();
    // Fallback if UI was hidden via F12 (zero UI draws hit backbuffer) yet a 3D scene rendered
    if (device && immediateCtx && stats.lastFrameDraws >= 15 &&
        WorldPostFx::Instance().IsEnabled() && !WorldPostFx::Instance().GetStats().activeThisFrame) {
        WorldPostFx::Instance().ApplyToBackbuffer(device, immediateCtx, swapChain);
    }

    if (g_overlayVisible.load(std::memory_order_acquire) && device && immediateCtx) {
        const auto uieStats = UiEnhancer::Instance().GetStats();
        DebugOverlay::Instance().Render(swapChain, device, immediateCtx, infoCopy, stats, uieStats, cfg);
    }

    Inspector::Instance().OnFrameBegin(cfg);
    UiSrCache::Instance().OnFrameBegin();
    WorldPostFx::Instance().OnFrameBegin();

    if (immediateCtx) immediateCtx->Release();
    if (device) device->Release();
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    ++g_presentDepth;
    const bool outerPresent = (g_presentDepth == 1 && !IsBypassed());
    if (outerPresent) {
        HandleFramePresent(This, false);
    }
    HRESULT hr = S_OK;
    {
        ScopedHookBypass bypass;
        hr = g_dispPresent.Resolve(This)(This, SyncInterval, Flags);
    }
    if (outerPresent) {
        EnsureImmediateContextHooks(This);
    }
    --g_presentDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(
    IDXGISwapChain1* This,
    UINT SyncInterval,
    UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters
) {
    ++g_presentDepth;
    const bool outerPresent = (g_presentDepth == 1 && !IsBypassed());
    if (outerPresent) {
        HandleFramePresent(This, true);
    }
    HRESULT hr = S_OK;
    {
        ScopedHookBypass bypass;
        hr = g_dispPresent1.Resolve(This)(This, SyncInterval, PresentFlags, pPresentParameters);
    }
    if (outerPresent) {
        EnsureImmediateContextHooks(This);
    }
    --g_presentDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(
    IDXGISwapChain* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags
) {
    DebugOverlay::Instance().OnBeforeResizeBuffers();
    WorldPostFx::Instance().OnBeforeResizeBuffers();
    HRESULT hr = S_OK;
    {
        ScopedHookBypass bypass;
        hr = g_dispResizeBuffers.Resolve(This)(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    if (SUCCEEDED(hr)) {
        UpdateSwapChainMetadata(This);
        EnsureImmediateContextHooks(This);
        UIE_LOG_INFO("SwapChain ResizeBuffers -> %ux%u", g_swapInfo.width, g_swapInfo.height);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(
    IDXGIFactory* This,
    IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain
) {
    if (pDevice) {
        ID3D11Device* d3dDev = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&d3dDev))) && d3dDev) {
            OnDeviceCreated(d3dDev, nullptr);
            d3dDev->Release();
        }
    }
    HRESULT hr = S_OK;
    {
        ScopedPrimaryVtableRestore<PfnPresent> restPresent(vtable_slots::swapchain::Present, HookPresent, g_dispPresent);
        ScopedPrimaryVtableRestore<PfnPresent1> restPresent1(vtable_slots::swapchain::Present1, HookPresent1, g_dispPresent1);
        ScopedPrimaryVtableRestore<PfnResizeBuffers> restResize(vtable_slots::swapchain::ResizeBuffers, HookResizeBuffers, g_dispResizeBuffers);
        ScopedHookBypass bypass;
        hr = g_dispCreateSwapChain.Resolve(This)(This, pDevice, pDesc, ppSwapChain);
    }
    UIE_LOG_INFO("IDXGIFactory::CreateSwapChain(%ux%u, Windowed=%d, HWND=%p) -> hr=0x%08X (swapChain=%p)",
                 pDesc ? pDesc->BufferDesc.Width : 0u,
                 pDesc ? pDesc->BufferDesc.Height : 0u,
                 pDesc ? (pDesc->Windowed ? 1 : 0) : -1,
                 pDesc ? static_cast<const void*>(pDesc->OutputWindow) : nullptr,
                 static_cast<unsigned>(hr),
                 (SUCCEEDED(hr) && ppSwapChain) ? static_cast<void*>(*ppSwapChain) : nullptr);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        OnSwapChainCreated(*ppSwapChain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain
) {
    if (pDevice) {
        ID3D11Device* d3dDev = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&d3dDev))) && d3dDev) {
            OnDeviceCreated(d3dDev, nullptr);
            d3dDev->Release();
        }
    }
    HRESULT hr = S_OK;
    {
        ScopedPrimaryVtableRestore<PfnPresent> restPresent(vtable_slots::swapchain::Present, HookPresent, g_dispPresent);
        ScopedPrimaryVtableRestore<PfnPresent1> restPresent1(vtable_slots::swapchain::Present1, HookPresent1, g_dispPresent1);
        ScopedPrimaryVtableRestore<PfnResizeBuffers> restResize(vtable_slots::swapchain::ResizeBuffers, HookResizeBuffers, g_dispResizeBuffers);
        ScopedHookBypass bypass;
        hr = g_dispCreateSwapChainForHwnd.Resolve(This)(
            This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    }
    UIE_LOG_INFO("IDXGIFactory2::CreateSwapChainForHwnd(%ux%u, HWND=%p) -> hr=0x%08X (swapChain=%p)",
                 pDesc ? pDesc->Width : 0u,
                 pDesc ? pDesc->Height : 0u,
                 static_cast<const void*>(hWnd),
                 static_cast<unsigned>(hr),
                 (SUCCEEDED(hr) && ppSwapChain) ? static_cast<void*>(*ppSwapChain) : nullptr);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        OnSwapChainCreated(*ppSwapChain);
    }
    return hr;
}

void PatchCoreContextSlots(ID3D11DeviceContext* ctx) {
    std::lock_guard<std::mutex> lock(g_contextPatchMutex);
    PatchVtableSlot(ctx, vtable_slots::context::PSSetShaderResources, HookPSSetShaderResources, g_dispPSSetSRVs, true);
    PatchVtableSlot(ctx, vtable_slots::context::PSSetShader, HookPSSetShader, g_dispPSSetShader, true);
    PatchVtableSlot(ctx, vtable_slots::context::PSSetSamplers, HookPSSetSamplers, g_dispPSSetSamplers, true);
    PatchVtableSlot(ctx, vtable_slots::context::VSSetShader, HookVSSetShader, g_dispVSSetShader, true);
    PatchVtableSlot(ctx, vtable_slots::context::CSSetShaderResources, HookCSSetShaderResources, g_dispCSSetSRVs, true);
    PatchVtableSlot(ctx, vtable_slots::context::CSSetUnorderedAccessViews, HookCSSetUnorderedAccessViews, g_dispCSSetUAVs, true);
    PatchVtableSlot(ctx, vtable_slots::context::CSSetShader, HookCSSetShader, g_dispCSSetShader, true);
    PatchVtableSlot(ctx, vtable_slots::context::DrawIndexed, HookDrawIndexed, g_dispDrawIndexed, true);
    PatchVtableSlot(ctx, vtable_slots::context::Draw, HookDraw, g_dispDraw, true);
    PatchVtableSlot(ctx, vtable_slots::context::DrawIndexedInstanced, HookDrawIndexedInstanced, g_dispDrawIndexedInstanced, true);
    PatchVtableSlot(ctx, vtable_slots::context::DrawInstanced, HookDrawInstanced, g_dispDrawInstanced, true);
    PatchVtableSlot(ctx, vtable_slots::context::DrawAuto, HookDrawAuto, g_dispDrawAuto, true);
    PatchVtableSlot(ctx, vtable_slots::context::DrawInstancedIndirect, HookDrawInstancedIndirect, g_dispDrawInstancedIndirect, true);
    PatchVtableSlot(ctx, vtable_slots::context::DrawIndexedInstancedIndirect,
                    HookDrawIndexedInstancedIndirect, g_dispDrawIndexedInstancedIndirect, true);
    PatchVtableSlot(ctx, vtable_slots::context::Dispatch, HookDispatch, g_dispDispatch, true);
    PatchVtableSlot(ctx, vtable_slots::context::DispatchIndirect, HookDispatchIndirect, g_dispDispatchIndirect, true);
    PatchVtableSlot(ctx, vtable_slots::context::Map, HookMap, g_dispMap, true);
    PatchVtableSlot(ctx, vtable_slots::context::Unmap, HookUnmap, g_dispUnmap, true);
    PatchVtableSlot(ctx, vtable_slots::context::IASetVertexBuffers, HookIASetVertexBuffers, g_dispIASetVBs, true);
    PatchVtableSlot(ctx, vtable_slots::context::IASetPrimitiveTopology, HookIASetPrimitiveTopology, g_dispIASetTopo, true);
    PatchVtableSlot(ctx, vtable_slots::context::OMSetRenderTargets, HookOMSetRenderTargets, g_dispOMSetRTs, true);
    PatchVtableSlot(ctx, vtable_slots::context::OMSetRenderTargetsAndUnorderedAccessViews,
                    HookOMSetRenderTargetsAndUAVs, g_dispOMSetRTsUAVs, true);
    PatchVtableSlot(ctx, vtable_slots::context::OMSetBlendState, HookOMSetBlendState, g_dispOMSetBlend, true);
    PatchVtableSlot(ctx, vtable_slots::context::OMSetDepthStencilState, HookOMSetDepthStencilState, g_dispOMSetDepth, true);
    PatchVtableSlot(ctx, vtable_slots::context::RSSetState, HookRSSetState, g_dispRSSetState, true);
    PatchVtableSlot(ctx, vtable_slots::context::RSSetViewports, HookRSSetViewports, g_dispRSSetViewports, true);
    PatchVtableSlot(ctx, vtable_slots::context::RSSetScissorRects, HookRSSetScissorRects, g_dispRSSetScissors, true);
    PatchVtableSlot(ctx, vtable_slots::context::CopySubresourceRegion, HookCopySubresourceRegion, g_dispCopySubresourceRegion, true);
    PatchVtableSlot(ctx, vtable_slots::context::CopyResource, HookCopyResource, g_dispCopyResource, true);
    PatchVtableSlot(ctx, vtable_slots::context::UpdateSubresource, HookUpdateSubresource, g_dispUpdateSubresource, true);
    PatchVtableSlot(ctx, vtable_slots::context::ClearRenderTargetView, HookClearRenderTargetView, g_dispClearRTV, true);
    PatchVtableSlot(ctx, vtable_slots::context::ClearDepthStencilView, HookClearDepthStencilView, g_dispClearDSV, true);
    PatchVtableSlot(ctx, vtable_slots::context::ResolveSubresource, HookResolveSubresource, g_dispResolveSubresource, true);
    PatchVtableSlot(ctx, vtable_slots::context::ExecuteCommandList, HookExecuteCommandList, g_dispExecuteCommandList, true);
    PatchVtableSlot(ctx, vtable_slots::context::FinishCommandList, HookFinishCommandList, g_dispFinishCommandList, true);
    PatchVtableSlot(ctx, vtable_slots::context::Flush, HookFlush, g_dispFlush, true);
    PatchVtableSlot(ctx, vtable_slots::context::ClearState, HookClearState, g_dispClearState, true);
}

void HookDeviceContext(ID3D11DeviceContext* ctx, bool isImmediate) {
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lock(g_hookedSetMutex);
        if (g_lastLoggedCtx != ctx) {
            g_lastLoggedCtx = ctx;
            void** vtbl = *reinterpret_cast<void***>(ctx);
            UIE_LOG_INFO("Hooking ID3D11DeviceContext (%p, imm=%d, vtbl=%p [%s], drawSlot=%zu)",
                         static_cast<void*>(ctx), isImmediate ? 1 : 0, static_cast<void*>(vtbl),
                         ModuleNameForAddress(vtbl).c_str(),
                         vtable_slots::context::Draw);
        }
    }
    if (isImmediate) {
        ID3D11DeviceContext* prev = g_immediateCtx.exchange(ctx, std::memory_order_acq_rel);
        if (prev != ctx) {
            std::lock_guard<std::mutex> lock(g_immediateStateMutex);
            g_immediateState.Clear();
        }
    }

    PatchCoreContextSlots(ctx);

    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
        const bool samePtr = (static_cast<void*>(ctx1) == static_cast<void*>(ctx));
        const bool sameVtbl = (*reinterpret_cast<void***>(ctx1) == *reinterpret_cast<void***>(ctx));
        {
            std::lock_guard<std::mutex> lock(g_hookedSetMutex);
            if (g_lastLoggedCtx1 != ctx1) {
                g_lastLoggedCtx1 = ctx1;
                UIE_LOG_INFO("ID3D11DeviceContext1 QI (%p, samePtr=%d, sameVtbl=%d)",
                             static_cast<void*>(ctx1), samePtr ? 1 : 0, sameVtbl ? 1 : 0);
            }
        }
        if (isImmediate && !samePtr) {
            g_immediateCtx1Alias.store(ctx1, std::memory_order_release);
        }
        if (!sameVtbl) {
            PatchCoreContextSlots(ctx1);
        }
        {
            std::lock_guard<std::mutex> lock(g_contextPatchMutex);
            PatchVtableSlot(ctx1, vtable_slots::context1::ClearView, HookClearView, g_dispClearView, true);
            PatchVtableSlot(ctx1, vtable_slots::context1::DiscardResource, HookDiscardResource, g_dispDiscardResource, true);
            PatchVtableSlot(ctx1, vtable_slots::context1::DiscardView, HookDiscardView, g_dispDiscardView, true);
            PatchVtableSlot(ctx1, vtable_slots::context1::DiscardView1, HookDiscardView1, g_dispDiscardView1, true);
            PatchVtableSlot(ctx1, vtable_slots::context1::SwapDeviceContextState,
                            HookSwapDeviceContextState, g_dispSwapDeviceContextState, true);
        }
        ctx1->Release();
    }
}

bool AreCoreContextHooksIntact(ID3D11DeviceContext* ctx) noexcept {
    if (!ctx) return true;
    void** vtbl = *reinterpret_cast<void***>(ctx);
    if (!vtbl) return true;
    return vtbl[vtable_slots::context::Draw] == reinterpret_cast<void*>(HookDraw) &&
           vtbl[vtable_slots::context::DrawIndexed] == reinterpret_cast<void*>(HookDrawIndexed) &&
           vtbl[vtable_slots::context::PSSetShader] == reinterpret_cast<void*>(HookPSSetShader) &&
           vtbl[vtable_slots::context::OMSetRenderTargets] == reinterpret_cast<void*>(HookOMSetRenderTargets) &&
           vtbl[vtable_slots::context::Map] == reinterpret_cast<void*>(HookMap) &&
           vtbl[vtable_slots::context::ClearState] == reinterpret_cast<void*>(HookClearState);
}

bool VerifyContextHooks(ID3D11DeviceContext* ctx, const char* caller) noexcept {
    if (!ctx || AreCoreContextHooksIntact(ctx)) return true;

    void** vtbl = *reinterpret_cast<void***>(ctx);
    void* drawFn = vtbl ? vtbl[vtable_slots::context::Draw] : nullptr;
    const std::string vtblMod = ModuleNameForAddress(vtbl);
    const std::string drawMod = ModuleNameForAddress(drawFn);
    const bool isImm = (ctx == g_immediateCtx.load(std::memory_order_acquire) ||
                        ctx == g_immediateCtx1Alias.load(std::memory_order_acquire));
    const bool isPostPresent = (caller && std::strcmp(caller, "PostPresentCheck") == 0);

    if (isPostPresent) {
        if (!g_loggedPostPresentReset.exchange(true, std::memory_order_relaxed)) {
            UIE_LOG_INFO("PostPresentCheck: DXGI Present reset context vtable (ctx=%p vtbl=%p(%s) draw=%s) -> auto re-hooking cleanly",
                         static_cast<void*>(ctx), static_cast<void*>(vtbl), vtblMod.c_str(), drawMod.c_str());
        }
    } else {
        if (g_vtableResetLogCount.fetch_add(1, std::memory_order_relaxed) < 25) {
            UIE_LOG_INFO("ALERT: Context vtable overwritten (%s): ctx=%p imm=%d vtbl=%p(%s) draw=%p(%s) -> re-hooking",
                         caller ? caller : "?", static_cast<void*>(ctx), isImm ? 1 : 0,
                         static_cast<void*>(vtbl), vtblMod.c_str(), drawFn, drawMod.c_str());
        }
        if (Inspector::Instance().IsCapturing()) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "caller=%s imm=%d vtbl=%p(%s) drawOwner=%s",
                          caller ? caller : "?", isImm ? 1 : 0,
                          static_cast<void*>(vtbl), vtblMod.c_str(), drawMod.c_str());
            Inspector::Instance().RecordGpuEvent("VtableReset", buf);
        }
    }

    HookDeviceContext(ctx, isImm);
    return false;
}

void EnsureImmediateContextHooks(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    ScopedHookBypass bypass;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateCtx = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        auto getImm = g_dispGetImmCtx.Resolve(device);
        if (getImm) getImm(device, &immediateCtx);
        else device->GetImmediateContext(&immediateCtx);
    }
    if (immediateCtx) {
        VerifyContextHooks(immediateCtx, "PostPresentCheck");
        immediateCtx->Release();
    }
    if (device) device->Release();
}

} // namespace

void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* immediateCtx) {
    if (!device) return;
    {
        std::lock_guard<std::mutex> lock(g_hookedSetMutex);
        if (g_lastLoggedDevice != device) {
            g_lastLoggedDevice = device;
            UIE_LOG_INFO("Hooking ID3D11Device (%p)", static_cast<void*>(device));
        }
    }

    PatchVtableSlot(device, vtable_slots::device::CreateTexture2D, HookCreateTexture2D, g_dispCreateTexture2D);
    PatchVtableSlot(device, vtable_slots::device::CreateShaderResourceView, HookCreateSRV, g_dispCreateSRV);
    PatchVtableSlot(device, vtable_slots::device::CreateRenderTargetView, HookCreateRTV, g_dispCreateRTV);
    PatchVtableSlot(device, vtable_slots::device::CreateDepthStencilView, HookCreateDSV, g_dispCreateDSV);
    PatchVtableSlot(device, vtable_slots::device::CreateVertexShader, HookCreateVS, g_dispCreateVS);
    PatchVtableSlot(device, vtable_slots::device::CreatePixelShader, HookCreatePS, g_dispCreatePS);
    PatchVtableSlot(device, vtable_slots::device::CreateComputeShader, HookCreateCS, g_dispCreateCS);
    PatchVtableSlot(device, vtable_slots::device::CreateBlendState, HookCreateBlendState, g_dispCreateBlend);
    PatchVtableSlot(device, vtable_slots::device::CreateDepthStencilState, HookCreateDepthStencilState, g_dispCreateDepth);
    PatchVtableSlot(device, vtable_slots::device::CreateRasterizerState, HookCreateRasterizerState, g_dispCreateRast);
    PatchVtableSlot(device, vtable_slots::device::CreateSamplerState, HookCreateSamplerState, g_dispCreateSampler);
    PatchVtableSlot(device, vtable_slots::device::CreateDeferredContext, HookCreateDeferredContext, g_dispCreateDeferredCtx);
    PatchVtableSlot(device, vtable_slots::device::GetImmediateContext, HookGetImmediateContext, g_dispGetImmCtx);

    ID3D11Device1* dev1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1))) && dev1) {
        {
            std::lock_guard<std::mutex> lock(g_hookedSetMutex);
            if (g_lastLoggedDevice1 != dev1) {
                g_lastLoggedDevice1 = dev1;
                UIE_LOG_INFO("ID3D11Device1 QI (%p, samePtr=%d, sameVtbl=%d)",
                             static_cast<void*>(dev1),
                             (static_cast<void*>(dev1) == static_cast<void*>(device)) ? 1 : 0,
                             (*reinterpret_cast<void***>(dev1) == *reinterpret_cast<void***>(device)) ? 1 : 0);
            }
        }
        PatchVtableSlot(dev1, vtable_slots::device1::GetImmediateContext1, HookGetImmediateContext1, g_dispGetImmCtx1);
        PatchVtableSlot(dev1, vtable_slots::device1::CreateDeferredContext1, HookCreateDeferredContext1, g_dispCreateDeferredCtx1);
        PatchVtableSlot(dev1, vtable_slots::device1::CreateDeviceContextState,
                        HookCreateDeviceContextState, g_dispCreateDeviceCtxState);
        dev1->Release();
    }

    if (immediateCtx) {
        HookDeviceContext(immediateCtx, true);
    } else {
        ID3D11DeviceContext* ctx = nullptr;
        auto getImm = g_dispGetImmCtx.Resolve(device);
        if (getImm) getImm(device, &ctx);
        else device->GetImmediateContext(&ctx);
        if (ctx) {
            HookDeviceContext(ctx, true);
            ctx->Release();
        }
    }

    // Also hook IDXGIFactory obtained from this device so CreateSwapChain is intercepted even if
    // the app didn't call D3D11CreateDeviceAndSwapChain.
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) && factory) {
                OnFactoryCreated(factory);
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    {
        ScopedHookBypass bypass;
        const auto& cfg = GetProxyRuntime().config;
        UiSrCache::Instance().InitializeFromConfig(cfg);
        WorldPostFx::Instance().InitializeFromConfig(cfg);
        auto& uie = UiEnhancer::Instance();
        uie.InitializeFromConfig(cfg);
        uie.OnDeviceCreated(device);
    }
}

void OnFactoryCreated(IUnknown* factoryUnk) {
    if (!factoryUnk) return;
    {
        std::lock_guard<std::mutex> lock(g_hookedSetMutex);
        if (g_lastLoggedFactory != factoryUnk) {
            g_lastLoggedFactory = factoryUnk;
            UIE_LOG_INFO("Hooking IDXGIFactory (%p)", static_cast<void*>(factoryUnk));
        }
    }
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(factoryUnk->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) && factory) {
        PatchVtableSlot(factory, vtable_slots::factory::CreateSwapChain, HookCreateSwapChain, g_dispCreateSwapChain);
        factory->Release();
    }
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factoryUnk->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) && factory2) {
        PatchVtableSlot(factory2, vtable_slots::factory::CreateSwapChainForHwnd,
                        HookCreateSwapChainForHwnd, g_dispCreateSwapChainForHwnd);
        factory2->Release();
    }
}

void OnSwapChainCreated(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    {
        std::lock_guard<std::mutex> lock(g_hookedSetMutex);
        if (g_lastLoggedSwapChain != swapChain) {
            g_lastLoggedSwapChain = swapChain;
            UIE_LOG_INFO("Hooking IDXGISwapChain (%p)", static_cast<void*>(swapChain));
        }
    }

    PatchVtableSlot(swapChain, vtable_slots::swapchain::Present, HookPresent, g_dispPresent);
    PatchVtableSlot(swapChain, vtable_slots::swapchain::ResizeBuffers, HookResizeBuffers, g_dispResizeBuffers);

    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))) && swapChain1) {
        PatchVtableSlot(swapChain1, vtable_slots::swapchain::Present1, HookPresent1, g_dispPresent1);
        swapChain1->Release();
    }

    // Ensure owning device & context are hooked (relevant in dxgi.dll mode).
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device) {
        OnDeviceCreated(device, nullptr);
        device->Release();
    }

    UpdateSwapChainMetadata(swapChain);
    {
        std::lock_guard<std::mutex> lock(g_swapInfoMutex);
        if (!g_lastActiveSwapChain) {
            g_lastActiveSwapChain = swapChain;
        }
    }
    {
        ScopedHookBypass bypass;
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
            ResourceTracker::Instance().MarkBackbufferTexture(bb);
            bb->Release();
        }
    }
    UIE_LOG_INFO("SwapChain initial size: %ux%u (buffers=%u, sample=%u)",
                 g_swapInfo.width, g_swapInfo.height, g_swapInfo.bufferCount, g_swapInfo.sampleCount);
}

void ShutdownDx11Hooks(bool /*processTerminating*/) {
    // Only flip the atomic flag here: DllMain(DLL_PROCESS_DETACH) holds the OS loader lock,
    // so calling COM Release() on D3D11/DXGI objects (e.g. DebugOverlay::Shutdown) is unsafe.
    UiSrCache::Instance().SetShuttingDown(true);
    ResourceTracker::Instance().SetShuttingDown(true);
}

} // namespace lotro_uie
