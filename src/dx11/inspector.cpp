#include "dx11/inspector.h"
#include "common/logger.h"
#include "dx11/dxgi_format_names.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>

namespace lotro_uie {

namespace {

const char* DrawKindName(DrawKind k) noexcept {
    switch (k) {
        case DrawKind::Draw:                         return "Draw";
        case DrawKind::DrawIndexed:                  return "DrawIndexed";
        case DrawKind::DrawInstanced:                return "DrawInstanced";
        case DrawKind::DrawIndexedInstanced:         return "DrawIndexedInstanced";
        case DrawKind::DrawAuto:                     return "DrawAuto";
        case DrawKind::DrawInstancedIndirect:        return "DrawInstancedIndirect";
        case DrawKind::DrawIndexedInstancedIndirect: return "DrawIndexedInstancedIndirect";
    }
    return "Draw";
}

std::optional<DsvMeta> LazyResolveDsv(ResourceTracker& tracker, ID3D11DepthStencilView* dsv) {
    if (!dsv) return std::nullopt;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res) return std::nullopt;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsv->GetDesc(&dsvDesc);
    tracker.TrackDsv(dsv, res, &dsvDesc);
    res->Release();
    return tracker.GetDsv(dsv);
}

std::string CsvEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else if (c == '\r' || c == '\n') out += ' ';
        else out += c;
    }
    return out;
}

std::string Hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string HeuristicTextureHint(const TextureMeta& meta, uint32_t uiDraws) {
    if (uiDraws == 0) return "";
    if (IsSingleChannelFormat(meta.format) || meta.dynamicUpdatesTotal > 0 || meta.usage == D3D11_USAGE_DYNAMIC) {
        return "likely Font Atlas / Dynamic Glyph Texture";
    }
    if (meta.mipLevels <= 1 && meta.width >= 512 && meta.height >= 512 &&
        (meta.width != meta.height || meta.width >= 1024)) {
        return "likely Map or Fullscreen/Large UI Panel";
    }
    if (meta.mipLevels <= 1) {
        return "likely Static UI Panel / Icon / Atlas";
    }
    return "2D Candidate Texture";
}

} // namespace

Inspector& Inspector::Instance() {
    static auto* instance = new Inspector();
    return *instance;
}

void Inspector::RequestCapture(bool dumpAssets) {
    std::lock_guard<std::mutex> lock(mutex_);
    captureArmed_ = true;
    armedDumpAssets_ = armedDumpAssets_ || dumpAssets;
    UIE_LOG_INFO("Frame capture armed (dumpAssets=%s)", armedDumpAssets_ ? "true" : "false");
}

bool Inspector::IsCapturing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capturingThisFrame_;
}

void Inspector::RecordGpuEvent(const std::string& type, const std::string& details) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capturingThisFrame_ || events_.size() >= 4096) return;
    CapturedGpuEvent ev;
    ev.eventIndex = static_cast<uint32_t>(events_.size() + 1);
    ev.afterDrawIndex = liveDraws_;
    ev.threadId = static_cast<uint32_t>(GetCurrentThreadId());
    ev.type = type;
    ev.details = details;
    events_.push_back(std::move(ev));
}

void Inspector::RetainResourceForCapture(ID3D11Resource* res) {
    if (!res) return;
    ID3D11Texture2D* tex2d = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) || !tex2d) {
        return;
    }
    auto& tracker = ResourceTracker::Instance();
    auto texMeta = tracker.GetTexture(tex2d);
    if (!texMeta) {
        D3D11_TEXTURE2D_DESC desc{};
        tex2d->GetDesc(&desc);
        tracker.TrackTexture2D(tex2d, &desc, nullptr, false);
        texMeta = tracker.GetTexture(tex2d);
    }
    if (texMeta && texMeta->id != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capturingThisFrame_ && retainedTextures_.find(texMeta->id) == retainedTextures_.end()) {
            tex2d->AddRef();
            retainedTextures_.emplace(texMeta->id, RetainedTex{*texMeta, tex2d});
        }
    }
    tex2d->Release();
}

void Inspector::RecordDispatch(const ContextState& state, UINT groupX, UINT groupY, UINT groupZ, bool indirect) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturingThisFrame_) return;
    }

    auto& tracker = ResourceTracker::Instance();
    std::string csDesc = "cs=0";
    if (state.cs) {
        if (auto cs = tracker.GetComputeShader(state.cs)) {
            csDesc = "cs=#" + std::to_string(cs->id) + "(" + Hex64(cs->hash) + ")";
            std::lock_guard<std::mutex> lock(mutex_);
            if (capturingThisFrame_) {
                capturedShaders_.try_emplace(cs->hash, cs);
            }
        }
    }

    std::string srvsDesc;
    for (UINT i = 0; i < static_cast<UINT>(ContextState::kMaxSrvs); ++i) {
        if (!state.csSrvs[i]) continue;
        ID3D11Resource* srvRes = nullptr;
        auto tex = tracker.GetTextureForSrv(state.csSrvs[i], &srvRes);
        if (tex) {
            if (!srvsDesc.empty()) srvsDesc += ';';
            srvsDesc += "t" + std::to_string(i) + ":tex_" + std::to_string(tex->id) +
                        "(" + std::to_string(tex->width) + "x" + std::to_string(tex->height) + " " +
                        DxgiFormatName(tex->format) + ")";
            if (srvRes) {
                RetainResourceForCapture(srvRes);
            }
        }
    }

    std::ostringstream oss;
    oss << "groups=" << groupX << "x" << groupY << "x" << groupZ << " " << csDesc;
    if (!srvsDesc.empty()) oss << " srvs=[" << srvsDesc << "]";
    RecordGpuEvent(indirect ? "DispatchIndirect" : "Dispatch", oss.str());
}

void Inspector::SetRuntimeDiagnostics(std::vector<std::string> lines) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtimeDiagnostics_ = std::move(lines);
}

InspectorLiveStats Inspector::GetLiveStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return InspectorLiveStats{
        frameIndex_,
        lastFrameDraws_,
        lastFrameUiCandidates_,
        lastCaptureStatus_
    };
}

void Inspector::ReleaseRetainedResources() {
    for (auto& [id, item] : retainedTextures_) {
        if (item.tex) {
            item.tex->Release();
            item.tex = nullptr;
        }
    }
    retainedTextures_.clear();
    capturedShaders_.clear();
}

void Inspector::OnFrameBegin(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    liveDraws_ = 0;
    liveUiCandidates_ = 0;

    if (!autoCaptureTriggered_ && config.autoCaptureFrame > 0 && frameIndex_ >= config.autoCaptureFrame) {
        autoCaptureTriggered_ = true;
        captureArmed_ = true;
        armedDumpAssets_ = armedDumpAssets_ || config.autoDumpAssets;
        UIE_LOG_INFO("AutoCapture triggered at frame %llu", static_cast<unsigned long long>(frameIndex_));
    }

    if (captureArmed_) {
        captureArmed_ = false;
        capturingThisFrame_ = true;
        dumpAssetsForThisFrame_ = armedDumpAssets_;
        armedDumpAssets_ = false;
        draws_.clear();
        draws_.reserve(4096);
        ReleaseRetainedResources();
        ResourceTracker::Instance().ResetPerFrameCounters();
    } else {
        capturingThisFrame_ = false;
        dumpAssetsForThisFrame_ = false;
    }
}

void Inspector::RecordDrawCall(const ContextState& state, DrawKind kind,
                               UINT count, UINT instanceCount, UINT startLoc, INT baseVertex) {
    auto& tracker = ResourceTracker::Instance();

    // Quick lightweight UI candidate check for live HUD stats.
    bool depthEnabled = true;
    if (state.depthStencilState) {
        if (auto ds = tracker.GetDepthStencilState(state.depthStencilState)) {
            depthEnabled = (ds->desc.DepthEnable != FALSE);
        }
    }
    const bool quickUiCand = (!depthEnabled) || (state.dsv == nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    ++liveDraws_;
    if (quickUiCand) {
        ++liveUiCandidates_;
    }

    if (!capturingThisFrame_) {
        return;
    }

    CapturedDraw d;
    d.drawIndex = liveDraws_;
    d.kind = kind;
    d.count = count;
    d.instanceCount = instanceCount;
    d.startLocation = startLoc;
    d.baseVertex = baseVertex;
    d.hasDsv = (state.dsv != nullptr);
    d.topology = state.topology;
    d.vbStride0 = state.vbStride0;
    d.viewport0 = state.viewport0;
    d.hasViewport0 = state.hasViewport0;

    d.numRtvs = static_cast<uint8_t>((std::min)(state.numRtvs, 255u));
    d.hasRtv0Ptr = (state.numRtvs > 0 && state.rtvs[0] != nullptr);
    d.scissor0 = state.scissor0;
    d.hasScissor0 = state.hasScissor0;
    if (d.hasRtv0Ptr) {
        auto rtv = tracker.GetOrTrackRtv(state.rtvs[0], &d.lazyRtv);
        if (rtv) {
            d.rtvId = rtv->id;
            d.rtvWidth = rtv->width;
            d.rtvHeight = rtv->height;
            d.rtvFormat = rtv->format;
            d.isBackbuffer = rtv->isBackbuffer;
        }
    }
    if (state.dsv) {
        auto dsv = tracker.GetDsv(state.dsv);
        if (!dsv) {
            dsv = LazyResolveDsv(tracker, state.dsv);
        }
        if (dsv) {
            d.dsvId = dsv->id;
            d.dsvWidth = dsv->width;
            d.dsvHeight = dsv->height;
            d.dsvFormat = dsv->format;
        }
    }

    std::shared_ptr<const ShaderMeta> vsMeta;
    std::shared_ptr<const ShaderMeta> psMeta;
    if (state.vs) {
        vsMeta = tracker.GetVertexShader(state.vs);
        if (vsMeta) {
            d.vsId = vsMeta->id;
            d.vsHash = vsMeta->hash;
        }
    }
    if (state.ps) {
        psMeta = tracker.GetPixelShader(state.ps);
        if (psMeta) {
            d.psId = psMeta->id;
            d.psHash = psMeta->hash;
        }
    }

    if (state.depthStencilState) {
        if (auto ds = tracker.GetDepthStencilState(state.depthStencilState)) {
            d.depthEnable = (ds->desc.DepthEnable != FALSE);
            d.depthWrite = (ds->desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO);
        }
    }
    if (state.blendState) {
        if (auto bl = tracker.GetBlendState(state.blendState)) {
            d.blendEnable = (bl->desc.RenderTarget[0].BlendEnable != FALSE);
        }
    }
    if (state.psSamplers[0]) {
        if (auto samp = tracker.GetSampler(state.psSamplers[0])) {
            d.hasSampler0 = true;
            d.sampler0Filter = samp->desc.Filter;
        }
    }

    bool hasDynamicTex = false;
    bool hasSingleChanTex = false;
    bool hasLarge2DTex = false;
    bool hasNoMipTex = false;

    for (UINT slot = 0; slot < ContextState::kMaxSrvs; ++slot) {
        ID3D11ShaderResourceView* srv = state.psSrvs[slot];
        if (!srv) continue;

        ID3D11Resource* res = nullptr;
        auto texMeta = tracker.GetTextureForSrv(srv, &res);
        if (!texMeta) continue;

        CapturedBoundTex bt;
        bt.slot = slot;
        bt.texId = texMeta->id;
        bt.width = texMeta->width;
        bt.height = texMeta->height;
        bt.mipLevels = texMeta->mipLevels;
        bt.format = texMeta->format;
        bt.usage = texMeta->usage;
        bt.dynamicUpdatesInFrame = texMeta->dynamicUpdatesInFrame;
        bt.dynamicUpdatesTotal = texMeta->dynamicUpdatesTotal;
        d.psTextures.push_back(bt);

        if (texMeta->usage == D3D11_USAGE_DYNAMIC || texMeta->dynamicUpdatesTotal > 0) hasDynamicTex = true;
        if (IsSingleChannelFormat(texMeta->format)) hasSingleChanTex = true;
        if (texMeta->mipLevels <= 1) hasNoMipTex = true;
        if (texMeta->mipLevels <= 1 && texMeta->width >= 512 && texMeta->height >= 512) hasLarge2DTex = true;

        if (res && retainedTextures_.find(texMeta->id) == retainedTextures_.end()) {
            ID3D11Texture2D* tex2d = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) && tex2d) {
                retainedTextures_.emplace(texMeta->id, RetainedTex{*texMeta, tex2d});
            }
        }
    }

    const bool isQuadCount = (count == 4 || count == 6 || (count > 6 && count % 6 == 0));
    d.isUiCandidate =
        (!d.depthEnable) ||
        (!d.hasDsv) ||
        (d.isBackbuffer && !d.depthWrite && isQuadCount) ||
        (hasDynamicTex && hasNoMipTex) ||
        (hasSingleChanTex && hasNoMipTex);

    std::vector<std::string> tagList;
    if (d.isBackbuffer) tagList.emplace_back("BACKBUFFER");
    else if (d.rtvWidth > 0) {
        tagList.emplace_back("RT_" + std::to_string(d.rtvWidth) + "x" + std::to_string(d.rtvHeight));
    }
    if (!d.depthEnable) tagList.emplace_back("DEPTH_OFF");
    if (!d.hasDsv) tagList.emplace_back("NO_DSV");
    if (d.blendEnable) tagList.emplace_back("ALPHA_BLEND");
    if (hasDynamicTex) tagList.emplace_back("DYNAMIC_TEX");
    if (hasSingleChanTex) tagList.emplace_back("SINGLE_CHAN");
    if (hasLarge2DTex) tagList.emplace_back("LARGE_2D");
    if (count == 4 || count == 6) tagList.emplace_back("SINGLE_QUAD");
    else if (count > 6 && count % 6 == 0) tagList.emplace_back("BATCHED_QUADS");

    for (size_t i = 0; i < tagList.size(); ++i) {
        if (i > 0) d.tags += '|';
        d.tags += tagList[i];
    }

    if (vsMeta && capturedShaders_.find(vsMeta->hash) == capturedShaders_.end()) {
        capturedShaders_.emplace(vsMeta->hash, vsMeta);
    }
    if (psMeta && capturedShaders_.find(psMeta->hash) == capturedShaders_.end()) {
        capturedShaders_.emplace(psMeta->hash, psMeta);
    }

    draws_.push_back(std::move(d));
}

void Inspector::OnFrameEnd(ID3D11Device* device, ID3D11DeviceContext* immediateCtx,
                           const SwapChainInfo& swapInfo, const Config& config) {
    std::vector<CapturedDraw> drawsLocal;
    std::unordered_map<uint32_t, RetainedTex> texLocal;
    std::unordered_map<uint64_t, std::shared_ptr<const ShaderMeta>> shadersLocal;
    bool doCapture = false;
    bool doDumpAssets = false;
    uint64_t capturedFrameIdx = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFrameDraws_ = liveDraws_;
        lastFrameUiCandidates_ = liveUiCandidates_;
        capturedFrameIdx = frameIndex_;
        ++frameIndex_;

        if (capturingThisFrame_) {
            doCapture = true;
            doDumpAssets = dumpAssetsForThisFrame_;
            capturingThisFrame_ = false;
            dumpAssetsForThisFrame_ = false;
            drawsLocal = std::move(draws_);
            texLocal = std::move(retainedTextures_);
            shadersLocal = std::move(capturedShaders_);
            draws_.clear();
            retainedTextures_.clear();
            capturedShaders_.clear();
        }
    }

    if (!doCapture) {
        return;
    }

    // Build output directory: <outputDir>/frame_<idx>_<HHMMSS>/
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char folderName[64];
    std::snprintf(folderName, sizeof(folderName), "frame_%06llu_%02u%02u%02u",
                  static_cast<unsigned long long>(capturedFrameIdx), st.wHour, st.wMinute, st.wSecond);
    const std::filesystem::path capDir = config.resolvedOutputDir / folderName;

    std::error_code ec;
    std::filesystem::create_directories(capDir, ec);
    if (ec) {
        UIE_LOG_ERROR("Failed to create capture dir '%ls': %s", capDir.c_str(), ec.message().c_str());
        for (auto& [id, rt] : texLocal) {
            if (rt.tex) rt.tex->Release();
        }
        return;
    }

    uint32_t uiDrawCount = 0;
    for (const auto& d : drawsLocal) {
        if (d.isUiCandidate) ++uiDrawCount;
    }

    // Aggregate RT passes, candidate pixel shaders, and candidate textures.
    std::vector<CapturedGpuEvent> eventsLocal;
    std::vector<std::string> diagLocal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eventsLocal.swap(events_);
        diagLocal.swap(runtimeDiagnostics_);
    }

    struct RtAgg {
        uint32_t rtvId = 0;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool isBackbuffer = false;
        uint32_t totalDraws = 0;
        uint32_t uiDraws = 0;
    };
    std::map<uint32_t, RtAgg> rtAggs;

    struct PsAgg {
        uint32_t psId = 0;
        uint64_t psHash = 0;
        uint32_t draws = 0;
        uint64_t totalCount = 0;
        std::unordered_set<uint64_t> vsHashes;
        std::unordered_set<uint32_t> texIds;
        std::string sampleTags;
        std::string sampleSampler;
    };
    std::unordered_map<uint64_t, PsAgg> uiPsAggs;

    struct TexAgg {
        TextureMeta meta;
        uint32_t totalDraws = 0;
        uint32_t uiDraws = 0;
        std::unordered_set<uint64_t> psHashes;
        std::string samplerFilter;
    };
    std::unordered_map<uint32_t, TexAgg> texAggs;

    std::unordered_set<uint32_t> dumpTexIds;
    std::unordered_set<uint64_t> dumpShaderHashes;

    for (const auto& d : drawsLocal) {
        auto& rt = rtAggs[d.rtvId];
        rt.rtvId = d.rtvId;
        rt.width = d.rtvWidth;
        rt.height = d.rtvHeight;
        rt.format = d.rtvFormat;
        rt.isBackbuffer = rt.isBackbuffer || d.isBackbuffer;
        ++rt.totalDraws;
        if (d.isUiCandidate) ++rt.uiDraws;

        const bool isLargeOrBackbufferDraw =
            d.isBackbuffer || d.lazyRtv ||
            (d.count <= 6 && (d.rtvWidth >= 1280 || (d.hasViewport0 && d.viewport0.Width >= 1280.0f)));

        if (d.isUiCandidate) {
            auto& psa = uiPsAggs[d.psHash];
            psa.psId = d.psId;
            psa.psHash = d.psHash;
            ++psa.draws;
            psa.totalCount += d.count;
            if (d.vsHash != 0) psa.vsHashes.insert(d.vsHash);
            if (psa.sampleTags.empty()) psa.sampleTags = d.tags;
            if (psa.sampleSampler.empty() && d.hasSampler0) psa.sampleSampler = D3D11FilterName(d.sampler0Filter);
            for (const auto& bt : d.psTextures) {
                psa.texIds.insert(bt.texId);
            }
        }

        if (d.isUiCandidate || isLargeOrBackbufferDraw || config.dumpAllFrameTextures ||
            dumpShaderHashes.size() < 256) {
            if (d.psHash != 0) dumpShaderHashes.insert(d.psHash);
            if (d.vsHash != 0) dumpShaderHashes.insert(d.vsHash);
        }

        for (const auto& bt : d.psTextures) {
            auto& ta = texAggs[bt.texId];
            if (ta.meta.id == 0) {
                auto it = texLocal.find(bt.texId);
                if (it != texLocal.end()) ta.meta = it->second.meta;
            }
            ++ta.totalDraws;
            if (d.isUiCandidate) {
                ++ta.uiDraws;
                dumpTexIds.insert(bt.texId);
            }
            if (d.psHash != 0) ta.psHashes.insert(d.psHash);
            if (ta.samplerFilter.empty() && d.hasSampler0) ta.samplerFilter = D3D11FilterName(d.sampler0Filter);
            const bool isInterestingNonUi =
                (!config.dumpOnlyUiTextures) && (
                    isLargeOrBackbufferDraw ||
                    ta.meta.dynamicUpdatesTotal > 0 ||
                    ta.meta.usage == D3D11_USAGE_DYNAMIC ||
                    IsSingleChannelFormat(ta.meta.format));
            if (config.dumpAllFrameTextures || (isInterestingNonUi && dumpTexIds.size() < 128)) {
                dumpTexIds.insert(bt.texId);
            }
        }
    }

    // Also include Compute Shaders and textures captured via Dispatch / CopyResource / ResolveSubresource.
    for (const auto& [shHash, shPtr] : shadersLocal) {
        if (shPtr && (shPtr->isComputeShader || dumpShaderHashes.size() < 256)) {
            dumpShaderHashes.insert(shHash);
        }
    }
    for (const auto& [texId, rt] : texLocal) {
        if (texAggs.find(texId) == texAggs.end() &&
            (config.dumpAllFrameTextures || (!config.dumpOnlyUiTextures && dumpTexIds.size() < 128))) {
            dumpTexIds.insert(texId);
        }
    }

    // 1. Write draws.csv
    {
        std::ofstream csv(capDir / "draws.csv");
        csv << "draw_idx,kind,count,instances,rtv_id,rtv_size,rtv_fmt,backbuffer,has_dsv,depth_en,depth_wr,blend_en,sampler0,vs_hash,ps_hash,textures,ui_cand,tags,vp_size,dsv_id,dsv_size,num_rtvs,has_rtv0,lazy_rtv,vb_stride0\n";
        for (const auto& d : drawsLocal) {
            std::string texStr;
            for (size_t i = 0; i < d.psTextures.size(); ++i) {
                if (i > 0) texStr += ';';
                const auto& t = d.psTextures[i];
                texStr += "t" + std::to_string(t.slot) + ":tex_" + std::to_string(t.texId) +
                          "(" + std::to_string(t.width) + "x" + std::to_string(t.height) + " " +
                          DxgiFormatName(t.format) + ")";
            }
            std::string tagsOut = d.tags;
            if (d.lazyRtv) tagsOut += "LAZY_RTV;";
            const UINT vpW = d.hasViewport0 ? static_cast<UINT>(d.viewport0.Width) : 0;
            const UINT vpH = d.hasViewport0 ? static_cast<UINT>(d.viewport0.Height) : 0;
            csv << d.drawIndex << ','
                << DrawKindName(d.kind) << ','
                << d.count << ','
                << d.instanceCount << ','
                << d.rtvId << ','
                << d.rtvWidth << 'x' << d.rtvHeight << ','
                << DxgiFormatName(d.rtvFormat) << ','
                << (d.isBackbuffer ? 1 : 0) << ','
                << (d.hasDsv ? 1 : 0) << ','
                << (d.depthEnable ? 1 : 0) << ','
                << (d.depthWrite ? 1 : 0) << ','
                << (d.blendEnable ? 1 : 0) << ','
                << (d.hasSampler0 ? D3D11FilterName(d.sampler0Filter) : "NONE") << ','
                << Hex64(d.vsHash) << ','
                << Hex64(d.psHash) << ','
                << '"' << texStr << '"' << ','
                << (d.isUiCandidate ? 1 : 0) << ','
                << '"' << CsvEscape(tagsOut) << '"' << ','
                << vpW << 'x' << vpH << ','
                << d.dsvId << ','
                << d.dsvWidth << 'x' << d.dsvHeight << ','
                << static_cast<unsigned>(d.numRtvs) << ','
                << (d.hasRtv0Ptr ? 1 : 0) << ','
                << (d.lazyRtv ? 1 : 0) << ','
                << d.vbStride0 << '\n';
        }
    }

    // 1b. Write timeline.csv (interleaved Clear/Copy/Resolve/Dispatch/CmdList/VtableReset events)
    {
        std::ofstream tl(capDir / "timeline.csv");
        tl << "event_idx,after_draw_idx,thread_id,type,details\n";
        for (const auto& ev : eventsLocal) {
            tl << ev.eventIndex << ','
               << ev.afterDrawIndex << ','
               << ev.threadId << ','
               << '"' << CsvEscape(ev.type) << '"' << ','
               << '"' << CsvEscape(ev.details) << '"' << '\n';
        }
    }

    // Sort UI Pixel Shaders by draw count descending
    std::vector<PsAgg> sortedPs;
    sortedPs.reserve(uiPsAggs.size());
    for (const auto& [h, agg] : uiPsAggs) sortedPs.push_back(agg);
    std::sort(sortedPs.begin(), sortedPs.end(), [](const PsAgg& a, const PsAgg& b) {
        return a.draws > b.draws;
    });

    // Sort candidate textures by UI draw count desc, then pixel area desc
    std::vector<TexAgg> sortedTex;
    for (const auto& [id, agg] : texAggs) {
        if (agg.uiDraws > 0 || config.dumpAllFrameTextures) {
            sortedTex.push_back(agg);
        }
    }
    std::sort(sortedTex.begin(), sortedTex.end(), [](const TexAgg& a, const TexAgg& b) {
        if (a.uiDraws != b.uiDraws) return a.uiDraws > b.uiDraws;
        return static_cast<uint64_t>(a.meta.width) * a.meta.height >
               static_cast<uint64_t>(b.meta.width) * b.meta.height;
    });

    // 2. Write summary.txt
    {
        std::ofstream sum(capDir / "summary.txt");
        sum << "LOTRO-UIE v0.1.1 DX11 Pipeline Capture Summary\n";
        sum << "==============================================\n";
        sum << "Frame Index     : " << capturedFrameIdx << "\n";
        sum << "SwapChain       : " << swapInfo.width << " x " << swapInfo.height
            << " (" << DxgiFormatName(swapInfo.format)
            << ", Buffers=" << swapInfo.bufferCount
            << ", MSAA=" << swapInfo.sampleCount
            << ", Windowed=" << (swapInfo.windowed ? "true" : "false")
            << ", Ptr=" << Hex64(swapInfo.swapChainPtr)
            << ", HWND=" << Hex64(swapInfo.hwnd) << ")\n";
        sum << "Total Draws     : " << drawsLocal.size() << "\n";
        sum << "UI Candidates   : " << uiDrawCount << "\n";
        sum << "GPU Events      : " << eventsLocal.size() << "\n";
        sum << "Asset Dump      : " << (doDumpAssets ? "ENABLED (F11)" : "DISABLED (F10)") << "\n\n";

        if (!diagLocal.empty()) {
            sum << "Runtime & Vtable Diagnostics:\n";
            for (const auto& line : diagLocal) {
                sum << "  " << line << "\n";
            }
            sum << "\n";
        }

        sum << "Draw Pass Sequence (grouped consecutive draws):\n";
        if (!drawsLocal.empty()) {
            size_t runStart = 0;
            while (runStart < drawsLocal.size()) {
                const auto& first = drawsLocal[runStart];
                size_t runEnd = runStart + 1;
                while (runEnd < drawsLocal.size() &&
                       drawsLocal[runEnd].rtvId == first.rtvId &&
                       drawsLocal[runEnd].dsvId == first.dsvId &&
                       drawsLocal[runEnd].isBackbuffer == first.isBackbuffer &&
                       drawsLocal[runEnd].blendEnable == first.blendEnable &&
                       drawsLocal[runEnd].depthEnable == first.depthEnable) {
                    ++runEnd;
                }
                const UINT vpW = first.hasViewport0 ? static_cast<UINT>(first.viewport0.Width) : 0;
                const UINT vpH = first.hasViewport0 ? static_cast<UINT>(first.viewport0.Height) : 0;
                sum << "  draws " << first.drawIndex << ".." << drawsLocal[runEnd - 1].drawIndex
                    << " (" << (runEnd - runStart) << "x): "
                    << "rtv=#" << first.rtvId << " (" << first.rtvWidth << "x" << first.rtvHeight
                    << " " << DxgiFormatName(first.rtvFormat)
                    << (first.isBackbuffer ? " BB" : "") << (first.lazyRtv ? " LAZY" : "") << ")"
                    << " | dsv=#" << first.dsvId << " (" << first.dsvWidth << "x" << first.dsvHeight << ")"
                    << " | vp=" << vpW << "x" << vpH
                    << " | dep=" << (first.depthEnable ? 1 : 0) << "/" << (first.depthWrite ? 1 : 0)
                    << " bld=" << (first.blendEnable ? 1 : 0) << "\n";
                runStart = runEnd;
            }
            sum << "\n";
        }

        if (!eventsLocal.empty()) {
            sum << "GPU Non-Draw Events (" << eventsLocal.size() << ", first 80):\n";
            const size_t limit = (std::min<size_t>)(eventsLocal.size(), 80);
            for (size_t i = 0; i < limit; ++i) {
                const auto& ev = eventsLocal[i];
                sum << "  ev #" << ev.eventIndex << " @after_draw=" << ev.afterDrawIndex
                    << " [tid=" << ev.threadId << "] " << ev.type << ": " << ev.details << "\n";
            }
            sum << "\n";
        }

        sum << "Render Targets Used:\n";
        for (const auto& [id, rt] : rtAggs) {
            sum << "  RT #" << id << ": " << rt.width << "x" << rt.height
                << " " << DxgiFormatName(rt.format)
                << (rt.isBackbuffer ? " [BACKBUFFER]" : " [OFFSCREEN]")
                << " | totalDraws=" << rt.totalDraws
                << " | uiCandDraws=" << rt.uiDraws << "\n";
        }

        sum << "\nCandidate UI/Font/Map Pixel Shaders (" << sortedPs.size() << "):\n";
        for (const auto& p : sortedPs) {
            sum << "  PS #" << p.psId << " (" << Hex64(p.psHash) << ")"
                << " | draws=" << p.draws
                << " | verts/indices=" << p.totalCount
                << " | sampler0=" << (p.sampleSampler.empty() ? "NONE" : p.sampleSampler)
                << " | uniqueTex=" << p.texIds.size()
                << " | tags=[" << p.sampleTags << "]\n";
        }

        sum << "\nCandidate UI/Font/Map Textures (" << sortedTex.size() << "):\n";
        for (const auto& t : sortedTex) {
            const std::string hint = HeuristicTextureHint(t.meta, t.uiDraws);
            sum << "  tex_" << t.meta.id
                << " | hash=" << Hex64(t.meta.initialHash)
                << " | " << t.meta.width << "x" << t.meta.height
                << " | " << DxgiFormatName(t.meta.format)
                << " | mips=" << t.meta.mipLevels
                << " | " << D3D11UsageName(t.meta.usage)
                << " | dynUpd(frame/tot)=" << t.meta.dynamicUpdatesInFrame << "/" << t.meta.dynamicUpdatesTotal
                << " | uiDraws=" << t.uiDraws << "/" << t.totalDraws
                << " | sampler0=" << (t.samplerFilter.empty() ? "NONE" : t.samplerFilter);
            if (!hint.empty()) sum << " | -> " << hint;
            sum << "\n";
        }
    }

    // 3. Write capture.json
    {
        std::ofstream js(capDir / "capture.json");
        js << "{\n";
        js << "  \"frameIndex\": " << capturedFrameIdx << ",\n";
        js << "  \"swapChain\": {\n";
        js << "    \"width\": " << swapInfo.width << ",\n";
        js << "    \"height\": " << swapInfo.height << ",\n";
        js << "    \"format\": \"" << DxgiFormatName(swapInfo.format) << "\",\n";
        js << "    \"bufferCount\": " << swapInfo.bufferCount << ",\n";
        js << "    \"sampleCount\": " << swapInfo.sampleCount << ",\n";
        js << "    \"windowed\": " << (swapInfo.windowed ? "true" : "false") << "\n";
        js << "  },\n";
        js << "  \"totalDraws\": " << drawsLocal.size() << ",\n";
        js << "  \"uiCandidateDraws\": " << uiDrawCount << ",\n";
        js << "  \"renderTargets\": [\n";
        size_t rtIdx = 0;
        for (const auto& [id, rt] : rtAggs) {
            js << "    {\"id\": " << id
               << ", \"width\": " << rt.width
               << ", \"height\": " << rt.height
               << ", \"format\": \"" << DxgiFormatName(rt.format) << "\""
               << ", \"isBackbuffer\": " << (rt.isBackbuffer ? "true" : "false")
               << ", \"totalDraws\": " << rt.totalDraws
               << ", \"uiCandidateDraws\": " << rt.uiDraws << "}"
               << (++rtIdx < rtAggs.size() ? "," : "") << "\n";
        }
        js << "  ],\n";
        js << "  \"uiPixelShaders\": [\n";
        for (size_t i = 0; i < sortedPs.size(); ++i) {
            const auto& p = sortedPs[i];
            js << "    {\"id\": " << p.psId
               << ", \"hash\": \"" << Hex64(p.psHash) << "\""
               << ", \"draws\": " << p.draws
               << ", \"totalElements\": " << p.totalCount
               << ", \"sampler0\": \"" << JsonEscape(p.sampleSampler) << "\""
               << ", \"tags\": \"" << JsonEscape(p.sampleTags) << "\"}"
               << (i + 1 < sortedPs.size() ? "," : "") << "\n";
        }
        js << "  ],\n";
        js << "  \"candidateTextures\": [\n";
        for (size_t i = 0; i < sortedTex.size(); ++i) {
            const auto& t = sortedTex[i];
            js << "    {\"id\": " << t.meta.id
               << ", \"hash\": \"" << Hex64(t.meta.initialHash) << "\""
               << ", \"width\": " << t.meta.width
               << ", \"height\": " << t.meta.height
               << ", \"format\": \"" << DxgiFormatName(t.meta.format) << "\""
               << ", \"mipLevels\": " << t.meta.mipLevels
               << ", \"usage\": \"" << D3D11UsageName(t.meta.usage) << "\""
               << ", \"dynamicUpdatesFrame\": " << t.meta.dynamicUpdatesInFrame
               << ", \"dynamicUpdatesTotal\": " << t.meta.dynamicUpdatesTotal
               << ", \"uiDraws\": " << t.uiDraws
               << ", \"totalDraws\": " << t.totalDraws
               << ", \"hint\": \"" << JsonEscape(HeuristicTextureHint(t.meta, t.uiDraws)) << "\"}"
               << (i + 1 < sortedTex.size() ? "," : "") << "\n";
        }
        js << "  ]\n";
        js << "}\n";
    }

    uint32_t dumpedTexCount = 0;
    uint32_t dumpedShaderCount = 0;

    if (doDumpAssets) {
        const auto texDir = capDir / "textures";
        const auto shDir = capDir / "shaders";
        std::filesystem::create_directories(texDir, ec);
        std::filesystem::create_directories(shDir, ec);

        for (uint32_t texId : dumpTexIds) {
            auto it = texLocal.find(texId);
            if (it == texLocal.end() || !it->second.tex) continue;
            const auto& m = it->second.meta;
            std::string name = "tex_" + std::to_string(m.id) + "_" +
                               std::to_string(m.width) + "x" + std::to_string(m.height) + "_" +
                               DxgiFormatName(m.format);
            if (DumpTextureToDisk(device, immediateCtx, it->second.tex, m, texDir, name, config.dumpFormat)) {
                ++dumpedTexCount;
            }
        }

        for (uint64_t shHash : dumpShaderHashes) {
            auto it = shadersLocal.find(shHash);
            if (it == shadersLocal.end() || !it->second) continue;
            const auto& sh = *it->second;
            std::string prefix = sh.isPixelShader ? "ps_" : (sh.isComputeShader ? "cs_" : "vs_");
            std::string name = prefix + std::to_string(sh.id) + "_" + Hex64(sh.hash);
            if (DumpShaderToDisk(sh, shDir, name)) {
                ++dumpedShaderCount;
            }
        }
    }

    for (auto& [id, rt] : texLocal) {
        if (rt.tex) rt.tex->Release();
    }

    char statusBuf[192];
    if (doDumpAssets) {
        std::snprintf(statusBuf, sizeof(statusBuf), "Saved %s (%u UI draws, %u tex, %u sh)",
                      folderName, uiDrawCount, dumpedTexCount, dumpedShaderCount);
    } else {
        std::snprintf(statusBuf, sizeof(statusBuf), "Saved %s (%u UI draws / %zu total)",
                      folderName, uiDrawCount, drawsLocal.size());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastCaptureStatus_ = statusBuf;
    }
    const std::wstring dirW = capDir.wstring();
    char dirUtf8[512]{};
    WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, dirUtf8, static_cast<int>(sizeof(dirUtf8)), nullptr, nullptr);
    UIE_LOG_INFO("Capture complete: %s -> %s", statusBuf, dirUtf8);
}

} // namespace lotro_uie
