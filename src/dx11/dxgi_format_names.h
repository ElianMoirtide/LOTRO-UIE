#pragma once

#include <d3d11.h>

namespace lotro_uie {

inline const char* DxgiFormatName(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_R8G8_TYPELESS: return "R8G8_TYPELESS";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
        case DXGI_FORMAT_R8_TYPELESS: return "R8_TYPELESS";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_A8_UNORM: return "A8_UNORM";
        case DXGI_FORMAT_BC1_TYPELESS: return "BC1_TYPELESS";
        case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC2_TYPELESS: return "BC2_TYPELESS";
        case DXGI_FORMAT_BC2_UNORM: return "BC2_UNORM";
        case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2_UNORM_SRGB";
        case DXGI_FORMAT_BC3_TYPELESS: return "BC3_TYPELESS";
        case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC4_TYPELESS: return "BC4_TYPELESS";
        case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
        case DXGI_FORMAT_BC5_TYPELESS: return "BC5_TYPELESS";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5_UNORM";
        case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return "B8G8R8X8_TYPELESS";
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "B8G8R8X8_UNORM_SRGB";
        case DXGI_FORMAT_BC6H_TYPELESS: return "BC6H_TYPELESS";
        case DXGI_FORMAT_BC6H_UF16: return "BC6H_UF16";
        case DXGI_FORMAT_BC7_TYPELESS: return "BC7_TYPELESS";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        default: return "OTHER";
    }
}

inline const char* D3D11UsageName(D3D11_USAGE usage) noexcept {
    switch (usage) {
        case D3D11_USAGE_DEFAULT:   return "DEFAULT";
        case D3D11_USAGE_IMMUTABLE: return "IMMUTABLE";
        case D3D11_USAGE_DYNAMIC:   return "DYNAMIC";
        case D3D11_USAGE_STAGING:   return "STAGING";
        default: return "UNKNOWN";
    }
}

inline const char* D3D11FilterName(D3D11_FILTER f) noexcept {
    switch (f) {
        case D3D11_FILTER_MIN_MAG_MIP_POINT: return "POINT";
        case D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR: return "POINT_MIP_LIN";
        case D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT: return "MIN_PT_MAG_LIN";
        case D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR: return "MIN_PT_MAG_MIP_LIN";
        case D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT: return "MIN_LIN_MAG_MIP_PT";
        case D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR: return "MIN_LIN_MAG_PT_MIP_LIN";
        case D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT: return "LINEAR_MIP_PT";
        case D3D11_FILTER_MIN_MAG_MIP_LINEAR: return "LINEAR";
        case D3D11_FILTER_ANISOTROPIC: return "ANISO";
        default: return "OTHER";
    }
}

inline bool IsSingleChannelFormat(DXGI_FORMAT fmt) noexcept {
    return fmt == DXGI_FORMAT_R8_UNORM || fmt == DXGI_FORMAT_A8_UNORM || fmt == DXGI_FORMAT_R8_TYPELESS;
}

inline bool IsColorRgbaUiFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

inline bool IsBlockCompressedFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

} // namespace lotro_uie
