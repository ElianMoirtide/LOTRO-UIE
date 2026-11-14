#define CINTERFACE
#define COBJMACROS
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "dx11/vtable_slots.h"

#define SLOT_OF(vtbl, member) (offsetof(vtbl, member) / sizeof(void*))

const struct LotroUieVtableSlots g_lotroUieVtableSlots = {
    .device = {
        .CreateTexture2D          = SLOT_OF(ID3D11DeviceVtbl, CreateTexture2D),
        .CreateShaderResourceView = SLOT_OF(ID3D11DeviceVtbl, CreateShaderResourceView),
        .CreateRenderTargetView   = SLOT_OF(ID3D11DeviceVtbl, CreateRenderTargetView),
        .CreateDepthStencilView   = SLOT_OF(ID3D11DeviceVtbl, CreateDepthStencilView),
        .CreateVertexShader       = SLOT_OF(ID3D11DeviceVtbl, CreateVertexShader),
        .CreatePixelShader        = SLOT_OF(ID3D11DeviceVtbl, CreatePixelShader),
        .CreateComputeShader      = SLOT_OF(ID3D11DeviceVtbl, CreateComputeShader),
        .CreateBlendState         = SLOT_OF(ID3D11DeviceVtbl, CreateBlendState),
        .CreateDepthStencilState  = SLOT_OF(ID3D11DeviceVtbl, CreateDepthStencilState),
        .CreateRasterizerState    = SLOT_OF(ID3D11DeviceVtbl, CreateRasterizerState),
        .CreateSamplerState       = SLOT_OF(ID3D11DeviceVtbl, CreateSamplerState),
        .CreateDeferredContext    = SLOT_OF(ID3D11DeviceVtbl, CreateDeferredContext),
        .GetImmediateContext      = SLOT_OF(ID3D11DeviceVtbl, GetImmediateContext),
    },
    .device1 = {
        .GetImmediateContext1     = SLOT_OF(ID3D11Device1Vtbl, GetImmediateContext1),
        .CreateDeferredContext1   = SLOT_OF(ID3D11Device1Vtbl, CreateDeferredContext1),
        .CreateDeviceContextState = SLOT_OF(ID3D11Device1Vtbl, CreateDeviceContextState),
    },
    .context = {
        .PSSetShaderResources     = SLOT_OF(ID3D11DeviceContextVtbl, PSSetShaderResources),
        .PSSetShader              = SLOT_OF(ID3D11DeviceContextVtbl, PSSetShader),
        .PSSetSamplers            = SLOT_OF(ID3D11DeviceContextVtbl, PSSetSamplers),
        .VSSetShader              = SLOT_OF(ID3D11DeviceContextVtbl, VSSetShader),
        .CSSetShaderResources     = SLOT_OF(ID3D11DeviceContextVtbl, CSSetShaderResources),
        .CSSetUnorderedAccessViews = SLOT_OF(ID3D11DeviceContextVtbl, CSSetUnorderedAccessViews),
        .CSSetShader              = SLOT_OF(ID3D11DeviceContextVtbl, CSSetShader),
        .DrawIndexed              = SLOT_OF(ID3D11DeviceContextVtbl, DrawIndexed),
        .Draw                     = SLOT_OF(ID3D11DeviceContextVtbl, Draw),
        .Map                      = SLOT_OF(ID3D11DeviceContextVtbl, Map),
        .Unmap                    = SLOT_OF(ID3D11DeviceContextVtbl, Unmap),
        .IASetInputLayout         = SLOT_OF(ID3D11DeviceContextVtbl, IASetInputLayout),
        .IASetVertexBuffers       = SLOT_OF(ID3D11DeviceContextVtbl, IASetVertexBuffers),
        .IASetIndexBuffer         = SLOT_OF(ID3D11DeviceContextVtbl, IASetIndexBuffer),
        .DrawIndexedInstanced     = SLOT_OF(ID3D11DeviceContextVtbl, DrawIndexedInstanced),
        .DrawInstanced            = SLOT_OF(ID3D11DeviceContextVtbl, DrawInstanced),
        .IASetPrimitiveTopology   = SLOT_OF(ID3D11DeviceContextVtbl, IASetPrimitiveTopology),
        .DrawIndexedInstancedIndirect = SLOT_OF(ID3D11DeviceContextVtbl, DrawIndexedInstancedIndirect),
        .DrawAuto                 = SLOT_OF(ID3D11DeviceContextVtbl, DrawAuto),
        .DrawInstancedIndirect    = SLOT_OF(ID3D11DeviceContextVtbl, DrawInstancedIndirect),
        .Dispatch                 = SLOT_OF(ID3D11DeviceContextVtbl, Dispatch),
        .DispatchIndirect         = SLOT_OF(ID3D11DeviceContextVtbl, DispatchIndirect),
        .OMSetRenderTargets       = SLOT_OF(ID3D11DeviceContextVtbl, OMSetRenderTargets),
        .OMSetRenderTargetsAndUnorderedAccessViews =
            SLOT_OF(ID3D11DeviceContextVtbl, OMSetRenderTargetsAndUnorderedAccessViews),
        .OMSetBlendState          = SLOT_OF(ID3D11DeviceContextVtbl, OMSetBlendState),
        .OMSetDepthStencilState   = SLOT_OF(ID3D11DeviceContextVtbl, OMSetDepthStencilState),
        .RSSetState               = SLOT_OF(ID3D11DeviceContextVtbl, RSSetState),
        .RSSetViewports           = SLOT_OF(ID3D11DeviceContextVtbl, RSSetViewports),
        .RSSetScissorRects        = SLOT_OF(ID3D11DeviceContextVtbl, RSSetScissorRects),
        .CopySubresourceRegion    = SLOT_OF(ID3D11DeviceContextVtbl, CopySubresourceRegion),
        .CopyResource             = SLOT_OF(ID3D11DeviceContextVtbl, CopyResource),
        .UpdateSubresource        = SLOT_OF(ID3D11DeviceContextVtbl, UpdateSubresource),
        .ClearRenderTargetView    = SLOT_OF(ID3D11DeviceContextVtbl, ClearRenderTargetView),
        .ClearDepthStencilView    = SLOT_OF(ID3D11DeviceContextVtbl, ClearDepthStencilView),
        .ResolveSubresource       = SLOT_OF(ID3D11DeviceContextVtbl, ResolveSubresource),
        .ExecuteCommandList       = SLOT_OF(ID3D11DeviceContextVtbl, ExecuteCommandList),
        .FinishCommandList        = SLOT_OF(ID3D11DeviceContextVtbl, FinishCommandList),
        .Flush                    = SLOT_OF(ID3D11DeviceContextVtbl, Flush),
        .ClearState               = SLOT_OF(ID3D11DeviceContextVtbl, ClearState),
    },
    .context1 = {
        .CopySubresourceRegion1   = SLOT_OF(ID3D11DeviceContext1Vtbl, CopySubresourceRegion1),
        .UpdateSubresource1       = SLOT_OF(ID3D11DeviceContext1Vtbl, UpdateSubresource1),
        .ClearView                = SLOT_OF(ID3D11DeviceContext1Vtbl, ClearView),
        .DiscardResource          = SLOT_OF(ID3D11DeviceContext1Vtbl, DiscardResource),
        .DiscardView              = SLOT_OF(ID3D11DeviceContext1Vtbl, DiscardView),
        .SwapDeviceContextState   = SLOT_OF(ID3D11DeviceContext1Vtbl, SwapDeviceContextState),
        .DiscardView1             = SLOT_OF(ID3D11DeviceContext1Vtbl, DiscardView1),
    },
    .factory = {
        .CreateSwapChain          = SLOT_OF(IDXGIFactoryVtbl, CreateSwapChain),
        .CreateSwapChainForHwnd   = SLOT_OF(IDXGIFactory2Vtbl, CreateSwapChainForHwnd),
    },
    .swapchain = {
        .Present                  = SLOT_OF(IDXGISwapChainVtbl, Present),
        .ResizeBuffers            = SLOT_OF(IDXGISwapChainVtbl, ResizeBuffers),
        .Present1                 = SLOT_OF(IDXGISwapChain1Vtbl, Present1),
    },
};
