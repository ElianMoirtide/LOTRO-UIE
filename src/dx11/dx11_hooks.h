#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace lotro_uie {

// Install vtable hooks on newly created D3D11 / DXGI objects.
void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* immediateCtx);
void OnFactoryCreated(IUnknown* factory);
void OnSwapChainCreated(IDXGISwapChain* swapChain);

void ShutdownDx11Hooks(bool processTerminating);

} // namespace lotro_uie
