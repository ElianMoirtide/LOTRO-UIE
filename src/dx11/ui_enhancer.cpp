#include "dx11/ui_enhancer.h"
#include "common/logger.h"
#include "dx11/asset_dumper.h"
#include "dx11/dxgi_format_names.h"
#include "dx11/resource_tracker.h"
#include "dx11/ui_sr_cache.h"
#include "dx11/world_postfx.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lotro_uie {

namespace {

// {4A1D8E30-6C92-45E1-9E55-12C9B7C01F2A}
const GUID kUiPsTagGuid = {
    0x4a1d8e30, 0x6c92, 0x45e1, {0x9e, 0x55, 0x12, 0xc9, 0xb7, 0xc0, 0x1f, 0x2a}
};

class UiPsDestructionTag final : public IUnknown {
public:
    explicit UiPsDestructionTag(ID3D11PixelShader* ps) noexcept : ps_(ps) {}

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
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rc = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rc == 0) {
            if (!ResourceTracker::Instance().IsShuttingDown() && ps_) {
                UiEnhancer::Instance().OnPixelShaderDestroyed(ps_);
            }
            delete this;
        }
        return rc;
    }

private:
    std::atomic<ULONG> refCount_{1};
    ID3D11PixelShader* ps_ = nullptr;
};

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

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Targeted LOTRO UI / Font / Map replacement Pixel Shader.
// Exact cb0 layout and input signature match observed LOTRO UI shaders:
//   Variant 0 (0x6B957ABB3F5938DB): c_AlphaTestThreshold at cb0[10].x
//   Variant 1 (0x43AA4E3C051DFEB6): c_TextureMatrix0 at cb0[10..13], c_AlphaTestThreshold at cb0[14].x
const char kUiEnhancePsSrc[] = R"(
Texture2D Tex0 : register(t0);
Texture2D BackdropBlurTex : register(t14);
SamplerState OrigSamp : register(s0);
SamplerState LinearSamp : register(s15);

cbuffer ShaderGlobals : register(b0) {
    float4 c_WorldSpaceClippingPlane0; // cb0[0]
    float4x4 c_ModelToClipMatrix;      // cb0[1..4]
    float4x4 c_ModelToWorldMatrix;     // cb0[5..8]
    float4 c_MaterialDiffuseColor;     // cb0[9]
#if HAS_TEX_MATRIX
    float4x4 c_TextureMatrix0;         // cb0[10..13]
#endif
    float c_AlphaTestThreshold;        // cb0[10].x or cb0[14].x
};

cbuffer UieParams : register(b12) {
    float g_sharpness;
    float g_anisoEdge;
    float g_wrapU;
    float g_wrapV;
    float g_isMap;
    float g_srActive;
    float g_mapBoost;
    float g_splitMinX;
    float g_fontOutlineBoost;
    float g_themeDarkSlate;
    float g_oledDimStrength;
    float g_screenHeight;
    float g_fontWeightBoost;
    float g_themeProfile;
    float g_eyeComfortMode;
    float g_hoverFocusBoost;
    float g_glassmorphism;
    float g_iconPolish;
    float g_screenWidth;
    float g_hasBackdropBlur;
};

struct PSIn {
    float4 pos   : SV_Position;
    float  clip0 : SV_ClipDistance0;
    float2 uv    : TEXCOORD0;
};

int ClampOrWrapCoord(int p, int dim, float wrapFlag) {
    if (wrapFlag > 0.5f) {
        uint uDim = (uint)dim;
        return (p >= 0) ? (int)((uint)p % uDim) : (dim - 1 - (int)((uint)(-1 - p) % uDim));
    }
    return clamp(p, 0, dim - 1);
}

float4 LoadTexel(int2 p, int2 texSizeI) {
    int x = ClampOrWrapCoord(p.x, texSizeI.x, g_wrapU);
    int y = ClampOrWrapCoord(p.y, texSizeI.y, g_wrapV);
    return Tex0.Load(int3(x, y, 0));
}

float UiLuma(float4 c) {
    return dot(c.rgb, float3(0.299f, 0.587f, 0.114f)) * (0.25f + 0.75f * c.a) + 0.35f * c.a;
}

float UiDiff(float4 a, float4 b) {
    float ma = 0.25f + 0.75f * max(a.a, b.a);
    float3 d = (a.rgb - b.rgb) * ma;
    return sqrt(dot(d, d) + (a.a - b.a) * (a.a - b.a) * 0.65f);
}

void ComputeEdgeFrame(float l[4][4], float2 fp, out float2 outNrm, out float outCoh) {
    // Multi-scale 3x3 + wide 3-texel tangent chord catches 2:1 & 3:1 shallow staircase slopes as well as 1px stems.
    float Jxx = 0.0f, Jyy = 0.0f, Jxy = 0.0f, jW = 0.0f;
    [unroll] for (int cy = 1; cy <= 2; ++cy) {
        [unroll] for (int cx = 1; cx <= 2; ++cx) {
            float dxTex = abs((float)(cx - 1) - fp.x);
            float dyTex = abs((float)(cy - 1) - fp.y);
            float w = (1.15f - 0.65f * dxTex) * (1.15f - 0.65f * dyTex);
            float gx1 = (l[cy - 1][cx + 1] + 2.0f * l[cy][cx + 1] + l[cy + 1][cx + 1]) -
                        (l[cy - 1][cx - 1] + 2.0f * l[cy][cx - 1] + l[cy + 1][cx - 1]);
            float gy1 = (l[cy + 1][cx - 1] + 2.0f * l[cy + 1][cx] + l[cy + 1][cx + 1]) -
                        (l[cy - 1][cx - 1] + 2.0f * l[cy - 1][cx] + l[cy - 1][cx + 1]);
            float gxW = (l[cy][3] - l[cy][0]) * 1.35f;
            float gyW = (l[3][cx] - l[0][cx]) * 1.35f;
            float gx = gx1 * 0.72f + gxW * 0.45f;
            float gy = gy1 * 0.72f + gyW * 0.45f;
            float dDiag1 = (l[cy - 1][cx - 1] + l[cy + 1][cx + 1] - 2.0f * l[cy][cx]);
            float dDiag2 = (l[cy - 1][cx + 1] + l[cy + 1][cx - 1] - 2.0f * l[cy][cx]);
            Jxx += w * (gx * gx + 0.60f * (dDiag1 * dDiag1 + dDiag2 * dDiag2));
            Jyy += w * (gy * gy + 0.60f * (dDiag1 * dDiag1 + dDiag2 * dDiag2));
            // dDiag1 varies along '/' (1,1) -> +xy; dDiag2 varies along '\' (1,-1) -> -xy.
            Jxy += w * (gx * gy + 1.15f * (dDiag1 * dDiag1 - dDiag2 * dDiag2));
            jW += w;
        }
    }
    Jxx /= jW;
    Jyy /= jW;
    Jxy /= jW;

    float trace = Jxx + Jyy;
    float diff = Jxx - Jyy;
    float disc = sqrt(max(0.0f, diff * diff + 4.0f * Jxy * Jxy));
    float lam1 = 0.5f * (trace + disc);
    float lam2 = 0.5f * (trace - disc);
    outCoh = (lam1 > 1.0e-5f) ? saturate((lam1 - lam2) / (lam1 + lam2 + 1.0e-4f)) : 0.0f;
    outNrm = float2(1.0f, 0.0f);
    if (disc > 1.0e-6f) {
        float2 cand = (Jxx >= Jyy) ? float2(Jxx - lam2, Jxy) : float2(Jxy, Jyy - lam2);
        outNrm = (length(cand) > 1.0e-6f) ? normalize(cand) : float2(0.0f, 1.0f);
    }
}

float4 EvalVectorContour(int2 ip, float2 fp, int2 texSizeI, float minTpp, float pNorm, float isMap, float contrast, out float patW) {
    int sx = (fp.x >= 0.5f) ? 1 : 0;
    int sy = (fp.y >= 0.5f) ? 1 : 0;
    int dirX = sx ? -1 : 1;
    int dirY = sy ? -1 : 1;
    int2 cIdx = ip + int2(sx, sy);

    float4 C  = LoadTexel(cIdx, texSizeI);
    float4 U  = LoadTexel(cIdx + int2(    0, -dirY), texSizeI);
    float4 D  = LoadTexel(cIdx + int2(    0,  dirY), texSizeI);
    float4 L  = LoadTexel(cIdx + int2(-dirX,     0), texSizeI);
    float4 R  = LoadTexel(cIdx + int2( dirX,     0), texSizeI);
    float4 UR = LoadTexel(cIdx + int2( dirX, -dirY), texSizeI);
    float4 DL = LoadTexel(cIdx + int2(-dirX,  dirY), texSizeI);
    float4 DR = LoadTexel(cIdx + int2( dirX,  dirY), texSizeI);

    float2 q = float2(sx ? (1.0f - fp.x) : fp.x, sy ? (1.0f - fp.y) : fp.y);

    float dRD = UiDiff(R, D);
    float dCR = UiDiff(C, R);
    float dCD = UiDiff(C, D);
    float dCDR = UiDiff(C, DR);

    float cornerStr = smoothstep(0.06f, 0.25f, min(dCR, dCD) - dRD * 1.05f);
    float diagCdStr = smoothstep(0.06f, 0.25f, min(dCR, dCD) - dCDR * 1.05f);

    // Connectedness/sparsity disambiguation: keeps 1px diagonals ('W','v','e','a','r') intact
    float cOuterSim  = exp(-UiDiff(C, U) * 9.0f) + exp(-UiDiff(C, L) * 9.0f);
    float rdOuterSim = exp(-UiDiff(R, UR) * 9.0f) + exp(-UiDiff(D, DL) * 9.0f);
    bool preferDiagCd = (diagCdStr >= cornerStr * 0.60f) && (cOuterSim <= rdOuterSim + 0.35f);

    float slopeH = smoothstep(0.04f, 0.20f, UiDiff(C, UR) - UiDiff(R, UR)) *
                   smoothstep(0.03f, 0.18f, UiDiff(L, R) * 0.4f + UiDiff(D, DL) * 0.6f - dRD * 0.5f);
    float slopeV = smoothstep(0.04f, 0.20f, UiDiff(C, DL) - UiDiff(D, DL)) *
                   smoothstep(0.03f, 0.18f, UiDiff(U, D) * 0.4f + UiDiff(R, UR) * 0.6f - dRD * 0.5f);

    float tilt = clamp(slopeH - slopeV, -1.0f, 1.0f);
    float2 lineN = normalize(float2(1.0f - 0.35f * tilt, 1.0f + 0.35f * tilt));
    float lineDist = dot(q, lineN) - dot(float2(0.25f, 0.25f), lineN);

    // Calibrated >=0.95 screen-px AA ramp so bevels stay smooth without stair-stepping at 3.00x UI zoom.
    float aaTex = clamp(lerp(0.65f, 0.42f, pNorm) * max(minTpp, 0.28f) * 1.25f, 0.16f, 0.36f);

    float chamferMix = smoothstep(-aaTex, aaTex, lineDist);
    float4 cornerResult = lerp(C, 0.5f * (R + D), chamferMix);

    float dDiagPerp = abs(q.x - q.y) * 0.7071f;
    float diagMix = smoothstep(0.25f - aaTex * 0.85f, 0.25f + aaTex * 0.85f, dDiagPerp);
    float4 diagSide = (q.x > q.y) ? R : D;
    float4 diagStroke = lerp(C, DR, 0.5f * (q.x + q.y));
    float4 diagResult = lerp(diagStroke, diagSide, diagMix);

    float patRaw = preferDiagCd ? diagCdStr : cornerStr;
    patW = saturate(patRaw) * lerp(0.25f, 0.68f, pNorm);
    if (isMap > 0.5f) {
        patW *= smoothstep(0.09f, 0.22f, contrast);
    }
    return preferDiagCd ? diagResult : cornerResult;
}

float4 main(PSIn i) : SV_Target {
    // Keep v1 SV_ClipDistance0 slot active to match LOTRO VS->PS register linkage (v0, v1, v2).
    if (i.clip0 < -1.0e20f) {
        discard;
    }

    float2 uvDdx = ddx(i.uv);
    float2 uvDdy = ddy(i.uv);
    float4 origSample = Tex0.SampleGrad(OrigSamp, i.uv, uvDdx, uvDdy);

    if (g_splitMinX > 0.5f && i.pos.x < g_splitMinX) {
        if (c_AlphaTestThreshold >= origSample.a) {
            discard;
        }
        return origSample * c_MaterialDiffuseColor;
    }

    uint texW = 1, texH = 1, mipCount = 1;
    Tex0.GetDimensions(0, texW, texH, mipCount);
    float2 texSize = float2(texW, texH);

    float tppX = length(float2(uvDdx.x, uvDdy.x)) * texSize.x;
    float tppY = length(float2(uvDdx.y, uvDdy.y)) * texSize.y;
    float minTpp = min(tppX, tppY);

    float isMap = step(0.5f, g_isMap);
    float isSr = step(0.5f, g_srActive);
    // When 2x Mini-CNN has already doubled texel density, keep F7 sharpness responsive down to ~1.5x original UI scale.
    float effMinTpp = (isSr > 0.5f) ? (minTpp * 0.5f) : minTpp;
    float upscaleAmt = saturate((0.96f - effMinTpp) / 0.22f);
    float effSharp = g_sharpness * lerp(1.0f, max(g_mapBoost, 0.5f), isMap);
    float s = clamp(effSharp, 0.0f, 1.50f);

    float4 texel = origSample;
    [branch]
    if (upscaleAmt > 0.01f && texW >= 8 && texH >= 8 && s > 0.001f) {
        float pNorm = saturate(s / 1.40f);
        int2 texSizeI = int2(texW, texH);
        float2 tc = i.uv * texSize - 0.5f;
        float2 baseF = floor(tc);
        int2 ip = int2(baseF);
        float2 fp = tc - baseF;

        float4 t[4][4];
        float  l[4][4];
        [unroll] for (int yy = 0; yy < 4; ++yy) {
            [unroll] for (int xx = 0; xx < 4; ++xx) {
                t[yy][xx] = LoadTexel(ip + int2(xx - 1, yy - 1), texSizeI);
                l[yy][xx] = UiLuma(t[yy][xx]);
            }
        }

        float4 c00 = t[1][1], c10 = t[1][2], c01 = t[2][1], c11 = t[2][2];
        float  l00 = l[1][1], l10 = l[1][2], l01 = l[2][1], l11 = l[2][2];
        float4 min4 = min(min(c00, c10), min(c01, c11));
        float4 max4 = max(max(c00, c10), max(c01, c11));
        float  contrast = max(max(l00, l10), max(l01, l11)) - min(min(l00, l10), min(l01, l11));

        // Map Mode: smooth subtle parchment grain while keeping sharp map labels, thin strokes & alpha overlays intact.
        float4 baseSample = origSample;
        if (isMap > 0.5f && min4.a >= 0.96f) {
            float4 accM = 0.0f;
            float wM = 0.0f;
            float spatialScale = (isSr > 0.5f) ? 0.22f : 0.75f;
            [unroll] for (int my = 0; my < 4; ++my) {
                [unroll] for (int mx = 0; mx < 4; ++mx) {
                    float2 mOff = float2((float)(mx - 1), (float)(my - 1)) - fp;
                    float d = UiDiff(t[my][mx], origSample);
                    float wm = exp(-d * d * 90.0f - dot(mOff, mOff) * spatialScale);
                    accM += t[my][mx] * wm;
                    wM += wm;
                }
            }
            float4 mapCalm = accM / max(wM, 1.0e-5f);
            float ridgeH = abs(0.5f * (l[1][0] + l[1][2]) - l[1][1]);
            float ridgeV = abs(0.5f * (l[0][1] + l[2][1]) - l[1][1]);
            float strokeMetric = max(contrast, max(ridgeH, ridgeV) * 1.65f);
            float textPresence = smoothstep(0.085f, 0.21f, strokeMetric);
            baseSample = lerp(mapCalm, origSample, textPresence);
        }

        float4 cLo = c00; float lLoV = l00;
        if (l10 < lLoV) { cLo = c10; lLoV = l10; }
        if (l01 < lLoV) { cLo = c01; lLoV = l01; }
        if (l11 < lLoV) { cLo = c11; lLoV = l11; }

        float4 cHi = c00; float lHiV = l00;
        if (l10 > lHiV) { cHi = c10; lHiV = l10; }
        if (l01 > lHiV) { cHi = c01; lHiV = l01; }
        if (l11 > lHiV) { cHi = c11; lHiV = l11; }

        float2 nrm = float2(1.0f, 0.0f);
        float coh = 0.0f;
        ComputeEdgeFrame(l, fp, nrm, coh);
        float2 tan = float2(-nrm.y, nrm.x);

        float aniso = saturate(g_anisoEdge) * coh * upscaleAmt * (0.45f + 0.55f * pNorm);
        float sigT = lerp(0.42f, (isSr > 0.5f) ? 1.05f : 1.15f, aniso);
        float sigN = lerp(0.42f, (isSr > 0.5f) ? (0.17f + 0.06f * (1.0f - pNorm)) : (0.23f + 0.08f * (1.0f - pNorm)), aniso);

        float4 accCol = 0.0f;
        float accW = 0.0f;
        float4 accNrmWide = 0.0f;
        float accNrmW = 0.0f;
        [unroll] for (int gy = 0; gy < 4; ++gy) {
            [unroll] for (int gx = 0; gx < 4; ++gx) {
                float2 off = float2((float)(gx - 1), (float)(gy - 1)) - fp;
                float dt = dot(off, tan);
                float dn = dot(off, nrm);
                float r2 = (dt / sigT) * (dt / sigT) + (dn / sigN) * (dn / sigN);
                if (r2 <= 6.0f) {
                    float w = exp(-1.45f * r2);
                    accCol += t[gy][gx] * w;
                    accW += w;
                }
                float r2Wide = (dt / max(sigT, 0.70f)) * (dt / max(sigT, 0.70f)) + (dn / 0.85f) * (dn / 0.85f);
                float wWide = exp(-1.35f * r2Wide);
                accNrmWide += t[gy][gx] * wWide;
                accNrmW += wWide;
            }
        }
        float4 dirOut = clamp(accCol / max(accW, 1.0e-5f), min4, max4);
        float4 tanWideBlur = accNrmWide / max(accNrmW, 1.0e-5f);
        float edgeThreshLo = lerp(0.055f, 0.115f, isMap);
        float edgeThreshHi = lerp(0.22f,  0.29f,  isMap);
        float edgeMask = smoothstep(edgeThreshLo, edgeThreshHi, contrast) * (0.35f + 0.65f * coh);

        float pCurve = pNorm * (0.54f + 0.46f * pNorm);
        float dirBlend = upscaleAmt * lerp(0.42f, 1.0f, pNorm);
        if (isMap > 0.5f) {
            dirBlend *= smoothstep(0.06f, 0.18f, contrast);
        }
        dirOut = lerp(baseSample, dirOut, dirBlend);

        float span = lHiV - lLoV;
        if (span > 0.04f && edgeMask > 0.001f) {
            float curL = UiLuma(dirOut);
            float tPos = saturate((curL - lLoV) / span);
            float maxK = (isSr > 0.5f) ? 3.70f : lerp(1.85f, 2.55f, saturate(minTpp / 0.75f));
            float k = lerp(0.70f, maxK, pCurve) * (0.45f + 0.55f * coh);
            float s0  = 1.0f / (1.0f + exp(-k * (tPos - 0.5f)));
            float sLo = 1.0f / (1.0f + exp( 0.5f * k));
            float sHi = 1.0f / (1.0f + exp(-0.5f * k));
            float tSharp = saturate((s0 - sLo) / max(sHi - sLo, 1.0e-4f));
            float4 endCol = lerp(cLo, cHi, tSharp);
            float targetL = lerp(lLoV, lHiV, tSharp);

            float3 lumShiftedRgb = dirOut.rgb + (targetL - curL);
            float3 aaRgb = lerp(lumShiftedRgb, endCol.rgb, 0.68f);
            float aaA = lerp(dirOut.a, endCol.a, 0.80f);

            float4 aaCrisp = clamp(float4(aaRgb, aaA), min4, max4);
            dirOut = lerp(dirOut, aaCrisp, edgeMask * lerp(0.16f, 0.90f, pNorm));
        }

        float4 outCol = dirOut;
        if (isSr < 0.5f) {
            float patW = 0.0f;
            float4 patCol = EvalVectorContour(ip, fp, texSizeI, minTpp, pNorm, isMap, contrast, patW);
            outCol = lerp(dirOut, patCol, patW * upscaleAmt);
        }

        // Edge-normal micro-deblur using tangent-smoothed reference (sharpens across curves without stair-stepping).
        float4 detail = outCol - tanWideBlur;

        float srScale = (isSr > 0.5f) ? lerp(0.88f, 1.56f, pCurve) : lerp(0.85f, 1.18f, pCurve);
        float mapScale = (isMap > 0.5f) ? 1.15f : 1.0f;
        float sharpDrive = pow(pNorm, 1.16f) * lerp(0.18f, 1.44f, pCurve) * edgeMask * srScale * mapScale;
        float3 overPad = (max4.rgb - min4.rgb) * (lerp(0.01f, 0.19f, pCurve) * coh * smoothstep(edgeThreshLo, edgeThreshHi, contrast) * min(srScale, 1.18f));

        float3 crispRgb = clamp(outCol.rgb + detail.rgb * sharpDrive,
                                max(0.0f, min4.rgb - overPad),
                                min(1.0f, max4.rgb + overPad));
        float crispA = clamp(outCol.a + detail.a * (sharpDrive * 0.45f), min4.a, max4.a);
        texel = lerp(baseSample, float4(saturate(crispRgb), saturate(crispA)), upscaleAmt);
    }

    // Modern Multi-Profile UI Color Themes (0 = Obsidian Slate, 1 = Elven Bronze, 2 = Dwarven Iron)
    if (g_themeDarkSlate > 0.001f && isMap < 0.5f && texel.a > 0.12f) {
        float rgbLum = dot(texel.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        float maxC = max(texel.r, max(texel.g, texel.b));
        float minC = min(texel.r, min(texel.g, texel.b));
        float sat = (maxC > 1.0e-4f) ? ((maxC - minC) / maxC) : 0.0f;
        float strength = saturate(g_themeDarkSlate);

        float3 panelTint = (g_themeProfile > 1.5f) ? float3(0.76f, 0.78f, 0.82f)
                         : (g_themeProfile > 0.5f) ? float3(0.95f, 0.78f, 0.58f)
                                                   : float3(0.68f, 0.80f, 0.98f);
        float3 paperTint = (g_themeProfile > 1.5f) ? float3(0.74f, 0.74f, 0.76f)
                         : (g_themeProfile > 0.5f) ? float3(0.82f, 0.75f, 0.64f)
                                                   : float3(0.76f, 0.74f, 0.70f);
        float darkScale  = (g_themeProfile > 1.5f) ? 0.38f : 0.44f;

        // 1. Dark/mid-tone stone & brown-grey UI panels -> rich dark theme palette (cap at 0.48 so text edges aren't crushed)
        float panelMask = smoothstep(0.64f, 0.22f, sat) *
                          smoothstep(0.01f, 0.05f, rgbLum) *
                          smoothstep(0.48f, 0.26f, rgbLum) * strength;
        float darkenedL = pow(max(rgbLum, 0.0f), 1.48f) * darkScale;
        float3 themedCol = lerp(texel.rgb * darkScale, panelTint * darkenedL, 0.84f);
        texel.rgb = lerp(texel.rgb, themedCol, panelMask * 0.94f);

        // Deepen semi-transparent dark HUD/chat/tooltip backdrops so foreground text stands out clearly
        if (texel.a < 0.96f) {
            float alphaBoost = (g_themeProfile > 1.5f) ? 0.32f : 0.24f;
            texel.a = lerp(texel.a, saturate(texel.a + alphaBoost * (1.0f - texel.a)), panelMask * 0.75f);
        }

        // 2. Bright low-saturation parchment/paper surfaces -> softer eye-friendly toned paper
        float parchmentMask = smoothstep(0.46f, 0.16f, sat) *
                              smoothstep(0.50f, 0.75f, rgbLum) * strength;
        float3 calmPaper = texel.rgb * paperTint;
        texel.rgb = lerp(texel.rgb, calmPaper, parchmentMask * 0.68f);

        // 3. Keep gold borders & warm accents vibrant against the darker panels
        float goldMask = saturate((texel.r - texel.b) * 2.2f) *
                         smoothstep(0.24f, 0.58f, rgbLum) *
                         smoothstep(0.24f, 0.55f, sat) * strength;
        texel.rgb = saturate(texel.rgb * (1.0f + goldMask * 0.22f));
    }

    // Universal Dual-Mode Font Weight (Stem Dilation) & Font Outline (Halo/Moat) Booster + Icon & Border Polish:
    // Works across translucent text overlays, opaque pre-composited UI windows, colorful skill/item icons,
    // and ornate gold/silver frames at any UI scale (1.0x..3.0x).
    float origMaxDim = max((float)texW, (float)texH) * ((isSr > 0.5f) ? 0.5f : 1.0f);
    float isTextPixel = 0.0f;
    [branch]
    if ((g_fontWeightBoost > 0.001f || g_fontOutlineBoost > 0.001f || g_iconPolish > 0.001f || g_glassmorphism > 0.001f) &&
        origMaxDim >= 24.0f && c_AlphaTestThreshold < 0.25f && isMap < 0.5f) {
        float2 subStep   = ((isSr > 0.5f) ? 1.25f : 0.62f) / texSize;
        float2 unitStep  = ((isSr > 0.5f) ? 1.85f : 0.92f) / texSize;
        float2 outerStep = ((isSr > 0.5f) ? 4.20f : 2.10f) / texSize;

        float4 cCenter = origSample;
        float4 nL = Tex0.SampleLevel(LinearSamp, i.uv + float2(-subStep.x, 0.0f), 0);
        float4 nR = Tex0.SampleLevel(LinearSamp, i.uv + float2( subStep.x, 0.0f), 0);
        float4 nU = Tex0.SampleLevel(LinearSamp, i.uv + float2(0.0f, -subStep.y), 0);
        float4 nD = Tex0.SampleLevel(LinearSamp, i.uv + float2(0.0f,  subStep.y), 0);

        float4 dTL = Tex0.SampleLevel(LinearSamp, i.uv + float2(-unitStep.x, -unitStep.y), 0);
        float4 dTR = Tex0.SampleLevel(LinearSamp, i.uv + float2( unitStep.x, -unitStep.y), 0);
        float4 dBL = Tex0.SampleLevel(LinearSamp, i.uv + float2(-unitStep.x,  unitStep.y), 0);
        float4 dBR = Tex0.SampleLevel(LinearSamp, i.uv + float2( unitStep.x,  unitStep.y), 0);

        float4 wL = Tex0.SampleLevel(LinearSamp, i.uv + float2(-outerStep.x, 0.0f), 0);
        float4 wR = Tex0.SampleLevel(LinearSamp, i.uv + float2( outerStep.x, 0.0f), 0);
        float4 wU = Tex0.SampleLevel(LinearSamp, i.uv + float2(0.0f, -outerStep.y), 0);
        float4 wD = Tex0.SampleLevel(LinearSamp, i.uv + float2(0.0f,  outerStep.y), 0);

        const float3 kLumaW = float3(0.2126f, 0.7152f, 0.0722f);
        float lC  = dot(cCenter.rgb, kLumaW) * cCenter.a;
        float lL  = dot(nL.rgb, kLumaW) * nL.a;
        float lR  = dot(nR.rgb, kLumaW) * nR.a;
        float lU  = dot(nU.rgb, kLumaW) * nU.a;
        float lD  = dot(nD.rgb, kLumaW) * nD.a;
        float lTL = dot(dTL.rgb, kLumaW) * dTL.a;
        float lTR = dot(dTR.rgb, kLumaW) * dTR.a;
        float lBL = dot(dBL.rgb, kLumaW) * dBL.a;
        float lBR = dot(dBR.rgb, kLumaW) * dBR.a;
        float lWL = dot(wL.rgb, kLumaW) * wL.a;
        float lWR = dot(wR.rgb, kLumaW) * wR.a;
        float lWU = dot(wU.rgb, kLumaW) * wU.a;
        float lWD = dot(wD.rgb, kLumaW) * wD.a;

        float4 cCardHi = cCenter; float maxCardL = lC;
        if (lL > maxCardL) { cCardHi = nL; maxCardL = lL; }
        if (lR > maxCardL) { cCardHi = nR; maxCardL = lR; }
        if (lU > maxCardL) { cCardHi = nU; maxCardL = lU; }
        if (lD > maxCardL) { cCardHi = nD; maxCardL = lD; }

        float4 cCardLo = cCenter; float minCardL = lC;
        if (lL < minCardL) { cCardLo = nL; minCardL = lL; }
        if (lR < minCardL) { cCardLo = nR; minCardL = lR; }
        if (lU < minCardL) { cCardLo = nU; minCardL = lU; }
        if (lD < minCardL) { cCardLo = nD; minCardL = lD; }

        float maxDiagL = max(max(lTL, lTR), max(lBL, lBR));
        float minDiagL = min(min(lTL, lTR), min(lBL, lBR));
        float maxHaloL = max(maxCardL, maxDiagL);
        float minHaloL = min(minCardL, minDiagL);
        float diagAvgL = 0.25f * (lTL + lTR + lBL + lBR);

        // 2-sided thin stroke ridge check: across at least one axis (H, V, or diagonals),
        // BOTH sides outside the stroke lie on the dark background (excludes 1-sided panel borders).
        float oppMinMax = min(min(max(lWL, lWR), max(lWU, lWD)),
                              min(max(lTL, lBR), max(lTR, lBL)));

        float maxCardA = max(cCenter.a, max(max(nL.a, nR.a), max(nU.a, nD.a)));
        float maxDiagA = max(max(dTL.a, dTR.a), max(dBL.a, dBR.a));
        float maxHaloA = max(maxCardA, maxDiagA);
        float minHaloA = min(min(cCenter.a, min(min(nL.a, nR.a), min(nU.a, nD.a))),
                             min(min(dTL.a, dTR.a), min(dBL.a, dBR.a)));

        float localSpan = maxHaloL - minHaloL;

        // Mode A+B: Light/Gold/Colored text on dark/mid-tone UI panels OR translucent overlays
        float lightTextMask = smoothstep(0.16f, 0.32f, localSpan) *
                              smoothstep(0.28f, 0.42f, maxHaloL) *
                              smoothstep(0.45f, 0.22f, minHaloL) *
                              smoothstep(0.55f, 0.32f, oppMinMax) *
                              smoothstep(0.62f, 0.38f, diagAvgL);

        // Mode C: Dark ink text on bright parchment/book pages
        float darkInkMask = smoothstep(0.18f, 0.34f, localSpan) *
                            smoothstep(0.45f, 0.62f, diagAvgL) *
                            smoothstep(0.36f, 0.18f, minHaloL) *
                            smoothstep(0.75f, 0.92f, minHaloA);

        isTextPixel = max(lightTextMask, darkInkMask);

        if (origMaxDim >= 48.0f) {
            // 1. Adaptive Font Weight / Stem Boost: sub-texel morphological dilation + core brightness lift
            if (g_fontWeightBoost > 0.001f) {
                float wBoost = saturate(g_fontWeightBoost);
                if (lightTextMask > 0.001f) {
                    float dilate = lightTextMask * wBoost * 0.82f;
                    texel.rgb = lerp(texel.rgb, max(texel.rgb, cCardHi.rgb), dilate);
                    texel.a   = lerp(texel.a,   max(texel.a,   cCardHi.a),   dilate);

                    float curL = dot(texel.rgb, kLumaW) * texel.a;
                    float coreMask = smoothstep(minHaloL + 0.15f * localSpan, maxHaloL, curL) * lightTextMask;
                    texel.rgb = saturate(texel.rgb * (1.0f + coreMask * wBoost * 0.32f));
                }
                if (darkInkMask > 0.001f) {
                    float inkDilate = darkInkMask * wBoost * 0.75f;
                    texel.rgb = lerp(texel.rgb, min(texel.rgb, cCardLo.rgb), inkDilate);
                }
            }

            // 2. Font Outline / Halo Boost: crisp dark border moat around glyphs + bright inner stroke contrast
            if (g_fontOutlineBoost > 0.001f && lightTextMask > 0.001f) {
                float oBoost = saturate(g_fontOutlineBoost) * lightTextMask;
                float curEffL = dot(texel.rgb, kLumaW) * texel.a;
                float relPos = saturate((curEffL - minHaloL) / max(localSpan, 1.0e-4f));

                float outerRing = smoothstep(0.52f, 0.18f, relPos);
                texel.rgb *= (1.0f - outerRing * oBoost * 0.85f);

                if (minHaloA < 0.85f) {
                    float haloAlphaTarget = maxHaloA * (0.55f + 0.30f * saturate(g_fontOutlineBoost));
                    texel.a = lerp(texel.a, max(texel.a, haloAlphaTarget), outerRing * oBoost);
                }

                float innerCore = smoothstep(0.45f, 0.80f, relPos);
                texel.rgb = saturate(texel.rgb * (1.0f + innerCore * oBoost * 0.24f));
            }
        }

        // 3. UI Icon & Gold Border Polish: micro-contrast + luster for skill/item icons and ornate bronze/gold frames
        if (g_iconPolish > 0.001f && texel.a > 0.22f && isTextPixel < 0.22f) {
            float minCardA = min(min(nL.a, nR.a), min(nU.a, nD.a));
            float pMaxC = max(texel.r, max(texel.g, texel.b));
            float pMinC = min(texel.r, min(texel.g, texel.b));
            float pSat  = (pMaxC > 1.0e-4f) ? ((pMaxC - pMinC) / pMaxC) : 0.0f;
            float pLum  = dot(texel.rgb, kLumaW);

            float polishStr = saturate(g_iconPolish);
            float warmGold = saturate((texel.r - texel.b) * 2.4f) *
                             smoothstep(0.16f, 0.42f, pSat) *
                             smoothstep(0.16f, 0.45f, pLum);

            float iconMask = smoothstep(0.20f, 0.45f, pSat) *
                             smoothstep(0.10f, 0.30f, pLum) *
                             smoothstep(0.60f, 0.85f, minCardA) *
                             (1.0f - isTextPixel);
            if (iconMask > 0.001f) {
                float3 nAvg = 0.25f * (nL.rgb + nR.rgb + nU.rgb + nD.rgb);
                float3 microHp = clamp(texel.rgb - nAvg, -0.14f, 0.14f);
                texel.rgb = saturate(texel.rgb + microHp * (polishStr * 0.85f * iconMask));
                texel.rgb = saturate(lerp(float3(pLum, pLum, pLum), texel.rgb,
                                          1.0f + polishStr * 0.24f * iconMask * (1.0f - pSat * 0.45f)));
            }

            if (warmGold > 0.001f) {
                float bevelHi = smoothstep(0.42f, 0.78f, pLum) * warmGold * polishStr;
                float bevelLo = smoothstep(0.32f, 0.12f, pLum) * warmGold * polishStr;
                texel.rgb = saturate(texel.rgb + float3(0.065f, 0.048f, 0.012f) * bevelHi - texel.rgb * (bevelLo * 0.22f));
            }
        }
    }

    // Dynamic Hovered Element / Tooltip Contrast & Backdrop Focus
    if (g_hoverFocusBoost > 0.5f && isMap < 0.5f && texel.a > 0.08f) {
        float hLum = dot(texel.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        if (hLum < 0.32f && texel.a < 0.96f) {
            texel.rgb *= 0.88f;
            texel.a = saturate(texel.a + 0.18f * (1.0f - texel.a));
        } else if (hLum > 0.55f) {
            texel.rgb = saturate(texel.rgb * 1.08f);
        }
    }

    // Eye-Care Mode: Soft-Knee Anti-Glare Highlight Roll-Off + 5000K/4300K Blue-Light Relief
    if (g_eyeComfortMode > 0.5f && texel.a > 0.01f) {
        float eLum = dot(texel.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        if (eLum > 0.70f) {
            float excess = eLum - 0.70f;
            float compStrength = (g_eyeComfortMode > 2.5f) ? 2.10f : 1.50f;
            float newLum = 0.70f + excess / (1.0f + excess * compStrength);
            texel.rgb *= (newLum / max(eLum, 1.0e-4f));
        }
        if (g_eyeComfortMode > 1.5f) {
            float3 warmFilter = (g_eyeComfortMode > 2.5f)
                ? float3(1.04f, 0.95f, 0.80f)  // Night 4300K
                : float3(1.02f, 0.98f, 0.89f); // Warm 5000K
            float origL = dot(texel.rgb, float3(0.2126f, 0.7152f, 0.0722f));
            float3 warmed = texel.rgb * warmFilter;
            float wLum = dot(warmed, float3(0.2126f, 0.7152f, 0.0722f));
            texel.rgb = saturate(warmed * (origL / max(wLum, 1.0e-4f)));
        }
    }

    // OLED Auto-Dim for static peripheral HUD bars when mouse cursor is in the central 3D area
    if (g_oledDimStrength > 0.001f && isMap < 0.5f) {
        float yNorm = i.pos.y / max(g_screenHeight, 360.0f);
        float edgeHud = max(smoothstep(0.20f, 0.08f, yNorm), smoothstep(0.76f, 0.88f, yNorm));
        texel.a *= (1.0f - edgeHud * saturate(g_oledDimStrength));
    }

    if (c_AlphaTestThreshold >= texel.a) {
        discard;
    }

    float4 outPx = texel * c_MaterialDiffuseColor;

    // UI Frosted Glassmorphism: blurs the 3D scene behind semi-transparent dark/slate HUD/chat/tooltip panels
    [branch]
    if (g_glassmorphism > 0.001f && g_hasBackdropBlur > 0.5f &&
        isMap < 0.5f && origMaxDim >= 24.0f && c_AlphaTestThreshold < 0.25f &&
        isTextPixel < 0.18f && outPx.a > 0.14f && outPx.a < 0.88f) {
        float gLum = dot(outPx.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        float gMax = max(outPx.r, max(outPx.g, outPx.b));
        float gMin = min(outPx.r, min(outPx.g, outPx.b));
        float gSat = (gMax > 1.0e-4f) ? ((gMax - gMin) / gMax) : 0.0f;

        float alphaWindow = smoothstep(0.14f, 0.28f, outPx.a) * smoothstep(0.88f, 0.74f, outPx.a);
        float darkPanelMask = smoothstep(0.34f, 0.16f, gLum) * smoothstep(0.46f, 0.18f, gSat);
        float glassMask = alphaWindow * darkPanelMask * (1.0f - isTextPixel) * saturate(g_glassmorphism);

        if (glassMask > 0.001f) {
            float2 gStep = ((isSr > 0.5f) ? 2.0f : 1.0f) / texSize;
            float gMinA = min(min(Tex0.SampleLevel(LinearSamp, i.uv + float2(-gStep.x, 0.0f), 0).a,
                                  Tex0.SampleLevel(LinearSamp, i.uv + float2( gStep.x, 0.0f), 0).a),
                              min(Tex0.SampleLevel(LinearSamp, i.uv + float2(0.0f, -gStep.y), 0).a,
                                  Tex0.SampleLevel(LinearSamp, i.uv + float2(0.0f,  gStep.y), 0).a));
            glassMask *= smoothstep(0.25f, 0.55f, gMinA);
            if (glassMask > 0.001f) {
                float2 screenUv = i.pos.xy / max(float2(g_screenWidth, g_screenHeight), 1.0f);
                float3 blurBg = BackdropBlurTex.SampleLevel(LinearSamp, screenUv, 0).rgb;
                float3 tintedFrost = lerp(blurBg * 0.80f + float3(0.015f, 0.020f, 0.028f),
                                          outPx.rgb,
                                          clamp(outPx.a * 0.95f, 0.22f, 0.68f));
                float frostedAlpha = lerp(outPx.a, max(outPx.a, 0.92f), glassMask);
                outPx.rgb = lerp(outPx.rgb, saturate(tintedFrost), glassMask);
                outPx.a   = frostedAlpha;
            }
        }
    }

    return outPx;
}
)";

ID3D11PixelShader* CompileUiPsVariant(ID3D11Device* device, PfnD3DCompile compileFn, bool hasTexMatrix) {
    const D3D_SHADER_MACRO macros[] = {
        {"HAS_TEX_MATRIX", hasTexMatrix ? "1" : "0"},
        {nullptr, nullptr}
    };
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    const HRESULT hr = compileFn(
        kUiEnhancePsSrc, sizeof(kUiEnhancePsSrc) - 1,
        hasTexMatrix ? "uie_ps_texmat" : "uie_ps_std",
        macros, nullptr, "main", "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob
    );
    if (errBlob && errBlob->GetBufferSize() > 0) {
        const char* msg = static_cast<const char*>(errBlob->GetBufferPointer());
        if (FAILED(hr)) {
            UIE_LOG_ERROR("UiEnhancer PS compile failed (variant=%d): %s", hasTexMatrix ? 1 : 0, msg);
        } else {
            UIE_LOG_WARN("UiEnhancer PS compile warning (variant=%d): %s", hasTexMatrix ? 1 : 0, msg);
        }
    }
    SafeRelease(errBlob);
    if (FAILED(hr) || !psBlob) {
        SafeRelease(psBlob);
        return nullptr;
    }

    ID3D11PixelShader* shader = nullptr;
    const HRESULT createHr = device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &shader);
    SafeRelease(psBlob);
    if (FAILED(createHr) || !shader) {
        UIE_LOG_ERROR("UiEnhancer CreatePixelShader failed (variant=%d): 0x%08X",
                      hasTexMatrix ? 1 : 0, static_cast<unsigned>(createHr));
        return nullptr;
    }
    return shader;
}

const char kUiElementScaleGsSrc[] = R"(
cbuffer UieScaleParams : register(b12) {
    float g_scaleX;
    float g_scaleY;
    float g_offsetNdcX;
    float g_offsetNdcY;
    float g_minWidthNdc;
    float g_maxWidthNdc;
    float g_minHeightNdc;
    float g_maxHeightNdc;
    float g_cursorNdcX;
    float g_cursorNdcY;
    float g_requireHover;
    float g_drawSlotTag;
};

struct GSIn {
    float4 pos   : SV_Position;
    float  clip0 : SV_ClipDistance0;
    float2 uv    : TEXCOORD0;
};

struct GSOut {
    float4 pos      : SV_Position;
    float  clip0    : SV_ClipDistance0;
    float2 uv       : TEXCOORD0;
    float4 hoverOut : TEXCOORD1;
};

[maxvertexcount(3)]
void main(triangle GSIn inp[3], inout TriangleStream<GSOut> outStream) {
    float2 n0 = inp[0].pos.xy / max(abs(inp[0].pos.w), 1.0e-6f);
    float2 n1 = inp[1].pos.xy / max(abs(inp[1].pos.w), 1.0e-6f);
    float2 n2 = inp[2].pos.xy / max(abs(inp[2].pos.w), 1.0e-6f);
    float2 bMin = min(n0, min(n1, n2));
    float2 bMax = max(n0, max(n1, n2));
    float2 span = bMax - bMin;
    bool isHovered = (g_cursorNdcX >= bMin.x - 0.008f && g_cursorNdcX <= bMax.x + 0.008f &&
                      g_cursorNdcY >= bMin.y - 0.008f && g_cursorNdcY <= bMax.y + 0.008f);
    bool sizeOk = (span.x >= g_minWidthNdc && span.x <= g_maxWidthNdc &&
                   span.y >= g_minHeightNdc && span.y <= g_maxHeightNdc);
    bool hoverOk = (g_requireHover < 0.5f) || isHovered;
    bool active = sizeOk && hoverOk &&
                  (abs(g_scaleX - 1.0f) > 1.0e-4f || abs(g_scaleY - 1.0f) > 1.0e-4f ||
                   abs(g_offsetNdcX) > 1.0e-5f || abs(g_offsetNdcY) > 1.0e-5f);
    float2 center = 0.5f * (bMin + bMax);
    float2 scale = active ? float2(g_scaleX, g_scaleY) : float2(1.0f, 1.0f);
    float2 offset = active ? float2(g_offsetNdcX, g_offsetNdcY) : float2(0.0f, 0.0f);
    float4 hovInfo = float4(g_drawSlotTag, isHovered ? 1.0f : 0.0f, span.x, span.y);
    [unroll] for (int i = 0; i < 3; ++i) {
        GSOut o;
        o.pos = inp[i].pos;
        o.clip0 = inp[i].clip0;
        o.uv = inp[i].uv;
        o.hoverOut = hovInfo;
        if (active) {
            float w = o.pos.w;
            float2 ndc = o.pos.xy / max(abs(w), 1.0e-6f);
            float2 scaledNdc = center + (ndc - center) * scale + offset;
            o.pos.xy = scaledNdc * w;
        }
        outStream.Append(o);
    }
}
)";

ID3D11GeometryShader* CompileUiScaleGs(ID3D11Device* device, PfnD3DCompile compileFn, bool& outHasSo) {
    outHasSo = false;
    ID3DBlob* gsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    const HRESULT hr = compileFn(
        kUiElementScaleGsSrc, sizeof(kUiElementScaleGsSrc) - 1,
        "uie_scale_gs", nullptr, nullptr, "main", "gs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &gsBlob, &errBlob
    );
    if (errBlob && errBlob->GetBufferPointer()) {
        const auto* msg = static_cast<const char*>(errBlob->GetBufferPointer());
        if (FAILED(hr)) {
            UIE_LOG_ERROR("UiEnhancer GS compile failed: %s", msg);
        } else {
            UIE_LOG_WARN("UiEnhancer GS compile warning: %s", msg);
        }
    }
    SafeRelease(errBlob);
    if (FAILED(hr) || !gsBlob) {
        SafeRelease(gsBlob);
        return nullptr;
    }
    ID3D11GeometryShader* gs = nullptr;
    const D3D11_SO_DECLARATION_ENTRY soDecl[] = {
        { 0, "TEXCOORD", 1, 0, 4, 0 }
    };
    const UINT bufferStrides[] = { sizeof(float) * 4 };
    HRESULT createHr = device->CreateGeometryShaderWithStreamOutput(
        gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(),
        soDecl, 1, bufferStrides, 1, 0, nullptr, &gs);
    if (SUCCEEDED(createHr) && gs) {
        outHasSo = true;
        SafeRelease(gsBlob);
        return gs;
    }

    createHr = device->CreateGeometryShader(
        gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), nullptr, &gs);
    SafeRelease(gsBlob);
    if (FAILED(createHr) || !gs) {
        UIE_LOG_ERROR("UiEnhancer CreateGeometryShader failed: 0x%08X", static_cast<unsigned>(createHr));
        return nullptr;
    }
    return gs;
}

} // namespace

UiEnhancer& UiEnhancer::Instance() {
    static auto* instance = new UiEnhancer();
    return *instance;
}

void UiEnhancer::ClearOverrideCacheLocked() {
    for (auto& [h, srv] : overrideSrvCache_) {
        if (srv) srv->Release();
    }
    overrideSrvCache_.clear();
    loadedOverrideCount_ = 0;
}

ID3D11ShaderResourceView* UiEnhancer::GetOrLoadOverrideSrvLocked(ID3D11Device* dev, uint64_t texHash, bool srgb) {
    if (!dev || texHash == 0 || !enableTextureOverrides_.load(std::memory_order_relaxed) || overrideDir_.empty()) {
        return nullptr;
    }
    auto it = overrideSrvCache_.find(texHash);
    if (it != overrideSrvCache_.end()) {
        return it->second;
    }

    char hexUpper[32];
    char hexLower[32];
    std::snprintf(hexUpper, sizeof(hexUpper), "0x%016llX", static_cast<unsigned long long>(texHash));
    std::snprintf(hexLower, sizeof(hexLower), "0x%016llx", static_cast<unsigned long long>(texHash));

    const std::filesystem::path candidates[] = {
        overrideDir_ / (std::string(hexUpper) + ".png"),
        overrideDir_ / (std::string(hexLower) + ".png"),
        overrideDir_ / (std::string(hexUpper) + ".dds"),
        overrideDir_ / (std::string(hexLower) + ".dds")
    };

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::filesystem::path matchedPath;
    for (const auto& cand : candidates) {
        if (LoadImageRgba8(cand, w, h, rgba) && w > 0 && h > 0 && rgba.size() == static_cast<size_t>(w) * h * 4) {
            matchedPath = cand;
            break;
        }
    }

    if (matchedPath.empty()) {
        overrideSrvCache_.emplace(texHash, nullptr);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{rgba.data(), w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex)) && tex) {
        dev->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }

    overrideSrvCache_.emplace(texHash, srv);
    if (srv) {
        ++loadedOverrideCount_;
        UIE_LOG_INFO("Loaded HD texture override for hash=%s (%ux%u)", hexUpper, w, h);
    }
    return srv;
}

void UiEnhancer::ApplyConfigLocked(const Config& cfg) {
    enabled_.store(cfg.enhanceUi, std::memory_order_release);
    enhanceNameplates_.store(cfg.uiEnhanceNameplates, std::memory_order_release);
    enhanceDepthTested_.store(cfg.uiEnhanceDepthTested, std::memory_order_release);
    mapMode_.store(cfg.uiMapMode, std::memory_order_release);
    splitScreen_.store(cfg.uiSplitScreen, std::memory_order_release);
    hoverMagnifier_.store(cfg.uiHoverMagnifier, std::memory_order_release);
    hoverFocusBoost_.store(cfg.uiHoverFocusBoost, std::memory_order_release);
    oledAutoDim_.store(cfg.uiOledAutoDim, std::memory_order_release);
    enableTextureOverrides_.store(cfg.enableTextureOverrides, std::memory_order_release);
    themeProfile_.store((std::min)(cfg.uiThemeProfile, 2u), std::memory_order_release);
    eyeComfortMode_.store((std::min)(cfg.uiEyeComfortMode, 3u), std::memory_order_release);
    glassmorphismAtomic_.store(cfg.uiGlassmorphism, std::memory_order_release);
    sharpness_ = cfg.uiSharpness;
    edgeAniso_ = cfg.uiEdgeAniso;
    mapSharpnessBoost_ = cfg.uiMapSharpnessBoost;
    fontOutlineBoost_ = cfg.uiFontOutlineBoost;
    fontWeightBoost_ = cfg.uiFontWeightBoost;
    themeDarkSlate_ = cfg.uiThemeDarkSlate;
    glassmorphism_ = cfg.uiGlassmorphism;
    iconPolish_ = cfg.uiIconPolish;
    oledDimOpacity_ = cfg.uiOledDimOpacity;
    hoverMagnifierKey_ = cfg.hoverMagnifierKey;
    hoverMagnifierScale_ = cfg.hoverMagnifierScale;
    overrideDir_ = cfg.resolvedOverrideDir;
    extraHashes_ = cfg.extraUiPsHashes;
    elementScaleRules_ = cfg.elementScaleRules;
    ClearOverrideCacheLocked();
    cbDirty_.store(true, std::memory_order_release);
}

void UiEnhancer::InitializeFromConfig(const Config& cfg) {
    bool expected = false;
    if (!initializedFromConfig_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ApplyConfigLocked(cfg);
    UIE_LOG_INFO("UiEnhancer configured: enabled=%s sharpness=%.2f edgeAniso=%.2f nameplates=%s extraHashes=%zu scaleRules=%zu",
                 cfg.enhanceUi ? "true" : "false", sharpness_, edgeAniso_,
                 cfg.uiEnhanceNameplates ? "true" : "false", extraHashes_.size(), elementScaleRules_.size());
}

void UiEnhancer::ReloadFromConfig(const Config& cfg) {
    initializedFromConfig_.store(true, std::memory_order_release);
    ID3D11Device* dev = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ApplyConfigLocked(cfg);
        dev = device_;
        hudNotification_ = "CONFIG RELOADED";
        hudNotificationFrames_ = 150;
    }
    if (dev) {
        const auto tracked = ResourceTracker::Instance().GetTrackedPixelShaderHashes();
        for (const auto& [ps, hash] : tracked) {
            OnPixelShaderCreated(dev, ps, hash);
        }
    }
    UIE_LOG_INFO("UiEnhancer reloaded config: enabled=%s sharpness=%.2f edgeAniso=%.2f nameplates=%s scaleRules=%zu",
                 cfg.enhanceUi ? "true" : "false", sharpness_, edgeAniso_,
                 cfg.uiEnhanceNameplates ? "true" : "false", cfg.elementScaleRules.size());
}

void UiEnhancer::ToggleEnabled() {
    const bool next = !enabled_.load(std::memory_order_relaxed);
    enabled_.store(next, std::memory_order_release);
    UIE_LOG_INFO("UI Enhancement toggled: %s", next ? "ON" : "OFF");
}

void UiEnhancer::ToggleSplitScreen() {
    const bool next = !splitScreen_.load(std::memory_order_relaxed);
    splitScreen_.store(next, std::memory_order_release);
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("Split-Screen A/B comparison toggled: %s", next ? "ON" : "OFF");
}

void UiEnhancer::ToggleMapMode() {
    const bool next = !mapMode_.load(std::memory_order_relaxed);
    mapMode_.store(next, std::memory_order_release);
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("MapMode toggled: %s", next ? "ON" : "OFF");
}

void UiEnhancer::ToggleHoverMagnifier() {
    const bool next = !hoverMagnifier_.load(std::memory_order_relaxed);
    hoverMagnifier_.store(next, std::memory_order_release);
    UIE_LOG_INFO("Smart Hover Magnifier toggled: %s", next ? "ON" : "OFF");
}

void UiEnhancer::ToggleHoverFocusBoost() {
    const bool next = !hoverFocusBoost_.load(std::memory_order_relaxed);
    hoverFocusBoost_.store(next, std::memory_order_release);
    UIE_LOG_INFO("Hover Focus & Contrast Boost toggled: %s", next ? "ON" : "OFF");
}

void UiEnhancer::ToggleOledAutoDim() {
    const bool next = !oledAutoDim_.load(std::memory_order_relaxed);
    oledAutoDim_.store(next, std::memory_order_release);
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("OLED Auto-Dim toggled: %s", next ? "ON" : "OFF");
}

void UiEnhancer::CycleSharpnessPreset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharpness_ < 0.52f) {
        sharpness_ = 0.70f;
    } else if (sharpness_ < 0.88f) {
        sharpness_ = 1.05f;
    } else if (sharpness_ < 1.22f) {
        sharpness_ = 1.40f;
    } else {
        sharpness_ = 0.35f;
    }
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("UI Enhancement sharpness cycled to %.2f", sharpness_);
}

void UiEnhancer::CycleFontOutlineBoost() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fontOutlineBoost_ < 0.10f) {
        fontOutlineBoost_ = 0.35f;
    } else if (fontOutlineBoost_ < 0.50f) {
        fontOutlineBoost_ = 0.65f;
    } else if (fontOutlineBoost_ < 0.85f) {
        fontOutlineBoost_ = 1.00f;
    } else {
        fontOutlineBoost_ = 0.00f;
    }
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("Font Outline Boost cycled to %.2f", fontOutlineBoost_);
}

void UiEnhancer::CycleFontWeightBoost() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fontWeightBoost_ < 0.10f) {
        fontWeightBoost_ = 0.25f;
    } else if (fontWeightBoost_ < 0.40f) {
        fontWeightBoost_ = 0.50f;
    } else if (fontWeightBoost_ < 0.70f) {
        fontWeightBoost_ = 0.80f;
    } else {
        fontWeightBoost_ = 0.00f;
    }
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("Font Weight / Stem Boost cycled to %.2f", fontWeightBoost_);
}

void UiEnhancer::CycleThemeDarkSlate() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t prof = themeProfile_.load(std::memory_order_relaxed);
    if (themeDarkSlate_ < 0.15f) {
        themeProfile_.store(0u, std::memory_order_release);
        themeDarkSlate_ = 0.75f; // Obsidian Slate 75%
    } else if (prof == 0u && themeDarkSlate_ < 0.90f) {
        themeProfile_.store(0u, std::memory_order_release);
        themeDarkSlate_ = 1.00f; // Midnight Slate 100%
    } else if (prof == 0u) {
        themeProfile_.store(1u, std::memory_order_release);
        themeDarkSlate_ = 0.85f; // Elven Bronze
    } else if (prof == 1u) {
        themeProfile_.store(2u, std::memory_order_release);
        themeDarkSlate_ = 0.90f; // Dwarven Iron
    } else {
        themeProfile_.store(0u, std::memory_order_release);
        themeDarkSlate_ = 0.00f; // Off
    }
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("Modern UI Theme cycled: profile=%u strength=%.2f",
                 themeProfile_.load(std::memory_order_relaxed), themeDarkSlate_);
}

void UiEnhancer::CycleEyeComfortMode() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t next = (eyeComfortMode_.load(std::memory_order_relaxed) + 1u) % 4u;
    eyeComfortMode_.store(next, std::memory_order_release);
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("Eye Comfort Mode cycled to %u", next);
}

void UiEnhancer::CycleGlassmorphism() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (glassmorphism_ < 0.10f) {
        glassmorphism_ = 0.40f;
    } else if (glassmorphism_ < 0.50f) {
        glassmorphism_ = 0.55f;
    } else if (glassmorphism_ < 0.72f) {
        glassmorphism_ = 0.85f;
    } else {
        glassmorphism_ = 0.00f;
    }
    glassmorphismAtomic_.store(glassmorphism_, std::memory_order_release);
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("UI Glassmorphism cycled to %.2f", glassmorphism_);
}

void UiEnhancer::CycleIconPolish() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (iconPolish_ < 0.10f) {
        iconPolish_ = 0.30f;
    } else if (iconPolish_ < 0.36f) {
        iconPolish_ = 0.40f;
    } else if (iconPolish_ < 0.60f) {
        iconPolish_ = 0.70f;
    } else {
        iconPolish_ = 0.00f;
    }
    cbDirty_.store(true, std::memory_order_release);
    UIE_LOG_INFO("UI Icon & Gold Border Polish cycled to %.2f", iconPolish_);
}

float UiEnhancer::GetGlassmorphism() const noexcept {
    return glassmorphismAtomic_.load(std::memory_order_acquire);
}

uint32_t UiEnhancer::GetEyeComfortMode() const noexcept {
    return eyeComfortMode_.load(std::memory_order_acquire);
}

void UiEnhancer::SetHudNotification(const std::string& msg, uint32_t durationFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    hudNotification_ = msg;
    hudNotificationFrames_ = durationFrames;
}

void UiEnhancer::ExportCurrentStateToConfig(Config& ioConfig) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ioConfig.enhanceUi = enabled_.load(std::memory_order_relaxed);
    ioConfig.uiMapMode = mapMode_.load(std::memory_order_relaxed);
    ioConfig.uiSplitScreen = splitScreen_.load(std::memory_order_relaxed);
    ioConfig.uiHoverMagnifier = hoverMagnifier_.load(std::memory_order_relaxed);
    ioConfig.uiHoverFocusBoost = hoverFocusBoost_.load(std::memory_order_relaxed);
    ioConfig.uiOledAutoDim = oledAutoDim_.load(std::memory_order_relaxed);
    ioConfig.enableTextureOverrides = enableTextureOverrides_.load(std::memory_order_relaxed);
    ioConfig.uiSharpness = sharpness_;
    ioConfig.uiEdgeAniso = edgeAniso_;
    ioConfig.uiMapSharpnessBoost = mapSharpnessBoost_;
    ioConfig.uiFontOutlineBoost = fontOutlineBoost_;
    ioConfig.uiFontWeightBoost = fontWeightBoost_;
    ioConfig.uiThemeDarkSlate = themeDarkSlate_;
    ioConfig.uiGlassmorphism = glassmorphism_;
    ioConfig.uiIconPolish = iconPolish_;
    ioConfig.uiThemeProfile = themeProfile_.load(std::memory_order_relaxed);
    ioConfig.uiEyeComfortMode = eyeComfortMode_.load(std::memory_order_relaxed);
    ioConfig.uiOledDimOpacity = oledDimOpacity_;
    ioConfig.elementScaleRules = elementScaleRules_;
}

bool UiEnhancer::CopyHoveredElementToClipboard(std::string* outRuleLine) {
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hash = lastHoveredTexHash_;
        w = lastHoveredTexW_;
        h = lastHoveredTexH_;
    }
    if (hash == 0 && (w == 0 || h == 0)) {
        SetHudNotification("NO HOVER ELEMENT UNDER CURSOR", 150);
        return false;
    }

    char lineBuf[256];
    if (hash != 0) {
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "Element_%ux%u_%04llX = 0x%016llX, +15%%",
                      w, h,
                      static_cast<unsigned long long>((hash >> 48) & 0xFFFFu),
                      static_cast<unsigned long long>(hash));
    } else {
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "Element_%ux%u = %ux%u, +15%%",
                      w, h, w, h);
    }
    const std::string ruleText(lineBuf);
    if (outRuleLine) *outRuleLine = ruleText;

    bool clipOk = false;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        const size_t bytes = ruleText.size() + 1;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void* ptr = GlobalLock(hMem);
            if (ptr) {
                std::memcpy(ptr, ruleText.c_str(), bytes);
                GlobalUnlock(hMem);
                if (SetClipboardData(CF_TEXT, hMem) != nullptr) {
                    clipOk = true;
                    hMem = nullptr; // Clipboard owns hMem now
                }
            }
            if (hMem) GlobalFree(hMem);
        }
        CloseClipboard();
    }

    char msg[128];
    if (hash != 0) {
        std::snprintf(msg, sizeof(msg), "COPIED 0x%016llX (%ux%u) TO CLIPBOARD",
                      static_cast<unsigned long long>(hash), w, h);
    } else {
        std::snprintf(msg, sizeof(msg), "COPIED %ux%u RULE TO CLIPBOARD", w, h);
    }
    SetHudNotification(msg, 180);
    UIE_LOG_INFO("Copied hovered UI element rule to clipboard (%s): %s",
                 clipOk ? "OK" : "LOG_ONLY", ruleText.c_str());
    return true;
}

bool UiEnhancer::AdjustHoveredElementScale(float deltaScale, Config& ioConfig) {
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    float newScale = 1.0f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hash = lastHoveredTexHash_;
        w = lastHoveredTexW_;
        h = lastHoveredTexH_;
        if (hash == 0 && (w == 0 || h == 0)) {
            hudNotification_ = "NO HOVER ELEMENT UNDER CURSOR";
            hudNotificationFrames_ = 150;
            return false;
        }

        ElementScaleRule* found = nullptr;
        for (auto& r : elementScaleRules_) {
            if ((hash != 0 && r.texHash == hash) ||
                (hash == 0 && r.texHash == 0 && r.texWidth == w && r.texHeight == h)) {
                found = &r;
                break;
            }
        }
        if (found) {
            newScale = (std::clamp)(found->scaleX + deltaScale, 0.50f, 3.00f);
            found->scaleX = newScale;
            found->scaleY = newScale;
        } else {
            ElementScaleRule rule{};
            char nameBuf[64];
            if (hash != 0) {
                std::snprintf(nameBuf, sizeof(nameBuf), "Element_%ux%u_%04llX",
                              w, h, static_cast<unsigned long long>((hash >> 48) & 0xFFFFu));
                rule.texHash = hash;
            } else {
                std::snprintf(nameBuf, sizeof(nameBuf), "Element_%ux%u", w, h);
                rule.texWidth = w;
                rule.texHeight = h;
            }
            rule.name = nameBuf;
            newScale = (std::clamp)(1.0f + deltaScale, 0.50f, 3.00f);
            rule.scaleX = newScale;
            rule.scaleY = newScale;
            rule.maxQuadWidthPx = 4096.0f;
            rule.maxQuadHeightPx = 4096.0f;
            elementScaleRules_.push_back(std::move(rule));
        }
        ioConfig.elementScaleRules = elementScaleRules_;

        char msg[128];
        const int pct = static_cast<int>(std::lround(newScale * 100.0f));
        if (hash != 0) {
            std::snprintf(msg, sizeof(msg), "HOVER 0x%08llX.. SCALE -> %d%% (SAVED)",
                          static_cast<unsigned long long>(hash >> 32), pct);
        } else {
            std::snprintf(msg, sizeof(msg), "HOVER %ux%u SCALE -> %d%% (SAVED)", w, h, pct);
        }
        hudNotification_ = msg;
        hudNotificationFrames_ = 180;
    }
    ioConfig.Save();
    UIE_LOG_INFO("Adjusted hovered UI element (hash=0x%016llX %ux%u) scale to %.2f and saved INI",
                 static_cast<unsigned long long>(hash), w, h, newScale);
    return true;
}

bool UiEnhancer::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

bool UiEnhancer::IsSplitScreenEnabled() const noexcept {
    return splitScreen_.load(std::memory_order_acquire);
}

void UiEnhancer::SetSwapChainViewportInfo(uint32_t width, uint32_t height, uintptr_t hwnd) noexcept {
    const uint32_t prevW = swapWidth_.exchange(width, std::memory_order_relaxed);
    const uint32_t prevH = swapHeight_.exchange(height, std::memory_order_relaxed);
    swapHwnd_.store(hwnd, std::memory_order_relaxed);
    if (prevW != width || prevH != height) {
        cbDirty_.store(true, std::memory_order_release);
    }
}

void UiEnhancer::ReleaseDeviceResources() {
    ready_ = false;
    scaleGsHasSo_ = false;
    hoverCandidates_.clear();
    ClearOverrideCacheLocked();
    SafeRelease(enhancedPsA_);
    SafeRelease(enhancedPsB_);
    SafeRelease(scaleGs_);
    SafeRelease(scaleCb_);
    SafeRelease(soBuffer_);
    SafeRelease(soStagingBuffer_);
    for (auto*& s : linearSamplers_) SafeRelease(s);
    for (auto*& cb : paramCbs_) SafeRelease(cb);
    for (auto*& cb : hoveredParamCbs_) SafeRelease(cb);
    device_ = nullptr;
}

void UiEnhancer::RefreshFastPointersLocked() {
    ID3D11PixelShader* a = nullptr;
    ID3D11PixelShader* b = nullptr;
    uint32_t countA = 0;
    uint32_t countB = 0;
    for (const auto& [ps, reg] : targetShaders_) {
        if (reg.device == device_) {
            if (reg.variant == 0) {
                if (!a) a = ps;
                ++countA;
            } else {
                if (!b) b = ps;
                ++countB;
            }
        }
    }
    fastOrigA_.store(a, std::memory_order_release);
    fastOrigB_.store(b, std::memory_order_release);
    fastPointersComplete_.store(countA <= 1 && countB <= 1, std::memory_order_release);
    registeredCount_.store(countA + countB, std::memory_order_release);
}

int UiEnhancer::MatchTargetVariant(uint64_t hash) const {
    if (hash == 0) return -1;
    if (hash == kHashUiColorOpacityPs) return 0;
    if (hash == kHashUiColorOpacityTexMatPs) return 1;
    for (uint64_t extra : extraHashes_) {
        if (hash == extra) return 0;
    }
    return -1;
}

void UiEnhancer::OnDeviceCreated(ID3D11Device* device) {
    if (!device) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ == device && (ready_ || initFailed_)) {
        return;
    }
    ReleaseDeviceResources();
    initFailed_ = false;
    device_ = device;

    auto compileFn = GetD3DCompileFn();
    if (!compileFn) {
        UIE_LOG_WARN("UiEnhancer: d3dcompiler_47.dll unavailable, UI enhancement disabled");
        initFailed_ = true;
        RefreshFastPointersLocked();
        return;
    }

    enhancedPsA_ = CompileUiPsVariant(device, compileFn, false);
    enhancedPsB_ = CompileUiPsVariant(device, compileFn, true);
    scaleGs_ = CompileUiScaleGs(device, compileFn, scaleGsHasSo_);
    if (!enhancedPsA_ || !enhancedPsB_) {
        ReleaseDeviceResources();
        initFailed_ = true;
        RefreshFastPointersLocked();
        return;
    }

    if (scaleGs_) {
        D3D11_BUFFER_DESC gsCbDesc{};
        gsCbDesc.ByteWidth = sizeof(UiElementScaleGpuParams);
        gsCbDesc.Usage = D3D11_USAGE_DEFAULT;
        gsCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        UiElementScaleGpuParams gsInit{};
        D3D11_SUBRESOURCE_DATA gsInitData{&gsInit, sizeof(gsInit), 0};
        if (FAILED(device->CreateBuffer(&gsCbDesc, &gsInitData, &scaleCb_)) || !scaleCb_) {
            SafeRelease(scaleGs_);
            scaleGsHasSo_ = false;
        }
        if (scaleGsHasSo_) {
            D3D11_BUFFER_DESC soDesc{};
            soDesc.ByteWidth = static_cast<UINT>(kMaxSoVertices * sizeof(float) * 4);
            soDesc.Usage = D3D11_USAGE_DEFAULT;
            soDesc.BindFlags = D3D11_BIND_STREAM_OUTPUT;

            D3D11_BUFFER_DESC stDesc{};
            stDesc.ByteWidth = soDesc.ByteWidth;
            stDesc.Usage = D3D11_USAGE_STAGING;
            stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            if (FAILED(device->CreateBuffer(&soDesc, nullptr, &soBuffer_)) || !soBuffer_ ||
                FAILED(device->CreateBuffer(&stDesc, nullptr, &soStagingBuffer_)) || !soStagingBuffer_) {
                SafeRelease(soBuffer_);
                SafeRelease(soStagingBuffer_);
                scaleGsHasSo_ = false;
            }
        }
    }

    for (int idx = 0; idx < 4; ++idx) {
        const bool wrapU = (idx & 1) != 0;
        const bool wrapV = (idx & 2) != 0;

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = wrapU ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.AddressV = wrapV ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&samp, &linearSamplers_[idx])) || !linearSamplers_[idx]) {
            UIE_LOG_ERROR("UiEnhancer CreateSamplerState failed (idx=%d)", idx);
            ReleaseDeviceResources();
            initFailed_ = true;
            RefreshFastPointersLocked();
            return;
        }
    }

    const float screenW = static_cast<float>((std::max)(swapWidth_.load(std::memory_order_relaxed), 64u));
    const float splitMinX = splitScreen_.load(std::memory_order_relaxed)
        ? (0.5f * screenW)
        : 0.0f;
    const float screenH = static_cast<float>((std::max)(swapHeight_.load(std::memory_order_relaxed), 360u));
    const float oledDim = (oledAutoDim_.load(std::memory_order_relaxed) && cursorInCenter3d_.load(std::memory_order_relaxed))
        ? (1.0f - oledDimOpacity_)
        : 0.0f;

    for (int cbIdx = 0; cbIdx < 32; ++cbIdx) {
        GpuParams initParams{
            sharpness_,
            edgeAniso_,
            ((cbIdx & 1) != 0) ? 1.0f : 0.0f,
            ((cbIdx & 2) != 0) ? 1.0f : 0.0f,
            ((cbIdx & 4) != 0) ? 1.0f : 0.0f,
            ((cbIdx & 8) != 0) ? 1.0f : 0.0f,
            mapSharpnessBoost_,
            splitMinX,
            fontOutlineBoost_,
            themeDarkSlate_,
            oledDim,
            screenH,
            fontWeightBoost_,
            static_cast<float>(themeProfile_.load(std::memory_order_relaxed)),
            static_cast<float>(eyeComfortMode_.load(std::memory_order_relaxed)),
            0.0f,
            glassmorphism_,
            iconPolish_,
            screenW,
            ((cbIdx & 16) != 0) ? 1.0f : 0.0f
        };
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(GpuParams);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initData{&initParams, sizeof(initParams), 0};
        if (FAILED(device->CreateBuffer(&cbDesc, &initData, &paramCbs_[cbIdx])) || !paramCbs_[cbIdx]) {
            UIE_LOG_ERROR("UiEnhancer CreateBuffer failed (cbIdx=%d)", cbIdx);
            ReleaseDeviceResources();
            initFailed_ = true;
            RefreshFastPointersLocked();
            return;
        }
        initParams.hoverFocusBoost = 1.0f;
        D3D11_SUBRESOURCE_DATA hovInitData{&initParams, sizeof(initParams), 0};
        if (FAILED(device->CreateBuffer(&cbDesc, &hovInitData, &hoveredParamCbs_[cbIdx])) || !hoveredParamCbs_[cbIdx]) {
            UIE_LOG_ERROR("UiEnhancer CreateBuffer (hovered) failed (cbIdx=%d)", cbIdx);
            ReleaseDeviceResources();
            initFailed_ = true;
            RefreshFastPointersLocked();
            return;
        }
    }

    cbDirty_.store(false, std::memory_order_release);
    ready_ = true;
    RefreshFastPointersLocked();
    UIE_LOG_INFO("UiEnhancer v0.5 initialized shaders on device %p (sharpness=%.2f, edgeAniso=%.2f, mapMode=%s)",
                 static_cast<void*>(device), sharpness_, edgeAniso_,
                 mapMode_.load(std::memory_order_relaxed) ? "true" : "false");
}

void UiEnhancer::OnPixelShaderCreated(ID3D11Device* device, ID3D11PixelShader* ps, uint64_t hash) {
    if (!device || !ps || hash == 0) return;

    int variant = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        variant = MatchTargetVariant(hash);
        if (variant < 0) return;
        const auto [it, inserted] = targetShaders_.try_emplace(
            ps, RegisteredPs{device, static_cast<uint8_t>(variant)});
        if (!inserted) {
            it->second = RegisteredPs{device, static_cast<uint8_t>(variant)};
        } else {
            auto* tag = new UiPsDestructionTag(ps);
            ps->SetPrivateDataInterface(kUiPsTagGuid, tag);
            tag->Release();
        }
        RefreshFastPointersLocked();
    }

    UIE_LOG_INFO("UiEnhancer matched target UI PixelShader %p (hash=0x%016llX, variant=%d)",
                 static_cast<void*>(ps), static_cast<unsigned long long>(hash), variant);
}

void UiEnhancer::OnPixelShaderDestroyed(ID3D11PixelShader* ps) {
    if (!ps) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (targetShaders_.erase(ps) > 0) {
        RefreshFastPointersLocked();
    }
}

bool UiEnhancer::PrepareDraw(ID3D11DeviceContext* ctx, const ContextState& st, UiDrawOverride& out, UINT drawCount) {
    out.active = false;
    out.isUiCandidateDraw = false;
    out.isBackbufferTarget = false;
    out.srApplied = false;
    out.isMapDraw = false;
    out.scaleApplied = false;
    out.gsHoverProbe = false;
    out.resetSoOffset = false;
    out.replacementPs = nullptr;
    out.replacementGs = nullptr;
    out.replacementSrv0 = nullptr;
    out.backdropSrv = nullptr;
    out.scaleCb = nullptr;
    out.soBuffer = nullptr;
    out.cbMask = 0;
    out.isHoverFocus = false;
    if (!st.ps || !st.psSrvs[0] || !st.rtvs[0]) return false;

    const uint32_t regCount = registeredCount_.load(std::memory_order_relaxed);
    if (regCount == 0) return false;

    // Fast lock-free filter for the 96% non-UI draw calls when <=1 target per variant exists.
    if (fastPointersComplete_.load(std::memory_order_acquire) &&
        st.ps != fastOrigA_.load(std::memory_order_relaxed) &&
        st.ps != fastOrigB_.load(std::memory_order_relaxed)) {
        return false;
    }

    uint8_t variant = 0;
    ID3D11Device* devPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) return false;
        auto it = targetShaders_.find(st.ps);
        if (it == targetShaders_.end() || it->second.device != device_) {
            return false;
        }
        variant = it->second.variant;
        if (!((variant == 1) ? enhancedPsB_ : enhancedPsA_)) return false;
        devPtr = device_;
    }

    auto& tracker = ResourceTracker::Instance();

    // Require Backbuffer or large UI-scale RT (excludes 256x256 offscreen compositing RTs).
    auto rtvMeta = tracker.GetOrTrackRtv(st.rtvs[0]);
    if (!rtvMeta) return false;
    if (!rtvMeta->isBackbuffer && !(rtvMeta->width >= 1280 && rtvMeta->height >= 720)) {
        return false;
    }

    // Skip depth-writing 3D draws, while optionally allowing read-only depth UI (floating 3D nameplates).
    if (st.dsv != nullptr) {
        bool depthEnabled = true;
        bool depthWrite = true;
        if (st.depthStencilState) {
            if (auto ds = tracker.GetDepthStencilState(st.depthStencilState)) {
                depthEnabled = (ds->desc.DepthEnable != FALSE);
                depthWrite = (ds->desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO);
            }
        }
        if (depthEnabled) {
            const bool allowNameplate = enhanceNameplates_.load(std::memory_order_relaxed) && !depthWrite;
            if (!enhanceDepthTested_.load(std::memory_order_relaxed) && !allowNameplate) {
                return false;
            }
        }
    }

    // Require 2D RGBA/BGRA UI/Map/Font texture >= 8x8 (skips 1x1 solid quads and 1280x720 A8_UNORM fog mask).
    auto texMeta = tracker.GetTextureForSrv(st.psSrvs[0]);
    if (!texMeta) return false;
    if (texMeta->width < 8 || texMeta->height < 8 ||
        texMeta->arraySize != 1 || texMeta->sampleCount != 1 ||
        !IsColorRgbaUiFormat(texMeta->format)) {
        return false;
    }

    // Mark as UI candidate targeting backbuffer BEFORE checking enabled_, so WorldPostFx (F5) still triggers
    // before the first UI draw even when UIEnhancer (F8) is toggled off!
    out.isUiCandidateDraw = true;
    out.isBackbufferTarget = rtvMeta->isBackbuffer;

    // Update fallback CPU hovered UI texture only when GPU Stream-Output hover probe is unavailable
    const int32_t curX = cursorX_.load(std::memory_order_relaxed);
    const int32_t curY = cursorY_.load(std::memory_order_relaxed);
    if (!scaleGsHasSo_ && curX >= 0 && curY >= 0 && texMeta->initialHash != 0) {
        bool hovered = false;
        if (st.hasScissor0) {
            const auto& sc = st.scissor0;
            const int32_t scW = sc.right - sc.left;
            const int32_t scH = sc.bottom - sc.top;
            if (scW > 0 && scH > 0 &&
                (static_cast<uint32_t>(scW) < rtvMeta->width || static_cast<uint32_t>(scH) < rtvMeta->height) &&
                curX >= sc.left && curX < sc.right && curY >= sc.top && curY < sc.bottom) {
                hovered = true;
            }
        } else if (st.hasViewport0) {
            const auto& vp = st.viewport0;
            if (vp.Width > 0 && vp.Height > 0 &&
                (vp.Width < static_cast<float>(rtvMeta->width) || vp.Height < static_cast<float>(rtvMeta->height)) &&
                curX >= static_cast<int32_t>(vp.TopLeftX) && curX < static_cast<int32_t>(vp.TopLeftX + vp.Width) &&
                curY >= static_cast<int32_t>(vp.TopLeftY) && curY < static_cast<int32_t>(vp.TopLeftY + vp.Height)) {
                hovered = true;
            }
        }
        if (hovered) {
            liveHoveredTexId_.store(texMeta->id, std::memory_order_relaxed);
            liveHoveredTexHash_.store(texMeta->initialHash, std::memory_order_relaxed);
            liveHoveredTexW_.store(texMeta->width, std::memory_order_relaxed);
            liveHoveredTexH_.store(texMeta->height, std::memory_order_relaxed);
        }
    }

    if (!enabled_.load(std::memory_order_relaxed)) return false;

    int wrapIdx = 0;
    if (st.psSamplers[0]) {
        if (auto samp = tracker.GetSampler(st.psSamplers[0])) {
            if (samp->desc.AddressU == D3D11_TEXTURE_ADDRESS_WRAP) wrapIdx |= 1;
            if (samp->desc.AddressV == D3D11_TEXTURE_ADDRESS_WRAP) wrapIdx |= 2;
        }
    }

    const bool isMapDraw = mapMode_.load(std::memory_order_relaxed) &&
                           (texMeta->bindFlags & D3D11_BIND_RENDER_TARGET) == 0 &&
                           texMeta->width >= 896 && texMeta->height >= 576;

    // 1. Check HD Texture Override from lotro_uie_overrides/<0xHash>.{png,dds}
    ID3D11ShaderResourceView* replacementSrv = nullptr;
    if (devPtr && texMeta->initialHash != 0 && enableTextureOverrides_.load(std::memory_order_relaxed)) {
        const bool isSrgb = (texMeta->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                             texMeta->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                             texMeta->format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
                             texMeta->format == DXGI_FORMAT_BC1_UNORM_SRGB ||
                             texMeta->format == DXGI_FORMAT_BC2_UNORM_SRGB ||
                             texMeta->format == DXGI_FORMAT_BC3_UNORM_SRGB);
        std::lock_guard<std::mutex> lock(mutex_);
        replacementSrv = GetOrLoadOverrideSrvLocked(devPtr, texMeta->initialHash, isSrgb);
    }

    // 2. Outside UiEnhancer mutex: run or look up 2x Mini-CNN upscaled texture from VRAM cache if no override SRV
    if (!replacementSrv && ctx && devPtr && UiSrCache::Instance().IsEnabled()) {
        replacementSrv = UiSrCache::Instance().GetOrUpscale(
            devPtr, ctx, st.psSrvs[0], (wrapIdx & 1) != 0, (wrapIdx & 2) != 0);
    }

    ID3D11ShaderResourceView* backdropSrv = nullptr;
    if (rtvMeta->isBackbuffer && !isMapDraw && glassmorphismAtomic_.load(std::memory_order_relaxed) > 0.001f) {
        backdropSrv = WorldPostFx::Instance().GetBackdropBlurSrv();
    }

    const int baseCbMask = wrapIdx | (isMapDraw ? 4 : 0) | (replacementSrv ? 8 : 0);
    const int cbMask = baseCbMask | (backdropSrv ? 16 : 0);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return false;
    ID3D11SamplerState* linearSamp = linearSamplers_[wrapIdx];
    const bool useHoverCb = hoverFocusBoost_.load(std::memory_order_relaxed) &&
                            !isMapDraw && texMeta->initialHash != 0 &&
                            texMeta->initialHash == lastHoveredTexHash_ &&
                            hoveredParamCbs_[cbMask] != nullptr;
    ID3D11Buffer* cb = useHoverCb ? hoveredParamCbs_[cbMask] : paramCbs_[cbMask];
    if (!linearSamp || !cb) return false;

    const float vpW = (st.hasViewport0 && st.viewport0.Width > 0.0f)
        ? st.viewport0.Width : static_cast<float>((std::max)(rtvMeta->width, 1u));
    const float vpH = (st.hasViewport0 && st.viewport0.Height > 0.0f)
        ? st.viewport0.Height : static_cast<float>((std::max)(rtvMeta->height, 1u));
    const float vpTopX = st.hasViewport0 ? st.viewport0.TopLeftX : 0.0f;
    const float vpTopY = st.hasViewport0 ? st.viewport0.TopLeftY : 0.0f;
    const float invW = 2.0f / (std::max)(vpW, 1.0f);
    const float invH = 2.0f / (std::max)(vpH, 1.0f);

    float curNdcX = -10.0f;
    float curNdcY = -10.0f;
    bool cursorInViewport = false;
    if (curX >= 0 && curY >= 0) {
        curNdcX = ((static_cast<float>(curX) - vpTopX) / (std::max)(vpW, 1.0f)) * 2.0f - 1.0f;
        curNdcY = 1.0f - ((static_cast<float>(curY) - vpTopY) / (std::max)(vpH, 1.0f)) * 2.0f;
        cursorInViewport = (curNdcX >= -1.0f && curNdcX <= 1.0f && curNdcY >= -1.0f && curNdcY <= 1.0f);
    }

    // Match optional [ElementScale] rules, Smart Hover Magnifier (Hold ALT), and GPU Quad Hover Probe
    if (scaleGs_ && scaleCb_ &&
        (st.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST ||
         st.topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)) {
        for (const auto& rule : elementScaleRules_) {
            const bool hashOk = (rule.texHash == 0) ? (rule.texWidth != 0 && rule.texHeight != 0)
                                                    : (texMeta->initialHash != 0 && rule.texHash == texMeta->initialHash);
            const bool sizeOk = (rule.texWidth == 0 && rule.texHeight == 0) ||
                                (rule.texWidth == texMeta->width && rule.texHeight == texMeta->height);
            if (hashOk && sizeOk) {
                out.scaleApplied = true;
                out.replacementGs = scaleGs_;
                out.scaleCb = scaleCb_;
                out.scaleParams.scaleX = rule.scaleX;
                out.scaleParams.scaleY = rule.scaleY;
                out.scaleParams.offsetNdcX = rule.offsetPxX * invW;
                out.scaleParams.offsetNdcY = -rule.offsetPxY * invH;
                out.scaleParams.minWidthNdc = rule.minQuadWidthPx * invW;
                out.scaleParams.maxWidthNdc = rule.maxQuadWidthPx * invW;
                out.scaleParams.minHeightNdc = rule.minQuadHeightPx * invH;
                out.scaleParams.maxHeightNdc = rule.maxQuadHeightPx * invH;
                out.scaleParams.cursorNdcX = curNdcX;
                out.scaleParams.cursorNdcY = curNdcY;
                out.scaleParams.requireHover = 0.0f;
                out.scaleParams.drawSlotTag = 0.0f;
                break;
            }
        }

        // Smart Hover Magnifier: when holding hoverMagnifierKey_ (ALT) over any compact UI quad
        if (!out.scaleApplied && !isMapDraw && cursorInViewport && texMeta->initialHash != 0 &&
            hoverMagnifier_.load(std::memory_order_relaxed) && hoverMagnifierKey_ != 0 &&
            (GetAsyncKeyState(static_cast<int>(hoverMagnifierKey_ & kHotkeyVkMask)) & 0x8000) != 0) {
            out.scaleApplied = true;
            out.replacementGs = scaleGs_;
            out.scaleCb = scaleCb_;
            out.scaleParams.scaleX = hoverMagnifierScale_;
            out.scaleParams.scaleY = hoverMagnifierScale_;
            out.scaleParams.offsetNdcX = 0.0f;
            out.scaleParams.offsetNdcY = 0.0f;
            out.scaleParams.minWidthNdc = 4.0f * invW;
            out.scaleParams.maxWidthNdc = 420.0f * invW;
            out.scaleParams.minHeightNdc = 4.0f * invH;
            out.scaleParams.maxHeightNdc = 220.0f * invH;
            out.scaleParams.cursorNdcX = curNdcX;
            out.scaleParams.cursorNdcY = curNdcY;
            out.scaleParams.requireHover = 1.0f;
            out.scaleParams.drawSlotTag = 0.0f;
        }

        // GPU Stream-Output Quad Hover Probe: exact NDC bounding-box hit test for visible UI quads
        if (scaleGsHasSo_ && soBuffer_ && soStagingBuffer_ && cursorInViewport &&
            !isMapDraw && texMeta->initialHash != 0 &&
            (drawCount == 0 || drawCount <= 24) &&
            hoverCandidates_.size() < kMaxHoverCandidates) {
            const uint32_t slotIdx = static_cast<uint32_t>(hoverCandidates_.size());
            hoverCandidates_.push_back(HoverCandidateDraw{
                texMeta->id, texMeta->initialHash, texMeta->width, texMeta->height
            });
            out.gsHoverProbe = true;
            out.resetSoOffset = (slotIdx == 0);
            out.soBuffer = soBuffer_;
            out.scaleParams.drawSlotTag = static_cast<float>(hoverFrameTag_ * 1024u + slotIdx);
            if (!out.scaleApplied) {
                out.replacementGs = scaleGs_;
                out.scaleCb = scaleCb_;
                out.scaleParams.scaleX = 1.0f;
                out.scaleParams.scaleY = 1.0f;
                out.scaleParams.offsetNdcX = 0.0f;
                out.scaleParams.offsetNdcY = 0.0f;
                out.scaleParams.minWidthNdc = 0.0f;
                out.scaleParams.maxWidthNdc = 10.0f;
                out.scaleParams.minHeightNdc = 0.0f;
                out.scaleParams.maxHeightNdc = 10.0f;
                out.scaleParams.cursorNdcX = curNdcX;
                out.scaleParams.cursorNdcY = curNdcY;
                out.scaleParams.requireHover = 0.0f;
            }
        }
    }

    out.active = true;
    out.isMapDraw = isMapDraw;
    out.cbMask = static_cast<uint8_t>(baseCbMask);
    out.isHoverFocus = useHoverCb;
    out.origPs = st.ps;
    out.replacementPs = (variant == 1) ? enhancedPsB_ : enhancedPsA_;
    out.replacementSrv0 = replacementSrv;
    out.backdropSrv = backdropSrv;
    out.srApplied = (replacementSrv != nullptr);
    out.linearSampler = linearSamp;
    out.paramCb = cb;
    liveEnhancedDraws_.fetch_add(1, std::memory_order_relaxed);
    if (out.srApplied) liveSrDraws_.fetch_add(1, std::memory_order_relaxed);
    if (isMapDraw) liveMapDraws_.fetch_add(1, std::memory_order_relaxed);
    if (out.scaleApplied) liveScaledDraws_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void UiEnhancer::AttachBackdropBlurIfNeeded(UiDrawOverride& ov) {
    if (!ov.active || !ov.isBackbufferTarget || ov.isMapDraw || ov.backdropSrv ||
        glassmorphismAtomic_.load(std::memory_order_relaxed) <= 0.001f) {
        return;
    }
    ID3D11ShaderResourceView* blurSrv = WorldPostFx::Instance().GetBackdropBlurSrv();
    if (!blurSrv) return;

    const int cbIdx = static_cast<int>(ov.cbMask & 15u) | 16;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return;
    ID3D11Buffer* cb = (ov.isHoverFocus && hoveredParamCbs_[cbIdx]) ? hoveredParamCbs_[cbIdx] : paramCbs_[cbIdx];
    if (cb) {
        ov.backdropSrv = blurSrv;
        ov.paramCb = cb;
    }
}

void UiEnhancer::SyncGpuConstants(ID3D11DeviceContext* immediateCtx) {
    if (!immediateCtx || !cbDirty_.load(std::memory_order_acquire)) return;
    ID3D11Device* ctxDev = nullptr;
    immediateCtx->GetDevice(&ctxDev);
    if (!ctxDev) return;

    std::lock_guard<std::mutex> lock(mutex_);
    const bool sameDevice = (ctxDev == device_);
    ctxDev->Release();
    if (!ready_ || !sameDevice || !cbDirty_.exchange(false, std::memory_order_acq_rel)) return;

    const float screenW = static_cast<float>((std::max)(swapWidth_.load(std::memory_order_relaxed), 64u));
    const float splitMinX = splitScreen_.load(std::memory_order_relaxed)
        ? (0.5f * screenW)
        : 0.0f;
    const float screenH = static_cast<float>((std::max)(swapHeight_.load(std::memory_order_relaxed), 360u));
    const float oledDim = (oledAutoDim_.load(std::memory_order_relaxed) && cursorInCenter3d_.load(std::memory_order_relaxed))
        ? (1.0f - oledDimOpacity_)
        : 0.0f;

    for (int cbIdx = 0; cbIdx < 32; ++cbIdx) {
        if (!paramCbs_[cbIdx]) continue;
        GpuParams params{
            sharpness_,
            edgeAniso_,
            ((cbIdx & 1) != 0) ? 1.0f : 0.0f,
            ((cbIdx & 2) != 0) ? 1.0f : 0.0f,
            ((cbIdx & 4) != 0) ? 1.0f : 0.0f,
            ((cbIdx & 8) != 0) ? 1.0f : 0.0f,
            mapSharpnessBoost_,
            splitMinX,
            fontOutlineBoost_,
            themeDarkSlate_,
            oledDim,
            screenH,
            fontWeightBoost_,
            static_cast<float>(themeProfile_.load(std::memory_order_relaxed)),
            static_cast<float>(eyeComfortMode_.load(std::memory_order_relaxed)),
            0.0f,
            glassmorphism_,
            iconPolish_,
            screenW,
            ((cbIdx & 16) != 0) ? 1.0f : 0.0f
        };
        immediateCtx->UpdateSubresource(paramCbs_[cbIdx], 0, nullptr, &params, 0, 0);
        if (hoveredParamCbs_[cbIdx]) {
            params.hoverFocusBoost = 1.0f;
            immediateCtx->UpdateSubresource(hoveredParamCbs_[cbIdx], 0, nullptr, &params, 0, 0);
        }
    }
}

void UiEnhancer::OnFrameEnd(ID3D11DeviceContext* immediateCtx, bool freezeHoverSelection) {
    const uint32_t count = liveEnhancedDraws_.exchange(0, std::memory_order_relaxed);
    const uint32_t srCount = liveSrDraws_.exchange(0, std::memory_order_relaxed);
    const uint32_t mapCount = liveMapDraws_.exchange(0, std::memory_order_relaxed);
    const uint32_t scaledCount = liveScaledDraws_.exchange(0, std::memory_order_relaxed);
    const uint32_t hovId = liveHoveredTexId_.exchange(0, std::memory_order_relaxed);
    const uint64_t hovHash = liveHoveredTexHash_.exchange(0, std::memory_order_relaxed);
    const uint32_t hovW = liveHoveredTexW_.exchange(0, std::memory_order_relaxed);
    const uint32_t hovH = liveHoveredTexH_.exchange(0, std::memory_order_relaxed);

    // Refresh cursor coordinates in client space for next frame's hover element inspector & OLED Auto-Dim
    const HWND hwnd = reinterpret_cast<HWND>(swapHwnd_.load(std::memory_order_relaxed));
    POINT pt{};
    bool inCenter = false;
    bool cursorInMenuRect = false;
    if (hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
        cursorX_.store(pt.x, std::memory_order_relaxed);
        cursorY_.store(pt.y, std::memory_order_relaxed);
        const float w = static_cast<float>((std::max)(swapWidth_.load(std::memory_order_relaxed), 1u));
        const float h = static_cast<float>((std::max)(swapHeight_.load(std::memory_order_relaxed), 1u));
        const float nx = static_cast<float>(pt.x) / w;
        const float ny = static_cast<float>(pt.y) / h;
        inCenter = (nx > 0.18f && nx < 0.82f && ny > 0.20f && ny < 0.76f);
        const int maxMenuY = (std::max)(720, static_cast<int>(h * 0.56f));
        cursorInMenuRect = freezeHoverSelection && (pt.x >= 16 && pt.x <= 960 && pt.y >= 16 && pt.y <= maxMenuY);
    } else {
        cursorX_.store(-1, std::memory_order_relaxed);
        cursorY_.store(-1, std::memory_order_relaxed);
    }
    const bool prevCenter = cursorInCenter3d_.exchange(inCenter, std::memory_order_relaxed);
    if (prevCenter != inCenter && oledAutoDim_.load(std::memory_order_relaxed)) {
        cbDirty_.store(true, std::memory_order_release);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    lastFrameEnhancedDraws_ = count;
    lastFrameSrDraws_ = srCount;
    lastFrameMapDraws_ = mapCount;
    lastFrameScaledDraws_ = scaledCount;

    if (immediateCtx && scaleGsHasSo_ && soBuffer_ && soStagingBuffer_ && !hoverCandidates_.empty()) {
        if (!cursorInMenuRect) {
            immediateCtx->CopyResource(soStagingBuffer_, soBuffer_);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(immediateCtx->Map(soStagingBuffer_, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                struct SoHoverVertex {
                    float tag;
                    float isHovered;
                    float spanX;
                    float spanY;
                };
                const auto* verts = static_cast<const SoHoverVertex*>(mapped.pData);
                const size_t maxVerts = (std::min)(hoverCandidates_.size() * 24, kMaxSoVertices);
                int bestSlot = -1;
                float bestArea = 1.0e9f;
                for (size_t i = 0; i < maxVerts; ++i) {
                    const SoHoverVertex& v = verts[i];
                    const uint32_t rawTag = (v.tag > 0.0f) ? static_cast<uint32_t>(v.tag + 0.5f) : 0u;
                    if ((rawTag / 1024u) != hoverFrameTag_) {
                        break;
                    }
                    if (v.isHovered > 0.5f &&
                        v.spanX >= 0.002f && v.spanY >= 0.002f &&
                        v.spanX <= 1.25f && v.spanY <= 1.25f) {
                        const uint32_t slot = rawTag % 1024u;
                        if (slot < hoverCandidates_.size()) {
                            const float area = v.spanX * v.spanY;
                            if (bestSlot < 0 || area < bestArea * 0.85f ||
                                (area <= bestArea * 1.15f && static_cast<int>(slot) >= bestSlot)) {
                                bestSlot = static_cast<int>(slot);
                                bestArea = area;
                            }
                        }
                    }
                }
                immediateCtx->Unmap(soStagingBuffer_, 0);
                if (bestSlot >= 0) {
                    const auto& win = hoverCandidates_[static_cast<size_t>(bestSlot)];
                    lastHoveredTexId_ = win.texId;
                    lastHoveredTexHash_ = win.texHash;
                    lastHoveredTexW_ = win.texW;
                    lastHoveredTexH_ = win.texH;
                }
            }
        }
        hoverCandidates_.clear();
        hoverFrameTag_ = (hoverFrameTag_ % 4095u) + 1u;
    } else if (!cursorInMenuRect && hovId != 0) {
        lastHoveredTexId_ = hovId;
        lastHoveredTexHash_ = hovHash;
        lastHoveredTexW_ = hovW;
        lastHoveredTexH_ = hovH;
    }

    if (hudNotificationFrames_ > 0) {
        --hudNotificationFrames_;
        if (hudNotificationFrames_ == 0) {
            hudNotification_.clear();
        }
    }
}

UiEnhancerStats UiEnhancer::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    UiEnhancerStats s;
    s.ready = ready_;
    s.enabled = enabled_.load(std::memory_order_acquire);
    s.mapMode = mapMode_.load(std::memory_order_acquire);
    s.splitScreen = splitScreen_.load(std::memory_order_acquire);
    s.hoverMagnifier = hoverMagnifier_.load(std::memory_order_acquire);
    s.hoverFocusBoost = hoverFocusBoost_.load(std::memory_order_acquire);
    s.oledAutoDim = oledAutoDim_.load(std::memory_order_acquire);
    s.textureOverridesEnabled = enableTextureOverrides_.load(std::memory_order_acquire);
    s.sharpness = sharpness_;
    s.edgeAniso = edgeAniso_;
    s.fontOutlineBoost = fontOutlineBoost_;
    s.fontWeightBoost = fontWeightBoost_;
    s.themeDarkSlate = themeDarkSlate_;
    s.glassmorphism = glassmorphism_;
    s.iconPolish = iconPolish_;
    s.themeProfile = themeProfile_.load(std::memory_order_acquire);
    s.eyeComfortMode = eyeComfortMode_.load(std::memory_order_acquire);
    s.lastFrameEnhancedDraws = lastFrameEnhancedDraws_;
    s.lastFrameSrDraws = lastFrameSrDraws_;
    s.lastFrameMapDraws = lastFrameMapDraws_;
    s.lastFrameScaledDraws = lastFrameScaledDraws_;
    s.elementScaleRules = static_cast<uint32_t>(elementScaleRules_.size());
    s.loadedOverrides = loadedOverrideCount_;
    s.hoveredTexId = lastHoveredTexId_;
    s.hoveredTexHash = lastHoveredTexHash_;
    s.hoveredTexW = lastHoveredTexW_;
    s.hoveredTexH = lastHoveredTexH_;
    s.registeredTargetShaders = registeredCount_.load(std::memory_order_relaxed);
    s.hudNotification = hudNotification_;
    return s;
}

} // namespace lotro_uie
