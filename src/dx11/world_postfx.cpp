#include "dx11/world_postfx.h"
#include "common/logger.h"
#include "dx11/d3d_compile_util.h"
#include "dx11/ui_enhancer.h"

#include <algorithm>
#include <cstring>

namespace lotro_uie {

namespace {

struct WorldPostFxCB {
    float casStrength;
    float clarity;
    float vibrance;
    float debandEnabled;
    float dehaze;
    float antiGlare;
    float eyeComfortMode;
    float colorProfile;
    float rimSpecular;
    float vignette;
    float horizonSoften;
    float _pad0;
};
static_assert(sizeof(WorldPostFxCB) == 48, "WorldPostFxCB must be 48 bytes");

DXGI_FORMAT ToTypedBackbufferFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return fmt;
    }
}

const char* kWorldPostFxVs = R"hlsl(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid == 1) ? 2.0f : 0.0f, (vid == 2) ? 2.0f : 0.0f);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)hlsl";

const char* kWorldPostFxPs = R"hlsl(
Texture2D<float4> SceneTex : register(t0);
SamplerState PointSamp  : register(s0);
SamplerState LinearSamp : register(s1);
cbuffer WorldFxParams : register(b0) {
    float g_casStrength;
    float g_clarity;
    float g_vibrance;
    float g_debandEnabled;
    float g_dehaze;
    float g_antiGlare;
    float g_eyeComfortMode;
    float g_colorProfile;
    float g_rimSpecular;
    float g_vignette;
    float g_horizonSoften;
    float g_pad0;
};

float IgnNoise(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float Luma(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint tw, th;
    SceneTex.GetDimensions(tw, th);
    float2 px = 1.0f / max(float2((float)tw, (float)th), 1.0f);

    float4 center = SceneTex.SampleLevel(PointSamp, uv, 0.0f);
    if (g_debandEnabled >= 1.5f && pos.x < 0.5f * (float)tw) {
        return center;
    }
    float3 c = center.rgb;

    // 1) Gentle Gradient-Only Debanding (calms 8-bit sky/fog/water banding, leaves sharp detail 100% untouched)
    float n = IgnNoise(pos.xy);
    if (fmod(g_debandEnabled, 2.0f) > 0.5f) {
        float ang = n * 6.2831853f;
        float2 sc = float2(cos(ang), sin(ang)) * px;
        float2 scP = float2(-sc.y, sc.x);
        float r1 = 3.25f, r2 = 6.50f;
        float3 d0 = SceneTex.SampleLevel(LinearSamp, uv + sc * r1, 0.0f).rgb;
        float3 d1 = SceneTex.SampleLevel(LinearSamp, uv - sc * r1, 0.0f).rgb;
        float3 d2 = SceneTex.SampleLevel(LinearSamp, uv + scP * r2, 0.0f).rgb;
        float3 d3 = SceneTex.SampleLevel(LinearSamp, uv - scP * r2, 0.0f).rgb;
        float3 avgD = 0.25f * (d0 + d1 + d2 + d3);
        float maxDelta = max(max(length(d0 - c), length(d1 - c)), max(length(d2 - c), length(d3 - c)));
        // Tight smoothstep ensures foliage/stars/hair (>0.032) never blur
        float bandMask = smoothstep(0.032f, 0.007f, maxDelta);
        c = lerp(c, avgD, bandMask * 0.75f);
    }

    // 2) Contrast-Adaptive Sharpening (CAS 5-tap) + Horizon Silhouette Anti-Shimmer
    float3 nCol = SceneTex.SampleLevel(PointSamp, uv + float2( 0.0f, -px.y), 0.0f).rgb;
    float3 sCol = SceneTex.SampleLevel(PointSamp, uv + float2( 0.0f,  px.y), 0.0f).rgb;
    float3 wCol = SceneTex.SampleLevel(PointSamp, uv + float2(-px.x,  0.0f), 0.0f).rgb;
    float3 eCol = SceneTex.SampleLevel(PointSamp, uv + float2( px.x,  0.0f), 0.0f).rgb;

    float lC = Luma(c);
    float lN = Luma(nCol);
    float lS = Luma(sCol);
    float lW = Luma(wCol);
    float lE = Luma(eCol);
    float3 avgCross = 0.25f * (nCol + sCol + wCol + eCol);
    float min4L = min(lC, min(min(lN, lS), min(lW, lE)));
    float max4L = max(lC, max(max(lN, lS), max(lW, lE)));

    float skyEdgeMask = 0.0f;
    if (g_horizonSoften > 0.001f) {
        float skyBand = smoothstep(0.56f, 0.22f, uv.y);
        float isBrightSkyEdge = smoothstep(0.50f, 0.72f, max4L) *
                                smoothstep(0.48f, 0.26f, min4L) *
                                smoothstep(0.16f, 0.34f, max4L - min4L);
        skyEdgeMask = skyBand * isBrightSkyEdge * smoothstep(0.045f, 0.16f, abs(Luma(avgCross) - lC));
    }

    float3 minRGB = min(min(c, nCol), min(min(sCol, wCol), eCol));
    float3 maxRGB = max(max(c, nCol), max(max(sCol, wCol), eCol));
    if (g_casStrength > 0.001f) {
        float3 amp = saturate(min(minRGB, max(2.0f - maxRGB, 0.0f)) / max(maxRGB, 1.0e-4f));
        amp = sqrt(amp);
        float peak = -1.0f / lerp(10.0f, 5.2f, saturate(g_casStrength));
        float3 w = amp * peak;
        float3 casOut = (c + (nCol + sCol + wCol + eCol) * w) / max(1.0f + 4.0f * w, 1.0e-4f);
        float casLocal = saturate(g_casStrength) * (1.0f - skyEdgeMask * saturate(g_horizonSoften) * 0.85f);
        c = clamp(lerp(c, casOut, casLocal), 0.0f, 1.0f);
    }

    if (skyEdgeMask > 0.001f) {
        c = lerp(c, avgCross, skyEdgeMask * saturate(g_horizonSoften) * 0.45f);
    }

    // 3) Midtone Local Clarity (crisp stone/terrain/armor micro-contrast without shadow crush)
    if (g_clarity > 0.001f) {
        float2 o1 = px * 1.75f;
        float2 o2 = px * 3.50f;
        float3 blur =
            0.125f * (SceneTex.SampleLevel(LinearSamp, uv + float2( o1.x,  o1.y), 0.0f).rgb +
                      SceneTex.SampleLevel(LinearSamp, uv + float2(-o1.x,  o1.y), 0.0f).rgb +
                      SceneTex.SampleLevel(LinearSamp, uv + float2( o1.x, -o1.y), 0.0f).rgb +
                      SceneTex.SampleLevel(LinearSamp, uv + float2(-o1.x, -o1.y), 0.0f).rgb) +
            0.125f * (SceneTex.SampleLevel(LinearSamp, uv + float2( o2.x,  0.0f), 0.0f).rgb +
                      SceneTex.SampleLevel(LinearSamp, uv + float2(-o2.x,  0.0f), 0.0f).rgb +
                      SceneTex.SampleLevel(LinearSamp, uv + float2( 0.0f,  o2.y), 0.0f).rgb +
                      SceneTex.SampleLevel(LinearSamp, uv + float2( 0.0f, -o2.y), 0.0f).rgb);
        float l = Luma(c);
        float midMask = saturate(4.0f * l * (1.0f - l)) * (1.0f - skyEdgeMask * 0.85f);
        float3 highPass = clamp(c - blur, -0.18f, 0.18f);
        c = saturate(c + highPass * (g_clarity * 1.35f * midMask));
    }

    // 4) Atmospheric Dehaze & Natural Depth (removes flat 2007 milky fog veil on midtones without crushing shadows)
    if (g_dehaze > 0.001f) {
        float l = Luma(c);
        float minCh = min(c.r, min(c.g, c.b));
        float veilMask = smoothstep(0.04f, 0.30f, minCh) * smoothstep(0.76f, 0.38f, l);
        float dehazeAmt = saturate(g_dehaze) * veilMask * 0.38f;
        c = saturate((c - dehazeAmt * 0.20f) / max(1.0f - dehazeAmt * 0.20f, 0.5f));
    }

    // 5) 3D Dynamic Rim-Lighting & Metallic Specular Pop (gives armor, trim & stonework subtle PBR sheen)
    if (g_rimSpecular > 0.001f) {
        float lCur = Luma(c);
        float lAvgN = 0.25f * (lN + lS + lW + lE);
        float localRelief = clamp(lCur - 0.5f * (lS + lW), 0.0f, 0.18f);
        float localRange = max(max4L - min4L, abs(lCur - lAvgN));
        float skyProtect = 1.0f - smoothstep(0.74f, 0.92f, lCur);
        float rimAmt = saturate(g_rimSpecular) * skyProtect * (1.0f - skyEdgeMask * 0.90f);
        float specMask = smoothstep(0.32f, 0.66f, lCur) * smoothstep(0.025f, 0.14f, localRange);
        float reliefMask = smoothstep(0.015f, 0.10f, localRange);
        float3 specTint = lerp(float3(1.0f, 1.0f, 1.0f), c / max(lCur, 0.08f), 0.55f);
        c = saturate(c + c * (specMask * rimAmt * 0.36f) + specTint * (localRelief * reliefMask * rimAmt * 0.52f));
    }

    // 6) Perceptual Vibrance (lifts washed tones without oversaturating bright spells)
    if (g_vibrance > 0.001f) {
        float mx = max(c.r, max(c.g, c.b));
        float mn = min(c.r, min(c.g, c.b));
        float sat = mx - mn;
        float lum = Luma(c);
        c = saturate(lerp(float3(lum, lum, lum), c, 1.0f + g_vibrance * (1.0f - sat)));
    }

    // 7) Middle-earth Color Grading Profiles (0=Natural, 1=Rivendell Gold, 2=Lothlorien Silver, 3=Mordor Grim)
    if (g_colorProfile > 0.5f) {
        float cgL = Luma(c);
        float sw = smoothstep(0.45f, 0.04f, cgL);
        float hw = smoothstep(0.20f, 0.85f, cgL);
        if (g_colorProfile < 1.5f) {
            float3 graded = c * lerp(float3(0.95f, 1.00f, 1.05f), float3(1.09f, 1.01f, 0.86f), hw);
            float gL = Luma(graded);
            graded *= (cgL / max(gL, 1.0e-4f));
            c = saturate(lerp(c, graded, 0.88f) + float3(-0.008f, 0.004f, 0.012f) * sw * cgL);
        } else if (g_colorProfile < 2.5f) {
            float3 graded = c * lerp(float3(0.92f, 0.98f, 1.06f), float3(0.96f, 1.01f, 1.11f), hw);
            float gL = Luma(graded);
            graded *= (lerp(cgL, cgL * 1.03f, hw) / max(gL, 1.0e-4f));
            c = saturate(lerp(c, graded, 0.86f));
        } else {
            float emberMask = saturate((c.r - max(c.g, c.b)) * 3.0f);
            float targetL = lerp(cgL, smoothstep(0.0f, 1.0f, cgL), 0.24f);
            float3 baseC = c * (targetL / max(cgL, 1.0e-4f));
            float3 coolDesat = lerp(float3(targetL, targetL, targetL), baseC, lerp(0.84f, 1.16f, emberMask));
            c = saturate(coolDesat * lerp(float3(0.92f, 0.95f, 1.05f), float3(1.02f, 0.99f, 0.96f), hw));
        }
    }

    // 8) Soft-Knee Highlight Roll-Off (Anti-Glare) + Eye-Care 5000K/4300K Warmth
    float effAntiGlare = saturate(g_antiGlare + ((g_eyeComfortMode > 0.5f) ? ((g_eyeComfortMode > 2.5f) ? 0.45f : 0.30f) : 0.0f));
    if (effAntiGlare > 0.001f) {
        float l = Luma(c);
        if (l > 0.72f) {
            float excess = l - 0.72f;
            float comp = effAntiGlare * 1.85f;
            float newL = 0.72f + excess / (1.0f + excess * comp);
            c *= (newL / max(l, 1.0e-4f));
        }
    }
    if (g_eyeComfortMode > 1.5f) {
        float3 warmFilter = (g_eyeComfortMode > 2.5f)
            ? float3(1.04f, 0.95f, 0.80f)  // Night 4300K
            : float3(1.02f, 0.98f, 0.89f); // Warm 5000K
        float origL = Luma(c);
        float3 warmed = c * warmFilter;
        float wL = Luma(warmed);
        c = saturate(warmed * (origL / max(wL, 1.0e-4f)));
    }

    // 9) Subtle Cinematic Vignette (draws focus cleanly to the center)
    if (g_vignette > 0.001f) {
        float2 vDist = (uv - 0.5f) * float2(1.0f, 0.86f);
        float vMask = smoothstep(0.36f, 0.86f, length(vDist));
        c *= (1.0f - vMask * vMask * saturate(g_vignette) * 0.62f);
    }

    // Subtle triangular 8-bit dither preserves smooth debanded skies
    c = saturate(c + (n - 0.5f) * (1.0f / 255.0f));
    return float4(c, center.a);
}
)hlsl";

const char* kGlassDownsamplePs = R"hlsl(
Texture2D<float4> SrcTex : register(t0);
SamplerState LinearSamp : register(s1);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint tw, th;
    SrcTex.GetDimensions(tw, th);
    float2 px = 1.0f / max(float2((float)tw, (float)th), 1.0f);
    float2 o = px * 1.5f;
    float3 acc =
        SrcTex.SampleLevel(LinearSamp, uv + float2(-o.x, -o.y), 0.0f).rgb +
        SrcTex.SampleLevel(LinearSamp, uv + float2( o.x, -o.y), 0.0f).rgb +
        SrcTex.SampleLevel(LinearSamp, uv + float2(-o.x,  o.y), 0.0f).rgb +
        SrcTex.SampleLevel(LinearSamp, uv + float2( o.x,  o.y), 0.0f).rgb;
    return float4(acc * 0.25f, 1.0f);
}
)hlsl";

const char* kGlassBlurPs = R"hlsl(
Texture2D<float4> SrcTex : register(t0);
SamplerState LinearSamp : register(s1);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint tw, th;
    SrcTex.GetDimensions(tw, th);
    float2 px = 1.0f / max(float2((float)tw, (float)th), 1.0f);
    float3 c = SrcTex.SampleLevel(LinearSamp, uv, 0.0f).rgb * 0.22f;
    float2 d1 = px * 1.65f;
    float2 d2 = px * 3.10f;
    c += 0.115f * (SrcTex.SampleLevel(LinearSamp, uv + float2( d1.x,  d1.y), 0.0f).rgb +
                   SrcTex.SampleLevel(LinearSamp, uv + float2(-d1.x,  d1.y), 0.0f).rgb +
                   SrcTex.SampleLevel(LinearSamp, uv + float2( d1.x, -d1.y), 0.0f).rgb +
                   SrcTex.SampleLevel(LinearSamp, uv + float2(-d1.x, -d1.y), 0.0f).rgb);
    c += 0.080f * (SrcTex.SampleLevel(LinearSamp, uv + float2( d2.x,  0.0f), 0.0f).rgb +
                   SrcTex.SampleLevel(LinearSamp, uv + float2(-d2.x,  0.0f), 0.0f).rgb +
                   SrcTex.SampleLevel(LinearSamp, uv + float2( 0.0f,  d2.y), 0.0f).rgb +
                   SrcTex.SampleLevel(LinearSamp, uv + float2( 0.0f, -d2.y), 0.0f).rgb);
    return float4(c, 1.0f);
}
)hlsl";

} // namespace

WorldPostFx& WorldPostFx::Instance() {
    static auto* inst = new WorldPostFx();
    return *inst;
}

void WorldPostFx::InitializeFromConfig(const Config& cfg) {
    bool expected = false;
    if (!initializedFromConfig_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    ReloadFromConfig(cfg);
}

void WorldPostFx::ReloadFromConfig(const Config& cfg) {
    initializedFromConfig_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_.store(cfg.worldPostFxEnabled && cfg.enabled, std::memory_order_release);
    casStrength_ = (std::clamp)(cfg.worldCasStrength, 0.0f, 1.0f);
    clarity_ = (std::clamp)(cfg.worldClarity, 0.0f, 0.6f);
    vibrance_ = (std::clamp)(cfg.worldVibrance, 0.0f, 0.6f);
    dehaze_ = (std::clamp)(cfg.worldDehaze, 0.0f, 0.6f);
    antiGlare_ = (std::clamp)(cfg.worldAntiGlare, 0.0f, 1.0f);
    colorProfile_ = (std::min)(cfg.worldColorProfile, 3u);
    rimSpecular_ = (std::clamp)(cfg.worldRimSpecular, 0.0f, 1.0f);
    vignette_ = (std::clamp)(cfg.worldVignette, 0.0f, 0.60f);
    horizonSoften_ = (std::clamp)(cfg.worldHorizonSoften, 0.0f, 1.0f);
    deband_ = cfg.worldDeband;
    UIE_LOG_INFO("WorldPostFx configured: enabled=%s cas=%.2f clarity=%.2f vibrance=%.2f dehaze=%.2f grade=%u rim=%.2f vig=%.2f horz=%.2f",
                 enabled_.load(std::memory_order_relaxed) ? "true" : "false",
                 casStrength_, clarity_, vibrance_, dehaze_,
                 colorProfile_, rimSpecular_, vignette_, horizonSoften_);
}

void WorldPostFx::ToggleEnabled() {
    const bool next = !enabled_.load(std::memory_order_relaxed);
    enabled_.store(next, std::memory_order_release);
    UIE_LOG_INFO("3D World PostFX toggled: %s", next ? "ON" : "OFF");
}

bool WorldPostFx::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

void WorldPostFx::CycleColorProfile() {
    std::lock_guard<std::mutex> lock(mutex_);
    colorProfile_ = (colorProfile_ + 1u) % 4u;
    UIE_LOG_INFO("Middle-earth Color Grade cycled to %u", colorProfile_);
}

void WorldPostFx::CycleRimSpecular() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rimSpecular_ < 0.10f) {
        rimSpecular_ = 0.28f;
    } else if (rimSpecular_ < 0.40f) {
        rimSpecular_ = 0.50f;
    } else {
        rimSpecular_ = 0.00f;
    }
    UIE_LOG_INFO("3D Rim-Specular Sheen cycled to %.2f", rimSpecular_);
}

void WorldPostFx::CycleVignetteHorizon() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vignette_ < 0.05f && horizonSoften_ < 0.05f) {
        vignette_ = 0.16f;
        horizonSoften_ = 0.25f;
    } else if (vignette_ < 0.24f) {
        vignette_ = 0.30f;
        horizonSoften_ = 0.45f;
    } else {
        vignette_ = 0.00f;
        horizonSoften_ = 0.00f;
    }
    UIE_LOG_INFO("Cinematic Vignette/Horizon AA cycled to vig=%.2f horz=%.2f", vignette_, horizonSoften_);
}

void WorldPostFx::ExportCurrentStateToConfig(Config& ioConfig) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ioConfig.worldPostFxEnabled = enabled_.load(std::memory_order_relaxed);
    ioConfig.worldCasStrength = casStrength_;
    ioConfig.worldClarity = clarity_;
    ioConfig.worldVibrance = vibrance_;
    ioConfig.worldDehaze = dehaze_;
    ioConfig.worldAntiGlare = antiGlare_;
    ioConfig.worldColorProfile = colorProfile_;
    ioConfig.worldRimSpecular = rimSpecular_;
    ioConfig.worldVignette = vignette_;
    ioConfig.worldHorizonSoften = horizonSoften_;
    ioConfig.worldDeband = deband_;
}

void WorldPostFx::OnFrameBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    appliedThisFrame_ = false;
    appliedBlurThisFrame_ = false;
    processedThisFrameAtomic_.store(false, std::memory_order_release);
    blurReadyThisFrame_.store(false, std::memory_order_release);
    liveBlurSrv_.store(nullptr, std::memory_order_release);
}

void WorldPostFx::OnBeforeResizeBuffers() {
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseRenderResources();
}

void WorldPostFx::ReleaseRenderResources() {
    blurReadyThisFrame_.store(false, std::memory_order_release);
    liveBlurSrv_.store(nullptr, std::memory_order_release);
    if (blurQuarterSrv_) { blurQuarterSrv_->Release(); blurQuarterSrv_ = nullptr; }
    if (blurQuarterRtv_) { blurQuarterRtv_->Release(); blurQuarterRtv_ = nullptr; }
    if (blurQuarterTex_) { blurQuarterTex_->Release(); blurQuarterTex_ = nullptr; }
    if (blurHalfSrv_) { blurHalfSrv_->Release(); blurHalfSrv_ = nullptr; }
    if (blurHalfRtv_) { blurHalfRtv_->Release(); blurHalfRtv_ = nullptr; }
    if (blurHalfTex_) { blurHalfTex_->Release(); blurHalfTex_ = nullptr; }
    if (bbRtv_) { bbRtv_->Release(); bbRtv_ = nullptr; }
    if (cachedBackbuffer_) { cachedBackbuffer_->Release(); cachedBackbuffer_ = nullptr; }
    if (resolvedSrv_) { resolvedSrv_->Release(); resolvedSrv_ = nullptr; }
    if (resolvedTex_) { resolvedTex_->Release(); resolvedTex_ = nullptr; }
    resW_ = resH_ = 0;
    blurHalfW_ = blurHalfH_ = 0;
    blurQuarterW_ = blurQuarterH_ = 0;
    resFmt_ = DXGI_FORMAT_UNKNOWN;
    resolveFmt_ = DXGI_FORMAT_UNKNOWN;
}

bool WorldPostFx::HasProcessedThisFrame() const noexcept {
    return processedThisFrameAtomic_.load(std::memory_order_acquire);
}

ID3D11ShaderResourceView* WorldPostFx::GetBackdropBlurSrv() const noexcept {
    return blurReadyThisFrame_.load(std::memory_order_acquire)
        ? liveBlurSrv_.load(std::memory_order_acquire)
        : nullptr;
}

bool WorldPostFx::EnsureShaders(ID3D11Device* device) {
    if (!device) return false;
    if (shadersReady_ && device_ == device) return true;
    if (device_ && device_ != device) {
        Shutdown();
    }
    if (initFailed_) return false;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* blurDownBlob = nullptr;
    ID3DBlob* blurWideBlob = nullptr;
    if (!CompileHlslBlob(kWorldPostFxVs, std::strlen(kWorldPostFxVs), "world_postfx_vs", nullptr, "main", "vs_5_0", &vsBlob) ||
        !CompileHlslBlob(kWorldPostFxPs, std::strlen(kWorldPostFxPs), "world_postfx_ps", nullptr, "main", "ps_5_0", &psBlob) ||
        !CompileHlslBlob(kGlassDownsamplePs, std::strlen(kGlassDownsamplePs), "glass_down_ps", nullptr, "main", "ps_5_0", &blurDownBlob) ||
        !CompileHlslBlob(kGlassBlurPs, std::strlen(kGlassBlurPs), "glass_blur_ps", nullptr, "main", "ps_5_0", &blurWideBlob)) {
        if (vsBlob) vsBlob->Release();
        if (psBlob) psBlob->Release();
        if (blurDownBlob) blurDownBlob->Release();
        if (blurWideBlob) blurWideBlob->Release();
        initFailed_ = true;
        return false;
    }

    HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(blurDownBlob->GetBufferPointer(), blurDownBlob->GetBufferSize(), nullptr, &blurDownPs_);
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(blurWideBlob->GetBufferPointer(), blurWideBlob->GetBufferSize(), nullptr, &blurWidePs_);
    }
    vsBlob->Release();
    psBlob->Release();
    blurDownBlob->Release();
    blurWideBlob->Release();
    if (FAILED(hr) || !vs_ || !ps_ || !blurDownPs_ || !blurWidePs_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(WorldPostFxCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &paramCb_)) || !paramCb_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    D3D11_SAMPLER_DESC psd{};
    psd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    psd.AddressU = psd.AddressV = psd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    psd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&psd, &pointSampler_)) || !pointSampler_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    D3D11_SAMPLER_DESC lsd{};
    lsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    lsd.AddressU = lsd.AddressV = lsd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    lsd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&lsd, &linearSampler_)) || !linearSampler_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &opaqueBlend_)) || !opaqueBlend_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&dsd, &noDepthState_)) || !noDepthState_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, &rastState_)) || !rastState_) {
        Shutdown();
        initFailed_ = true;
        return false;
    }

    device_ = device;
    shadersReady_ = true;
    UIE_LOG_INFO("WorldPostFx initialized (3D CAS + Deband + Clarity + Rim + ColorGrade + FrostedGlass ready)");
    return true;
}

bool WorldPostFx::EnsureRenderResources(ID3D11Device* device, ID3D11Texture2D* backbuffer, bool needBlurResources) {
    D3D11_TEXTURE2D_DESC bbDesc{};
    backbuffer->GetDesc(&bbDesc);
    if ((bbDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || bbDesc.Width == 0 || bbDesc.Height == 0) {
        return false;
    }

    const DXGI_FORMAT typedFmt = ToTypedBackbufferFormat(bbDesc.Format);
    if (cachedBackbuffer_ != backbuffer || !bbRtv_ || resFmt_ != bbDesc.Format ||
        resW_ != bbDesc.Width || resH_ != bbDesc.Height) {
        if (bbRtv_) { bbRtv_->Release(); bbRtv_ = nullptr; }
        if (cachedBackbuffer_) { cachedBackbuffer_->Release(); cachedBackbuffer_ = nullptr; }

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = typedFmt;
        rtvDesc.ViewDimension = (bbDesc.SampleDesc.Count > 1)
            ? D3D11_RTV_DIMENSION_TEXTURE2DMS
            : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(backbuffer, &rtvDesc, &bbRtv_)) || !bbRtv_) {
            return false;
        }
        backbuffer->AddRef();
        cachedBackbuffer_ = backbuffer;
    }

    if (!resolvedTex_ || !resolvedSrv_ || resW_ != bbDesc.Width || resH_ != bbDesc.Height || resolveFmt_ != typedFmt) {
        if (resolvedSrv_) { resolvedSrv_->Release(); resolvedSrv_ = nullptr; }
        if (resolvedTex_) { resolvedTex_->Release(); resolvedTex_ = nullptr; }

        D3D11_TEXTURE2D_DESC rtd{};
        rtd.Width = bbDesc.Width;
        rtd.Height = bbDesc.Height;
        rtd.MipLevels = 1;
        rtd.ArraySize = 1;
        rtd.Format = typedFmt;
        rtd.SampleDesc.Count = 1;
        rtd.Usage = D3D11_USAGE_DEFAULT;
        rtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&rtd, nullptr, &resolvedTex_)) || !resolvedTex_ ||
            FAILED(device->CreateShaderResourceView(resolvedTex_, nullptr, &resolvedSrv_)) || !resolvedSrv_) {
            ReleaseRenderResources();
            return false;
        }
        resW_ = bbDesc.Width;
        resH_ = bbDesc.Height;
        resFmt_ = bbDesc.Format;
        resolveFmt_ = typedFmt;
    }

    if (needBlurResources) {
        const uint32_t hw = (std::max)(bbDesc.Width / 4u, 16u);
        const uint32_t hh = (std::max)(bbDesc.Height / 4u, 16u);
        const uint32_t qw = (std::max)(bbDesc.Width / 8u, 16u);
        const uint32_t qh = (std::max)(bbDesc.Height / 8u, 16u);
        if (!blurHalfTex_ || !blurHalfRtv_ || !blurHalfSrv_ || blurHalfW_ != hw || blurHalfH_ != hh ||
            !blurQuarterTex_ || !blurQuarterRtv_ || !blurQuarterSrv_ || blurQuarterW_ != qw || blurQuarterH_ != qh) {
            liveBlurSrv_.store(nullptr, std::memory_order_release);
            blurReadyThisFrame_.store(false, std::memory_order_release);
            if (blurQuarterSrv_) { blurQuarterSrv_->Release(); blurQuarterSrv_ = nullptr; }
            if (blurQuarterRtv_) { blurQuarterRtv_->Release(); blurQuarterRtv_ = nullptr; }
            if (blurQuarterTex_) { blurQuarterTex_->Release(); blurQuarterTex_ = nullptr; }
            if (blurHalfSrv_) { blurHalfSrv_->Release(); blurHalfSrv_ = nullptr; }
            if (blurHalfRtv_) { blurHalfRtv_->Release(); blurHalfRtv_ = nullptr; }
            if (blurHalfTex_) { blurHalfTex_->Release(); blurHalfTex_ = nullptr; }

            D3D11_TEXTURE2D_DESC btd{};
            btd.Width = hw;
            btd.Height = hh;
            btd.MipLevels = 1;
            btd.ArraySize = 1;
            btd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            btd.SampleDesc.Count = 1;
            btd.Usage = D3D11_USAGE_DEFAULT;
            btd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            if (FAILED(device->CreateTexture2D(&btd, nullptr, &blurHalfTex_)) || !blurHalfTex_ ||
                FAILED(device->CreateRenderTargetView(blurHalfTex_, nullptr, &blurHalfRtv_)) || !blurHalfRtv_ ||
                FAILED(device->CreateShaderResourceView(blurHalfTex_, nullptr, &blurHalfSrv_)) || !blurHalfSrv_) {
                if (blurHalfSrv_) { blurHalfSrv_->Release(); blurHalfSrv_ = nullptr; }
                if (blurHalfRtv_) { blurHalfRtv_->Release(); blurHalfRtv_ = nullptr; }
                if (blurHalfTex_) { blurHalfTex_->Release(); blurHalfTex_ = nullptr; }
                return true; // Non-fatal: 3D PostFX still works without glass blur
            }

            btd.Width = qw;
            btd.Height = qh;
            if (FAILED(device->CreateTexture2D(&btd, nullptr, &blurQuarterTex_)) || !blurQuarterTex_ ||
                FAILED(device->CreateRenderTargetView(blurQuarterTex_, nullptr, &blurQuarterRtv_)) || !blurQuarterRtv_ ||
                FAILED(device->CreateShaderResourceView(blurQuarterTex_, nullptr, &blurQuarterSrv_)) || !blurQuarterSrv_) {
                if (blurQuarterSrv_) { blurQuarterSrv_->Release(); blurQuarterSrv_ = nullptr; }
                if (blurQuarterRtv_) { blurQuarterRtv_->Release(); blurQuarterRtv_ = nullptr; }
                if (blurQuarterTex_) { blurQuarterTex_->Release(); blurQuarterTex_ = nullptr; }
                return true;
            }
            blurHalfW_ = hw;
            blurHalfH_ = hh;
            blurQuarterW_ = qw;
            blurQuarterH_ = qh;
        }
    }

    return true;
}

bool WorldPostFx::ApplyToBackbuffer(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    IDXGISwapChain* swapChain
) {
    if (!device || !ctx || !swapChain) {
        return false;
    }
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }

    const bool doWorldFx = enabled_.load(std::memory_order_acquire);
    const bool doBlur = UiEnhancer::Instance().IsEnabled() &&
                        (UiEnhancer::Instance().GetGlassmorphism() > 0.001f);
    if (!doWorldFx && !doBlur) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const bool needWorldFx = doWorldFx && !appliedThisFrame_;
    const bool needBlur = doBlur && !appliedBlurThisFrame_;
    if (!needWorldFx && !needBlur) {
        return false;
    }

    if (!EnsureShaders(device)) {
        return false;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) || !backbuffer) {
        return false;
    }

    const bool ready = EnsureRenderResources(device, backbuffer, needBlur);
    if (!ready) {
        backbuffer->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC bbDesc{};
    backbuffer->GetDesc(&bbDesc);
    const bool canBlurNow = needBlur && blurHalfRtv_ && blurHalfSrv_ && blurQuarterRtv_ && blurQuarterSrv_;

    // Save entire pipeline state we will temporarily override
    ID3D11InputLayout* prevIl = nullptr;
    ctx->IAGetInputLayout(&prevIl);
    D3D11_PRIMITIVE_TOPOLOGY prevTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&prevTopo);

    ID3D11VertexShader* prevVs = nullptr;
    ID3D11ClassInstance* prevVsInst[8]{};
    UINT prevVsInstCount = 8;
    ctx->VSGetShader(&prevVs, prevVsInst, &prevVsInstCount);

    ID3D11PixelShader* prevPs = nullptr;
    ID3D11ClassInstance* prevPsInst[8]{};
    UINT prevPsInstCount = 8;
    ctx->PSGetShader(&prevPs, prevPsInst, &prevPsInstCount);

    ID3D11GeometryShader* prevGs = nullptr;
    ID3D11ClassInstance* prevGsInst[8]{};
    UINT prevGsInstCount = 8;
    ctx->GSGetShader(&prevGs, prevGsInst, &prevGsInstCount);

    ID3D11HullShader* prevHs = nullptr;
    ID3D11ClassInstance* prevHsInst[8]{};
    UINT prevHsInstCount = 8;
    ctx->HSGetShader(&prevHs, prevHsInst, &prevHsInstCount);

    ID3D11DomainShader* prevDs = nullptr;
    ID3D11ClassInstance* prevDsInst[8]{};
    UINT prevDsInstCount = 8;
    ctx->DSGetShader(&prevDs, prevDsInst, &prevDsInstCount);

    ID3D11RenderTargetView* prevRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* prevDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtvs, &prevDsv);

    ID3D11BlendState* prevBlend = nullptr;
    FLOAT prevBlendFactor[4]{};
    UINT prevSampleMask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(&prevBlend, prevBlendFactor, &prevSampleMask);

    ID3D11DepthStencilState* prevDss = nullptr;
    UINT prevStencilRef = 0;
    ctx->OMGetDepthStencilState(&prevDss, &prevStencilRef);

    ID3D11RasterizerState* prevRast = nullptr;
    ctx->RSGetState(&prevRast);

    D3D11_VIEWPORT prevVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT prevNumVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&prevNumVps, prevVps);

    ID3D11ShaderResourceView* prevPsSrvs[1]{};
    ctx->PSGetShaderResources(0, 1, prevPsSrvs);

    ID3D11SamplerState* prevPsSamps[2]{};
    ctx->PSGetSamplers(0, 2, prevPsSamps);

    ID3D11Buffer* prevPsCbs[1]{};
    ctx->PSGetConstantBuffers(0, 1, prevPsCbs);

    // Unbind backbuffer RTV briefly while resolving/copying from it
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (bbDesc.SampleDesc.Count > 1) {
        ctx->ResolveSubresource(resolvedTex_, 0, backbuffer, 0, resolveFmt_);
    } else {
        ctx->CopyResource(resolvedTex_, backbuffer);
    }

    const FLOAT bf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ctx->RSSetState(rastState_);
    ctx->OMSetBlendState(opaqueBlend_, bf, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(noDepthState_, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11SamplerState* samps[2] = { pointSampler_, linearSampler_ };
    ctx->PSSetSamplers(0, 2, samps);

    if (needWorldFx) {
        WorldPostFxCB cbData{};
        cbData.casStrength = casStrength_;
        cbData.clarity = clarity_;
        cbData.vibrance = vibrance_;
        cbData.debandEnabled = (deband_ ? 1.0f : 0.0f) + (UiEnhancer::Instance().IsSplitScreenEnabled() ? 2.0f : 0.0f);
        cbData.dehaze = dehaze_;
        cbData.antiGlare = antiGlare_;
        cbData.eyeComfortMode = static_cast<float>(UiEnhancer::Instance().GetEyeComfortMode());
        cbData.colorProfile = static_cast<float>(colorProfile_);
        cbData.rimSpecular = rimSpecular_;
        cbData.vignette = vignette_;
        cbData.horizonSoften = horizonSoften_;
        cbData._pad0 = 0.0f;
        ctx->UpdateSubresource(paramCb_, 0, nullptr, &cbData, 0, 0);

        D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(bbDesc.Width), static_cast<float>(bbDesc.Height), 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &bbRtv_, nullptr);
        ctx->PSSetShader(ps_, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &resolvedSrv_);
        ctx->PSSetConstantBuffers(0, 1, &paramCb_);
        ctx->Draw(3, 0);

        if (canBlurNow) {
            ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
            if (bbDesc.SampleDesc.Count > 1) {
                ctx->ResolveSubresource(resolvedTex_, 0, backbuffer, 0, resolveFmt_);
            } else {
                ctx->CopyResource(resolvedTex_, backbuffer);
            }
        }
        appliedThisFrame_ = true;
        ++totalPassesApplied_;
    }

    if (canBlurNow) {
        D3D11_VIEWPORT vpHalf{ 0.0f, 0.0f, static_cast<float>(blurHalfW_), static_cast<float>(blurHalfH_), 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vpHalf);
        ctx->OMSetRenderTargets(1, &blurHalfRtv_, nullptr);
        ctx->PSSetShader(blurDownPs_, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &resolvedSrv_);
        ctx->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrvTmp = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrvTmp);

        D3D11_VIEWPORT vpQuarter{ 0.0f, 0.0f, static_cast<float>(blurQuarterW_), static_cast<float>(blurQuarterH_), 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vpQuarter);
        ctx->OMSetRenderTargets(1, &blurQuarterRtv_, nullptr);
        ctx->PSSetShader(blurWidePs_, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &blurHalfSrv_);
        ctx->Draw(3, 0);

        appliedBlurThisFrame_ = true;
        liveBlurSrv_.store(blurQuarterSrv_, std::memory_order_release);
        blurReadyThisFrame_.store(true, std::memory_order_release);
    }

    backbuffer->Release();

    // Unbind SRV and restore full caller pipeline state cleanly
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtvs, prevDsv);
    ctx->OMSetBlendState(prevBlend, prevBlendFactor, prevSampleMask);
    ctx->OMSetDepthStencilState(prevDss, prevStencilRef);
    ctx->RSSetState(prevRast);
    if (prevNumVps > 0) {
        ctx->RSSetViewports(prevNumVps, prevVps);
    }
    ctx->IASetInputLayout(prevIl);
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->VSSetShader(prevVs, prevVsInst, prevVsInstCount);
    ctx->GSSetShader(prevGs, prevGsInst, prevGsInstCount);
    ctx->HSSetShader(prevHs, prevHsInst, prevHsInstCount);
    ctx->DSSetShader(prevDs, prevDsInst, prevDsInstCount);
    ctx->PSSetShader(prevPs, prevPsInst, prevPsInstCount);
    ctx->PSSetShaderResources(0, 1, prevPsSrvs);
    ctx->PSSetSamplers(0, 2, prevPsSamps);
    ctx->PSSetConstantBuffers(0, 1, prevPsCbs);

    if (prevIl) prevIl->Release();
    if (prevVs) prevVs->Release();
    if (prevPs) prevPs->Release();
    if (prevGs) prevGs->Release();
    if (prevHs) prevHs->Release();
    if (prevDs) prevDs->Release();
    for (UINT i = 0; i < prevVsInstCount; ++i) if (prevVsInst[i]) prevVsInst[i]->Release();
    for (UINT i = 0; i < prevPsInstCount; ++i) if (prevPsInst[i]) prevPsInst[i]->Release();
    for (UINT i = 0; i < prevGsInstCount; ++i) if (prevGsInst[i]) prevGsInst[i]->Release();
    for (UINT i = 0; i < prevHsInstCount; ++i) if (prevHsInst[i]) prevHsInst[i]->Release();
    for (UINT i = 0; i < prevDsInstCount; ++i) if (prevDsInst[i]) prevDsInst[i]->Release();
    for (auto* r : prevRtvs) if (r) r->Release();
    if (prevDsv) prevDsv->Release();
    if (prevBlend) prevBlend->Release();
    if (prevDss) prevDss->Release();
    if (prevRast) prevRast->Release();
    for (auto* s : prevPsSrvs) if (s) s->Release();
    for (auto* s : prevPsSamps) if (s) s->Release();
    for (auto* b : prevPsCbs) if (b) b->Release();

    processedThisFrameAtomic_.store(true, std::memory_order_release);
    return true;
}

WorldPostFxStats WorldPostFx::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WorldPostFxStats s{};
    s.initialized = shadersReady_;
    s.activeThisFrame = appliedThisFrame_;
    s.enabled = enabled_.load(std::memory_order_acquire);
    s.colorProfile = colorProfile_;
    s.rimSpecular = rimSpecular_;
    s.vignette = vignette_;
    s.horizonSoften = horizonSoften_;
    s.totalPassesApplied = totalPassesApplied_;
    return s;
}

void WorldPostFx::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseRenderResources();
    if (rastState_) { rastState_->Release(); rastState_ = nullptr; }
    if (noDepthState_) { noDepthState_->Release(); noDepthState_ = nullptr; }
    if (opaqueBlend_) { opaqueBlend_->Release(); opaqueBlend_ = nullptr; }
    if (linearSampler_) { linearSampler_->Release(); linearSampler_ = nullptr; }
    if (pointSampler_) { pointSampler_->Release(); pointSampler_ = nullptr; }
    if (paramCb_) { paramCb_->Release(); paramCb_ = nullptr; }
    if (blurWidePs_) { blurWidePs_->Release(); blurWidePs_ = nullptr; }
    if (blurDownPs_) { blurDownPs_->Release(); blurDownPs_ = nullptr; }
    if (ps_) { ps_->Release(); ps_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    device_ = nullptr;
    shadersReady_ = false;
    initFailed_ = false;
}

}  // namespace lotro_uie
