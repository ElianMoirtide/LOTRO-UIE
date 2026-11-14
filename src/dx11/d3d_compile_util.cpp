#include "dx11/d3d_compile_util.h"
#include "common/logger.h"

#include <Windows.h>
#include <mutex>

namespace lotro_uie {

namespace {

using PfnD3DCompile = HRESULT(WINAPI*)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs
);

PfnD3DCompile ResolveD3DCompile() {
    static std::mutex s_mutex;
    static PfnD3DCompile s_pfn = nullptr;
    static bool s_attempted = false;

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_attempted) {
        return s_pfn;
    }
    s_attempted = true;

    const wchar_t* const candidates[] = { L"d3dcompiler_47.dll", L"d3dcompiler_43.dll" };
    for (const wchar_t* dll : candidates) {
        HMODULE mod = GetModuleHandleW(dll);
        if (!mod) {
            mod = LoadLibraryW(dll);
        }
        if (mod) {
            auto pfn = reinterpret_cast<PfnD3DCompile>(GetProcAddress(mod, "D3DCompile"));
            if (pfn) {
                s_pfn = pfn;
                return s_pfn;
            }
        }
    }
    UIE_LOG_ERROR("D3DCompile unavailable (d3dcompiler_47.dll / d3dcompiler_43.dll missing)");
    return nullptr;
}

} // namespace

bool CompileHlslBlob(
    const char* source,
    size_t sourceLen,
    const char* sourceName,
    const D3D_SHADER_MACRO* defines,
    const char* entryPoint,
    const char* target,
    ID3DBlob** outBlob
) {
    if (!outBlob) return false;
    *outBlob = nullptr;
    if (!source || sourceLen == 0) return false;

    PfnD3DCompile compileFn = ResolveD3DCompile();
    if (!compileFn) return false;

    ID3DBlob* errBlob = nullptr;
    constexpr UINT kCompileOptimizationLevel3 = (1u << 15);
    const HRESULT hr = compileFn(
        source,
        sourceLen,
        sourceName,
        defines,
        nullptr,
        entryPoint,
        target,
        kCompileOptimizationLevel3,
        0,
        outBlob,
        &errBlob
    );
    if (FAILED(hr)) {
        const char* msg = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "unknown error";
        UIE_LOG_ERROR("HLSL compile failed (%s, target=%s, hr=0x%08X): %s",
                      sourceName ? sourceName : "<mem>",
                      target ? target : "?",
                      static_cast<unsigned>(hr),
                      msg);
        if (errBlob) errBlob->Release();
        if (*outBlob) {
            (*outBlob)->Release();
            *outBlob = nullptr;
        }
        return false;
    }
    if (errBlob) {
        errBlob->Release();
    }
    return (*outBlob != nullptr);
}

} // namespace lotro_uie
