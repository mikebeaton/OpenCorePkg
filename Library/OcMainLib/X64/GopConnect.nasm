;------------------------------------------------------------------------------
;  @file
;  Copyright (C) 2022, mikebeaton. All rights reserved.
;
;  All rights reserved.
;  SPDX-License-Identifier: BSD-3-Clause
;------------------------------------------------------------------------------

; ########################################################################
; ### Binary Data
BITS 64
DEFAULT  REL

SECTION  .data
extern ASM_PFX(gEfiGraphicsOutputProtocolGuid)

extern ASM_PFX(aGop)
extern ASM_PFX(aGopAlreadyConnected)

extern ASM_PFX(gBS)
extern ASM_PFX(gST)

; ########################################################################
; ### Code
SECTION  .text

;------------------------------------------------------------------------------
; EFI_STATUS
; EFIAPI
; GopConnect (
;   VOID
;   );
;------------------------------------------------------------------------------
global ASM_PFX(GopConnect)
ASM_PFX(GopConnect):

    push    rsi
    push    rdi
    sub     rsp, 28h

    mov     rsi, [ASM_PFX(gBS)]
    ;lea     rsi, [rsi+98h]          ; = HandleProtocol - same result and same size as following
    add     rsi, 98h                ; = HandleProtocol

    xor     rdi, rdi
    sub     rdi, 2                  ; loop index

call_loop:
    mov     rcx, [ASM_PFX(gST)]     ; arg0
    mov     rcx, [rcx+38h]          ; gST->ConsoleOutHandle

    lea     rdx, [ASM_PFX(gEfiGraphicsOutputProtocolGuid)]    ; arg1

    lea     r8, [ASM_PFX(aGop)]     ; arg2

    inc     rdi
    js      do_call
    jnz     locate                  ; we need to manually fix this up to a long jump
    nop                             ; if we are splitting the function;
    nop                             ; http://ref.x86asm.net/coder64.html
    nop
    nop

uninstall:
    sub     rsi, 8h                 ; -= (HandleProtocol - UninstallProtocolInterface)
    mov     r8, [r8]

do_call:
    ; we can uninstall even if protocol is not installed, since uninstall will just fail
    call    [rsi]
    jmp     call_loop

locate:
    ;;add     rsi, 0B0h               ; += (LocateProtocol - UninstallProtocolInterface)
    mov     rcx, rdx
    xor     rdx, rdx

    call    [rsi + 0B0h]            ; + (LocateProtocol - UninstallProtocolInterface)
    test    rax, rax
    js      exit

    mov     rcx, [ASM_PFX(gST)]     ; arg0
    lea     rcx, [rcx+38h]          ; &gST->ConsoleOutHandle

    lea     rdx, [ASM_PFX(gEfiGraphicsOutputProtocolGuid)]    ; arg1

    xor     r8, r8                  ; arg2

    lea     r9, [ASM_PFX(aGop)]     ; arg3
    mov     r9, [r9]

    ;;sub     rsi, 0C0h               ; -= (LocateProtocol - InstallProtocolInterface)

    call    [rsi - 10h]             ; - (UninstallProtocolInterface - InstallProtocolInterface)

exit:
    ; properly conditionally set the bool; rdi is 1
    xor     rcx, rcx
    test    rax, rax
    cmovz   rcx, rdi
    mov     byte [ASM_PFX(aGopAlreadyConnected)], cl
    lea     r8, [ASM_PFX(aGop)]
    jnz     exit_failure

    ; apparently we need to set the initial gfx mode, if we want to run without OC's help.
    ; mode 0 of the card seems to be valid as reasonable 'default' (at least for now).
    ; is this really needed when installed in firmware? quite possibly, because the mode won't
    ; be available to set until after drivers are connected by the picker, which means that
    ; here is the first time we can do it (this will also mean that if, e.g., the firmware
    ; password app does _not_ connect drivers, then the current fix still won't give it gfx;
    ; adding something to that to connect drivers would be a security-lowering way to bring it back).
    mov     rcx, [r8]               ; aGop->SetMode (aGop, 0);
    xor     rdx, rdx
    call    [rcx + 8]               ; SetMode
    jmp     exit_success

exit_failure:
    ; zero aGop on failure
    mov     [r8], rcx

exit_success:
    add     rsp, 28h
    pop     rdi
    pop     rsi

    retn
