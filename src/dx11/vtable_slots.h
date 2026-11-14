#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct LotroUieVtableSlots {
    struct {
        size_t CreateTexture2D;
        size_t CreateShaderResourceView;
        size_t CreateRenderTargetView;
        size_t CreateDepthStencilView;
        size_t CreateVertexShader;
        size_t CreatePixelShader;
        size_t CreateComputeShader;
        size_t CreateBlendState;
        size_t CreateDepthStencilState;
        size_t CreateRasterizerState;
        size_t CreateSamplerState;
        size_t CreateDeferredContext;
        size_t GetImmediateContext;
    } device;

    struct {
        size_t GetImmediateContext1;
        size_t CreateDeferredContext1;
        size_t CreateDeviceContextState;
    } device1;

    struct {
        size_t PSSetShaderResources;
        size_t PSSetShader;
        size_t PSSetSamplers;
        size_t VSSetShader;
        size_t CSSetShaderResources;
        size_t CSSetUnorderedAccessViews;
        size_t CSSetShader;
        size_t DrawIndexed;
        size_t Draw;
        size_t Map;
        size_t Unmap;
        size_t IASetInputLayout;
        size_t IASetVertexBuffers;
        size_t IASetIndexBuffer;
        size_t DrawIndexedInstanced;
        size_t DrawInstanced;
        size_t IASetPrimitiveTopology;
        size_t DrawIndexedInstancedIndirect;
        size_t DrawAuto;
        size_t DrawInstancedIndirect;
        size_t Dispatch;
        size_t DispatchIndirect;
        size_t OMSetRenderTargets;
        size_t OMSetRenderTargetsAndUnorderedAccessViews;
        size_t OMSetBlendState;
        size_t OMSetDepthStencilState;
        size_t RSSetState;
        size_t RSSetViewports;
        size_t RSSetScissorRects;
        size_t CopySubresourceRegion;
        size_t CopyResource;
        size_t UpdateSubresource;
        size_t ClearRenderTargetView;
        size_t ClearDepthStencilView;
        size_t ResolveSubresource;
        size_t ExecuteCommandList;
        size_t FinishCommandList;
        size_t Flush;
        size_t ClearState;
    } context;

    struct {
        size_t CopySubresourceRegion1;
        size_t UpdateSubresource1;
        size_t ClearView;
        size_t DiscardResource;
        size_t DiscardView;
        size_t SwapDeviceContextState;
        size_t DiscardView1;
    } context1;

    struct {
        size_t CreateSwapChain;
        size_t CreateSwapChainForHwnd;
    } factory;

    struct {
        size_t Present;
        size_t ResizeBuffers;
        size_t Present1;
    } swapchain;
};

extern const struct LotroUieVtableSlots g_lotroUieVtableSlots;

#ifdef __cplusplus
} // extern "C"

namespace lotro_uie::vtable_slots {
namespace device {
inline const size_t& CreateTexture2D          = g_lotroUieVtableSlots.device.CreateTexture2D;
inline const size_t& CreateShaderResourceView = g_lotroUieVtableSlots.device.CreateShaderResourceView;
inline const size_t& CreateRenderTargetView   = g_lotroUieVtableSlots.device.CreateRenderTargetView;
inline const size_t& CreateDepthStencilView   = g_lotroUieVtableSlots.device.CreateDepthStencilView;
inline const size_t& CreateVertexShader       = g_lotroUieVtableSlots.device.CreateVertexShader;
inline const size_t& CreatePixelShader        = g_lotroUieVtableSlots.device.CreatePixelShader;
inline const size_t& CreateComputeShader      = g_lotroUieVtableSlots.device.CreateComputeShader;
inline const size_t& CreateBlendState         = g_lotroUieVtableSlots.device.CreateBlendState;
inline const size_t& CreateDepthStencilState  = g_lotroUieVtableSlots.device.CreateDepthStencilState;
inline const size_t& CreateRasterizerState    = g_lotroUieVtableSlots.device.CreateRasterizerState;
inline const size_t& CreateSamplerState       = g_lotroUieVtableSlots.device.CreateSamplerState;
inline const size_t& CreateDeferredContext    = g_lotroUieVtableSlots.device.CreateDeferredContext;
inline const size_t& GetImmediateContext      = g_lotroUieVtableSlots.device.GetImmediateContext;
} // namespace device

namespace device1 {
inline const size_t& GetImmediateContext1     = g_lotroUieVtableSlots.device1.GetImmediateContext1;
inline const size_t& CreateDeferredContext1   = g_lotroUieVtableSlots.device1.CreateDeferredContext1;
inline const size_t& CreateDeviceContextState = g_lotroUieVtableSlots.device1.CreateDeviceContextState;
} // namespace device1

namespace context {
inline const size_t& PSSetShaderResources     = g_lotroUieVtableSlots.context.PSSetShaderResources;
inline const size_t& PSSetShader              = g_lotroUieVtableSlots.context.PSSetShader;
inline const size_t& PSSetSamplers            = g_lotroUieVtableSlots.context.PSSetSamplers;
inline const size_t& VSSetShader              = g_lotroUieVtableSlots.context.VSSetShader;
inline const size_t& CSSetShaderResources     = g_lotroUieVtableSlots.context.CSSetShaderResources;
inline const size_t& CSSetUnorderedAccessViews = g_lotroUieVtableSlots.context.CSSetUnorderedAccessViews;
inline const size_t& CSSetShader              = g_lotroUieVtableSlots.context.CSSetShader;
inline const size_t& DrawIndexed              = g_lotroUieVtableSlots.context.DrawIndexed;
inline const size_t& Draw                     = g_lotroUieVtableSlots.context.Draw;
inline const size_t& Map                      = g_lotroUieVtableSlots.context.Map;
inline const size_t& Unmap                    = g_lotroUieVtableSlots.context.Unmap;
inline const size_t& IASetInputLayout         = g_lotroUieVtableSlots.context.IASetInputLayout;
inline const size_t& IASetVertexBuffers       = g_lotroUieVtableSlots.context.IASetVertexBuffers;
inline const size_t& IASetIndexBuffer         = g_lotroUieVtableSlots.context.IASetIndexBuffer;
inline const size_t& DrawIndexedInstanced     = g_lotroUieVtableSlots.context.DrawIndexedInstanced;
inline const size_t& DrawInstanced            = g_lotroUieVtableSlots.context.DrawInstanced;
inline const size_t& IASetPrimitiveTopology   = g_lotroUieVtableSlots.context.IASetPrimitiveTopology;
inline const size_t& DrawIndexedInstancedIndirect = g_lotroUieVtableSlots.context.DrawIndexedInstancedIndirect;
inline const size_t& DrawAuto                 = g_lotroUieVtableSlots.context.DrawAuto;
inline const size_t& DrawInstancedIndirect    = g_lotroUieVtableSlots.context.DrawInstancedIndirect;
inline const size_t& Dispatch                 = g_lotroUieVtableSlots.context.Dispatch;
inline const size_t& DispatchIndirect         = g_lotroUieVtableSlots.context.DispatchIndirect;
inline const size_t& OMSetRenderTargets       = g_lotroUieVtableSlots.context.OMSetRenderTargets;
inline const size_t& OMSetRenderTargetsAndUnorderedAccessViews =
    g_lotroUieVtableSlots.context.OMSetRenderTargetsAndUnorderedAccessViews;
inline const size_t& OMSetBlendState          = g_lotroUieVtableSlots.context.OMSetBlendState;
inline const size_t& OMSetDepthStencilState   = g_lotroUieVtableSlots.context.OMSetDepthStencilState;
inline const size_t& RSSetState               = g_lotroUieVtableSlots.context.RSSetState;
inline const size_t& RSSetViewports           = g_lotroUieVtableSlots.context.RSSetViewports;
inline const size_t& RSSetScissorRects        = g_lotroUieVtableSlots.context.RSSetScissorRects;
inline const size_t& CopySubresourceRegion    = g_lotroUieVtableSlots.context.CopySubresourceRegion;
inline const size_t& CopyResource             = g_lotroUieVtableSlots.context.CopyResource;
inline const size_t& UpdateSubresource        = g_lotroUieVtableSlots.context.UpdateSubresource;
inline const size_t& ClearRenderTargetView    = g_lotroUieVtableSlots.context.ClearRenderTargetView;
inline const size_t& ClearDepthStencilView    = g_lotroUieVtableSlots.context.ClearDepthStencilView;
inline const size_t& ResolveSubresource       = g_lotroUieVtableSlots.context.ResolveSubresource;
inline const size_t& ExecuteCommandList       = g_lotroUieVtableSlots.context.ExecuteCommandList;
inline const size_t& FinishCommandList        = g_lotroUieVtableSlots.context.FinishCommandList;
inline const size_t& Flush                    = g_lotroUieVtableSlots.context.Flush;
inline const size_t& ClearState               = g_lotroUieVtableSlots.context.ClearState;
} // namespace context

namespace context1 {
inline const size_t& CopySubresourceRegion1   = g_lotroUieVtableSlots.context1.CopySubresourceRegion1;
inline const size_t& UpdateSubresource1       = g_lotroUieVtableSlots.context1.UpdateSubresource1;
inline const size_t& ClearView                = g_lotroUieVtableSlots.context1.ClearView;
inline const size_t& DiscardResource          = g_lotroUieVtableSlots.context1.DiscardResource;
inline const size_t& DiscardView              = g_lotroUieVtableSlots.context1.DiscardView;
inline const size_t& SwapDeviceContextState   = g_lotroUieVtableSlots.context1.SwapDeviceContextState;
inline const size_t& DiscardView1             = g_lotroUieVtableSlots.context1.DiscardView1;
} // namespace context1

namespace factory {
inline const size_t& CreateSwapChain          = g_lotroUieVtableSlots.factory.CreateSwapChain;
inline const size_t& CreateSwapChainForHwnd   = g_lotroUieVtableSlots.factory.CreateSwapChainForHwnd;
} // namespace factory

namespace swapchain {
inline const size_t& Present                  = g_lotroUieVtableSlots.swapchain.Present;
inline const size_t& ResizeBuffers            = g_lotroUieVtableSlots.swapchain.ResizeBuffers;
inline const size_t& Present1                 = g_lotroUieVtableSlots.swapchain.Present1;
} // namespace swapchain
} // namespace lotro_uie::vtable_slots
#endif
