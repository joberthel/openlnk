; cleanroom/dos/openlnk_sys_entry.asm
;
; Clean-room 16-bit Real-Mode MS-DOS 5.0/6.22 Block Device Driver (OPENLNK.SYS)
; assembly entry stub, private 896-byte driver stack, far-memory copy helpers,
; and pure-8086 32-bit unsigned multiply/divide helpers (zero CRT).
;
; Single-segment Tiny/DGROUP layout (CS == DS == ES == SS, <= 64 KiB):
;   Offset 0x0000 : 18-byte MS-DOS Block Device Driver Header (SYS_HEAD)
;   Resident Code : _TEXT
;   Resident Data : CONST, CONST2, _DATA
;   Zero-init BSS : _BSS .. _BSSEND (explicitly zeroed on first Interrupt call)
;   Private Stack : _DRVSTK (private 8088-safe driver stack)
;   Unit Tables   : _UNIT_BSS (per-unit BPB/label slots; trimmed to /DRIVES:<N>)
;   --- Break Address <= _sys_resident_end (MS-DOS reclaims everything above) ---
;   Discard INIT  : INIT_TEXT, INIT_DATA .. _sys_init_end (freed after Cmd 0x00)
;
; Strictly 8086/8088 compatible (assembled with Open Watcom WASM -0 -ms -wx -we).

        .8086

DGROUP  GROUP SYS_HEAD, _TEXT, CONST, CONST2, _DATA, _BSS, _BSSEND, _DRVSTK, _UNIT_BSS, _RESEND, INIT_TEXT, INIT_DATA, _INITEND

        EXTRN _olnk_dos_sys_interrupt_c:NEAR

SYS_HEAD SEGMENT BYTE PUBLIC USE16 'CODE'
        ASSUME CS:DGROUP, DS:DGROUP, SS:DGROUP

; ============================================================================
; 18-Byte MS-DOS Block Device Driver Header — MUST reside at file offset 0x0000
; ============================================================================
        PUBLIC _sys_dev_header
        PUBLIC _sys_header_units
        PUBLIC _sys_strategy
        PUBLIC _sys_interrupt

_sys_dev_header:
        dd 0FFFFFFFFh                     ; +0x00: Pointer to next driver (-1 = last)
        dw 02842h                         ; +0x04: Block | Non-IBM(13) | Open/Close/RM(11) | GenIOCTL(6) | LBA32(1)
        dw offset DGROUP:_sys_strategy    ; +0x06: Strategy routine near offset
        dw offset DGROUP:_sys_interrupt   ; +0x08: Interrupt routine near offset
_sys_header_units:
        db 01h                            ; +0x0A: Block unit count (patched on INIT)
        db 'OPENLNK'                      ; +0x0B..+0x11: 7-byte identification signature

; ============================================================================
; MS-DOS Strategy Entry (FAR): captures Request Header ES:BX CS-relatively
; ============================================================================
_sys_strategy PROC FAR
        mov cs:[saved_rh_off], bx
        mov cs:[saved_rh_seg], es
        retf
_sys_strategy ENDP

; ============================================================================
; MS-DOS Interrupt Entry (FAR):
;   1. Preserves all 8086 registers & flags on DOS stack
;   2. Switches SS:SP atomically (8088-safe) to private 896-byte driver stack
;   3. Sets DS = ES = CS (DGROUP) and clears DF
;   4. Zeroes _BSS once on first invocation (MS-DOS does not zero .SYS BSS)
;   5. Dispatches into C99 Request-Header marshaller
;   6. Restores caller SS:SP, registers, flags and returns via RETF
; ============================================================================
_sys_interrupt PROC FAR
        pushf
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push bp
        push ds
        push es

        ; Save caller FLAGS (incl. IF) in DX while switching SS:SP atomically
        pushf
        pop dx
        cli
        mov cs:[saved_sp], sp
        mov cs:[saved_ss], ss
        mov ax, cs
        mov ss, ax
        mov sp, offset DGROUP:_sys_stack_top
        push dx
        popf

        ; Establish DS = ES = CS and DF = 0 for C99 runtime
        cld
        mov ds, ax
        mov es, ax

        ; Zero _BSS once on very first entry
        cmp byte ptr cs:[bss_cleared], 0
        jne bss_ready
        mov byte ptr cs:[bss_cleared], 1
        mov di, offset DGROUP:_sys_bss_start
        mov cx, offset DGROUP:_sys_bss_end
        sub cx, di
        jbe bss_ready
        xor al, al
        rep stosb
bss_ready:

        ; Call C99 marshaller:
        ;   _olnk_dos_sys_interrupt_c(AX=rh_seg, DX=rh_off, BX=drv_seg, CX=resident_end)
        mov ax, cs:[saved_rh_seg]
        mov dx, cs:[saved_rh_off]
        mov bx, cs
        mov cx, offset DGROUP:_sys_resident_end
        call near ptr _olnk_dos_sys_interrupt_c

        ; Restore caller SS:SP atomically (preserving IF)
        pushf
        pop dx
        cli
        mov ss, cs:[saved_ss]
        mov sp, cs:[saved_sp]
        push dx
        popf

        pop es
        pop ds
        pop bp
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        popf
        retf
_sys_interrupt ENDP

; ============================================================================
; 8086 Far-Memory Helpers (called from C via #pragma aux)
; ============================================================================
        PUBLIC _sys_asm_far_read
        PUBLIC _sys_asm_far_write

; Copy CX bytes from far AX:SI into near CS:DI (preserves all registers)
_sys_asm_far_read PROC NEAR
        push ds
        push es
        push si
        push di
        push cx
        push cs
        pop es
        mov ds, ax
        cld
        shr cx, 1
        rep movsw
        jnc fr_done
        movsb
fr_done:
        pop cx
        pop di
        pop si
        pop es
        pop ds
        ret
_sys_asm_far_read ENDP

; Copy CX bytes from near CS:SI into far AX:DI (preserves all registers)
_sys_asm_far_write PROC NEAR
        push ds
        push es
        push si
        push di
        push cx
        push cs
        pop ds
        mov es, ax
        cld
        shr cx, 1
        rep movsw
        jnc fw_done
        movsb
fw_done:
        pop cx
        pop di
        pop si
        pop es
        pop ds
        ret
_sys_asm_far_write ENDP

; ============================================================================
; Pure-8086 32-Bit Math Helpers for Open Watcom Zero-CRT Linking
;   __U4M / __I4M: DX:AX * CX:BX -> DX:AX (low 32 bits)
;   __U4D        : DX:AX / CX:BX -> DX:AX (quotient), CX:BX (remainder)
; ============================================================================
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
        ; Guard against division by zero without clobbering SI
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

; ============================================================================
; CS-Relative Driver Stub State (guaranteed initialized inside .SYS image)
; ============================================================================
saved_rh_off    dw 0
saved_rh_seg    dw 0
saved_ss        dw 0
saved_sp        dw 0
bss_cleared     db 0

SYS_HEAD ENDS

_TEXT   SEGMENT BYTE PUBLIC USE16 'CODE'
_TEXT   ENDS

CONST   SEGMENT WORD PUBLIC USE16 'DATA'
CONST   ENDS

CONST2  SEGMENT WORD PUBLIC USE16 'DATA'
CONST2  ENDS

_DATA   SEGMENT WORD PUBLIC USE16 'DATA'
_DATA   ENDS

_BSS    SEGMENT WORD PUBLIC USE16 'BSS'
        PUBLIC _sys_bss_start
_sys_bss_start  label byte
_BSS    ENDS

_BSSEND SEGMENT BYTE PUBLIC USE16 'BSSEND'
        PUBLIC _sys_bss_end
_sys_bss_end    label byte
_BSSEND ENDS

; Private 8088-safe driver stack (896 B: live MS-DOS 6.22 peak measured at 464 B
; across online/offline INIT, 24 KiB COPY/FC, CHKDSK, VOL/IOCTL, and lazy-connect,
; leaving +432 B safety headroom; uses class 'DRVSTK' rather than 'STACK' so
; wlink does not force a default 4096-byte EXE stack minimum before INIT_TEXT).
_DRVSTK SEGMENT PARA PUBLIC USE16 'DRVSTK'
        PUBLIC _sys_stack_bottom
        PUBLIC _sys_stack_top
_sys_stack_bottom label byte
_sys_stack_area db 896 dup (0)
_sys_stack_top  label byte
_DRVSTK ENDS

_UNIT_BSS SEGMENT WORD PUBLIC USE16 'UNIT_BSS'
        PUBLIC _sys_units_base
_sys_units_base label byte
_UNIT_BSS ENDS

_RESEND SEGMENT BYTE PUBLIC USE16 'RESEND'
        PUBLIC _sys_resident_end
_sys_resident_end label byte
_RESEND ENDS

; ============================================================================
; Discardable INIT-Only Helpers (reclaimed by MS-DOS after Cmd 0x00 returns)
; ============================================================================
INIT_TEXT SEGMENT BYTE PUBLIC USE16 'INIT_CODE'
        ASSUME CS:DGROUP, DS:DGROUP, SS:DGROUP

        PUBLIC _sys_asm_set_header_units
        PUBLIC _sys_asm_dos_print

; Patch header byte +0x0A (unit_count) with AL
_sys_asm_set_header_units PROC NEAR
        mov byte ptr cs:[_sys_header_units], al
        ret
_sys_asm_set_header_units ENDP

; Print '$'-terminated string at DS:DX via INT 21h AH=09h (ONLY during INIT!)
_sys_asm_dos_print PROC NEAR
        push ax
        mov ah, 09h
        int 21h
        pop ax
        ret
_sys_asm_dos_print ENDP

INIT_TEXT ENDS

INIT_DATA SEGMENT WORD PUBLIC USE16 'INIT_DATA'
        ; Pin _sys_init_end in OPENLNK.MAP (discarded after INIT; 0 B resident)
        dw offset DGROUP:_sys_init_end
INIT_DATA ENDS

_INITEND SEGMENT BYTE PUBLIC USE16 'INIT_END'
        PUBLIC _sys_init_end
_sys_init_end   label byte
_INITEND ENDS

        END _sys_dev_header
