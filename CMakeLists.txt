cmake_minimum_required(VERSION 3.22)
project(LOTRO-UIE VERSION 0.1.0 LANGUAGES C CXX ASM_MASM)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LOTRO-UIE requires 64-bit Windows (x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link MSVC runtime statically so the proxy DLL works standalone in LOTRO's x64 folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(lotro_uie_core STATIC
    src/common/config.h
    src/common/config.cpp
    src/common/hash.h
    src/common/logger.h
    src/common/logger.cpp
    src/proxy/proxy_loader.h
    src/proxy/proxy_loader.cpp
    src/dx11/asset_dumper.h
    src/dx11/asset_dumper.cpp
    src/dx11/context_state.h
    src/dx11/debug_overlay.h
    src/dx11/debug_overlay.cpp
    src/dx11/dx11_hooks.h
    src/dx11/dx11_hooks.cpp
    src/dx11/dxgi_format_names.h
    src/dx11/inspector.h
    src/dx11/inspector.cpp
    src/dx11/resource_tracker.h
    src/dx11/resource_tracker.cpp
    src/dx11/ui_enhancer.h
    src/dx11/ui_enhancer.cpp
    src/dx11/ui_sr_cache.cpp
    src/dx11/world_postfx.cpp
    src/dx11/d3d_compile_util.cpp
    src/dx11/vtable_hook.h
    src/dx11/vtable_slots.h
    src/dx11/vtable_slots.c
)

target_include_directories(lotro_uie_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(lotro_uie_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_compile_options(lotro_uie_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->
    $<$<COMPILE_LANGUAGE:C>:/W3>
)

# NOTE: Intentionally do NOT link d3d11.lib or dxgi.lib to avoid recursive DLL self-imports.
target_link_libraries(lotro_uie_core PUBLIC
    user32
)

# Primary proxy target: d3d11.dll
add_library(lotro_uie_d3d11 SHARED
    src/proxy/d3d11_target.cpp
    src/proxy/d3d11_stubs.asm
    src/proxy/d3d11_exports.def
)
set_target_properties(lotro_uie_d3d11 PROPERTIES
    OUTPUT_NAME "d3d11"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/d3d11"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist/d3d11"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist/d3d11"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist/d3d11"
)
target_compile_options(lotro_uie_d3d11 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
target_link_libraries(lotro_uie_d3d11 PRIVATE lotro_uie_core)
add_custom_command(TARGET lotro_uie_d3d11 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/lotro_uie.ini"
        "$<TARGET_FILE_DIR:lotro_uie_d3d11>/lotro_uie.ini"
)

# Alternative proxy target: dxgi.dll (optional if d3d11.dll cannot intercept SwapChain creation)
add_library(lotro_uie_dxgi SHARED
    src/proxy/dxgi_target.cpp
    src/proxy/dxgi_stubs.asm
    src/proxy/dxgi_exports.def
)
set_target_properties(lotro_uie_dxgi PROPERTIES
    OUTPUT_NAME "dxgi"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/dxgi"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist/dxgi"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist/dxgi"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist/dxgi"
)
target_compile_options(lotro_uie_dxgi PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
target_link_libraries(lotro_uie_dxgi PRIVATE lotro_uie_core)
add_custom_command(TARGET lotro_uie_dxgi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/lotro_uie.ini"
        "$<TARGET_FILE_DIR:lotro_uie_dxgi>/lotro_uie.ini"
)

# Standalone WARP DirectX 11 smoke test executable (loads proxy d3d11.dll dynamically)
add_executable(lotro_uie_smoke_test
    tests/smoke_test.cpp
)
target_compile_definitions(lotro_uie_smoke_test PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_compile_options(lotro_uie_smoke_test PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(lotro_uie_smoke_test PRIVATE user32)
