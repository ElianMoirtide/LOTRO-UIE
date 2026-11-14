#include "dx11/ui_sr_cache.h"
#include "common/logger.h"
#include "dx11/d3d_compile_util.h"
#include "dx11/resource_tracker.h"
#include "dx11/ui_sr_weights.h"

#include <algorithm>
#include <cstring>

namespace lotro_uie {

namespace {

// {A8C3F190-5B2E-4C17-9D82-4E6A2018B401}
constexpr GUID kGuidUiSrDestroySentinel = {
    0xa8c3f190, 0x5b2e, 0x4c17, { 0x9d, 0x82, 0x4e, 0x6a, 0x20, 0x18, 0xb4, 0x01 }
};

class DestroySentinel final : public IUnknown {
public:
    DestroySentinel(ID3D11Texture2D* tex, uint64_t generation)
        : tex_(tex), generation_(generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppvObject = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = static_cast<ULONG>(refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1);
        if (r == 0) {
            UiSrCache::Instance().OnSourceTextureDestroyed(tex_, generation_);
            delete this;
        }
        return r;
    }

private:
    std::atomic<uint32_t> refCount_{1};
    ID3D11Texture2D* tex_ = nullptr;
    uint64_t generation_ = 0;
};

struct UiSrWeightsGpu {
    float w1[576];  float b1[16];
    float w2[2304]; float b2[16];
    float w3[2304]; float b3[16];
    float w4[2304]; float b4[16];
};
static_assert(sizeof(UiSrWeightsGpu) == (144 + 4 + 576 + 4 + 576 + 4 + 576 + 4) * 16, "CB size mismatch");

struct UiSrDimsGpu {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t wrapU = 0;
    uint32_t wrapV = 0;
};

bool IsSrgbFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

bool IsSupportedUiSourceFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

const char* kCsHeader = R"hlsl(
cbuffer UiSrWeightsCB : register(b0) {
    float4 w1[144]; float4 b1[4];
    float4 w2[576]; float4 b2[4];
    float4 w3[576]; float4 b3[4];
    float4 w4[576]; float4 b4[4];
};
cbuffer UiSrDimsCB : register(b1) {
    uint g_w;
    uint g_h;
    uint g_wrapU;
    uint g_wrapV;
};
int2 WrapOrClamp(int2 p) {
    int x = ((g_wrapU & 1u) != 0u) ? ((p.x % (int)g_w + (int)g_w) % (int)g_w) : clamp(p.x, 0, (int)g_w - 1);
    int y = ((g_wrapV & 1u) != 0u) ? ((p.y % (int)g_h + (int)g_h) % (int)g_h) : clamp(p.y, 0, (int)g_h - 1);
    return int2(x, y);
}
float4 LeakyRelu(float4 v) { return max(v, v * 0.1f); }
float4 ToSrgbIfNeeded(float4 c) {
    if ((g_wrapU & 2u) != 0u) {
        c.rgb = pow(max(c.rgb, 0.0f), 1.0f / 2.2f);
    }
    return c;
}
)hlsl";

const char* kCsConv1 = R"hlsl(
Texture2D<float4> srcTex : register(t0);
RWTexture2DArray<float4> featOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) return;
    float4 acc0 = b1[0], acc1 = b1[1], acc2 = b1[2], acc3 = b1[3];
    int k = 0;
    [unroll] for (int ky = -1; ky <= 1; ++ky) {
        [unroll] for (int kx = -1; kx <= 1; ++kx) {
            int2 p = WrapOrClamp(int2(id.xy) + int2(kx, ky));
            float4 f = ToSrgbIfNeeded(srcTex.Load(int3(p, 0)));
            int off0 = ((0 * 9 + k) * 1 + 0) * 4;
            int off1 = ((1 * 9 + k) * 1 + 0) * 4;
            int off2 = ((2 * 9 + k) * 1 + 0) * 4;
            int off3 = ((3 * 9 + k) * 1 + 0) * 4;
            acc0 += f.x * w1[off0+0] + f.y * w1[off0+1] + f.z * w1[off0+2] + f.w * w1[off0+3];
            acc1 += f.x * w1[off1+0] + f.y * w1[off1+1] + f.z * w1[off1+2] + f.w * w1[off1+3];
            acc2 += f.x * w1[off2+0] + f.y * w1[off2+1] + f.z * w1[off2+2] + f.w * w1[off2+3];
            acc3 += f.x * w1[off3+0] + f.y * w1[off3+1] + f.z * w1[off3+2] + f.w * w1[off3+3];
            k++;
        }
    }
    featOut[uint3(id.xy, 0)] = LeakyRelu(acc0);
    featOut[uint3(id.xy, 1)] = LeakyRelu(acc1);
    featOut[uint3(id.xy, 2)] = LeakyRelu(acc2);
    featOut[uint3(id.xy, 3)] = LeakyRelu(acc3);
}
)hlsl";

const char* kCsConv2 = R"hlsl(
Texture2DArray<float4> featIn : register(t0);
RWTexture2DArray<float4> featOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) return;
    float4 acc0 = b2[0], acc1 = b2[1], acc2 = b2[2], acc3 = b2[3];
    int k = 0;
    [unroll] for (int ky = -1; ky <= 1; ++ky) {
        [unroll] for (int kx = -1; kx <= 1; ++kx) {
            int2 p = WrapOrClamp(int2(id.xy) + int2(kx, ky));
            [unroll] for (int ig = 0; ig < 4; ++ig) {
                float4 f = featIn.Load(int4(p, ig, 0));
                int off0 = ((0 * 9 + k) * 4 + ig) * 4;
                int off1 = ((1 * 9 + k) * 4 + ig) * 4;
                int off2 = ((2 * 9 + k) * 4 + ig) * 4;
                int off3 = ((3 * 9 + k) * 4 + ig) * 4;
                acc0 += f.x * w2[off0+0] + f.y * w2[off0+1] + f.z * w2[off0+2] + f.w * w2[off0+3];
                acc1 += f.x * w2[off1+0] + f.y * w2[off1+1] + f.z * w2[off1+2] + f.w * w2[off1+3];
                acc2 += f.x * w2[off2+0] + f.y * w2[off2+1] + f.z * w2[off2+2] + f.w * w2[off2+3];
                acc3 += f.x * w2[off3+0] + f.y * w2[off3+1] + f.z * w2[off3+2] + f.w * w2[off3+3];
            }
            k++;
        }
    }
    featOut[uint3(id.xy, 0)] = LeakyRelu(acc0);
    featOut[uint3(id.xy, 1)] = LeakyRelu(acc1);
    featOut[uint3(id.xy, 2)] = LeakyRelu(acc2);
    featOut[uint3(id.xy, 3)] = LeakyRelu(acc3);
}
)hlsl";

const char* kCsConv3 = R"hlsl(
Texture2DArray<float4> featIn : register(t0);
Texture2DArray<float4> featSkip : register(t1);
RWTexture2DArray<float4> featOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) return;
    float4 acc0 = b3[0] + featSkip.Load(int4(id.xy, 0, 0));
    float4 acc1 = b3[1] + featSkip.Load(int4(id.xy, 1, 0));
    float4 acc2 = b3[2] + featSkip.Load(int4(id.xy, 2, 0));
    float4 acc3 = b3[3] + featSkip.Load(int4(id.xy, 3, 0));
    int k = 0;
    [unroll] for (int ky = -1; ky <= 1; ++ky) {
        [unroll] for (int kx = -1; kx <= 1; ++kx) {
            int2 p = WrapOrClamp(int2(id.xy) + int2(kx, ky));
            [unroll] for (int ig = 0; ig < 4; ++ig) {
                float4 f = featIn.Load(int4(p, ig, 0));
                int off0 = ((0 * 9 + k) * 4 + ig) * 4;
                int off1 = ((1 * 9 + k) * 4 + ig) * 4;
                int off2 = ((2 * 9 + k) * 4 + ig) * 4;
                int off3 = ((3 * 9 + k) * 4 + ig) * 4;
                acc0 += f.x * w3[off0+0] + f.y * w3[off0+1] + f.z * w3[off0+2] + f.w * w3[off0+3];
                acc1 += f.x * w3[off1+0] + f.y * w3[off1+1] + f.z * w3[off1+2] + f.w * w3[off1+3];
                acc2 += f.x * w3[off2+0] + f.y * w3[off2+1] + f.z * w3[off2+2] + f.w * w3[off2+3];
                acc3 += f.x * w3[off3+0] + f.y * w3[off3+1] + f.z * w3[off3+2] + f.w * w3[off3+3];
            }
            k++;
        }
    }
    featOut[uint3(id.xy, 0)] = LeakyRelu(acc0);
    featOut[uint3(id.xy, 1)] = LeakyRelu(acc1);
    featOut[uint3(id.xy, 2)] = LeakyRelu(acc2);
    featOut[uint3(id.xy, 3)] = LeakyRelu(acc3);
}
)hlsl";

const char* kCsConv4 = R"hlsl(
Texture2D<float4> srcTex : register(t0);
Texture2DArray<float4> featIn : register(t1);
RWTexture2D<unorm float4> outTex : register(u0);

float SrColDiff(float4 a, float4 b) {
    float3 d = a.rgb - b.rgb;
    return sqrt(dot(d, d) + (a.a - b.a) * (a.a - b.a) * 0.65f);
}

float4 EvalBaseQ(float4 cC, float4 cX, float4 cY, float4 cD,
                 float4 cOppX, float4 cOppY, float4 cX_OppY, float4 cY_OppX, float4 cOppD) {
    float dCX = SrColDiff(cC, cX);
    float dCY = SrColDiff(cC, cY);
    float dXY = SrColDiff(cX, cY);
    float dCD = SrColDiff(cC, cD);
    float dCOppX = SrColDiff(cC, cOppX);
    float dCOppY = SrColDiff(cC, cOppY);
    float dXOppX = SrColDiff(cX, cOppX);
    float dYOppY = SrColDiff(cY, cOppY);

    // EPX/Scale2x topology guard: only chamfer outer corners when cC continues along OppX & OppY.
    // Prevents isolated 1x1 dots (i-dots, umlauts) and 1px stroke caps from being chamfer-eroded.
    float epxGuard  = smoothstep(0.06f, 0.22f, min(dXOppX, dYOppY));
    float cornerRaw = smoothstep(0.06f, 0.25f, min(dCX, dCY) - dXY * 1.15f) * epxGuard;
    float diagRaw   = smoothstep(0.06f, 0.25f, min(dCX, dCY) - dCD * 1.15f);
    float cOuter  = exp(-dCOppX * 8.0f) + exp(-dCOppY * 8.0f);
    float xyOuter = exp(-SrColDiff(cX, cX_OppY) * 8.0f) + exp(-SrColDiff(cY, cY_OppX) * 8.0f);
    bool useDiag = (diagRaw > cornerRaw * 0.65f) && (cOuter <= xyOuter + 0.30f) && (diagRaw > 0.05f);

    // Peak preservation for 1px thin stems and isolated 1x1 dots (never dims i-dots or thin letters)
    float dDiagMin = min(min(dCD, SrColDiff(cC, cOppD)), min(SrColDiff(cC, cX_OppY), SrColDiff(cC, cY_OppX)));
    float notDiagCorner = smoothstep(0.08f, 0.24f, dDiagMin);
    float thinX = smoothstep(0.14f, 0.42f, min(dCX, dCOppX) - dXOppX * 0.75f);
    float thinY = smoothstep(0.14f, 0.42f, min(dCY, dCOppY) - dYOppY * 0.75f);
    float ridgeKeep = saturate(max(thinX, thinY) * 0.65f + thinX * thinY * 0.28f) *
                      lerp(0.15f, 1.0f, notDiagCorner);

    float4 bil = 0.5625f * cC + 0.1875f * (cX + cY) + 0.0625f * cD;
    bil = lerp(bil, cC, ridgeKeep * 0.80f);
    if (useDiag) {
        float4 bridge = 0.5f * (cC + cD);
        return lerp(bil, bridge, 0.78f * diagRaw);
    }
    float4 chamfer = 0.5f * (cX + cY);
    return lerp(bil, chamfer, 0.62f * cornerRaw);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) return;
    float4 acc0 = b4[0], acc1 = b4[1], acc2 = b4[2], acc3 = b4[3];
    int k = 0;
    [unroll] for (int ky = -1; ky <= 1; ++ky) {
        [unroll] for (int kx = -1; kx <= 1; ++kx) {
            int2 p = WrapOrClamp(int2(id.xy) + int2(kx, ky));
            [unroll] for (int ig = 0; ig < 4; ++ig) {
                float4 f = featIn.Load(int4(p, ig, 0));
                int off0 = ((0 * 9 + k) * 4 + ig) * 4;
                int off1 = ((1 * 9 + k) * 4 + ig) * 4;
                int off2 = ((2 * 9 + k) * 4 + ig) * 4;
                int off3 = ((3 * 9 + k) * 4 + ig) * 4;
                acc0 += f.x * w4[off0+0] + f.y * w4[off0+1] + f.z * w4[off0+2] + f.w * w4[off0+3];
                acc1 += f.x * w4[off1+0] + f.y * w4[off1+1] + f.z * w4[off1+2] + f.w * w4[off1+3];
                acc2 += f.x * w4[off2+0] + f.y * w4[off2+1] + f.z * w4[off2+2] + f.w * w4[off2+3];
                acc3 += f.x * w4[off3+0] + f.y * w4[off3+1] + f.z * w4[off3+2] + f.w * w4[off3+3];
            }
            k++;
        }
    }
    int2 xy = int2(id.xy);
    float4 cC  = ToSrgbIfNeeded(srcTex.Load(int3(xy, 0)));
    float4 cL  = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2(-1,  0)), 0)));
    float4 cR  = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2( 1,  0)), 0)));
    float4 cU  = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2( 0, -1)), 0)));
    float4 cD  = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2( 0,  1)), 0)));
    float4 cUL = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2(-1, -1)), 0)));
    float4 cUR = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2( 1, -1)), 0)));
    float4 cDL = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2(-1,  1)), 0)));
    float4 cDR = ToSrgbIfNeeded(srcTex.Load(int3(WrapOrClamp(xy + int2( 1,  1)), 0)));

    float4 min9 = min(min(min(cC, min(cL, cR)), min(cU, cD)), min(min(cUL, cUR), min(cDL, cDR)));
    float4 max9 = max(max(max(cC, max(cL, cR)), max(cU, cD)), max(max(cUL, cUR), max(cDL, cDR)));
    float4 lo = saturate(min9 - 0.004f);
    float4 hi = saturate(max9 + 0.004f);

    float4 range9 = max9 - min9;
    float maxRange9 = max(max(range9.r, range9.g), max(range9.b, range9.a));
    float detailGate = smoothstep(0.025f, 0.095f, maxRange9);

    // Neutral-chroma guard: prevent colored (e.g. green) chroma halos around neutral greyscale/white UI text
    float maxChroma9 = 0.0f;
    {
        float4 nb[9] = { cC, cL, cR, cU, cD, cUL, cUR, cDL, cDR };
        [unroll] for (int ii = 0; ii < 9; ++ii) {
            float cm = max(nb[ii].r, max(nb[ii].g, nb[ii].b)) - min(nb[ii].r, min(nb[ii].g, nb[ii].b));
            maxChroma9 = max(maxChroma9, cm);
        }
    }
    float chromaKeep = smoothstep(0.02f, 0.16f, maxChroma9);

    float4 base00 = EvalBaseQ(cC, cL, cU, cUL, cR, cD, cDL, cUR, cDR);
    float4 base01 = EvalBaseQ(cC, cR, cU, cUR, cL, cD, cDR, cUL, cDL);
    float4 base10 = EvalBaseQ(cC, cL, cD, cDL, cR, cU, cUL, cDR, cUR);
    float4 base11 = EvalBaseQ(cC, cR, cD, cDR, cL, cU, cUR, cDL, cUL);

    float4 accs[4] = { acc0, acc1, acc2, acc3 };
    [unroll] for (int ii = 0; ii < 4; ++ii) {
        float dL = dot(accs[ii].rgb, float3(0.299f, 0.587f, 0.114f));
        accs[ii].rgb = float3(dL, dL, dL) + (accs[ii].rgb - dL) * chromaKeep;
    }

    uint2 o = id.xy * 2;
    outTex[o + uint2(0, 0)] = clamp(base00 + accs[0] * detailGate, lo, hi);
    outTex[o + uint2(1, 0)] = clamp(base01 + accs[1] * detailGate, lo, hi);
    outTex[o + uint2(0, 1)] = clamp(base10 + accs[2] * detailGate, lo, hi);
    outTex[o + uint2(1, 1)] = clamp(base11 + accs[3] * detailGate, lo, hi);
}
)hlsl";

std::mutex g_retiredMutex;

} // namespace

UiSrCache& UiSrCache::Instance() {
    static auto* inst = new UiSrCache();
    return *inst;
}

void UiSrCache::InitializeFromConfig(const Config& cfg) {
    bool expected = false;
    if (!initializedFromConfig_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    ReloadFromConfig(cfg);
}

void UiSrCache::ReloadFromConfig(const Config& cfg) {
    initializedFromConfig_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_.store(cfg.cnnEnabled && cfg.enabled, std::memory_order_release);
    upscaleUiRts_ = cfg.cnnUpscaleUiRenderTargets;
    cacheMaxMb_ = (std::max)(cfg.cnnCacheMaxMb, 16u);
    maxNewPerFrame_ = cfg.cnnMaxNewPerFrame;
    maxDynamicPerFrame_ = cfg.cnnMaxDynamicPerFrame;
    UIE_LOG_INFO("UiSrCache configured: enabled=%s upscaleRts=%s cacheMaxMb=%u maxNew=%u maxDyn=%u",
                 enabled_.load(std::memory_order_relaxed) ? "true" : "false",
                 upscaleUiRts_ ? "true" : "false",
                 cacheMaxMb_, maxNewPerFrame_, maxDynamicPerFrame_);
}

void UiSrCache::ToggleEnabled() {
    const bool next = !enabled_.load(std::memory_order_relaxed);
    enabled_.store(next, std::memory_order_release);
    UIE_LOG_INFO("Mini-CNN 2x Super-Resolution toggled: %s", next ? "ON" : "OFF");
}

bool UiSrCache::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

void UiSrCache::OnFrameBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frameIndex_;
    newUpscalesInFrame_ = 0;
    dynRebuildsInFrame_ = 0;
    hitsInFrame_ = 0;
    missesInFrame_ = 0;
    DrainRetiredLocked();
    if (enabled_.load(std::memory_order_relaxed)) {
        const uint64_t budgetBytes = static_cast<uint64_t>(cacheMaxMb_) * 1024ull * 1024ull;
        EvictToBudgetLocked(budgetBytes);
    }
}

void UiSrCache::MarkResourceDirty(ID3D11Resource* res) {
    if (!res || ResourceTracker::Instance().IsShuttingDown() || shuttingDown_.load(std::memory_order_acquire)) return;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(res);
    std::lock_guard<std::mutex> lock(mutex_);
    if (texGenerations_.find(tex) != texGenerations_.end()) {
        ++dirtyEpochs_[tex];
    }
}

void UiSrCache::MarkRenderTargetViewDirty(ID3D11RenderTargetView* rtv) {
    if (!rtv || ResourceTracker::Instance().IsShuttingDown() || shuttingDown_.load(std::memory_order_acquire)) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (res) {
        MarkResourceDirty(res);
        res->Release();
    }
}

void UiSrCache::OnSourceTextureDestroyed(ID3D11Texture2D* tex, uint64_t generation) {
    if (!tex || ResourceTracker::Instance().IsShuttingDown() || shuttingDown_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> qlock(g_retiredMutex);
    retiredQueue_.push_back(RetiredTex{tex, generation});
}

void UiSrCache::ReleaseEntryResources(CacheEntry& e) {
    if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
    if (e.outSrv) { e.outSrv->Release(); e.outSrv = nullptr; }
    if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
    if (totalBytes_ >= e.byteSize) totalBytes_ -= e.byteSize;
    else totalBytes_ = 0;
    e.byteSize = 0;
}

void UiSrCache::DrainRetiredLocked() {
    std::vector<RetiredTex> local;
    {
        std::lock_guard<std::mutex> qlock(g_retiredMutex);
        if (retiredQueue_.empty()) return;
        local.swap(retiredQueue_);
    }
    for (const RetiredTex& r : local) {
        auto git = texGenerations_.find(r.tex);
        if (git != texGenerations_.end() && git->second == r.generation) {
            texGenerations_.erase(git);
            dirtyEpochs_.erase(r.tex);
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.tex == r.tex && it->second.generation == r.generation) {
                ReleaseEntryResources(it->second);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void UiSrCache::EvictToBudgetLocked(uint64_t maxBytes) {
    while (totalBytes_ > maxBytes && !entries_.empty()) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.lastUsedFrame < oldest->second.lastUsedFrame) {
                oldest = it;
            }
        }
        ReleaseEntryResources(oldest->second);
        entries_.erase(oldest);
    }
}

bool UiSrCache::EnsureInitialized(ID3D11Device* device) {
    if (!device) return false;
    if (initialized_ && device_ == device) return true;
    if (device_ && device_ != device) {
        Shutdown();
        shuttingDown_.store(false, std::memory_order_release);
    }
    if (initFailed_) return false;

    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        UIE_LOG_WARN("UiSrCache disabled: feature level < 11.0");
        initFailed_ = true;
        return false;
    }

    auto compileCs = [&](const char* name, const char* body, ID3D11ComputeShader** outCs) -> bool {
        const std::string src = std::string(kCsHeader) + body;
        ID3DBlob* blob = nullptr;
        if (!CompileHlslBlob(src.c_str(), src.size(), name, nullptr, "main", "cs_5_0", &blob) || !blob) {
            return false;
        }
        const HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, outCs);
        blob->Release();
        return SUCCEEDED(hr) && *outCs != nullptr;
    };

    if (!compileCs("ui_sr_conv1", kCsConv1, &csConv1_) ||
        !compileCs("ui_sr_conv2", kCsConv2, &csConv2_) ||
        !compileCs("ui_sr_conv3", kCsConv3, &csConv3_) ||
        !compileCs("ui_sr_conv4", kCsConv4, &csConv4_)) {
        ReleaseScratch();
        if (csConv1_) { csConv1_->Release(); csConv1_ = nullptr; }
        if (csConv2_) { csConv2_->Release(); csConv2_ = nullptr; }
        if (csConv3_) { csConv3_->Release(); csConv3_ = nullptr; }
        if (csConv4_) { csConv4_->Release(); csConv4_ = nullptr; }
        initFailed_ = true;
        return false;
    }

    UiSrWeightsGpu weights{};
    std::memcpy(weights.w1, kUiSrConv1Weights, sizeof(weights.w1));
    std::memcpy(weights.b1, kUiSrConv1Biases,  sizeof(weights.b1));
    std::memcpy(weights.w2, kUiSrConv2Weights, sizeof(weights.w2));
    std::memcpy(weights.b2, kUiSrConv2Biases,  sizeof(weights.b2));
    std::memcpy(weights.w3, kUiSrConv3Weights, sizeof(weights.w3));
    std::memcpy(weights.b3, kUiSrConv3Biases,  sizeof(weights.b3));
    std::memcpy(weights.w4, kUiSrConv4Weights, sizeof(weights.w4));
    std::memcpy(weights.b4, kUiSrConv4Biases,  sizeof(weights.b4));

    D3D11_BUFFER_DESC wDesc{};
    wDesc.ByteWidth = sizeof(weights);
    wDesc.Usage = D3D11_USAGE_IMMUTABLE;
    wDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA wInit{ &weights, 0, 0 };
    if (FAILED(device->CreateBuffer(&wDesc, &wInit, &weightsCb_)) || !weightsCb_) {
        initFailed_ = true;
        return false;
    }

    D3D11_BUFFER_DESC dDesc{};
    dDesc.ByteWidth = sizeof(UiSrDimsGpu);
    dDesc.Usage = D3D11_USAGE_DEFAULT;
    dDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&dDesc, nullptr, &dimsCb_)) || !dimsCb_) {
        initFailed_ = true;
        return false;
    }

    device_ = device;
    initialized_ = true;
    UIE_LOG_INFO("UiSrCache initialized (4-layer 2x Residual Subpixel CNN ready)");
    return true;
}

bool UiSrCache::EnsureScratch(ID3D11Device* device, uint32_t w, uint32_t h) {
    if (scratchW_ >= w && scratchH_ >= h && featTex_[0] && featTex_[1] && featTex_[2]) {
        return true;
    }
    const uint32_t newW = (std::max)(scratchW_, (std::max)(w, 256u));
    const uint32_t newH = (std::max)(scratchH_, (std::max)(h, 256u));
    ReleaseScratch();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = newW;
    td.Height = newH;
    td.MipLevels = 1;
    td.ArraySize = 4;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = 4;

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = td.Format;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    ud.Texture2DArray.ArraySize = 4;

    for (int i = 0; i < 3; ++i) {
        if (FAILED(device->CreateTexture2D(&td, nullptr, &featTex_[i])) || !featTex_[i] ||
            FAILED(device->CreateShaderResourceView(featTex_[i], &sd, &featSrv_[i])) || !featSrv_[i] ||
            FAILED(device->CreateUnorderedAccessView(featTex_[i], &ud, &featUav_[i])) || !featUav_[i]) {
            ReleaseScratch();
            return false;
        }
    }
    scratchW_ = newW;
    scratchH_ = newH;
    return true;
}

void UiSrCache::ReleaseScratch() {
    for (int i = 0; i < 3; ++i) {
        if (featUav_[i]) { featUav_[i]->Release(); featUav_[i] = nullptr; }
        if (featSrv_[i]) { featSrv_[i]->Release(); featSrv_[i] = nullptr; }
        if (featTex_[i]) { featTex_[i]->Release(); featTex_[i] = nullptr; }
    }
    scratchW_ = 0;
    scratchH_ = 0;
}

bool UiSrCache::RunCnnPass(
    ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView* srcSrv,
    uint32_t w,
    uint32_t h,
    bool wrapU,
    bool wrapV,
    ID3D11UnorderedAccessView* outUav
) {
    if (!ctx || !srcSrv || !outUav) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srcDesc{};
    srcSrv->GetDesc(&srcDesc);
    const bool isSrgb = IsSrgbFormat(srcDesc.Format);

    // Save existing CS state cleanly
    ID3D11ComputeShader* prevCs = nullptr;
    ID3D11ClassInstance* prevInst[8]{};
    UINT prevInstCount = 8;
    ctx->CSGetShader(&prevCs, prevInst, &prevInstCount);

    ID3D11ShaderResourceView* prevSrvs[2]{};
    ctx->CSGetShaderResources(0, 2, prevSrvs);

    ID3D11UnorderedAccessView* prevUavs[1]{};
    ctx->CSGetUnorderedAccessViews(0, 1, prevUavs);

    ID3D11Buffer* prevCbs[2]{};
    ctx->CSGetConstantBuffers(0, 2, prevCbs);

    // Pack wrap flags + sRGB conversion bit into dims CB
    UiSrDimsGpu dims{};
    dims.width = w;
    dims.height = h;
    dims.wrapU = (wrapU ? 1u : 0u) | (isSrgb ? 2u : 0u);
    dims.wrapV = (wrapV ? 1u : 0u);
    ctx->UpdateSubresource(dimsCb_, 0, nullptr, &dims, 0, 0);

    ID3D11Buffer* cbs[2] = { weightsCb_, dimsCb_ };
    ctx->CSSetConstantBuffers(0, 2, cbs);

    const UINT gx = (w + 7u) / 8u;
    const UINT gy = (h + 7u) / 8u;
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUav[1] = { nullptr };

    // Pass 1: Conv1 (src -> feat0)
    ctx->CSSetShader(csConv1_, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srcSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &featUav_[0], nullptr);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSrvs);

    // Pass 2: Conv2 (feat0 -> feat1)
    ctx->CSSetShader(csConv2_, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &featSrv_[0]);
    ctx->CSSetUnorderedAccessViews(0, 1, &featUav_[1], nullptr);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSrvs);

    // Pass 3: Conv3 + Skip(feat0) (feat1 + feat0 -> feat2)
    ID3D11ShaderResourceView* p3Srvs[2] = { featSrv_[1], featSrv_[0] };
    ctx->CSSetShader(csConv3_, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, p3Srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &featUav_[2], nullptr);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSrvs);

    // Pass 4: Conv4 Subpixel + Bilinear2x Skip (src + feat2 -> 2W x 2H RGBA8 output)
    ID3D11ShaderResourceView* p4Srvs[2] = { srcSrv, featSrv_[2] };
    ctx->CSSetShader(csConv4_, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, p4Srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSrvs);

    // Restore caller CS state
    ctx->CSSetConstantBuffers(0, 2, prevCbs);
    ctx->CSSetUnorderedAccessViews(0, 1, prevUavs, nullptr);
    ctx->CSSetShaderResources(0, 2, prevSrvs);
    ctx->CSSetShader(prevCs, prevInst, prevInstCount);

    for (UINT i = 0; i < prevInstCount; ++i) {
        if (prevInst[i]) prevInst[i]->Release();
    }
    if (prevCs) prevCs->Release();
    for (auto* s : prevSrvs) if (s) s->Release();
    for (auto* u : prevUavs) if (u) u->Release();
    for (auto* b : prevCbs) if (b) b->Release();

    return true;
}

ID3D11ShaderResourceView* UiSrCache::GetOrUpscale(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView* srcSrv,
    bool wrapU,
    bool wrapV
) {
    if (!enabled_.load(std::memory_order_acquire) || !device || !ctx || !srcSrv) return nullptr;
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srcSrv->GetDesc(&srvDesc);
    if (srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return nullptr;
    if (srvDesc.Texture2D.MostDetailedMip != 0) return nullptr;
    if (!IsSupportedUiSourceFormat(srvDesc.Format)) return nullptr;

    ID3D11Resource* res = nullptr;
    srcSrv->GetResource(&res);
    if (!res) return nullptr;
    ID3D11Texture2D* srcTex = nullptr;
    const HRESULT qiHr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
    res->Release();
    if (FAILED(qiHr) || !srcTex) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    srcTex->GetDesc(&td);

    // Strict UI/Map/Offscreen-UI-RT eligibility: single-mip, non-depth, non-MSAA, practical UI dimensions & pixel cap
    const uint64_t pixelCount = static_cast<uint64_t>(td.Width) * td.Height;
    const bool isRenderTarget = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    const bool rtAllowed = !isRenderTarget ||
                           (upscaleUiRts_ && td.Width <= 1920 && td.Height <= 1200 &&
                            !ResourceTracker::Instance().IsBackbufferTexture(srcTex));
    const bool eligible =
        td.Width >= 16 && td.Height >= 16 &&
        td.Width <= 2304 && td.Height <= 2304 &&
        pixelCount <= 2359296ULL &&
        td.ArraySize == 1 &&
        td.MipLevels == 1 &&
        td.SampleDesc.Count == 1 &&
        rtAllowed &&
        (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0;
    if (!eligible) {
        srcTex->Release();
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    DrainRetiredLocked();
    if (!EnsureInitialized(device)) {
        srcTex->Release();
        return nullptr;
    }

    // Track source texture lifetime with COM destruction sentinel
    uint64_t gen = 0;
    auto git = texGenerations_.find(srcTex);
    if (git == texGenerations_.end()) {
        gen = nextGeneration_++;
        texGenerations_[srcTex] = gen;
        auto* sentinel = new DestroySentinel(srcTex, gen);
        srcTex->SetPrivateDataInterface(kGuidUiSrDestroySentinel, sentinel);
        sentinel->Release(); // Device holds ref now
    } else {
        gen = git->second;
    }

    const uint64_t curDirty = dirtyEpochs_.count(srcTex) ? dirtyEpochs_[srcTex] : 0;
    const CacheKey key{ srcTex, wrapU, wrapV };
    auto it = entries_.find(key);

    if (it != entries_.end()) {
        CacheEntry& e = it->second;
        // Validate descriptor in case pointer was somehow reused cleanly before COM retire drained
        if (e.srcWidth == td.Width && e.srcHeight == td.Height && e.srcFormat == srvDesc.Format) {
            e.lastUsedFrame = frameIndex_;
            if (e.builtDirtyEpoch == curDirty) {
                if (e.lastDirtyFrame != frameIndex_) {
                    e.consecutiveDirtyFrames = 0;
                    if (e.cooldownFrames > 0) --e.cooldownFrames;
                }
                ++hitsInFrame_;
                srcTex->Release();
                return e.outSrv;
            }
            // Entry exists but source texture content changed -> dynamic update (counted once per frame)
            const bool firstDirtyCheckThisFrame = (e.lastDirtyFrame != frameIndex_);
            e.lastDirtyFrame = frameIndex_;
            if (e.cooldownFrames > 0) {
                if (firstDirtyCheckThisFrame) --e.cooldownFrames;
                srcTex->Release();
                return nullptr;
            }
            if (dynRebuildsInFrame_ >= maxDynamicPerFrame_) {
                // Return previous 2x frame texture instead of nullptr to prevent 2x/1x flicker
                ++hitsInFrame_;
                srcTex->Release();
                return e.outSrv;
            }
            if (firstDirtyCheckThisFrame) {
                ++e.consecutiveDirtyFrames;
            }
            if (e.consecutiveDirtyFrames >= 4) {
                // Streaming animated texture detected -> temporarily bypass CNN to guarantee 0 stutter
                e.cooldownFrames = 30;
                srcTex->Release();
                return nullptr;
            }
            if (!EnsureScratch(device, td.Width, td.Height) ||
                !RunCnnPass(ctx, srcSrv, td.Width, td.Height, wrapU, wrapV, e.outUav)) {
                srcTex->Release();
                return nullptr;
            }
            e.builtDirtyEpoch = curDirty;
            ++dynRebuildsInFrame_;
            ++missesInFrame_;
            ++totalUpscales_;
            srcTex->Release();
            return e.outSrv;
        }
        ReleaseEntryResources(e);
        entries_.erase(it);
    }

    // Cache miss -> check per-frame budget before allocating & running CNN
    if (newUpscalesInFrame_ >= maxNewPerFrame_) {
        srcTex->Release();
        return nullptr;
    }

    const uint64_t outBytes = static_cast<uint64_t>(td.Width * 2) * static_cast<uint64_t>(td.Height * 2) * 4ull;
    const uint64_t maxBytes = static_cast<uint64_t>(cacheMaxMb_) * 1024ull * 1024ull;
    if (totalBytes_ + outBytes > maxBytes) {
        EvictToBudgetLocked(maxBytes > outBytes ? (maxBytes - outBytes) : 0);
    }

    if (!EnsureScratch(device, td.Width, td.Height)) {
        srcTex->Release();
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = td.Width * 2;
    outDesc.Height = td.Height * 2;
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    CacheEntry entry{};
    entry.srcWidth = td.Width;
    entry.srcHeight = td.Height;
    entry.srcFormat = srvDesc.Format;
    entry.byteSize = outBytes;
    entry.generation = gen;
    entry.builtDirtyEpoch = curDirty;
    entry.lastUsedFrame = frameIndex_;

    D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
    outSrvDesc.Format = IsSrgbFormat(srvDesc.Format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    outSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Texture2D.MipLevels = 1;

    D3D11_UNORDERED_ACCESS_VIEW_DESC outUavDesc{};
    outUavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

    if (FAILED(device->CreateTexture2D(&outDesc, nullptr, &entry.outTex)) || !entry.outTex ||
        FAILED(device->CreateShaderResourceView(entry.outTex, &outSrvDesc, &entry.outSrv)) || !entry.outSrv ||
        FAILED(device->CreateUnorderedAccessView(entry.outTex, &outUavDesc, &entry.outUav)) || !entry.outUav) {
        ReleaseEntryResources(entry);
        srcTex->Release();
        return nullptr;
    }

    if (!RunCnnPass(ctx, srcSrv, td.Width, td.Height, wrapU, wrapV, entry.outUav)) {
        ReleaseEntryResources(entry);
        srcTex->Release();
        return nullptr;
    }

    totalBytes_ += outBytes;
    ++newUpscalesInFrame_;
    ++missesInFrame_;
    ++totalUpscales_;
    ID3D11ShaderResourceView* resultSrv = entry.outSrv;
    entries_.emplace(key, entry);

    srcTex->Release();
    return resultSrv;
}

UiSrStats UiSrCache::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    UiSrStats st{};
    st.initialized = initialized_;
    st.active = initialized_ && (hitsInFrame_ > 0 || missesInFrame_ > 0);
    st.entryCount = static_cast<uint32_t>(entries_.size());
    st.cacheBytesUsed = totalBytes_;
    st.hitsInFrame = hitsInFrame_;
    st.missesInFrame = missesInFrame_;
    st.dynamicRebuildsInFrame = dynRebuildsInFrame_;
    st.totalUpscales = totalUpscales_;
    return st;
}

void UiSrCache::Shutdown() {
    shuttingDown_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    {
        std::lock_guard<std::mutex> qlock(g_retiredMutex);
        retiredQueue_.clear();
    }
    for (auto& [k, e] : entries_) {
        ReleaseEntryResources(e);
    }
    entries_.clear();
    dirtyEpochs_.clear();
    texGenerations_.clear();
    totalBytes_ = 0;

    ReleaseScratch();
    if (dimsCb_) { dimsCb_->Release(); dimsCb_ = nullptr; }
    if (weightsCb_) { weightsCb_->Release(); weightsCb_ = nullptr; }
    if (csConv1_) { csConv1_->Release(); csConv1_ = nullptr; }
    if (csConv2_) { csConv2_->Release(); csConv2_ = nullptr; }
    if (csConv3_) { csConv3_->Release(); csConv3_ = nullptr; }
    if (csConv4_) { csConv4_->Release(); csConv4_ = nullptr; }

    device_ = nullptr;
    initialized_ = false;
    initFailed_ = false;
}

} // namespace lotro_uie
