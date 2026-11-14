OPTION CASEMAP:NONE

EXTERN EnsureProxyLoaded:PROC

FORWARD_EXPORT MACRO name
    .data
    ALIGN 8
    PUBLIC orig_&name
    orig_&name QWORD 0

    .code
    PUBLIC Proxy_&name
    Proxy_&name PROC
        mov rax, QWORD PTR [orig_&name]
        test rax, rax
        jnz short ready_&name
        call LazyInitFromStub
        mov rax, QWORD PTR [orig_&name]
        test rax, rax
        jz short missing_&name
    ready_&name:
        jmp rax
    missing_&name:
        xor eax, eax
        ret
    Proxy_&name ENDP
ENDM

.code

LazyInitFromStub PROC
    sub rsp, 128
    mov QWORD PTR [rsp + 20h], rcx
    mov QWORD PTR [rsp + 28h], rdx
    mov QWORD PTR [rsp + 30h], r8
    mov QWORD PTR [rsp + 38h], r9
    movdqu XMMWORD PTR [rsp + 40h], xmm0
    movdqu XMMWORD PTR [rsp + 50h], xmm1
    movdqu XMMWORD PTR [rsp + 60h], xmm2
    movdqu XMMWORD PTR [rsp + 70h], xmm3

    call EnsureProxyLoaded

    movdqu xmm3, XMMWORD PTR [rsp + 70h]
    movdqu xmm2, XMMWORD PTR [rsp + 60h]
    movdqu xmm1, XMMWORD PTR [rsp + 50h]
    movdqu xmm0, XMMWORD PTR [rsp + 40h]
    mov r9, QWORD PTR [rsp + 38h]
    mov r8, QWORD PTR [rsp + 30h]
    mov rdx, QWORD PTR [rsp + 28h]
    mov rcx, QWORD PTR [rsp + 20h]
    add rsp, 128
    ret
LazyInitFromStub ENDP

FORWARD_EXPORT CreateDirect3D11DeviceFromDXGIDevice
FORWARD_EXPORT CreateDirect3D11SurfaceFromDXGISurface
FORWARD_EXPORT D3D11CoreCreateDevice
FORWARD_EXPORT D3D11CoreCreateLayeredDevice
FORWARD_EXPORT D3D11CoreGetLayeredDeviceSize
FORWARD_EXPORT D3D11CoreRegisterLayers
FORWARD_EXPORT D3D11CreateDeviceForD3D12
FORWARD_EXPORT D3D11On12CreateDevice
FORWARD_EXPORT D3DKMTCloseAdapter
FORWARD_EXPORT D3DKMTCreateAllocation
FORWARD_EXPORT D3DKMTCreateContext
FORWARD_EXPORT D3DKMTCreateDevice
FORWARD_EXPORT D3DKMTCreateSynchronizationObject
FORWARD_EXPORT D3DKMTDestroyAllocation
FORWARD_EXPORT D3DKMTDestroyContext
FORWARD_EXPORT D3DKMTDestroyDevice
FORWARD_EXPORT D3DKMTDestroySynchronizationObject
FORWARD_EXPORT D3DKMTEscape
FORWARD_EXPORT D3DKMTGetContextSchedulingPriority
FORWARD_EXPORT D3DKMTGetDeviceState
FORWARD_EXPORT D3DKMTGetDisplayModeList
FORWARD_EXPORT D3DKMTGetMultisampleMethodList
FORWARD_EXPORT D3DKMTGetRuntimeData
FORWARD_EXPORT D3DKMTGetSharedPrimaryHandle
FORWARD_EXPORT D3DKMTLock
FORWARD_EXPORT D3DKMTOpenAdapterFromHdc
FORWARD_EXPORT D3DKMTOpenResource
FORWARD_EXPORT D3DKMTPresent
FORWARD_EXPORT D3DKMTQueryAdapterInfo
FORWARD_EXPORT D3DKMTQueryAllocationResidency
FORWARD_EXPORT D3DKMTQueryResourceInfo
FORWARD_EXPORT D3DKMTRender
FORWARD_EXPORT D3DKMTSetAllocationPriority
FORWARD_EXPORT D3DKMTSetContextSchedulingPriority
FORWARD_EXPORT D3DKMTSetDisplayMode
FORWARD_EXPORT D3DKMTSetDisplayPrivateDriverFormat
FORWARD_EXPORT D3DKMTSetGammaRamp
FORWARD_EXPORT D3DKMTSetVidPnSourceOwner
FORWARD_EXPORT D3DKMTSignalSynchronizationObject
FORWARD_EXPORT D3DKMTUnlock
FORWARD_EXPORT D3DKMTWaitForSynchronizationObject
FORWARD_EXPORT D3DKMTWaitForVerticalBlankEvent
FORWARD_EXPORT D3DPerformance_BeginEvent
FORWARD_EXPORT D3DPerformance_EndEvent
FORWARD_EXPORT D3DPerformance_GetStatus
FORWARD_EXPORT D3DPerformance_SetMarker
FORWARD_EXPORT EnableFeatureLevelUpgrade
FORWARD_EXPORT OpenAdapter10
FORWARD_EXPORT OpenAdapter10_2

END
