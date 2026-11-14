#pragma once

#include <d3dcommon.h>
#include <cstddef>

namespace lotro_uie {

bool CompileHlslBlob(
    const char* source,
    size_t sourceLen,
    const char* sourceName,
    const D3D_SHADER_MACRO* defines,
    const char* entryPoint,
    const char* target,
    ID3DBlob** outBlob
);

} // namespace lotro_uie
