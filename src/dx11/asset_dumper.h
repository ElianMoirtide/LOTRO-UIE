#pragma once

#include "dx11/resource_tracker.h"

#include <d3d11.h>
#include <filesystem>
#include <string>

namespace lotro_uie {

// Dumps mip 0 of a Texture2D to <outDir>/<baseName>.{png,dds,bmp} according to dumpFormat ("png", "dds", "png+dds", "all").
bool DumpTextureToDisk(
    ID3D11Device* device,
    ID3D11DeviceContext* immediateCtx,
    ID3D11Texture2D* tex,
    const TextureMeta& meta,
    const std::filesystem::path& outDir,
    const std::string& baseName,
    const std::string& dumpFormat = "png"
);

// Dumps shader DXBC bytecode and D3DDisassemble() assembly text to <outDir>/<baseName>.{dxbc,asm}.
bool DumpShaderToDisk(
    const ShaderMeta& shader,
    const std::filesystem::path& outDir,
    const std::string& baseName
);

// Loads a .png, .dds, .bmp, or .jpg image from disk into tightly-packed 32-bit RGBA8 pixels.
bool LoadImageRgba8(
    const std::filesystem::path& filePath,
    uint32_t& outWidth,
    uint32_t& outHeight,
    std::vector<uint8_t>& outRgba
);

} // namespace lotro_uie
