#include "dx11/debug_overlay.h"
#include "common/logger.h"
#include "dx11/ui_sr_cache.h"
#include "dx11/world_postfx.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lotro_uie {

namespace {

constexpr UINT kMaxOverlayVerts = 24576;

struct OverlayVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

// Compact, clean 8x8 monochrome bitmap font for ASCII 0x20 (' ') through 0x7E ('~'), MSB-left per row.
// Row index 0 = top row, bit 7 = leftmost pixel.
constexpr uint8_t kFont8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' 0x20
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // '!' 0x21
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // '"' 0x22
    {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00}, // '#' 0x23
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // '$' 0x24
    {0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00,0x00}, // '%' 0x25
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&' 0x26
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // ''' 0x27
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '(' 0x28
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')' 0x29
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*' 0x2A
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+' 0x2B
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ',' 0x2C
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-' 0x2D
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.' 0x2E
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x00,0x00}, // '/' 0x2F
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // '0' 0x30
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1' 0x31
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, // '2' 0x32
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3' 0x33
    {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00}, // '4' 0x34
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5' 0x35
    {0x3C,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6' 0x36
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7' 0x37
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8' 0x38
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9' 0x39
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // ':' 0x3A
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // ';' 0x3B
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<' 0x3C
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // '=' 0x3D
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // '>' 0x3E
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // '?' 0x3F
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // '@' 0x40
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'A' 0x41
    {0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00}, // 'B' 0x42
    {0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00}, // 'C' 0x43
    {0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00}, // 'D' 0x44
    {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00}, // 'E' 0x45
    {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00}, // 'F' 0x46
    {0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7E,0x00}, // 'G' 0x47
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'H' 0x48
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 'I' 0x49
    {0x3E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 'J' 0x4A
    {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00}, // 'K' 0x4B
    {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00}, // 'L' 0x4C
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 'M' 0x4D
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'N' 0x4E
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'O' 0x4F
    {0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00}, // 'P' 0x50
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xCC,0x76,0x00}, // 'Q' 0x51
    {0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00}, // 'R' 0x52
    {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, // 'S' 0x53
    {0xFE,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 'T' 0x54
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'U' 0x55
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 'V' 0x56
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 'W' 0x57
    {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00}, // 'X' 0x58
    {0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x00}, // 'Y' 0x59
    {0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00}, // 'Z' 0x5A
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '[' 0x5B
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x00,0x00}, // '\' 0x5C
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']' 0x5D
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^' 0x5E
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00}, // '_' 0x5F
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`' 0x60
    {0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00}, // 'a' 0x61
    {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00}, // 'b' 0x62
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 'c' 0x63
    {0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00}, // 'd' 0x64
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 'e' 0x65
    {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00}, // 'f' 0x66
    {0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x7C}, // 'g' 0x67
    {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, // 'h' 0x68
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i' 0x69
    {0x0C,0x00,0x1C,0x0C,0x0C,0xCC,0x78,0x00}, // 'j' 0x6A
    {0xC0,0xC0,0xCC,0xD8,0xF0,0xD8,0xCC,0x00}, // 'k' 0x6B
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l' 0x6C
    {0x00,0x00,0xCC,0xFE,0xD6,0xC6,0xC6,0x00}, // 'm' 0x6D
    {0x00,0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, // 'n' 0x6E
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o' 0x6F
    {0x00,0x00,0xFC,0xC6,0xC6,0xFC,0xC0,0xC0}, // 'p' 0x70
    {0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x06}, // 'q' 0x71
    {0x00,0x00,0xDC,0xE6,0xC0,0xC0,0xC0,0x00}, // 'r' 0x72
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 's' 0x73
    {0x30,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, // 't' 0x74
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x00}, // 'u' 0x75
    {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 'v' 0x76
    {0x00,0x00,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 'w' 0x77
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 'x' 0x78
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C}, // 'y' 0x79
    {0x00,0x00,0xFE,0x0C,0x38,0x60,0xFE,0x00}, // 'z' 0x7A
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{' 0x7B
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|' 0x7C
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // '}' 0x7D
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~' 0x7E
};

const char kOverlayVsSrc[] = R"(
struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0f, 1.0f);
    o.uv  = i.uv;
    o.col = i.col;
    return o;
}
)";

const char kOverlayPsSrc[] = R"(
Texture2D<float> FontTex : register(t0);
SamplerState PointSamp : register(s0);
struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
float4 main(PSIn i) : SV_TARGET {
    float a = FontTex.Sample(PointSamp, i.uv).r;
    return float4(i.col.rgb, i.col.a * a);
}
)";

using PfnD3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

PfnD3DCompile GetD3DCompileFn() {
    static PfnD3DCompile fn = []() -> PfnD3DCompile {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<PfnD3DCompile>(GetProcAddress(mod, "D3DCompile"));
    }();
    return fn;
}

void PushQuad(
    std::vector<OverlayVertex>& verts,
    float x0, float y0, float x1, float y1,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float a,
    float screenW, float screenH
) {
    if (verts.size() + 6 > kMaxOverlayVerts) return;
    auto toNdcX = [screenW](float px) { return (px / screenW) * 2.0f - 1.0f; };
    auto toNdcY = [screenH](float py) { return 1.0f - (py / screenH) * 2.0f; };

    const float nx0 = toNdcX(x0), ny0 = toNdcY(y0);
    const float nx1 = toNdcX(x1), ny1 = toNdcY(y1);

    verts.push_back({nx0, ny0, u0, v0, r, g, b, a});
    verts.push_back({nx1, ny0, u1, v0, r, g, b, a});
    verts.push_back({nx0, ny1, u0, v1, r, g, b, a});

    verts.push_back({nx1, ny0, u1, v0, r, g, b, a});
    verts.push_back({nx1, ny1, u1, v1, r, g, b, a});
    verts.push_back({nx0, ny1, u0, v1, r, g, b, a});
}

void PushText(
    std::vector<OverlayVertex>& verts,
    float startX, float startY, float scale,
    const std::string& text,
    float r, float g, float b, float a,
    float screenW, float screenH
) {
    float x = startX;
    const float adv = 8.0f * scale;
    const float gh = 8.0f * scale;
    for (unsigned char ch : text) {
        if (ch >= 0x20 && ch <= 0x7E) {
            if (ch != ' ') {
                const int idx = ch - 0x20;
                const int col = idx % 16;
                const int row = idx / 16;
                const float u0 = (col * 8.0f) / 128.0f;
                const float v0 = (row * 8.0f) / 64.0f;
                const float u1 = ((col + 1) * 8.0f) / 128.0f;
                const float v1 = ((row + 1) * 8.0f) / 64.0f;
                PushQuad(verts, x, startY, x + adv, startY + gh, u0, v0, u1, v1, r, g, b, a, screenW, screenH);
            }
        }
        x += adv;
    }
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

DebugOverlay& DebugOverlay::Instance() {
    static auto* instance = new DebugOverlay();
    return *instance;
}

void DebugOverlay::ToggleMenu() {
    const bool next = !menuOpen_.load(std::memory_order_relaxed);
    menuOpen_.store(next, std::memory_order_release);
    UIE_LOG_INFO("Interactive In-Game Menu toggled: %s", next ? "OPEN" : "CLOSED");
}

bool DebugOverlay::IsMenuOpen() const noexcept {
    return menuOpen_.load(std::memory_order_acquire);
}

void DebugOverlay::OnBeforeResizeBuffers() {
    // We don't retain a swapchain backbuffer RTV across frames, so ResizeBuffers never blocks on us.
}

void DebugOverlay::Shutdown() {
    SafeRelease(rastState_);
    SafeRelease(depthState_);
    SafeRelease(blendState_);
    SafeRelease(pointSampler_);
    SafeRelease(fontSrv_);
    SafeRelease(fontTex_);
    SafeRelease(vb_);
    SafeRelease(inputLayout_);
    SafeRelease(ps_);
    SafeRelease(vs_);
    device_ = nullptr;
    initialized_ = false;
}

bool DebugOverlay::EnsureInitialized(ID3D11Device* device) {
    if (initialized_ && device_ == device) return true;
    if (initFailed_) return false;
    if (!device) return false;

    if (device_ && device_ != device) {
        Shutdown();
    }
    device_ = device;

    auto compileFn = GetD3DCompileFn();
    if (!compileFn) {
        UIE_LOG_WARN("DebugOverlay: d3dcompiler_47.dll unavailable, HUD disabled");
        initFailed_ = true;
        return false;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = compileFn(kOverlayVsSrc, sizeof(kOverlayVsSrc) - 1, "uie_vs", nullptr, nullptr,
                           "main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) {
        UIE_LOG_ERROR("DebugOverlay VS compile failed: %s",
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "unknown");
        SafeRelease(errBlob);
        initFailed_ = true;
        return false;
    }
    SafeRelease(errBlob);

    hr = compileFn(kOverlayPsSrc, sizeof(kOverlayPsSrc) - 1, "uie_ps", nullptr, nullptr,
                   "main", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) {
        UIE_LOG_ERROR("DebugOverlay PS compile failed: %s",
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "unknown");
        SafeRelease(errBlob);
        SafeRelease(vsBlob);
        initFailed_ = true;
        return false;
    }
    SafeRelease(errBlob);

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
    }

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(OverlayVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(OverlayVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(OverlayVertex, r), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (SUCCEEDED(hr)) {
        hr = device->CreateInputLayout(layoutDesc, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
    }
    SafeRelease(vsBlob);
    SafeRelease(psBlob);

    if (FAILED(hr)) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    // Build 128x64 R8_UNORM font atlas (16x8 grid of 8x8 glyphs, cell 127 = solid white block).
    std::array<uint8_t, 128 * 64> atlas{};
    for (int idx = 0; idx < 95; ++idx) {
        const int cellCol = idx % 16;
        const int cellRow = idx / 16;
        for (int r = 0; r < 8; ++r) {
            const uint8_t bits = kFont8x8[idx][r];
            for (int c = 0; c < 8; ++c) {
                const bool on = (bits & (0x80 >> c)) != 0;
                const int px = cellCol * 8 + c;
                const int py = cellRow * 8 + r;
                atlas[static_cast<size_t>(py) * 128 + px] = on ? 255 : 0;
            }
        }
    }
    // Cell (15, 7) -> solid white block for background panels.
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            atlas[static_cast<size_t>(7 * 8 + r) * 128 + (15 * 8 + c)] = 255;
        }
    }

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = 128;
    texDesc.Height = 64;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = atlas.data();
    initData.SysMemPitch = 128;

    hr = device->CreateTexture2D(&texDesc, &initData, &fontTex_);
    if (SUCCEEDED(hr)) {
        hr = device->CreateShaderResourceView(fontTex_, nullptr, &fontSrv_);
    }

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = kMaxOverlayVerts * sizeof(OverlayVertex);
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) {
        hr = device->CreateBuffer(&vbDesc, nullptr, &vb_);
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (SUCCEEDED(hr)) {
        hr = device->CreateSamplerState(&sampDesc, &pointSampler_);
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) {
        hr = device->CreateBlendState(&blendDesc, &blendState_);
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (SUCCEEDED(hr)) {
        hr = device->CreateDepthStencilState(&depthDesc, &depthState_);
    }

    D3D11_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.ScissorEnable = FALSE;
    if (SUCCEEDED(hr)) {
        hr = device->CreateRasterizerState(&rastDesc, &rastState_);
    }

    if (FAILED(hr)) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    initialized_ = true;
    return true;
}

void DebugOverlay::Render(
    IDXGISwapChain* swapChain,
    ID3D11Device* device,
    ID3D11DeviceContext* immediateCtx,
    const SwapChainInfo& swapInfo,
    const InspectorLiveStats& stats,
    const UiEnhancerStats& uieStats,
    Config& config
) {
    if (!swapChain || !device || !immediateCtx || !EnsureInitialized(device)) {
        return;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) || !backbuffer) {
        return;
    }

    D3D11_TEXTURE2D_DESC bbDesc{};
    backbuffer->GetDesc(&bbDesc);
    const float screenW = static_cast<float>(bbDesc.Width > 0 ? bbDesc.Width : swapInfo.width);
    const float screenH = static_cast<float>(bbDesc.Height > 0 ? bbDesc.Height : swapInfo.height);
    if (screenW <= 0.0f || screenH <= 0.0f) {
        backbuffer->Release();
        return;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT rtvHr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    backbuffer->Release();
    if (FAILED(rtvHr) || !rtv) {
        return;
    }

    // Crisp integer scaling so the HUD itself is legible on 4K (2x at >=1440p, 1x at <=1080p).
    const float scale = (screenH >= 1400.0f) ? 2.0f : 1.0f;

    const auto srStats = UiSrCache::Instance().GetStats();
    const auto wfxStats = WorldPostFx::Instance().GetStats();

    char line1[200];
    if (uieStats.hoveredTexId != 0) {
        std::snprintf(
            line1, sizeof(line1),
            "LOTRO-UIE v0.6 | %ux%u | Draws:%u (UI:%u, SR:%u, Map:%u) | Hover:0x%016llX(%ux%u)",
            static_cast<unsigned>(screenW),
            static_cast<unsigned>(screenH),
            stats.lastFrameDraws,
            uieStats.lastFrameEnhancedDraws,
            uieStats.lastFrameSrDraws,
            uieStats.lastFrameMapDraws,
            static_cast<unsigned long long>(uieStats.hoveredTexHash),
            uieStats.hoveredTexW,
            uieStats.hoveredTexH
        );
    } else {
        std::snprintf(
            line1, sizeof(line1),
            "LOTRO-UIE v0.6 | %ux%u | Draws:%u (UI:%u, SR:%u, Map:%u, Scale:%u/%u, HD:%u)",
            static_cast<unsigned>(screenW),
            static_cast<unsigned>(screenH),
            stats.lastFrameDraws,
            uieStats.lastFrameEnhancedDraws,
            uieStats.lastFrameSrDraws,
            uieStats.lastFrameMapDraws,
            uieStats.lastFrameScaledDraws,
            uieStats.elementScaleRules,
            uieStats.loadedOverrides
        );
    }

    const char* sharpTag = (uieStats.sharpness < 0.52f) ? "Low"
                         : (uieStats.sharpness < 0.88f) ? "Med"
                         : (uieStats.sharpness < 1.22f) ? "High" : "Ultra";
    char line2[200];
    std::snprintf(
        line2, sizeof(line2),
        "[%s] UIE:%s | [%s] Sharp:%.2f(%s) | [%s] CNN:%s(%u,%lluMB) | [%s] 3DFX:%s | [%s] Split:%s",
        VirtualKeyName(config.hotkeyToggleEnhance).c_str(),
        uieStats.ready ? (uieStats.enabled ? "ON " : "OFF") : "N/A",
        VirtualKeyName(config.hotkeyCycleSharpness).c_str(),
        uieStats.sharpness,
        sharpTag,
        VirtualKeyName(config.hotkeyToggleCnn).c_str(),
        UiSrCache::Instance().IsEnabled() ? "ON " : "OFF",
        srStats.entryCount,
        static_cast<unsigned long long>(srStats.cacheBytesUsed / (1024ull * 1024ull)),
        VirtualKeyName(config.hotkeyToggleWorldFx).c_str(),
        (WorldPostFx::Instance().IsEnabled() && wfxStats.initialized) ? "ON "
            : (WorldPostFx::Instance().IsEnabled() ? "RDY" : "OFF"),
        VirtualKeyName(config.hotkeyToggleSplit).c_str(),
        uieStats.splitScreen ? "ON " : "OFF"
    );

    char line3[200];
    std::snprintf(
        line3, sizeof(line3),
        "[%s] Menu  [%s] CopyHover  [%s/%s] Scale-+  [%s] Reload  [%s] HUD  [%s] Dump(%s)",
        VirtualKeyName(config.hotkeyToggleMenu).c_str(),
        VirtualKeyName(config.hotkeyCopyHover).c_str(),
        VirtualKeyName(config.hotkeyScaleDown).c_str(),
        VirtualKeyName(config.hotkeyScaleUp).c_str(),
        VirtualKeyName(config.hotkeyReloadConfig).c_str(),
        VirtualKeyName(config.hotkeyToggleOverlay).c_str(),
        VirtualKeyName(config.hotkeyCaptureAndDump).c_str(),
        config.dumpFormat.c_str()
    );

    const std::string line4 = !uieStats.hudNotification.empty()
        ? (">> " + uieStats.hudNotification)
        : stats.lastCaptureStatus;
    const size_t maxChars = (std::max)({std::strlen(line1), std::strlen(line2), std::strlen(line3), line4.size()});

    const float pad = 8.0f * scale;
    const float lineH = 11.0f * scale;
    const float boxX = 12.0f * scale;
    const float boxY = 12.0f * scale;
    const float boxW = static_cast<float>(maxChars) * (8.0f * scale) + pad * 2.0f;
    const int numLines = line4.empty() ? 3 : 4;
    const float boxH = static_cast<float>(numLines) * lineH + pad * 1.5f;

    std::vector<OverlayVertex> verts;
    verts.reserve(4096);

    // Solid white block at cell (15, 7) center UV to avoid edge bleeding.
    const float solidU = (15.5f * 8.0f) / 128.0f;
    const float solidV = (7.5f * 8.0f) / 64.0f;
    PushQuad(verts, boxX, boxY, boxX + boxW, boxY + boxH, solidU, solidV, solidU, solidV,
             0.04f, 0.06f, 0.10f, 0.82f, screenW, screenH);
    // Subtle top accent border
    PushQuad(verts, boxX, boxY, boxX + boxW, boxY + 2.0f * scale, solidU, solidV, solidU, solidV,
             uieStats.enabled ? 0.25f : 0.75f,
             uieStats.enabled ? 0.90f : 0.45f,
             uieStats.enabled ? 0.55f : 0.20f,
             0.95f, screenW, screenH);

    PushText(verts, boxX + pad, boxY + pad, scale, line1, 0.95f, 0.98f, 1.0f, 1.0f, screenW, screenH);
    if (uieStats.ready && uieStats.enabled) {
        PushText(verts, boxX + pad, boxY + pad + lineH, scale, line2, 0.45f, 0.96f, 0.65f, 1.0f, screenW, screenH);
    } else {
        PushText(verts, boxX + pad, boxY + pad + lineH, scale, line2, 0.95f, 0.75f, 0.35f, 1.0f, screenW, screenH);
    }
    PushText(verts, boxX + pad, boxY + pad + lineH * 2.0f, scale, line3, 0.72f, 0.88f, 0.98f, 1.0f, screenW, screenH);
    if (!line4.empty()) {
        PushText(verts, boxX + pad, boxY + pad + lineH * 3.0f, scale, line4, 0.98f, 0.88f, 0.32f, 1.0f, screenW, screenH);
    }

    // Interactive In-Game Control Center (Ctrl+F9)
    if (menuOpen_.load(std::memory_order_acquire)) {
        const float menuX = boxX;
        const float menuY = boxY + boxH + 6.0f * scale;
        const float rowH = 13.0f * scale;
        constexpr int kMenuRows = 21;
        const float menuW = 58.0f * 8.0f * scale + pad * 2.0f;
        const float menuH = (static_cast<float>(kMenuRows) + 1.5f) * rowH + pad * 2.0f;

        POINT curPt{-1, -1};
        const HWND hwnd = reinterpret_cast<HWND>(swapInfo.hwnd);
        if (hwnd && GetCursorPos(&curPt)) {
            ScreenToClient(hwnd, &curPt);
        }
        const float mx = static_cast<float>(curPt.x);
        const float my = static_cast<float>(curPt.y);
        const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool lClicked = lDown && !prevLButton_;
        prevLButton_ = lDown;

        // Menu background & gold header bar
        PushQuad(verts, menuX, menuY, menuX + menuW, menuY + menuH, solidU, solidV, solidU, solidV,
                 0.05f, 0.07f, 0.12f, 0.92f, screenW, screenH);
        PushQuad(verts, menuX, menuY, menuX + menuW, menuY + rowH + 4.0f * scale, solidU, solidV, solidU, solidV,
                 0.16f, 0.22f, 0.32f, 0.96f, screenW, screenH);
        PushQuad(verts, menuX, menuY, menuX + menuW, menuY + 2.0f * scale, solidU, solidV, solidU, solidV,
                 0.92f, 0.76f, 0.32f, 0.98f, screenW, screenH);
        PushText(verts, menuX + pad, menuY + 4.0f * scale, scale,
                 "LOTRO-UIE CONTROL CENTER (CLICK TO TOGGLE / ADJUST)",
                 0.98f, 0.88f, 0.42f, 1.0f, screenW, screenH);

        struct MenuRow {
            std::string label;
            std::string value;
            bool active = false;
        };
        const WorldPostFxStats wpStats = WorldPostFx::Instance().GetStats();
        char valBuf[64];
        std::vector<MenuRow> rows(kMenuRows);
        rows[0] = {"1. UI & Font Enhancer (F8)", uieStats.enabled ? "[ ON ]" : "[ OFF ]", uieStats.enabled};
        std::snprintf(valBuf, sizeof(valBuf), "[ %s (%.2f) ]", sharpTag, uieStats.sharpness);
        rows[1] = {"2. Sharpness Preset (F7)", valBuf, true};
        rows[2] = {"3. 2x Mini-CNN Super-Res (Ctrl+F4)", UiSrCache::Instance().IsEnabled() ? "[ ON ]" : "[ OFF ]", UiSrCache::Instance().IsEnabled()};
        rows[3] = {"4. 3D World PostFX + Dehaze (Ctrl+F5)", wpStats.enabled ? "[ ON ]" : "[ OFF ]", wpStats.enabled};
        rows[4] = {"5. Map Mode (Grain Calm + Text)", uieStats.mapMode ? "[ ON ]" : "[ OFF ]", uieStats.mapMode};
        rows[5] = {"6. Split-Screen A/B (Ctrl+F3)", uieStats.splitScreen ? "[ ON ]" : "[ OFF ]", uieStats.splitScreen};
        std::snprintf(valBuf, sizeof(valBuf), uieStats.fontOutlineBoost > 0.01f ? "[ %d%% ]" : "[ OFF ]",
                      static_cast<int>(std::lround(uieStats.fontOutlineBoost * 100.0f)));
        rows[6] = {"7. Font Outline / Halo Boost", valBuf, uieStats.fontOutlineBoost > 0.01f};
        const char* weightLabel = (uieStats.fontWeightBoost < 0.05f) ? "[ OFF ]"
                                : (uieStats.fontWeightBoost < 0.38f) ? "[ Soft 25% ]"
                                : (uieStats.fontWeightBoost < 0.65f) ? "[ Med 50% ]" : "[ Bold 80% ]";
        rows[7] = {"8. Font Weight / Stem Boost", weightLabel, uieStats.fontWeightBoost >= 0.05f};
        if (uieStats.themeDarkSlate < 0.05f) {
            std::snprintf(valBuf, sizeof(valBuf), "[ OFF ]");
        } else if (uieStats.themeProfile == 1u) {
            std::snprintf(valBuf, sizeof(valBuf), "[ Elven Bronze ]");
        } else if (uieStats.themeProfile == 2u) {
            std::snprintf(valBuf, sizeof(valBuf), "[ Dwarven Iron ]");
        } else if (uieStats.themeDarkSlate >= 0.95f) {
            std::snprintf(valBuf, sizeof(valBuf), "[ Midnight 100%% ]");
        } else {
            std::snprintf(valBuf, sizeof(valBuf), "[ Obsidian %d%% ]",
                          static_cast<int>(std::lround(uieStats.themeDarkSlate * 100.0f)));
        }
        rows[8] = {"9. Modern UI Color Theme", valBuf, uieStats.themeDarkSlate >= 0.05f};
        if (uieStats.glassmorphism > 0.01f) {
            std::snprintf(valBuf, sizeof(valBuf), "[ %d%% ]", static_cast<int>(std::lround(uieStats.glassmorphism * 100.0f)));
        } else {
            std::snprintf(valBuf, sizeof(valBuf), "[ OFF ]");
        }
        rows[9] = {"10. UI Frosted Glassmorphism", valBuf, uieStats.glassmorphism > 0.01f};
        if (uieStats.iconPolish > 0.01f) {
            std::snprintf(valBuf, sizeof(valBuf), "[ %d%% ]", static_cast<int>(std::lround(uieStats.iconPolish * 100.0f)));
        } else {
            std::snprintf(valBuf, sizeof(valBuf), "[ OFF ]");
        }
        rows[10] = {"11. UI Icon & Gold Border Polish", valBuf, uieStats.iconPolish > 0.01f};
        const char* gradeLabel = (wpStats.colorProfile == 1u) ? "[ Rivendell Gold ]"
                               : (wpStats.colorProfile == 2u) ? "[ Lothlorien Silver ]"
                               : (wpStats.colorProfile == 3u) ? "[ Mordor Grim ]"
                               : "[ Natural Modern ]";
        rows[11] = {"12. Middle-earth Color Grade", gradeLabel, wpStats.colorProfile != 0u};
        if (wpStats.rimSpecular > 0.01f) {
            std::snprintf(valBuf, sizeof(valBuf), "[ %d%% ]", static_cast<int>(std::lround(wpStats.rimSpecular * 100.0f)));
        } else {
            std::snprintf(valBuf, sizeof(valBuf), "[ OFF ]");
        }
        rows[12] = {"13. 3D Rim-Light & Armor Specular", valBuf, wpStats.rimSpecular > 0.01f};
        if (wpStats.vignette > 0.01f || wpStats.horizonSoften > 0.01f) {
            std::snprintf(valBuf, sizeof(valBuf), "[ V%d%% / H%d%% ]",
                          static_cast<int>(std::lround(wpStats.vignette * 100.0f)),
                          static_cast<int>(std::lround(wpStats.horizonSoften * 100.0f)));
        } else {
            std::snprintf(valBuf, sizeof(valBuf), "[ OFF ]");
        }
        rows[13] = {"14. Vignette & Horizon Anti-Alias", valBuf,
                    (wpStats.vignette > 0.01f || wpStats.horizonSoften > 0.01f)};
        const char* eyeLabel = (uieStats.eyeComfortMode == 1u) ? "[ Anti-Glare ]"
                             : (uieStats.eyeComfortMode == 2u) ? "[ Warm 5000K ]"
                             : (uieStats.eyeComfortMode == 3u) ? "[ Night 4300K ]" : "[ OFF ]";
        rows[14] = {"15. Eye-Care & Anti-Glare Mode", eyeLabel, uieStats.eyeComfortMode != 0u};
        rows[15] = {"16. Hover Focus & Contrast Boost", uieStats.hoverFocusBoost ? "[ ON ]" : "[ OFF ]", uieStats.hoverFocusBoost};
        rows[16] = {"17. Smart Hover Lupe (Hold ALT)", uieStats.hoverMagnifier ? "[ ON (+25%) ]" : "[ OFF ]", uieStats.hoverMagnifier};
        rows[17] = {"18. OLED Auto-Dim Static HUD", uieStats.oledAutoDim ? "[ ON (72%) ]" : "[ OFF ]", uieStats.oledAutoDim};
        rows[18] = {"19. Adjust Hovered Element Scale", "[ -5%  |  +5% ]", uieStats.hoveredTexId != 0};
        rows[19] = {"20. Copy Hovered Hash (Ctrl+F2)", "[ COPY RULE ]", uieStats.hoveredTexId != 0};
        rows[20] = {"21. Save All Settings to INI", "[ SAVE INI ]", true};

        for (int i = 0; i < kMenuRows; ++i) {
            const float ry0 = menuY + (static_cast<float>(i) + 1.2f) * rowH + 4.0f * scale;
            const float ry1 = ry0 + rowH - 1.0f * scale;
            const bool hov = (mx >= menuX && mx <= menuX + menuW && my >= ry0 && my <= ry1);
            if (hov) {
                PushQuad(verts, menuX + 2.0f * scale, ry0, menuX + menuW - 2.0f * scale, ry1,
                         solidU, solidV, solidU, solidV, 0.22f, 0.30f, 0.44f, 0.90f, screenW, screenH);
                if (lClicked) {
                    auto& uie = UiEnhancer::Instance();
                    switch (i) {
                        case 0: uie.ToggleEnabled(); break;
                        case 1: uie.CycleSharpnessPreset(); break;
                        case 2: UiSrCache::Instance().ToggleEnabled(); break;
                        case 3: WorldPostFx::Instance().ToggleEnabled(); break;
                        case 4: uie.ToggleMapMode(); break;
                        case 5: uie.ToggleSplitScreen(); break;
                        case 6: uie.CycleFontOutlineBoost(); break;
                        case 7: uie.CycleFontWeightBoost(); break;
                        case 8: uie.CycleThemeDarkSlate(); break;
                        case 9: uie.CycleGlassmorphism(); break;
                        case 10: uie.CycleIconPolish(); break;
                        case 11: WorldPostFx::Instance().CycleColorProfile(); break;
                        case 12: WorldPostFx::Instance().CycleRimSpecular(); break;
                        case 13: WorldPostFx::Instance().CycleVignetteHorizon(); break;
                        case 14: uie.CycleEyeComfortMode(); break;
                        case 15: uie.ToggleHoverFocusBoost(); break;
                        case 16: uie.ToggleHoverMagnifier(); break;
                        case 17: uie.ToggleOledAutoDim(); break;
                        case 18: {
                            const bool plusHalf = (mx >= menuX + menuW * 0.72f);
                            uie.AdjustHoveredElementScale(plusHalf ? +0.05f : -0.05f, config);
                            break;
                        }
                        case 19: uie.CopyHoveredElementToClipboard(); break;
                        case 20: {
                            uie.ExportCurrentStateToConfig(config);
                            WorldPostFx::Instance().ExportCurrentStateToConfig(config);
                            config.cnnEnabled = UiSrCache::Instance().IsEnabled();
                            config.worldPostFxEnabled = WorldPostFx::Instance().IsEnabled();
                            if (config.Save()) {
                                uie.SetHudNotification("SAVED SETTINGS TO LOTRO_UIE.INI", 180);
                            }
                            break;
                        }
                        default: break;
                    }
                }
            }
            const float textY = ry0 + 2.5f * scale;
            PushText(verts, menuX + pad, textY, scale, rows[i].label,
                     hov ? 1.0f : 0.88f, hov ? 0.95f : 0.92f, hov ? 0.65f : 0.96f, 1.0f, screenW, screenH);
            const float valX = menuX + menuW - pad - static_cast<float>(rows[i].value.size()) * 8.0f * scale;
            PushText(verts, valX, textY, scale, rows[i].value,
                     rows[i].active ? 0.45f : 0.75f,
                     rows[i].active ? 0.96f : 0.55f,
                     rows[i].active ? 0.65f : 0.45f,
                     1.0f, screenW, screenH);
        }
    } else {
        prevLButton_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }

    // Split-Screen vertical divider line & top comparison badge
    if (uieStats.splitScreen) {
        const float midX = screenW * 0.5f;
        PushQuad(verts, midX - 1.0f * scale, 0.0f, midX + 1.0f * scale, screenH,
                 solidU, solidV, solidU, solidV, 0.25f, 0.90f, 0.95f, 0.85f, screenW, screenH);
        const char* badge = "< ORIG | UIE >";
        const float badgeW = 14.0f * 8.0f * scale + pad * 2.0f;
        const float badgeX = midX - badgeW * 0.5f;
        const float badgeY = 12.0f * scale;
        PushQuad(verts, badgeX, badgeY, badgeX + badgeW, badgeY + lineH + pad,
                 solidU, solidV, solidU, solidV, 0.04f, 0.06f, 0.10f, 0.88f, screenW, screenH);
        PushText(verts, badgeX + pad, badgeY + pad * 0.5f, scale, badge, 0.30f, 0.95f, 0.98f, 1.0f, screenW, screenH);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(immediateCtx->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
        std::memcpy(mapped.pData, verts.data(), verts.size() * sizeof(OverlayVertex));
        immediateCtx->Unmap(vb_, 0);
    } else {
        rtv->Release();
        return;
    }

    // Save current context state (including GS/HS/DS and shader class instances)
    ID3D11RenderTargetView* oldRtvs[8]{};
    ID3D11DepthStencilView* oldDsv = nullptr;
    immediateCtx->OMGetRenderTargets(8, oldRtvs, &oldDsv);

    UINT oldNumVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    immediateCtx->RSGetViewports(&oldNumVps, oldVps);

    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBlendFactor[4]{};
    UINT oldSampleMask = 0xFFFFFFFF;
    immediateCtx->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);

    ID3D11DepthStencilState* oldDepth = nullptr;
    UINT oldStencilRef = 0;
    immediateCtx->OMGetDepthStencilState(&oldDepth, &oldStencilRef);

    ID3D11RasterizerState* oldRast = nullptr;
    immediateCtx->RSGetState(&oldRast);

    ID3D11InputLayout* oldLayout = nullptr;
    immediateCtx->IAGetInputLayout(&oldLayout);

    D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    immediateCtx->IAGetPrimitiveTopology(&oldTopo);

    ID3D11Buffer* oldVb = nullptr;
    UINT oldStride = 0, oldOffset = 0;
    immediateCtx->IAGetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);

    ID3D11VertexShader* oldVs = nullptr;
    ID3D11ClassInstance* oldVsInst[8]{};
    UINT oldVsInstCount = 8;
    immediateCtx->VSGetShader(&oldVs, oldVsInst, &oldVsInstCount);

    ID3D11PixelShader* oldPs = nullptr;
    ID3D11ClassInstance* oldPsInst[8]{};
    UINT oldPsInstCount = 8;
    immediateCtx->PSGetShader(&oldPs, oldPsInst, &oldPsInstCount);

    ID3D11GeometryShader* oldGs = nullptr;
    ID3D11ClassInstance* oldGsInst[8]{};
    UINT oldGsInstCount = 8;
    immediateCtx->GSGetShader(&oldGs, oldGsInst, &oldGsInstCount);

    ID3D11HullShader* oldHs = nullptr;
    ID3D11ClassInstance* oldHsInst[8]{};
    UINT oldHsInstCount = 8;
    immediateCtx->HSGetShader(&oldHs, oldHsInst, &oldHsInstCount);

    ID3D11DomainShader* oldDs = nullptr;
    ID3D11ClassInstance* oldDsInst[8]{};
    UINT oldDsInstCount = 8;
    immediateCtx->DSGetShader(&oldDs, oldDsInst, &oldDsInstCount);

    ID3D11ShaderResourceView* oldSrv = nullptr;
    immediateCtx->PSGetShaderResources(0, 1, &oldSrv);

    ID3D11SamplerState* oldSamp = nullptr;
    immediateCtx->PSGetSamplers(0, 1, &oldSamp);

    // Render HUD
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = screenW;
    vp.Height = screenH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    const UINT stride = sizeof(OverlayVertex);
    const UINT offset = 0;
    const float blendFactors[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    immediateCtx->OMSetRenderTargets(1, &rtv, nullptr);
    immediateCtx->RSSetViewports(1, &vp);
    immediateCtx->OMSetBlendState(blendState_, blendFactors, 0xFFFFFFFF);
    immediateCtx->OMSetDepthStencilState(depthState_, 0);
    immediateCtx->RSSetState(rastState_);
    immediateCtx->IASetInputLayout(inputLayout_);
    immediateCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    immediateCtx->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    immediateCtx->VSSetShader(vs_, nullptr, 0);
    immediateCtx->GSSetShader(nullptr, nullptr, 0);
    immediateCtx->HSSetShader(nullptr, nullptr, 0);
    immediateCtx->DSSetShader(nullptr, nullptr, 0);
    immediateCtx->PSSetShader(ps_, nullptr, 0);
    immediateCtx->PSSetShaderResources(0, 1, &fontSrv_);
    immediateCtx->PSSetSamplers(0, 1, &pointSampler_);

    immediateCtx->Draw(static_cast<UINT>(verts.size()), 0);

    // Restore original context state
    immediateCtx->PSSetSamplers(0, 1, &oldSamp);
    immediateCtx->PSSetShaderResources(0, 1, &oldSrv);
    immediateCtx->PSSetShader(oldPs, oldPsInst, oldPsInstCount);
    immediateCtx->DSSetShader(oldDs, oldDsInst, oldDsInstCount);
    immediateCtx->HSSetShader(oldHs, oldHsInst, oldHsInstCount);
    immediateCtx->GSSetShader(oldGs, oldGsInst, oldGsInstCount);
    immediateCtx->VSSetShader(oldVs, oldVsInst, oldVsInstCount);
    immediateCtx->IASetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOffset);
    immediateCtx->IASetPrimitiveTopology(oldTopo);
    immediateCtx->IASetInputLayout(oldLayout);
    immediateCtx->RSSetState(oldRast);
    immediateCtx->OMSetDepthStencilState(oldDepth, oldStencilRef);
    immediateCtx->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    if (oldNumVps > 0) {
        immediateCtx->RSSetViewports(oldNumVps, oldVps);
    }
    immediateCtx->OMSetRenderTargets(8, oldRtvs, oldDsv);

    // Release temporary COM references acquired by Get* calls and RTV.
    for (UINT i = 0; i < oldVsInstCount; ++i) SafeRelease(oldVsInst[i]);
    for (UINT i = 0; i < oldPsInstCount; ++i) SafeRelease(oldPsInst[i]);
    for (UINT i = 0; i < oldGsInstCount; ++i) SafeRelease(oldGsInst[i]);
    for (UINT i = 0; i < oldHsInstCount; ++i) SafeRelease(oldHsInst[i]);
    for (UINT i = 0; i < oldDsInstCount; ++i) SafeRelease(oldDsInst[i]);
    SafeRelease(oldSamp);
    SafeRelease(oldSrv);
    SafeRelease(oldPs);
    SafeRelease(oldDs);
    SafeRelease(oldHs);
    SafeRelease(oldGs);
    SafeRelease(oldVs);
    SafeRelease(oldVb);
    SafeRelease(oldLayout);
    SafeRelease(oldRast);
    SafeRelease(oldDepth);
    SafeRelease(oldBlend);
    SafeRelease(oldDsv);
    for (auto*& r : oldRtvs) SafeRelease(r);
    SafeRelease(rtv);
}

} // namespace lotro_uie
