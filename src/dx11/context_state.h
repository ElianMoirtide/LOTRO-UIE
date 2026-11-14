#pragma once

#include <d3d11.h>
#include <array>
#include <cstdint>

namespace lotro_uie {

struct ContextState {
    static constexpr size_t kMaxSrvs = 8;
    static constexpr size_t kMaxSamplers = 4;
    static constexpr size_t kMaxRtvs = 8;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    std::array<ID3D11ShaderResourceView*, kMaxSrvs> psSrvs{};
    std::array<ID3D11SamplerState*, kMaxSamplers> psSamplers{};
    std::array<ID3D11ShaderResourceView*, kMaxSrvs> csSrvs{};
    std::array<ID3D11UnorderedAccessView*, kMaxRtvs> csUavs{};

    std::array<ID3D11RenderTargetView*, kMaxRtvs> rtvs{};
    UINT numRtvs = 0;
    ID3D11DepthStencilView* dsv = nullptr;

    ID3D11DepthStencilState* depthStencilState = nullptr;
    UINT stencilRef = 0;
    ID3D11BlendState* blendState = nullptr;
    ID3D11RasterizerState* rasterizerState = nullptr;

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT vbStride0 = 0;

    D3D11_VIEWPORT viewport0{};
    bool hasViewport0 = false;
    D3D11_RECT scissor0{};
    bool hasScissor0 = false;

    uint32_t drawCount = 0;

    void Clear() {
        vs = nullptr;
        ps = nullptr;
        cs = nullptr;
        psSrvs.fill(nullptr);
        psSamplers.fill(nullptr);
        csSrvs.fill(nullptr);
        csUavs.fill(nullptr);
        rtvs.fill(nullptr);
        numRtvs = 0;
        dsv = nullptr;
        depthStencilState = nullptr;
        stencilRef = 0;
        blendState = nullptr;
        rasterizerState = nullptr;
        topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        vbStride0 = 0;
        viewport0 = {};
        hasViewport0 = false;
        scissor0 = {};
        hasScissor0 = false;
    }
};

} // namespace lotro_uie
