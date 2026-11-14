#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

using PfnD3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
);

using PfnD3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**
);

using PfnCreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);

using PfnD3DCompile = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**
);

const char kTestVs[] = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    o.pos = float4(p, 0.0f, 1.0f);
    o.uv = float2((vid & 1) ? 1.0f : 0.0f, (vid & 2) ? 1.0f : 0.0f);
    return o;
}
)";

const char kTestPs[] = R"(
Texture2D Tex0 : register(t0);
SamplerState Samp0 : register(s0);
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET {
    return Tex0.Sample(Samp0, i.uv);
}
)";

const char kTestCs[] = R"(
[numthreads(1, 1, 1)]
void main() {
}
)";

const char kLotroUiVs[] = R"(
struct VSOut {
    float4 pos   : SV_Position;
    float  clip0 : SV_ClipDistance0;
    float2 uv    : TEXCOORD0;
};
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    o.pos = float4(p, 0.0f, 1.0f);
    o.clip0 = 1.0f;
    o.uv = float2((vid & 1) ? 1.0f : 0.0f, (vid & 2) ? 1.0f : 0.0f);
    return o;
}
)";

const char kLotroUiHalfVs[] = R"(
struct VSOut {
    float4 pos   : SV_Position;
    float  clip0 : SV_ClipDistance0;
    float2 uv    : TEXCOORD0;
};
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid & 1) ? 0.5f : -0.5f, (vid & 2) ? -0.5f : 0.5f);
    o.pos = float4(p, 0.0f, 1.0f);
    o.clip0 = 1.0f;
    o.uv = float2((vid & 1) ? 1.0f : 0.0f, (vid & 2) ? 1.0f : 0.0f);
    return o;
}
)";

const char kLotroUiPs[] = R"(
Texture2D Tex0 : register(t0);
SamplerState Samp0 : register(s0);
cbuffer ShaderGlobals : register(b0) {
    float4 c_Clip;
    float4x4 c_M2C;
    float4x4 c_M2W;
    float4 c_MatDiffuse;
    float c_AlphaThresh;
    float3 _pad0;
};
struct PSIn {
    float4 pos   : SV_Position;
    float  clip0 : SV_ClipDistance0;
    float2 uv    : TEXCOORD0;
};
float4 main(PSIn i) : SV_TARGET {
    float4 r = Tex0.Sample(Samp0, i.uv);
    if (c_AlphaThresh >= r.a) discard;
    return r * c_MatDiffuse;
}
)";

// Same semantics as kLotroUiPs with slightly distinct bytecode so hash differs for A/B comparison.
const char kLotroUiBaselinePs[] = R"(
Texture2D Tex0 : register(t0);
SamplerState Samp0 : register(s0);
cbuffer ShaderGlobals : register(b0) {
    float4 c_Clip;
    float4x4 c_M2C;
    float4x4 c_M2W;
    float4 c_MatDiffuse;
    float c_AlphaThresh;
    float3 _pad0;
};
struct PSIn {
    float4 pos   : SV_Position;
    float  clip0 : SV_ClipDistance0;
    float2 uv    : TEXCOORD0;
};
float4 main(PSIn i) : SV_TARGET {
    float4 r = Tex0.Sample(Samp0, i.uv);
    if (c_AlphaThresh >= r.a) discard;
    return float4(saturate(r.rgb * c_MatDiffuse.rgb), r.a * c_MatDiffuse.a);
}
)";

uint64_t ComputeFnv1a64(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ULL;
    for (size_t idx = 0; idx < size; ++idx) {
        h ^= static_cast<uint64_t>(bytes[idx]);
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: lotro_uie_smoke_test [--dxgi] [--probe-aa <sharpness>] <path_to_proxy_dll>\n");
        return 2;
    }

    bool dxgiMode = false;
    bool probeAaMode = false;
    std::string probeSharpnessStr = "0.70";
    std::wstring dllArg;
    for (int a = 1; a < argc; ++a) {
        if (std::wcscmp(argv[a], L"--dxgi") == 0) {
            dxgiMode = true;
        } else if (std::wcscmp(argv[a], L"--probe-aa") == 0 && a + 1 < argc) {
            probeAaMode = true;
            char buf[32]{};
            WideCharToMultiByte(CP_UTF8, 0, argv[++a], -1, buf, sizeof(buf), nullptr, nullptr);
            probeSharpnessStr = buf;
        } else {
            dllArg = argv[a];
        }
    }
    if (dllArg.empty()) {
        std::fprintf(stderr, "Missing proxy DLL path\n");
        return 2;
    }
    const fs::path dllPath = fs::absolute(dllArg);
    const fs::path workDir = dllPath.parent_path();

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<PfnD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        std::fprintf(stderr, "Failed to load d3dcompiler_47.dll\n");
        return 5;
    }

    ID3DBlob* lotroUiVsBlob = nullptr;
    ID3DBlob* lotroUiHalfVsBlob = nullptr;
    ID3DBlob* lotroUiPsBlob = nullptr;
    ID3DBlob* lotroUiBasePsBlob = nullptr;
    compileFn(kLotroUiVs, sizeof(kLotroUiVs) - 1, "lotro_ui_vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &lotroUiVsBlob, nullptr);
    compileFn(kLotroUiHalfVs, sizeof(kLotroUiHalfVs) - 1, "lotro_ui_half_vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &lotroUiHalfVsBlob, nullptr);
    compileFn(kLotroUiPs, sizeof(kLotroUiPs) - 1, "lotro_ui_ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &lotroUiPsBlob, nullptr);
    compileFn(kLotroUiBaselinePs, sizeof(kLotroUiBaselinePs) - 1, "lotro_ui_base_ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &lotroUiBasePsBlob, nullptr);
    if (!lotroUiVsBlob || !lotroUiHalfVsBlob || !lotroUiPsBlob || !lotroUiBasePsBlob) {
        std::fprintf(stderr, "Failed to compile UI enhancer test shaders\n");
        return 5;
    }
    const uint64_t lotroUiPsHash = ComputeFnv1a64(lotroUiPsBlob->GetBufferPointer(), lotroUiPsBlob->GetBufferSize());

    // Preserve pre-existing lotro_uie.ini (e.g. production defaults in build/dist/) across the test run.
    const fs::path iniPath = workDir / L"lotro_uie.ini";
    bool hadOriginalIni = false;
    std::string originalIniBytes;
    if (fs::exists(iniPath)) {
        hadOriginalIni = true;
        std::ifstream origIn(iniPath, std::ios::binary);
        originalIniBytes.assign(std::istreambuf_iterator<char>(origIn), std::istreambuf_iterator<char>());
    }
    struct IniRestorer {
        fs::path path;
        bool restoreOriginal = false;
        std::string bytes;
        bool done = false;
        void RestoreNow() {
            if (done) return;
            done = true;
            if (restoreOriginal) {
                std::ofstream origOut(path, std::ios::binary | std::ios::trunc);
                origOut.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
        }
        ~IniRestorer() { RestoreNow(); }
    } iniRestorer{ iniPath, hadOriginalIni, originalIniBytes, false };

    // Clean any old test capture output and configure AutoCaptureFrame=1 + Enhancer TargetPsHash.
    std::error_code ec;
    if (!probeAaMode) {
        fs::remove_all(workDir / L"lotro_uie_captures", ec);
    }
    fs::remove(workDir / L"lotro_uie.log", ec);
    {
        char hashBuf[32];
        std::snprintf(hashBuf, sizeof(hashBuf), "0x%016llX", static_cast<unsigned long long>(lotroUiPsHash));
        std::ofstream ini(iniPath);
        ini << "[General]\nEnabled = true\nLogLevel = info\nShowOverlay = true\n"
            << "[Enhancer]\nEnabled = true\nSharpness = " << (probeAaMode ? probeSharpnessStr : "0.70")
            << "\nEdgeAniso = 0.85\nEnhanceNameplates = true\nMapMode = true\nMapSharpnessBoost = 1.20\nTargetPsHash = " << hashBuf << "\n"
            << "[ElementScale]\nTestBadge = 20x12, +100%\n"
            << "[CNN]\nEnabled = true\nUpscaleUiRenderTargets = true\nCacheMaxMb = 192\nMaxNewPerFrame = 8\nMaxDynamicPerFrame = 4\n"
            << "[WorldPostFX]\nEnabled = true\nCasStrength = 0.45\nClarity = 0.14\nVibrance = 0.12\nDeband = true\n"
            << "[Samplers]\nForceAnisotropy = 16\nMipLodBias = -0.35\n"
            << "[Inspector]\nAutoCaptureFrame = " << (probeAaMode ? "0" : "1")
            << "\nAutoDumpAssets = " << (probeAaMode ? "false" : "true") << "\n"
            << "DumpAllFrameTextures = false\nDumpOnlyUiTextures = false\nDumpFormat = all\nOutputDir = lotro_uie_captures\n";
    }

    HMODULE proxyMod = LoadLibraryW(dllPath.c_str());
    if (!proxyMod) {
        std::fprintf(stderr, "LoadLibraryW(%ls) failed: %lu\n", dllPath.c_str(), GetLastError());
        return 3;
    }

    HMODULE sysD3d11 = nullptr;
    PfnD3D11CreateDevice createDev = nullptr;
    PfnCreateDXGIFactory1 createFactory1 = nullptr;
    PfnD3D11CreateDeviceAndSwapChain createDevSwap = nullptr;

    if (dxgiMode) {
        createFactory1 = reinterpret_cast<PfnCreateDXGIFactory1>(
            GetProcAddress(proxyMod, "CreateDXGIFactory1"));
        if (!createFactory1) {
            std::fprintf(stderr, "Missing CreateDXGIFactory1 export\n");
            return 4;
        }
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        sysD3d11 = LoadLibraryW((fs::path(sysDir) / L"d3d11.dll").c_str());
        createDev = sysD3d11 ? reinterpret_cast<PfnD3D11CreateDevice>(
            GetProcAddress(sysD3d11, "D3D11CreateDevice")) : nullptr;
        if (!createDev) {
            std::fprintf(stderr, "Missing System32 D3D11CreateDevice\n");
            return 4;
        }
    } else {
        createDevSwap = reinterpret_cast<PfnD3D11CreateDeviceAndSwapChain>(
            GetProcAddress(proxyMod, "D3D11CreateDeviceAndSwapChain"));
        if (!createDevSwap) {
            std::fprintf(stderr, "Missing D3D11CreateDeviceAndSwapChain export\n");
            return 4;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LOTRO_UIE_SMOKE";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"SmokeTest", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!hwnd) {
        std::fprintf(stderr, "CreateWindowExW failed\n");
        return 6;
    }

    DXGI_SWAP_CHAIN_DESC scDesc{};
    scDesc.BufferCount = 2;
    scDesc.BufferDesc.Width = 640;
    scDesc.BufferDesc.Height = 360;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = hwnd;
    scDesc.SampleDesc.Count = 1;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = E_FAIL;

    if (dxgiMode) {
        IDXGIFactory1* factory = nullptr;
        hr = createFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            std::fprintf(stderr, "CreateDXGIFactory1 failed: 0x%08X\n", static_cast<unsigned>(hr));
            DestroyWindow(hwnd);
            return 7;
        }
        // Probe + FreeLibrary simulation: proxy DXGI must stay pinned so factory remains valid.
        FreeLibrary(proxyMod);
        proxyMod = LoadLibraryW(dllPath.c_str());

        hr = createDev(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                       &device, &fl, &ctx);
        if (SUCCEEDED(hr) && device) {
            hr = factory->CreateSwapChain(device, &scDesc, &swapChain);
        }
        factory->Release();
    } else {
        // Mirror LOTRO startup: create DXGIFactory first, run single-threaded swapchainless probe,
        // release probe device and call FreeLibrary(d3d11.dll), then continue using factory + d3d11.
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        HMODULE sysDxgi = LoadLibraryW((fs::path(sysDir) / L"dxgi.dll").c_str());
        auto sysCreateFactory1 = sysDxgi ? reinterpret_cast<PfnCreateDXGIFactory1>(
            GetProcAddress(sysDxgi, "CreateDXGIFactory1")) : nullptr;
        IDXGIFactory1* persistentFactory = nullptr;
        if (sysCreateFactory1) {
            sysCreateFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&persistentFactory));
        }

        IDXGIAdapter* probeAdapter = nullptr;
        if (persistentFactory) {
            persistentFactory->EnumAdapters(0, &probeAdapter);
        }
        ID3D11Device* probeDevice = nullptr;
        D3D_FEATURE_LEVEL probeFl{};
        hr = createDevSwap(
            probeAdapter,
            probeAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_SINGLETHREADED,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            nullptr,
            nullptr,
            &probeDevice,
            &probeFl,
            nullptr
        );
        if (probeAdapter) probeAdapter->Release();
        if (probeDevice) probeDevice->Release();
        if (FAILED(hr)) {
            std::fprintf(stderr, "Probe D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", static_cast<unsigned>(hr));
            if (persistentFactory) persistentFactory->Release();
            if (sysDxgi) FreeLibrary(sysDxgi);
            DestroyWindow(hwnd);
            return 7;
        }

        FreeLibrary(proxyMod);
        proxyMod = LoadLibraryW(dllPath.c_str());
        createDevSwap = proxyMod ? reinterpret_cast<PfnD3D11CreateDeviceAndSwapChain>(
            GetProcAddress(proxyMod, "D3D11CreateDeviceAndSwapChain")) : nullptr;

        hr = createDevSwap
            ? createDevSwap(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                            nullptr, nullptr, &device, &fl, &ctx)
            : E_FAIL;
        if (SUCCEEDED(hr) && device && persistentFactory) {
            hr = persistentFactory->CreateSwapChain(device, &scDesc, &swapChain);
        } else if (SUCCEEDED(hr) && createDevSwap) {
            if (ctx) { ctx->Release(); ctx = nullptr; }
            if (device) { device->Release(); device = nullptr; }
            hr = createDevSwap(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                               &scDesc, &swapChain, &device, &fl, &ctx);
        }
        if (persistentFactory) persistentFactory->Release();
        if (sysDxgi) FreeLibrary(sysDxgi);
    }
    if (FAILED(hr) || !swapChain || !device || !ctx) {
        std::fprintf(stderr, "SwapChain/Device creation failed: 0x%08X\n", static_cast<unsigned>(hr));
        DestroyWindow(hwnd);
        return 7;
    }

    ID3D11Texture2D* bbTex = nullptr;
    ID3D11RenderTargetView* bbRtv = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bbTex));
    device->CreateRenderTargetView(bbTex, nullptr, &bbRtv);
    bbTex->Release();

    // Compile VS, PS, and CS
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* csBlob = nullptr;
    compileFn(kTestVs, sizeof(kTestVs) - 1, "vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, nullptr);
    compileFn(kTestPs, sizeof(kTestPs) - 1, "ps", nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, nullptr);
    compileFn(kTestCs, sizeof(kTestCs) - 1, "cs", nullptr, nullptr, "main", "cs_4_0", 0, 0, &csBlob, nullptr);

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    if (csBlob) {
        device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &cs);
        csBlob->Release();
    }
    vsBlob->Release();
    psBlob->Release();

    ID3D11VertexShader* lotroUiVs = nullptr;
    ID3D11VertexShader* lotroUiHalfVs = nullptr;
    ID3D11PixelShader* lotroUiPs = nullptr;
    ID3D11PixelShader* lotroUiBasePs = nullptr;
    device->CreateVertexShader(lotroUiVsBlob->GetBufferPointer(), lotroUiVsBlob->GetBufferSize(), nullptr, &lotroUiVs);
    device->CreateVertexShader(lotroUiHalfVsBlob->GetBufferPointer(), lotroUiHalfVsBlob->GetBufferSize(), nullptr, &lotroUiHalfVs);
    device->CreatePixelShader(lotroUiPsBlob->GetBufferPointer(), lotroUiPsBlob->GetBufferSize(), nullptr, &lotroUiPs);
    device->CreatePixelShader(lotroUiBasePsBlob->GetBufferPointer(), lotroUiBasePsBlob->GetBufferSize(), nullptr, &lotroUiBasePs);
    lotroUiVsBlob->Release();
    lotroUiHalfVsBlob->Release();
    lotroUiPsBlob->Release();
    lotroUiBasePsBlob->Release();

    // 16x16 crisp step-edge UI texture, smooth AA glyph ramp texture, and alpha-discard test texture
    std::vector<uint32_t> uiStepPixels(16 * 16);
    std::vector<uint32_t> uiAaPixels(16 * 16);
    std::vector<uint32_t> uiAlphaPixels(16 * 16);
    for (UINT y = 0; y < 16; ++y) {
        for (UINT x = 0; x < 16; ++x) {
            const uint8_t lum = (x < 8) ? 20 : 240;
            uiStepPixels[y * 16 + x] = (0xFFu << 24) | (uint32_t(lum) << 16) | (uint32_t(lum) << 8) | uint32_t(lum);

            static constexpr uint8_t kAaRamp[16] = {20, 20, 20, 20, 20, 48, 95, 145, 190, 224, 240, 240, 240, 240, 240, 240};
            const uint8_t aaLum = kAaRamp[x];
            uiAaPixels[y * 16 + x] = (0xFFu << 24) | (uint32_t(aaLum) << 16) | (uint32_t(aaLum) << 8) | uint32_t(aaLum);

            const uint8_t a = (x < 8) ? 50 : 240;
            uiAlphaPixels[y * 16 + x] = (uint32_t(a) << 24) | 0x00FFFFFFu;
        }
    }
    D3D11_TEXTURE2D_DESC uiTexDesc{};
    uiTexDesc.Width = 16;
    uiTexDesc.Height = 16;
    uiTexDesc.MipLevels = 1;
    uiTexDesc.ArraySize = 1;
    uiTexDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uiTexDesc.SampleDesc.Count = 1;
    uiTexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    uiTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA uiStepInit{uiStepPixels.data(), 16 * 4, 0};
    D3D11_SUBRESOURCE_DATA uiAaInit{uiAaPixels.data(), 16 * 4, 0};
    D3D11_SUBRESOURCE_DATA uiAlphaInit{uiAlphaPixels.data(), 16 * 4, 0};
    ID3D11Texture2D* uiStepTex = nullptr;
    ID3D11ShaderResourceView* uiStepSrv = nullptr;
    ID3D11Texture2D* uiAaTex = nullptr;
    ID3D11ShaderResourceView* uiAaSrv = nullptr;
    ID3D11Texture2D* uiAlphaTex = nullptr;
    ID3D11ShaderResourceView* uiAlphaSrv = nullptr;
    device->CreateTexture2D(&uiTexDesc, &uiStepInit, &uiStepTex);
    device->CreateShaderResourceView(uiStepTex, nullptr, &uiStepSrv);
    device->CreateTexture2D(&uiTexDesc, &uiAaInit, &uiAaTex);
    device->CreateShaderResourceView(uiAaTex, nullptr, &uiAaSrv);
    device->CreateTexture2D(&uiTexDesc, &uiAlphaInit, &uiAlphaTex);
    device->CreateShaderResourceView(uiAlphaTex, nullptr, &uiAlphaSrv);

    D3D11_BUFFER_DESC uiCbDesc{};
    uiCbDesc.ByteWidth = 192; // cb0[0..11]
    uiCbDesc.Usage = D3D11_USAGE_DEFAULT;
    uiCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* uiCb = nullptr;
    device->CreateBuffer(&uiCbDesc, nullptr, &uiCb);

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = 640;
    stagingDesc.Height = 360;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* bbStaging = nullptr;
    device->CreateTexture2D(&stagingDesc, nullptr, &bbStaging);

    ID3D11Texture2D* bbPreWorldFxStaging = nullptr;
    device->CreateTexture2D(&stagingDesc, nullptr, &bbPreWorldFxStaging);

    // WRAP + full mip sampler to verify [Samplers] 16x Aniso + MipLODBias = -0.35 upgrade
    ID3D11SamplerState* worldSampler = nullptr;
    {
        D3D11_SAMPLER_DESC wsd{};
        wsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        wsd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        wsd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        wsd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        wsd.MaxAnisotropy = 1;
        wsd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        wsd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&wsd, &worldSampler);
    }

    // sRGB 16x16 UI texture (CNN sRGB round-trip check)
    ID3D11Texture2D* uiSrgbTex = nullptr;
    ID3D11ShaderResourceView* uiSrgbSrv = nullptr;
    {
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        device->CreateTexture2D(&d, &uiStepInit, &uiSrgbTex);
        if (uiSrgbTex) device->CreateShaderResourceView(uiSrgbTex, nullptr, &uiSrgbSrv);
    }

    // BC3_UNORM 16x16 compressed UI texture (4x4 blocks -> 16 blocks, 256 bytes total)
    ID3D11Texture2D* uiBc3Tex = nullptr;
    ID3D11ShaderResourceView* uiBc3Srv = nullptr;
    {
        std::vector<uint8_t> bc3Data(16 * 16, 0);
        for (UINT by = 0; by < 4; ++by) {
            for (UINT bx = 0; bx < 4; ++bx) {
                uint8_t* blk = bc3Data.data() + (by * 4 + bx) * 16;
                blk[0] = 0xFF;
                blk[1] = 0xFF; // opaque alpha = 255 for all 16 pixels
                const uint16_t c = (bx < 2) ? 0x18E3u : 0xEF7Du;
                blk[8] = static_cast<uint8_t>(c & 0xFFu);
                blk[9] = static_cast<uint8_t>((c >> 8) & 0xFFu);
                blk[10] = blk[8];
                blk[11] = blk[9];
            }
        }
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Format = DXGI_FORMAT_BC3_UNORM;
        D3D11_SUBRESOURCE_DATA init{ bc3Data.data(), 4 * 16, 0 };
        device->CreateTexture2D(&d, &init, &uiBc3Tex);
        if (uiBc3Tex) device->CreateShaderResourceView(uiBc3Tex, nullptr, &uiBc3Srv);
    }

    // Dynamic DEFAULT texture for UpdateSubresource dirty invalidation test
    ID3D11Texture2D* uiDynTex = nullptr;
    ID3D11ShaderResourceView* uiDynSrv = nullptr;
    {
        std::vector<uint32_t> dynInit(16 * 16, 0xFFC8C8C8u); // neutral grey (200,200,200)
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA init{ dynInit.data(), 16 * 4, 0 };
        device->CreateTexture2D(&d, &init, &uiDynTex);
        if (uiDynTex) device->CreateShaderResourceView(uiDynTex, nullptr, &uiDynSrv);
    }

    // DYNAMIC texture for Map/Unmap dirty invalidation test
    ID3D11Texture2D* uiMapDynTex = nullptr;
    ID3D11ShaderResourceView* uiMapDynSrv = nullptr;
    {
        std::vector<uint32_t> mapInit(16 * 16, 0xFFDC281Eu); // ABGR -> R=30, G=40, B=220 (blue)
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA init{ mapInit.data(), 16 * 4, 0 };
        device->CreateTexture2D(&d, &init, &uiMapDynTex);
        if (uiMapDynTex) device->CreateShaderResourceView(uiMapDynTex, nullptr, &uiMapDynSrv);
    }

    // 20x12 badge texture for [ElementScale] Geometry Shader scaling test
    ID3D11Texture2D* uiScaleTex = nullptr;
    ID3D11ShaderResourceView* uiScaleSrv = nullptr;
    {
        std::vector<uint32_t> scaleInit(20 * 12, 0xFFDC1EE6u); // ABGR -> R=230, G=30, B=220 (magenta)
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Width = 20;
        d.Height = 12;
        D3D11_SUBRESOURCE_DATA init{ scaleInit.data(), 20 * 4, 0 };
        device->CreateTexture2D(&d, &init, &uiScaleTex);
        if (uiScaleTex) device->CreateShaderResourceView(uiScaleTex, nullptr, &uiScaleSrv);
    }

    // Offscreen 16x16 UI RenderTarget texture for UpscaleUiRenderTargets CNN test
    ID3D11Texture2D* uiOffRtTex = nullptr;
    ID3D11RenderTargetView* uiOffRtRtv = nullptr;
    ID3D11ShaderResourceView* uiOffRtSrv = nullptr;
    {
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&d, &uiStepInit, &uiOffRtTex);
        if (uiOffRtTex) {
            device->CreateRenderTargetView(uiOffRtTex, nullptr, &uiOffRtRtv);
            device->CreateShaderResourceView(uiOffRtTex, nullptr, &uiOffRtSrv);
        }
    }

    // 3D test texture (64x64 checker + warm foliage/earth gradient) for WorldPostFX test
    ID3D11Texture2D* world3dTex = nullptr;
    ID3D11ShaderResourceView* world3dSrv = nullptr;
    {
        std::vector<uint32_t> w3d(64 * 64);
        for (UINT y = 0; y < 64; ++y) {
            for (UINT x = 0; x < 64; ++x) {
                if (y < 32) {
                    const uint8_t l = ((x / 4 + y / 4) & 1) ? 145 : 70;
                    w3d[y * 64 + x] = (0xFFu << 24) | (uint32_t(l) << 16) | (uint32_t(l) << 8) | uint32_t(l);
                } else {
                    const uint8_t r = 155;
                    const uint8_t g = static_cast<uint8_t>(85 + (x / 4));
                    const uint8_t b = 50;
                    w3d[y * 64 + x] = (0xFFu << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
                }
            }
        }
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Width = 64;
        d.Height = 64;
        D3D11_SUBRESOURCE_DATA init{ w3d.data(), 64 * 4, 0 };
        device->CreateTexture2D(&d, &init, &world3dTex);
        if (world3dTex) device->CreateShaderResourceView(world3dTex, nullptr, &world3dSrv);
    }

    // 896x576 noisy parchment map + dark ink stroke for MapMode verification
    ID3D11Texture2D* parchmentTex = nullptr;
    ID3D11ShaderResourceView* parchmentSrv = nullptr;
    ID3D11Texture2D* mapOutTex = nullptr;
    ID3D11RenderTargetView* mapOutRtv = nullptr;
    ID3D11Texture2D* mapOutStagingEnh = nullptr;
    ID3D11Texture2D* mapOutStagingBase = nullptr;
    {
        std::vector<uint32_t> mapPx(896u * 576u);
        for (UINT y = 0; y < 576; ++y) {
            for (UINT x = 0; x < 896; ++x) {
                if (x >= 440 && x < 456) {
                    mapPx[y * 896 + x] = 0xFF16202Au; // dark ink stroke R=42, G=32, B=22
                } else {
                    const uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ 0x9E3779B9u;
                    const int n = static_cast<int>(h % 29u) - 14; // [-14, +14] paper grain
                    const uint8_t r = static_cast<uint8_t>(std::max(0, std::min(255, 198 + n)));
                    const uint8_t g = static_cast<uint8_t>(std::max(0, std::min(255, 178 + n)));
                    const uint8_t b = static_cast<uint8_t>(std::max(0, std::min(255, 138 + n)));
                    mapPx[y * 896 + x] = (0xFFu << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
                }
            }
        }
        D3D11_TEXTURE2D_DESC d = uiTexDesc;
        d.Width = 896;
        d.Height = 576;
        D3D11_SUBRESOURCE_DATA init{ mapPx.data(), 896 * 4, 0 };
        device->CreateTexture2D(&d, &init, &parchmentTex);
        if (parchmentTex) device->CreateShaderResourceView(parchmentTex, nullptr, &parchmentSrv);

        D3D11_TEXTURE2D_DESC md{};
        md.Width = 1920;
        md.Height = 1080;
        md.MipLevels = 1;
        md.ArraySize = 1;
        md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        md.SampleDesc.Count = 1;
        md.Usage = D3D11_USAGE_DEFAULT;
        md.BindFlags = D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&md, nullptr, &mapOutTex);
        if (mapOutTex) device->CreateRenderTargetView(mapOutTex, nullptr, &mapOutRtv);

        md.Usage = D3D11_USAGE_STAGING;
        md.BindFlags = 0;
        md.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&md, nullptr, &mapOutStagingEnh);
        device->CreateTexture2D(&md, nullptr, &mapOutStagingBase);
    }

    // Create secondary small SwapChain (320x240) on a secondary HWND to test secondary Present filtering.
    HWND hwndSec = CreateWindowExW(
        0, wc.lpszClassName, L"SmokeTestSecondary", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, wc.lpszClassName ? wc.hInstance : nullptr, nullptr
    );
    IDXGISwapChain* secondarySwapChain = nullptr;
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (hwndSec && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
                IDXGIFactory* fac = nullptr;
                if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&fac))) && fac) {
                    DXGI_SWAP_CHAIN_DESC secDesc = scDesc;
                    secDesc.BufferDesc.Width = 320;
                    secDesc.BufferDesc.Height = 240;
                    secDesc.OutputWindow = hwndSec;
                    fac->CreateSwapChain(device, &secDesc, &secondarySwapChain);
                    fac->Release();
                }
                adapter->Release();
            }
            dxgiDev->Release();
        }
    }

    // Create DSV (640x360), indirect draw args buffer, copy/resolve textures
    ID3D11Texture2D* dsvTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    {
        D3D11_TEXTURE2D_DESC dsvDesc{};
        dsvDesc.Width = 640;
        dsvDesc.Height = 360;
        dsvDesc.MipLevels = 1;
        dsvDesc.ArraySize = 1;
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.SampleDesc.Count = 1;
        dsvDesc.Usage = D3D11_USAGE_DEFAULT;
        dsvDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(device->CreateTexture2D(&dsvDesc, nullptr, &dsvTex)) && dsvTex) {
            device->CreateDepthStencilView(dsvTex, nullptr, &dsv);
        }
    }

    ID3D11Buffer* indirectArgsBuf = nullptr;
    {
        const UINT args[4] = {3, 1, 0, 0};
        D3D11_BUFFER_DESC bDesc{};
        bDesc.ByteWidth = sizeof(args);
        bDesc.Usage = D3D11_USAGE_DEFAULT;
        bDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        D3D11_SUBRESOURCE_DATA init{args, sizeof(args), 0};
        hr = device->CreateBuffer(&bDesc, &init, &indirectArgsBuf);
        if (FAILED(hr) || !indirectArgsBuf) {
            std::fprintf(stderr, "CreateBuffer indirectArgs failed: 0x%08X\n", static_cast<unsigned>(hr));
            return 12;
        }
    }

    ID3D11Texture2D* copyDstTex = nullptr;
    ID3D11Texture2D* copySrcTex = nullptr;
    ID3D11Texture2D* resolveMsaaTex = nullptr;
    ID3D11Texture2D* resolveDstTex = nullptr;
    {
        D3D11_TEXTURE2D_DESC cDesc{};
        cDesc.Width = 32;
        cDesc.Height = 32;
        cDesc.MipLevels = 1;
        cDesc.ArraySize = 1;
        cDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cDesc.SampleDesc.Count = 1;
        cDesc.Usage = D3D11_USAGE_DEFAULT;
        cDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&cDesc, nullptr, &copySrcTex);
        device->CreateTexture2D(&cDesc, nullptr, &copyDstTex);
        device->CreateTexture2D(&cDesc, nullptr, &resolveDstTex);
        cDesc.SampleDesc.Count = 2;
        cDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&cDesc, nullptr, &resolveMsaaTex);
    }

    // 1. Static BC1 "Map" texture (512x512)
    std::vector<uint8_t> bc1Data((512 / 4) * (512 / 4) * 8, 0xFF);
    D3D11_TEXTURE2D_DESC mapDesc{};
    mapDesc.Width = 512;
    mapDesc.Height = 512;
    mapDesc.MipLevels = 1;
    mapDesc.ArraySize = 1;
    mapDesc.Format = DXGI_FORMAT_BC1_UNORM;
    mapDesc.SampleDesc.Count = 1;
    mapDesc.Usage = D3D11_USAGE_IMMUTABLE;
    mapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA mapInit{bc1Data.data(), (512 / 4) * 8, 0};
    ID3D11Texture2D* mapTex = nullptr;
    ID3D11ShaderResourceView* mapSrv = nullptr;
    device->CreateTexture2D(&mapDesc, &mapInit, &mapTex);
    device->CreateShaderResourceView(mapTex, nullptr, &mapSrv);

    // 1b. Tight guard-page-adjacent 32x32 BC3_UNORM (1024 bytes) & 16x1 R8_UNORM (16 bytes)
    //     to prove HashInitialSubresource never reads past tight initial allocations.
    auto* guardRegion = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    );
    if (!guardRegion) {
        std::fprintf(stderr, "VirtualAlloc failed\n");
        return 11;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(guardRegion + 4096, 4096, PAGE_NOACCESS, &oldProt)) {
        std::fprintf(stderr, "VirtualProtect guard page failed\n");
        VirtualFree(guardRegion, 0, MEM_RELEASE);
        return 11;
    }

    uint8_t* tightBc3Ptr = guardRegion + 4096 - 1024;
    std::memset(tightBc3Ptr, 0xAA, 1024);
    D3D11_TEXTURE2D_DESC bc3Desc{};
    bc3Desc.Width = 32;
    bc3Desc.Height = 32;
    bc3Desc.MipLevels = 1;
    bc3Desc.ArraySize = 1;
    bc3Desc.Format = DXGI_FORMAT_BC3_UNORM;
    bc3Desc.SampleDesc.Count = 1;
    bc3Desc.Usage = D3D11_USAGE_IMMUTABLE;
    bc3Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA bc3Init{tightBc3Ptr, (32 / 4) * 16, 0};
    ID3D11Texture2D* bc3Tex = nullptr;
    ID3D11ShaderResourceView* bc3Srv = nullptr;
    hr = device->CreateTexture2D(&bc3Desc, &bc3Init, &bc3Tex);
    if (FAILED(hr) || !bc3Tex) {
        std::fprintf(stderr, "Small BC3 CreateTexture2D failed: 0x%08X\n", static_cast<unsigned>(hr));
        VirtualFree(guardRegion, 0, MEM_RELEASE);
        return 12;
    }
    device->CreateShaderResourceView(bc3Tex, nullptr, &bc3Srv);

    uint8_t* tightOneRowPtr = guardRegion + 4096 - 16;
    std::memset(tightOneRowPtr, 0x55, 16);
    D3D11_TEXTURE2D_DESC oneRowDesc{};
    oneRowDesc.Width = 16;
    oneRowDesc.Height = 1;
    oneRowDesc.MipLevels = 1;
    oneRowDesc.ArraySize = 1;
    oneRowDesc.Format = DXGI_FORMAT_R8_UNORM;
    oneRowDesc.SampleDesc.Count = 1;
    oneRowDesc.Usage = D3D11_USAGE_IMMUTABLE;
    oneRowDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA oneRowInit{tightOneRowPtr, 64, 0};
    ID3D11Texture2D* oneRowTex = nullptr;
    ID3D11ShaderResourceView* oneRowSrv = nullptr;
    hr = device->CreateTexture2D(&oneRowDesc, &oneRowInit, &oneRowTex);
    if (FAILED(hr) || !oneRowTex) {
        std::fprintf(stderr, "16x1 R8 CreateTexture2D failed: 0x%08X\n", static_cast<unsigned>(hr));
        VirtualFree(guardRegion, 0, MEM_RELEASE);
        return 12;
    }
    device->CreateShaderResourceView(oneRowTex, nullptr, &oneRowSrv);

    // 2. Dynamic R8 "Font Atlas" texture (256x256)
    D3D11_TEXTURE2D_DESC fontDesc{};
    fontDesc.Width = 256;
    fontDesc.Height = 256;
    fontDesc.MipLevels = 1;
    fontDesc.ArraySize = 1;
    fontDesc.Format = DXGI_FORMAT_R8_UNORM;
    fontDesc.SampleDesc.Count = 1;
    fontDesc.Usage = D3D11_USAGE_DYNAMIC;
    fontDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    fontDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* fontTex = nullptr;
    ID3D11ShaderResourceView* fontSrv = nullptr;
    device->CreateTexture2D(&fontDesc, nullptr, &fontTex);
    device->CreateShaderResourceView(fontTex, nullptr, &fontSrv);

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState* sampler = nullptr;
    device->CreateSamplerState(&sampDesc, &sampler);

    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ID3D11DepthStencilState* dsState = nullptr;
    device->CreateDepthStencilState(&dsDesc, &dsState);

    D3D11_DEPTH_STENCIL_DESC dsRoDesc{};
    dsRoDesc.DepthEnable = TRUE;
    dsRoDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsRoDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState* dsReadOnlyState = nullptr;
    device->CreateDepthStencilState(&dsRoDesc, &dsReadOnlyState);

    D3D11_DEPTH_STENCIL_DESC dsWrDesc{};
    dsWrDesc.DepthEnable = TRUE;
    dsWrDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsWrDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState* dsWriteState = nullptr;
    device->CreateDepthStencilState(&dsWrDesc, &dsWriteState);

    D3D11_BLEND_DESC blDesc{};
    blDesc.RenderTarget[0].BlendEnable = TRUE;
    blDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState* blState = nullptr;
    device->CreateBlendState(&blDesc, &blState);
    // D3D11 deduplicates identical state objects; verify re-tracking preserves live entry.
    ID3D11BlendState* blStateDup = nullptr;
    device->CreateBlendState(&blDesc, &blStateDup);

    // Render 3 frames (AutoCaptureFrame=1 captures frame 1 cleanly)
    for (int f = 0; f < 3; ++f) {
        const float clearCol[4] = {0.1f, 0.2f, 0.3f, 1.0f};
        ctx->ClearRenderTargetView(bbRtv, clearCol);
        if (dsv) {
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        }

        D3D11_VIEWPORT vp{0, 0, 640, 360, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &bbRtv, dsv);
        ctx->OMSetDepthStencilState(dsState, 0);
        const float bf[4] = {1, 1, 1, 1};
        ctx->OMSetBlendState(blState, bf, 0xFFFFFFFF);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetSamplers(0, 1, worldSampler ? &worldSampler : &sampler);

        // Draw 1: Map quad
        ctx->PSSetShaderResources(0, 1, &mapSrv);
        ctx->Draw(4, 0);

        // Draw 1b: Small BC3 & 1-row textures
        ctx->PSSetShaderResources(0, 1, &bc3Srv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &oneRowSrv);
        ctx->Draw(3, 0);

        // Secondary SwapChain Present mid-frame (must NOT truncate main frame capture after frame 0!).
        if (secondarySwapChain && f >= 1) {
            secondarySwapChain->Present(0, 0);
        }

        if (copyDstTex && copySrcTex) {
            ctx->CopyResource(copyDstTex, copySrcTex);
        }
        if (resolveDstTex && resolveMsaaTex) {
            ctx->ResolveSubresource(resolveDstTex, 0, resolveMsaaTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        if (cs) {
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &mapSrv);
            ctx->Dispatch(1, 1, 1);
            ctx->CSSetShader(nullptr, nullptr, 0);
        }
        if (indirectArgsBuf) {
            ctx->DrawInstancedIndirect(indirectArgsBuf, 0);
        }

        // Update dynamic font atlas & Draw 2: Font quad
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(fontTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
            for (UINT r = 0; r < 256; ++r) {
                std::memset(static_cast<uint8_t*>(mapped.pData) + r * mapped.RowPitch, (r % 32 < 16) ? 255 : 0, 256);
            }
            ctx->Unmap(fontTex, 0);
        }
        ctx->PSSetShaderResources(0, 1, &fontSrv);
        ctx->Draw(6, 0);

        // Warm dynamic CNN entries on frame 1 before mutating them in frame 2
        if (f == 1 && !probeAaMode && lotroUiVs && lotroUiPs && uiCb && uiDynSrv && uiMapDynSrv) {
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->VSSetShader(lotroUiVs, nullptr, 0);
            ctx->PSSetShader(lotroUiPs, nullptr, 0);
            ctx->PSSetSamplers(0, 1, &sampler);
            ctx->PSSetConstantBuffers(0, 1, &uiCb);
            float cbInit[48]{};
            cbInit[36] = 1.0f; cbInit[37] = 1.0f; cbInit[38] = 1.0f; cbInit[39] = 1.0f;
            ctx->UpdateSubresource(uiCb, 0, nullptr, cbInit, 0, 0);

            D3D11_VIEWPORT vpDyn{ 550, 280, 40, 40, 0, 1 };
            ctx->RSSetViewports(1, &vpDyn);
            ctx->PSSetShaderResources(0, 1, &uiDynSrv);
            ctx->Draw(4, 0);

            D3D11_VIEWPORT vpMapDyn{ 594, 280, 40, 40, 0, 1 };
            ctx->RSSetViewports(1, &vpMapDyn);
            ctx->PSSetShaderResources(0, 1, &uiMapDynSrv);
            ctx->Draw(4, 0);
        }

        // Draw 3..5 (UI Enhancer v0.2 A/B & Alpha-Test verification on final frame cleanly)
        if (f == 2 && lotroUiVs && lotroUiPs && lotroUiBasePs && uiCb && uiStepSrv && uiAlphaSrv) {
            ctx->ClearRenderTargetView(bbRtv, clearCol);

            if (!probeAaMode && uiDynTex && uiMapDynTex && world3dSrv) {
                // Trigger UpdateSubresource dirty invalidation on uiDynTex -> right half bright green
                std::vector<uint32_t> dynUpd(16 * 16);
                for (UINT y = 0; y < 16; ++y) {
                    for (UINT x = 0; x < 16; ++x) {
                        dynUpd[y * 16 + x] = (x < 8) ? 0xFF141414u : 0xFF32E123u; // (35, 225, 50) right
                    }
                }
                ctx->UpdateSubresource(uiDynTex, 0, nullptr, dynUpd.data(), 16 * 4, 0);

                // Trigger Map(WRITE_DISCARD) + Unmap dirty invalidation on uiMapDynTex -> orange (240,140,30)
                D3D11_MAPPED_SUBRESOURCE mdDyn{};
                if (SUCCEEDED(ctx->Map(uiMapDynTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mdDyn)) && mdDyn.pData) {
                    for (UINT r = 0; r < 16; ++r) {
                        auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mdDyn.pData) + r * mdDyn.RowPitch);
                        for (UINT c = 0; c < 16; ++c) row[c] = 0xFF1E8CF0u; // ABGR -> R=240, G=140, B=30
                    }
                    ctx->Unmap(uiMapDynTex, 0);
                }

                // Draw non-UI 3D scene quad BEFORE first UI draw, then capture bbPreWorldFxStaging
                D3D11_VIEWPORT vpWorld{ 320, 140, 128, 128, 0, 1 };
                ctx->RSSetViewports(1, &vpWorld);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                ctx->VSSetShader(vs, nullptr, 0);
                ctx->PSSetShader(ps, nullptr, 0);
                ctx->PSSetSamplers(0, 1, worldSampler ? &worldSampler : &sampler);
                ctx->PSSetShaderResources(0, 1, &world3dSrv);
                ctx->Draw(4, 0);

                if (bbPreWorldFxStaging) {
                    ID3D11Texture2D* curBb = nullptr;
                    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&curBb))) && curBb) {
                        ctx->CopyResource(bbPreWorldFxStaging, curBb);
                        curBb->Release();
                    }
                }
            }

            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->VSSetShader(lotroUiVs, nullptr, 0);
            ctx->PSSetSamplers(0, 1, &sampler);
            ctx->PSSetConstantBuffers(0, 1, &uiCb);

            float cbData[48]{};
            // cb0[9] = c_MaterialDiffuseColor = (1, 1, 1, 1); cb0[10].x = c_AlphaThresh = 0.0
            cbData[36] = 1.0f; cbData[37] = 1.0f; cbData[38] = 1.0f; cbData[39] = 1.0f;
            cbData[40] = 0.0f;
            ctx->UpdateSubresource(uiCb, 0, nullptr, cbData, 0, 0);

            // Region 0 (x=0..128): UIE Enhanced
            D3D11_VIEWPORT vpEnh{0, 0, 128, 128, 0, 1};
            ctx->RSSetViewports(1, &vpEnh);
            ctx->PSSetShader(lotroUiPs, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &uiStepSrv);
            ctx->Draw(4, 0);

            // Region 1 (x=160..288): Vanilla Bilinear baseline (non-matching hash)
            D3D11_VIEWPORT vpBase{160, 0, 128, 128, 0, 1};
            ctx->RSSetViewports(1, &vpBase);
            ctx->PSSetShader(lotroUiBasePs, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &uiStepSrv);
            ctx->Draw(4, 0);

            // Region 2 (x=320..448, y=0..128): UIE Enhanced Alpha Discard + Diffuse Tint
            cbData[36] = 1.0f; cbData[37] = 0.5f; cbData[38] = 0.25f; cbData[39] = 1.0f;
            cbData[40] = 0.60f;
            ctx->UpdateSubresource(uiCb, 0, nullptr, cbData, 0, 0);
            D3D11_VIEWPORT vpAlpha{320, 0, 128, 128, 0, 1};
            ctx->RSSetViewports(1, &vpAlpha);
            ctx->PSSetShader(lotroUiPs, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &uiAlphaSrv);
            ctx->Draw(4, 0);

            // Restore neutral cb0[9]/alphaThresh for Regions 3..6
            cbData[36] = 1.0f; cbData[37] = 1.0f; cbData[38] = 1.0f; cbData[39] = 1.0f;
            cbData[40] = 0.0f;
            ctx->UpdateSubresource(uiCb, 0, nullptr, cbData, 0, 0);
            ctx->PSSetShaderResources(0, 1, &uiStepSrv);

            // Region 3 (x=480..608, y=0..128): Read-only depth floating nameplate (DepthEnable=1, Write=0 -> MUST enhance)
            if (dsReadOnlyState) ctx->OMSetDepthStencilState(dsReadOnlyState, 0);
            D3D11_VIEWPORT vpNameplate{480, 0, 128, 128, 0, 1};
            ctx->RSSetViewports(1, &vpNameplate);
            ctx->PSSetShader(lotroUiPs, nullptr, 0);
            ctx->Draw(4, 0);

            // Region 4 (x=480..608, y=140..268): Depth-writing 3D draw (DepthEnable=1, Write=ALL -> MUST stay baseline)
            if (dsWriteState) ctx->OMSetDepthStencilState(dsWriteState, 0);
            D3D11_VIEWPORT vpDepthWrite{480, 140, 128, 128, 0, 1};
            ctx->RSSetViewports(1, &vpDepthWrite);
            ctx->PSSetShader(lotroUiPs, nullptr, 0);
            ctx->Draw(4, 0);

            ctx->OMSetDepthStencilState(dsState, 0);
            if (uiAaSrv) {
                // Region 5 (x=0..128, y=140..268): AA glyph ramp UIE Enhanced
                D3D11_VIEWPORT vpAaEnh{0, 140, 128, 128, 0, 1};
                ctx->RSSetViewports(1, &vpAaEnh);
                ctx->PSSetShader(lotroUiPs, nullptr, 0);
                ctx->PSSetShaderResources(0, 1, &uiAaSrv);
                ctx->Draw(4, 0);

                // Region 6 (x=160..288, y=140..268): AA glyph ramp Vanilla Bilinear baseline
                D3D11_VIEWPORT vpAaBase{160, 140, 128, 128, 0, 1};
                ctx->RSSetViewports(1, &vpAaBase);
                ctx->PSSetShader(lotroUiBasePs, nullptr, 0);
                ctx->Draw(4, 0);
            }

            if (!probeAaMode) {
                ctx->OMSetDepthStencilState(dsState, 0);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                ctx->VSSetShader(lotroUiVs, nullptr, 0);
                ctx->PSSetSamplers(0, 1, &sampler);
                ctx->PSSetConstantBuffers(0, 1, &uiCb);
                ctx->UpdateSubresource(uiCb, 0, nullptr, cbData, 0, 0);

                // Region 7a/7b (x=320..384 / 400..464, y=280..344): sRGB 16x16 CNN vs Baseline
                if (uiSrgbSrv) {
                    D3D11_VIEWPORT vpSrgbEnh{ 320, 280, 64, 64, 0, 1 };
                    ctx->RSSetViewports(1, &vpSrgbEnh);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &uiSrgbSrv);
                    ctx->Draw(4, 0);

                    D3D11_VIEWPORT vpSrgbBase{ 400, 280, 64, 64, 0, 1 };
                    ctx->RSSetViewports(1, &vpSrgbBase);
                    ctx->PSSetShader(lotroUiBasePs, nullptr, 0);
                    ctx->Draw(4, 0);
                }

                // Region 8 (x=480..544, y=280..344): BC3_UNORM 16x16 CNN Enhanced
                if (uiBc3Srv) {
                    D3D11_VIEWPORT vpBc3{ 480, 280, 64, 64, 0, 1 };
                    ctx->RSSetViewports(1, &vpBc3);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &uiBc3Srv);
                    ctx->Draw(4, 0);
                }

                // Region 9 (x=550..590, y=280..320): UpdateSubresource invalidated CNN texture -> green right half
                if (uiDynSrv) {
                    D3D11_VIEWPORT vpDyn{ 550, 280, 40, 40, 0, 1 };
                    ctx->RSSetViewports(1, &vpDyn);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &uiDynSrv);
                    ctx->Draw(4, 0);
                }

                // Region 10 (x=594..634, y=280..320): Map/Unmap invalidated CNN texture -> orange
                if (uiMapDynSrv) {
                    D3D11_VIEWPORT vpMapDyn{ 594, 280, 40, 40, 0, 1 };
                    ctx->RSSetViewports(1, &vpMapDyn);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &uiMapDynSrv);
                    ctx->Draw(4, 0);
                }

                // Region 11 (x=200..240, y=280..320): [ElementScale] 20x12 badge scaled +100% (2.0x) via Geometry Shader
                if (uiScaleSrv && lotroUiHalfVs) {
                    D3D11_VIEWPORT vpScale{ 200, 280, 40, 40, 0, 1 };
                    ctx->RSSetViewports(1, &vpScale);
                    ctx->VSSetShader(lotroUiHalfVs, nullptr, 0);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &uiScaleSrv);
                    ctx->Draw(4, 0);
                    ctx->VSSetShader(lotroUiVs, nullptr, 0);
                }

                // Region 12a/12b (x=40..104 / 120..184, y=280..344): Offscreen UI RenderTarget CNN Upscale vs Baseline
                if (uiOffRtSrv) {
                    D3D11_VIEWPORT vpOffEnh{ 40, 280, 64, 64, 0, 1 };
                    ctx->RSSetViewports(1, &vpOffEnh);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &uiOffRtSrv);
                    ctx->Draw(4, 0);

                    D3D11_VIEWPORT vpOffBase{ 120, 280, 64, 64, 0, 1 };
                    ctx->RSSetViewports(1, &vpOffBase);
                    ctx->PSSetShader(lotroUiBasePs, nullptr, 0);
                    ctx->Draw(4, 0);
                }

                // Offscreen 1920x1080 MapMode verification (~2.14x UI zoom on 896x576 noisy parchment map)
                if (parchmentSrv && mapOutRtv && mapOutTex && mapOutStagingEnh && mapOutStagingBase) {
                    ctx->OMSetRenderTargets(1, &mapOutRtv, nullptr);
                    D3D11_VIEWPORT vpMap{ 0, 0, 1920, 1080, 0, 1 };
                    ctx->RSSetViewports(1, &vpMap);
                    ctx->PSSetShaderResources(0, 1, &parchmentSrv);

                    ctx->ClearRenderTargetView(mapOutRtv, clearCol);
                    ctx->PSSetShader(lotroUiPs, nullptr, 0);
                    ctx->Draw(4, 0);
                    ctx->CopyResource(mapOutStagingEnh, mapOutTex);

                    ctx->ClearRenderTargetView(mapOutRtv, clearCol);
                    ctx->PSSetShader(lotroUiBasePs, nullptr, 0);
                    ctx->Draw(4, 0);
                    ctx->CopyResource(mapOutStagingBase, mapOutTex);

                    ctx->OMSetRenderTargets(1, &bbRtv, dsv);
                }
            }

            // Read back rendered UI test pixels BEFORE Present overlay runs
            if (bbStaging) {
                ID3D11Texture2D* curBb = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&curBb))) && curBb) {
                    ctx->CopyResource(bbStaging, curBb);
                    curBb->Release();
                }
            }
        }

        swapChain->Present(0, 0);
    }

    // Verify rendered pixel sharpness, nameplate depth filtering, alpha discard, color tint, and preset curve!
    if (!bbStaging) {
        std::fprintf(stderr, "Missing staging texture for pixel verification\n");
        return 15;
    }

    if (!probeAaMode && bbPreWorldFxStaging && mapOutStagingEnh && mapOutStagingBase) {
        D3D11_MAPPED_SUBRESOURCE mapCur{}, mapPre{};
        if (SUCCEEDED(ctx->Map(bbStaging, 0, D3D11_MAP_READ, 0, &mapCur)) &&
            SUCCEEDED(ctx->Map(bbPreWorldFxStaging, 0, D3D11_MAP_READ, 0, &mapPre))) {
            auto pxAt = [](const D3D11_MAPPED_SUBRESOURCE& m, UINT x, UINT y, int ch) -> int {
                const auto* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
                return row[x * 4 + ch];
            };

            // 1. WorldPostFX check on x=324..444, y=144..264 (non-UI 3D quad transformed right before Region 0)
            uint64_t worldFxDelta = 0;
            for (UINT y = 144; y < 264; y += 2) {
                for (UINT x = 324; x < 444; x += 2) {
                    for (int ch = 0; ch < 3; ++ch) {
                        worldFxDelta += static_cast<uint64_t>(std::abs(pxAt(mapCur, x, y, ch) - pxAt(mapPre, x, y, ch)));
                    }
                }
            }
            if (worldFxDelta < 300) {
                std::fprintf(stderr, "WorldPostFx delta too small on 3D quad: %llu\n",
                             static_cast<unsigned long long>(worldFxDelta));
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 22;
            }

            // 2. sRGB 16x16 CNN edge sharpness (Region 7a x=320..384 vs Region 7b x=400..464 at y=312)
            int srgbSlopeEnh = 0;
            int srgbSlopeBase = 0;
            for (UINT dx = 2; dx < 62; ++dx) {
                srgbSlopeEnh = (std::max)(srgbSlopeEnh, std::abs(pxAt(mapCur, 320 + dx + 1, 312, 0) - pxAt(mapCur, 320 + dx, 312, 0)));
                srgbSlopeBase = (std::max)(srgbSlopeBase, std::abs(pxAt(mapCur, 400 + dx + 1, 312, 0) - pxAt(mapCur, 400 + dx, 312, 0)));
            }
            if (srgbSlopeEnh <= srgbSlopeBase || srgbSlopeEnh < 60) {
                std::fprintf(stderr, "sRGB CNN check failed: enh=%d base=%d\n", srgbSlopeEnh, srgbSlopeBase);
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 23;
            }

            // 3. BC3 16x16 CNN step edge check (Region 8 x=480..544 at y=312)
            const int bc3Left = pxAt(mapCur, 488, 312, 0);
            const int bc3Right = pxAt(mapCur, 536, 312, 0);
            int bc3Slope = 0;
            for (UINT dx = 2; dx < 62; ++dx) {
                bc3Slope = (std::max)(bc3Slope, std::abs(pxAt(mapCur, 480 + dx + 1, 312, 0) - pxAt(mapCur, 480 + dx, 312, 0)));
            }
            if (bc3Left > 45 || bc3Right < 215 || bc3Slope < 60) {
                std::fprintf(stderr, "BC3 CNN check failed: left=%d right=%d slope=%d\n", bc3Left, bc3Right, bc3Slope);
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 24;
            }

            // 4. UpdateSubresource dirty invalidation check (Region 9 x=578, y=300 -> green (35,225,50))
            const int updR = pxAt(mapCur, 578, 300, 0);
            const int updG = pxAt(mapCur, 578, 300, 1);
            if (updG < 180 || updR > 80) {
                std::fprintf(stderr, "UpdateSubresource CNN invalidation failed: R=%d G=%d\n", updR, updG);
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 25;
            }

            // 5. Map/Unmap dirty invalidation check (Region 10 x=614, y=300 -> orange (240,140,30))
            const int mapR = pxAt(mapCur, 614, 300, 0);
            const int mapB = pxAt(mapCur, 614, 300, 2);
            if (mapR < 200 || mapB > 70) {
                std::fprintf(stderr, "Map/Unmap CNN invalidation failed: R=%d B=%d\n", mapR, mapB);
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 26;
            }

            // 6. [ElementScale] Geometry Shader +100% scale check (Region 11 outer border x=204, y=300 -> magenta (230,30,220))
            const int scaleR = pxAt(mapCur, 204, 300, 0);
            const int scaleG = pxAt(mapCur, 204, 300, 1);
            const int scaleB = pxAt(mapCur, 204, 300, 2);
            if (scaleR < 180 || scaleB < 180 || scaleG > 70) {
                std::fprintf(stderr, "[ElementScale] Geometry Shader check failed at outer quad border: R=%d G=%d B=%d\n",
                             scaleR, scaleG, scaleB);
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 28;
            }

            // 7. Offscreen UI RenderTarget CNN upscale check (Region 12a x=40..104 vs Region 12b x=120..184 at y=312)
            int offRtSlopeEnh = 0;
            int offRtSlopeBase = 0;
            for (UINT dx = 2; dx < 62; ++dx) {
                offRtSlopeEnh = (std::max)(offRtSlopeEnh, std::abs(pxAt(mapCur, 40 + dx + 1, 312, 0) - pxAt(mapCur, 40 + dx, 312, 0)));
                offRtSlopeBase = (std::max)(offRtSlopeBase, std::abs(pxAt(mapCur, 120 + dx + 1, 312, 0) - pxAt(mapCur, 120 + dx, 312, 0)));
            }
            if (offRtSlopeEnh <= offRtSlopeBase || offRtSlopeEnh < 60) {
                std::fprintf(stderr, "Offscreen UI RenderTarget CNN check failed: enh=%d base=%d\n", offRtSlopeEnh, offRtSlopeBase);
                ctx->Unmap(bbPreWorldFxStaging, 0);
                ctx->Unmap(bbStaging, 0);
                return 29;
            }

            ctx->Unmap(bbPreWorldFxStaging, 0);
            ctx->Unmap(bbStaging, 0);

            // 8. MapMode (896x576 @ 1920x1080) parchment calm vs ink stroke sharpness
            D3D11_MAPPED_SUBRESOURCE mapEnh{}, mapBase{};
            if (SUCCEEDED(ctx->Map(mapOutStagingEnh, 0, D3D11_MAP_READ, 0, &mapEnh)) &&
                SUCCEEDED(ctx->Map(mapOutStagingBase, 0, D3D11_MAP_READ, 0, &mapBase))) {
                const auto* rowEnh = static_cast<const uint8_t*>(mapEnh.pData) + 540 * mapEnh.RowPitch;
                const auto* rowBase = static_cast<const uint8_t*>(mapBase.pData) + 540 * mapBase.RowPitch;

                uint64_t noiseDiffEnh = 0;
                uint64_t noiseDiffBase = 0;
                for (UINT x = 160; x < 780; ++x) {
                    noiseDiffEnh += static_cast<uint64_t>(std::abs(int(rowEnh[(x + 1) * 4]) - int(rowEnh[x * 4])));
                    noiseDiffBase += static_cast<uint64_t>(std::abs(int(rowBase[(x + 1) * 4]) - int(rowBase[x * 4])));
                }

                int inkSlopeEnh = 0;
                int inkSlopeBase = 0;
                for (UINT x = 920; x < 1000; ++x) {
                    inkSlopeEnh = (std::max)(inkSlopeEnh, std::abs(int(rowEnh[(x + 1) * 4]) - int(rowEnh[x * 4])));
                    inkSlopeBase = (std::max)(inkSlopeBase, std::abs(int(rowBase[(x + 1) * 4]) - int(rowBase[x * 4])));
                }

                ctx->Unmap(mapOutStagingEnh, 0);
                ctx->Unmap(mapOutStagingBase, 0);

                std::printf("UIE v0.5 check: worldFxDelta=%llu srgbSlope(%d vs %d) offRtSlope(%d vs %d) scale(%d,%d,%d) bc3(%d->%d slope=%d) upd(%d,%d) mapDyn(%d,%d) mapMode(ink=%d vs %d, noise=%llu vs %llu)\n",
                            static_cast<unsigned long long>(worldFxDelta),
                            srgbSlopeEnh, srgbSlopeBase,
                            offRtSlopeEnh, offRtSlopeBase,
                            scaleR, scaleG, scaleB,
                            bc3Left, bc3Right, bc3Slope,
                            updR, updG, mapR, mapB,
                            inkSlopeEnh, inkSlopeBase,
                            static_cast<unsigned long long>(noiseDiffEnh),
                            static_cast<unsigned long long>(noiseDiffBase));

                if (inkSlopeEnh <= inkSlopeBase || noiseDiffEnh > noiseDiffBase) {
                    std::fprintf(stderr, "MapMode check failed: ink(%d vs %d) noise(%llu vs %llu)\n",
                                 inkSlopeEnh, inkSlopeBase,
                                 static_cast<unsigned long long>(noiseDiffEnh),
                                 static_cast<unsigned long long>(noiseDiffBase));
                    return 27;
                }
            } else {
                std::fprintf(stderr, "Failed to map MapMode staging buffers\n");
                return 27;
            }
        } else {
            if (mapCur.pData) ctx->Unmap(bbStaging, 0);
            if (mapPre.pData) ctx->Unmap(bbPreWorldFxStaging, 0);
        }
    }

    int aaProbeSlope = 0;
    int aaProbeDiffFromBase = 0;
    std::string aaProbeHex;
    {
        D3D11_MAPPED_SUBRESOURCE rb{};
        hr = ctx->Map(bbStaging, 0, D3D11_MAP_READ, 0, &rb);
        if (FAILED(hr) || !rb.pData) {
            std::fprintf(stderr, "Staging Map failed: 0x%08X\n", static_cast<unsigned>(hr));
            return 15;
        }
        const auto* row = static_cast<const uint8_t*>(rb.pData) + 64 * rb.RowPitch;
        const auto* row2 = static_cast<const uint8_t*>(rb.pData) + 204 * rb.RowPitch;
        auto pixR = [&](UINT x) -> int { return row[x * 4 + 0]; };
        auto pixG = [&](UINT x) -> int { return row[x * 4 + 1]; };
        auto pixB = [&](UINT x) -> int { return row[x * 4 + 2]; };

        int maxSlopeEnh = 0;
        int maxSlopeBase = 0;
        int maxSlopeNp = 0;
        int maxSlopeDw = 0;
        int transWidthEnh = 0;
        int transWidthBase = 0;
        for (UINT dx = 48; dx < 80; ++dx) {
            const int rE0 = pixR(dx);
            const int rE1 = pixR(dx + 1);
            if (!probeAaMode && (rE0 < 15 || rE0 > 245)) {
                std::fprintf(stderr, "UIE overshoot detected at x=%u: R=%d\n", dx, rE0);
                ctx->Unmap(bbStaging, 0);
                return 16;
            }
            maxSlopeEnh = (std::max)(maxSlopeEnh, std::abs(rE1 - rE0));
            if (rE0 > 45 && rE0 < 215) ++transWidthEnh;

            const int rB0 = pixR(160 + dx);
            const int rB1 = pixR(160 + dx + 1);
            maxSlopeBase = (std::max)(maxSlopeBase, std::abs(rB1 - rB0));
            if (rB0 > 45 && rB0 < 215) ++transWidthBase;

            maxSlopeNp = (std::max)(maxSlopeNp, std::abs(pixR(480 + dx + 1) - pixR(480 + dx)));
            maxSlopeDw = (std::max)(maxSlopeDw, std::abs(static_cast<int>(row2[(480 + dx + 1) * 4]) - static_cast<int>(row2[(480 + dx) * 4])));
        }

        for (UINT dx = 32; dx <= 96; ++dx) {
            const int e = row2[dx * 4 + 0];
            const int b = row2[(160 + dx) * 4 + 0];
            aaProbeDiffFromBase += std::abs(e - b);
            if (dx < 96) {
                aaProbeSlope = (std::max)(aaProbeSlope, std::abs(static_cast<int>(row2[(dx + 1) * 4]) - e));
            }
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X", e);
            aaProbeHex += hex;
        }

        const int discR = pixR(320 + 24), discB = pixB(320 + 24);
        const int keepR = pixR(320 + 104), keepG = pixG(320 + 104), keepB = pixB(320 + 104);
        ctx->Unmap(bbStaging, 0);

        if (!probeAaMode) {
            std::printf("UIE pixel check: maxSlope(enh=%d vs base=%d np=%d dw=%d) transPx(enh=%d vs base=%d) discard=(%d,?,%d) keep=(%d,%d,%d)\n",
                        maxSlopeEnh, maxSlopeBase, maxSlopeNp, maxSlopeDw, transWidthEnh, transWidthBase, discR, discB, keepR, keepG, keepB);

            if (maxSlopeEnh <= maxSlopeBase || transWidthEnh >= transWidthBase) {
                std::fprintf(stderr, "UIE did not sharpen edge strictly better than bilinear!\n");
                return 16;
            }
            if (maxSlopeNp <= maxSlopeBase || maxSlopeDw != maxSlopeBase) {
                std::fprintf(stderr, "UIE nameplate/depth-write gate failed (np=%d dw=%d base=%d)!\n",
                             maxSlopeNp, maxSlopeDw, maxSlopeBase);
                return 16;
            }
            if (!(discR < 40 && discB > discR)) {
                std::fprintf(stderr, "UIE alpha-test discard check failed (discR=%d discB=%d)!\n", discR, discB);
                return 17;
            }
            if (!(keepR > 210 && keepR > keepG + 40 && keepG > keepB + 20)) {
                std::fprintf(stderr, "UIE cb0 diffuse tint check failed (keep=%d,%d,%d)!\n", keepR, keepG, keepB);
                return 17;
            }
        }
    }

    // Verify ResizeBuffers works cleanly with overlay active!
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    bbRtv->Release();
    hr = swapChain->ResizeBuffers(2, 800, 600, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ResizeBuffers failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 8;
    }

    // Cleanup
    if (bbStaging) bbStaging->Release();
    if (uiCb) uiCb->Release();
    if (uiAlphaSrv) uiAlphaSrv->Release();
    if (uiAlphaTex) uiAlphaTex->Release();
    if (uiAaSrv) uiAaSrv->Release();
    if (uiAaTex) uiAaTex->Release();
    if (uiStepSrv) uiStepSrv->Release();
    if (uiStepTex) uiStepTex->Release();
    if (dsWriteState) dsWriteState->Release();
    if (dsReadOnlyState) dsReadOnlyState->Release();
    if (lotroUiBasePs) lotroUiBasePs->Release();
    if (lotroUiPs) lotroUiPs->Release();
    if (lotroUiHalfVs) lotroUiHalfVs->Release();
    if (lotroUiVs) lotroUiVs->Release();
    if (resolveMsaaTex) resolveMsaaTex->Release();
    if (resolveDstTex) resolveDstTex->Release();
    if (copySrcTex) copySrcTex->Release();
    if (copyDstTex) copyDstTex->Release();
    if (indirectArgsBuf) indirectArgsBuf->Release();
    if (dsv) dsv->Release();
    if (dsvTex) dsvTex->Release();
    if (secondarySwapChain) secondarySwapChain->Release();
    if (hwndSec) DestroyWindow(hwndSec);
    if (mapOutStagingBase) mapOutStagingBase->Release();
    if (mapOutStagingEnh) mapOutStagingEnh->Release();
    if (mapOutRtv) mapOutRtv->Release();
    if (mapOutTex) mapOutTex->Release();
    if (parchmentSrv) parchmentSrv->Release();
    if (parchmentTex) parchmentTex->Release();
    if (world3dSrv) world3dSrv->Release();
    if (world3dTex) world3dTex->Release();
    if (uiOffRtSrv) uiOffRtSrv->Release();
    if (uiOffRtRtv) uiOffRtRtv->Release();
    if (uiOffRtTex) uiOffRtTex->Release();
    if (uiScaleSrv) uiScaleSrv->Release();
    if (uiScaleTex) uiScaleTex->Release();
    if (uiMapDynSrv) uiMapDynSrv->Release();
    if (uiMapDynTex) uiMapDynTex->Release();
    if (uiDynSrv) uiDynSrv->Release();
    if (uiDynTex) uiDynTex->Release();
    if (uiBc3Srv) uiBc3Srv->Release();
    if (uiBc3Tex) uiBc3Tex->Release();
    if (uiSrgbSrv) uiSrgbSrv->Release();
    if (uiSrgbTex) uiSrgbTex->Release();
    if (worldSampler) worldSampler->Release();
    if (bbPreWorldFxStaging) bbPreWorldFxStaging->Release();
    if (cs) cs->Release();
    if (blStateDup) blStateDup->Release();
    blState->Release();
    dsState->Release();
    sampler->Release();
    oneRowSrv->Release();
    oneRowTex->Release();
    bc3Srv->Release();
    bc3Tex->Release();
    VirtualFree(guardRegion, 0, MEM_RELEASE);
    fontSrv->Release();
    fontTex->Release();
    mapSrv->Release();
    mapTex->Release();
    ps->Release();
    vs->Release();
    ctx->Release();
    device->Release();
    swapChain->Release();
    DestroyWindow(hwnd);
    if (sysD3d11) FreeLibrary(sysD3d11);
    FreeLibrary(proxyMod);

    if (probeAaMode) {
        std::printf("PROBE_AA %d %d %s\n", aaProbeSlope, aaProbeDiffFromBase, aaProbeHex.c_str());
        iniRestorer.RestoreNow();
        fs::remove(workDir / L"lotro_uie.log", ec);
        return 0;
    }

    // Inspect generated capture artifacts
    const fs::path capRoot = workDir / L"lotro_uie_captures";
    if (!fs::exists(capRoot)) {
        std::fprintf(stderr, "Capture folder was not created!\n");
        return 9;
    }

    auto readAll = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };

    bool foundSummary = false;
    bool foundDraws = false;
    bool foundTimeline = false;
    bool foundDds = false;
    bool foundBmp = false;
    bool foundPng = false;
    bool validPngSig = false;
    bool foundAsm = false;
    bool foundCsAsm = false;
    std::string summaryText;
    std::string drawsText;
    std::string timelineText;
    for (const auto& entry : fs::recursive_directory_iterator(capRoot)) {
        const auto name = entry.path().filename().string();
        if (name == "summary.txt") {
            foundSummary = true;
            summaryText = readAll(entry.path());
        }
        if (name == "draws.csv") {
            foundDraws = true;
            drawsText = readAll(entry.path());
        }
        if (name == "timeline.csv") {
            foundTimeline = true;
            timelineText = readAll(entry.path());
        }
        if (entry.path().extension() == ".dds") foundDds = true;
        if (entry.path().extension() == ".bmp") foundBmp = true;
        if (entry.path().extension() == ".png") {
            foundPng = true;
            const std::string pngBytes = readAll(entry.path());
            static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            if (pngBytes.size() >= 8 && std::memcmp(pngBytes.data(), kPngSig, 8) == 0) {
                validPngSig = true;
            }
        }
        if (entry.path().extension() == ".asm") {
            foundAsm = true;
            if (name.rfind("cs_", 0) == 0) foundCsAsm = true;
        }
    }

    if (!foundSummary || !foundDraws || !foundTimeline || !foundDds || !foundBmp || !foundPng || !validPngSig || !foundAsm || !foundCsAsm) {
        std::fprintf(stderr, "Missing expected capture files: summary=%d draws=%d timeline=%d dds=%d bmp=%d png=%d validPng=%d asm=%d csAsm=%d\n",
                     foundSummary, foundDraws, foundTimeline, foundDds, foundBmp, foundPng, validPngSig, foundAsm, foundCsAsm);
        return 10;
    }
    if (summaryText.find("Runtime & Vtable Diagnostics:") == std::string::npos ||
        summaryText.find("hash=0x") == std::string::npos ||
        drawsText.find("vp_size,dsv_id,dsv_size") == std::string::npos ||
        drawsText.find("DrawInstancedIndirect") == std::string::npos ||
        drawsText.find("640x360") == std::string::npos ||
        drawsText.find(",6,1,") == std::string::npos) {
        std::fprintf(stderr, "Missing expected summary/draws diagnostics!\n");
        return 13;
    }
    for (const char* evName : {"ClearRTV", "ClearDSV", "CopyResource", "ResolveSubresource", "Dispatch", "PresentSecondary"}) {
        if (timelineText.find(evName) == std::string::npos) {
            std::fprintf(stderr, "Missing expected timeline event: %s\n", evName);
            return 14;
        }
    }

    const std::string logText = readAll(workDir / L"lotro_uie.log");
    if (logText.find("UiEnhancer v0.5 initialized shaders") == std::string::npos ||
        logText.find("UiEnhancer matched target UI PixelShader") == std::string::npos ||
        logText.find("UiSrCache initialized") == std::string::npos ||
        logText.find("WorldPostFx initialized") == std::string::npos ||
        logText.find("Sampler upgraded #1 -> 16x Aniso, MipLODBias=-0.35") == std::string::npos ||
        logText.find("compile failed") != std::string::npos ||
        logText.find("compile warning") != std::string::npos ||
        logText.find("[ERROR]") != std::string::npos ||
        logText.find("[WARN ]") != std::string::npos) {
        std::fprintf(stderr, "Unexpected UiEnhancer status in log:\n%s\n", logText.c_str());
        iniRestorer.RestoreNow();
        return 18;
    }

    // Verify all 4 Sharpness presets (Low=0.35, Med=0.70, High=1.05, Ultra=1.40) produce strictly distinct & monotonic GPU pixels!
    struct ProbeResult {
        const char* name;
        const wchar_t* val;
        int slope = 0;
        int diffBase = 0;
        std::vector<int> rowVals;
    };
    ProbeResult probes[] = {
        {"Low",   L"0.35"},
        {"Med",   L"0.70"},
        {"High",  L"1.05"},
        {"Ultra", L"1.40"}
    };
    for (auto& pr : probes) {
        std::wstring cmdLine = L"\"" + std::wstring(argv[0]) + L"\"";
        if (dxgiMode) cmdLine += L" --dxgi";
        cmdLine += L" --probe-aa ";
        cmdLine += pr.val;
        cmdLine += L" \"";
        cmdLine += dllPath.wstring();
        cmdLine += L"\"";

        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE hRead = nullptr, hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
            std::fprintf(stderr, "CreatePipe failed for sharpness probe\n");
            iniRestorer.RestoreNow();
            return 19;
        }
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(hWrite);
        if (!ok) {
            CloseHandle(hRead);
            std::fprintf(stderr, "CreateProcessW failed for sharpness probe\n");
            iniRestorer.RestoreNow();
            return 19;
        }
        std::string outText;
        char chunk[512];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0) {
            outText.append(chunk, bytesRead);
        }
        CloseHandle(hRead);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        const size_t pos = outText.find("PROBE_AA ");
        if (exitCode != 0 || pos == std::string::npos) {
            std::fprintf(stderr, "Sharpness probe %ls failed (exit=%lu): %s\n", pr.val, exitCode, outText.c_str());
            iniRestorer.RestoreNow();
            return 19;
        }
        int slope = 0, diffBase = 0;
        char hexBuf[256]{};
        if (sscanf_s(outText.c_str() + pos, "PROBE_AA %d %d %255s", &slope, &diffBase,
                     hexBuf, static_cast<unsigned>(sizeof(hexBuf))) != 3) {
            std::fprintf(stderr, "Malformed PROBE_AA output: %s\n", outText.c_str());
            iniRestorer.RestoreNow();
            return 19;
        }
        pr.slope = slope;
        pr.diffBase = diffBase;
        const size_t hexLen = std::strlen(hexBuf);
        for (size_t k = 0; k + 1 < hexLen; k += 2) {
            auto hexNib = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                return 0;
            };
            pr.rowVals.push_back((hexNib(hexBuf[k]) << 4) | hexNib(hexBuf[k + 1]));
        }
    }

    int diffLowMed = 0, diffMedHigh = 0, diffHighUltra = 0;
    for (size_t i = 0; i < probes[0].rowVals.size(); ++i) {
        diffLowMed += std::abs(probes[0].rowVals[i] - probes[1].rowVals[i]);
        diffMedHigh += std::abs(probes[1].rowVals[i] - probes[2].rowVals[i]);
        diffHighUltra += std::abs(probes[2].rowVals[i] - probes[3].rowVals[i]);
    }
    std::printf("UIE preset progression: Low(slope=%d,diffBase=%d) Med(slope=%d,diffBase=%d) High(slope=%d,diffBase=%d) Ultra(slope=%d,diffBase=%d) | pairL1=(%d, %d, %d)\n",
                probes[0].slope, probes[0].diffBase,
                probes[1].slope, probes[1].diffBase,
                probes[2].slope, probes[2].diffBase,
                probes[3].slope, probes[3].diffBase,
                diffLowMed, diffMedHigh, diffHighUltra);
    if (!(probes[0].diffBase > 0 &&
          probes[0].diffBase < probes[1].diffBase &&
          probes[1].diffBase < probes[2].diffBase &&
          probes[2].diffBase < probes[3].diffBase &&
          diffLowMed >= 30 && diffMedHigh >= 30 && diffHighUltra >= 30)) {
        std::fprintf(stderr, "UIE sharpness presets did not produce strictly distinct progressive pixel changes!\n");
        iniRestorer.RestoreNow();
        return 19;
    }

    iniRestorer.RestoreNow();
    fs::remove_all(capRoot, ec);
    fs::remove(workDir / L"lotro_uie.log", ec);
    std::printf("SMOKE TEST PASSED! All capture artifacts, nameplate gate & UIE preset checks verified.\n");
    return 0;
}
