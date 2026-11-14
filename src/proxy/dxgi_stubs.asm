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

FORWARD_EXPORT ApplyCompatResolutionQuirking
FORWARD_EXPORT CompatString
FORWARD_EXPORT CompatValue
FORWARD_EXPORT DXGID3D10CreateDevice
FORWARD_EXPORT DXGID3D10CreateLayeredDevice
FORWARD_EXPORT DXGID3D10GetLayeredDeviceSize
FORWARD_EXPORT DXGID3D10RegisterLayers
FORWARD_EXPORT DXGIDeclareAdapterRemovalSupport
FORWARD_EXPORT DXGIDisableVBlankVirtualization
FORWARD_EXPORT DXGIDumpJournal
FORWARD_EXPORT DXGIGetDebugInterface1
FORWARD_EXPORT DXGIReportAdapterConfiguration
FORWARD_EXPORT PIXBeginCapture
FORWARD_EXPORT PIXEndCapture
FORWARD_EXPORT PIXGetCaptureState
FORWARD_EXPORT SetAppCompatStringPointer
FORWARD_EXPORT UpdateHMDEmulationStatus

END
