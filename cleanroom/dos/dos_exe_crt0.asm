; cleanroom/dos/dos_exe_crt0.asm
;
; Clean-room 16-bit Real-Mode MS-DOS MZ .EXE minimal Zero-CRT startup stub
; for OPENSVR.EXE and OPENLNK.EXE (eliminates Open Watcom clibs.lib cstart,
; initargv, and heap allocator nmalloc/nfree/sbrk bloat).
;
; Responsibilities on entry from MS-DOS INT 21h / AH=4Bh:
;   1. Saves PSP segment (entered in DS/ES).
;   2. Normalizes SS:SP from STACK:sp -> DGROUP:(STACK_off + sp) so SS == DS == ES
;      as required by Open Watcom Small Model (-ms).
;   3. Zeroes _BSS (_exe_bss_start .. _exe_bss_end) deterministically.
;   4. Copies MS-DOS PSP command tail (PSP:0080h, clamped <= 127 bytes) into
;      zero-terminated static buffer _exe_psp_tail in DGROUP.
;   5. Calls C99 trampoline _olnk_dos_exe_run(tail) -> olnk_dos_main(argc, argv).
;   6. Exits cleanly to MS-DOS via INT 21h / AH=4Ch with AL = return code.
;   7. Provides pure-8086 32-bit unsigned/signed multiply & unsigned divide
;      helpers (__U4M / __I4M / __U4D) so zero clibs.lib code is linked.
;
; Strictly 8086/8088 compatible (assembled with Open Watcom WASM -0 -ms -wx -we).

        .8086

DGROUP  GROUP _NULL, CONST, CONST2, _DATA, _BSSBEG, _BSS, _BSSEND, STACK

        EXTRN _olnk_dos_exe_run:NEAR

; Guard word at DGROUP:0000 so no valid C data object/string ever has offset 0 (NULL)
_NULL   SEGMENT PARA PUBLIC USE16 'BEGDATA'
        PUBLIC __exe_null_guard
__exe_null_guard dw 0
_NULL   ENDS

CONST   SEGMENT WORD PUBLIC USE16 'DATA'
CONST   ENDS

CONST2  SEGMENT WORD PUBLIC USE16 'DATA'
CONST2  ENDS

_DATA   SEGMENT WORD PUBLIC USE16 'DATA'
_DATA   ENDS

_BSSBEG SEGMENT WORD PUBLIC USE16 'BSSBEG'
        PUBLIC _exe_bss_start
_exe_bss_start  label byte
_BSSBEG ENDS

_BSS    SEGMENT WORD PUBLIC USE16 'BSS'
        PUBLIC _exe_psp_tail
_exe_psp_tail   db 128 dup (?)
_BSS    ENDS

_BSSEND SEGMENT BYTE PUBLIC USE16 'BSSEND'
        PUBLIC _exe_bss_end
_exe_bss_end    label byte
_BSSEND ENDS

; Actual stack size is controlled by wlink "option stack=..."
STACK   SEGMENT PARA STACK USE16 'STACK'
STACK   ENDS

; Placed first in CODE class so _exe_start sits at 0000:0000 and no C function
; pointer in _TEXT ever collides with offset 0x0000 (NULL).
BEGTEXT SEGMENT BYTE PUBLIC USE16 'CODE'
        ASSUME CS:BEGTEXT, DS:DGROUP, SS:DGROUP

        PUBLIC _exe_start
_exe_start PROC NEAR
        ; Save PSP segment (MS-DOS enters MZ .EXE with DS = ES = PSP)
        mov dx, ds

        ; Load relocated DGROUP segment and re-base SS:SP into DGROUP
        ; (8088-safe atomic SS:SP update with CLI/STI; pure 8086 SHL by CL)
        mov ax, DGROUP
        mov bx, ss
        sub bx, ax                  ; BX = (STACK_seg - DGROUP_seg) in paragraphs
        mov cl, 4
        shl bx, cl                  ; BX = byte offset of STACK in DGROUP
        add bx, sp                  ; BX = byte offset of top-of-stack in DGROUP
        cli
        mov ss, ax
        mov sp, bx
        sti

        ; Establish DS = ES = DGROUP and DF = 0 for C99 runtime
        cld
        mov ds, ax
        mov es, ax

        ; Zero _BSS (_exe_bss_start .. _exe_bss_end)
        mov di, offset DGROUP:_exe_bss_start
        mov cx, offset DGROUP:_exe_bss_end
        sub cx, di
        jbe bss_done
        xor al, al
        rep stosb
bss_done:

        ; Copy MS-DOS PSP command tail (PSP:0080h len, PSP:0081h.. bytes)
        ; into zeroed _exe_psp_tail[128] (trailing NUL guaranteed)
        push ds
        mov ds, dx                  ; DS = PSP segment (ES remains DGROUP)
        xor ch, ch
        mov cl, byte ptr ds:[0080h]
        cmp cl, 127
        jbe tail_len_ok
        mov cl, 127
tail_len_ok:
        mov si, 0081h
        mov di, offset DGROUP:_exe_psp_tail
        rep movsb
        pop ds

        ; Call C99 command-line parser & main trampoline:
        ;   int _olnk_dos_exe_run(char *tail)  [AX in -> AX out]
        mov ax, offset DGROUP:_exe_psp_tail
        call near ptr _olnk_dos_exe_run

        ; Exit to MS-DOS with return code in AL
        mov ah, 4Ch
        int 21h
_exe_start ENDP

BEGTEXT ENDS

; ============================================================================
; Pure-8086 32-Bit Math Helpers for Open Watcom Zero-CRT Linking
;   __U4M / __I4M: DX:AX * CX:BX -> DX:AX (low 32 bits)
;   __U4D        : DX:AX / CX:BX -> DX:AX (quotient), CX:BX (remainder)
; ============================================================================
_TEXT   SEGMENT BYTE PUBLIC USE16 'CODE'
        ASSUME CS:_TEXT, DS:DGROUP, SS:DGROUP

        PUBLIC __U4M
        PUBLIC __I4M
        PUBLIC __U4D

__U4M PROC NEAR
__I4M:
        push si
        mov si, ax                  ; SI = a_lo
        mov ax, dx                  ; AX = a_hi
        mul bx                      ; AX = low(a_hi * b_lo)
        xchg cx, ax                 ; CX = low(a_hi * b_lo), AX = b_hi
        mul si                      ; AX = low(b_hi * a_lo)
        add cx, ax                  ; CX = cross_sum
        mov ax, si                  ; AX = a_lo
        mul bx                      ; DX:AX = a_lo * b_lo
        add dx, cx                  ; DX += cross_sum
        pop si
        ret
__U4M ENDP

__U4D PROC NEAR
        or cx, cx
        jnz u4d_nonzero
        or bx, bx
        jnz u4d_nonzero
        xor ax, ax
        xor dx, dx
        xor bx, bx
        xor cx, cx
        ret
u4d_nonzero:
        push si
        push di
        push bp
        xor si, si                  ; DI:SI = 32-bit remainder = 0
        xor di, di
        mov bp, 32                  ; 32 quotient bits
u4d_loop:
        shl ax, 1                   ; Shift (DI:SI : DX:AX) left by 1
        rcl dx, 1
        rcl si, 1
        rcl di, 1
        jc u4d_sub                  ; 33rd bit set => R >= 2^32 > D
        cmp di, cx
        jb u4d_next
        ja u4d_sub
        cmp si, bx
        jb u4d_next
u4d_sub:
        sub si, bx
        sbb di, cx
        inc ax                      ; Set quotient LSB
u4d_next:
        dec bp
        jnz u4d_loop
        mov cx, di                  ; CX:BX = remainder
        mov bx, si
        pop bp
        pop di
        pop si
        ret
__U4D ENDP

_TEXT   ENDS

        END _exe_start
