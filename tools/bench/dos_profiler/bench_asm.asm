; tools/bench/dos_profiler/bench_asm.asm
; 16-bit Real-Mode MS-DOS assembly helpers for MEMBENCH.EXE (Open Watcom WASM -0 -ms).
;
; All exported functions use __cdecl (stack arguments, leading underscore, return in AX):
;   - _mb_install_hooks(first_mcb, indos_seg, indos_off, inj_ticks, inj_key)
;   - _mb_uninstall_hooks()
;   - _mb_exec_child(path, epb)
;   - _mb_read_stats(dst_struct)
;
; Strictly 8086/8088 compatible.
;
        .8086

DGROUP  GROUP _DATA

_DATA   SEGMENT WORD PUBLIC USE16 'DATA'
_DATA   ENDS

_TEXT   SEGMENT BYTE PUBLIC USE16 'CODE'
        ASSUME CS:_TEXT, DS:DGROUP

; --- CS-relative state (accessible from ISRs without depending on DS) ---
orig_int21      dd 0
orig_int1c      dd 0
saved_ss        dw 0
saved_sp        dw 0
saved_ds        dw 0
hook_active     db 0
hooks_installed db 0

first_mcb_seg   dw 0
indos_seg       dw 0
indos_off       dw 0

tick_count      dw 0
inject_ticks    dw 0
inject_key      dw 0
inject_sent     dw 0

MAX_MCBS        EQU 32
live_mcb_cnt    dw 0
live_mcbs       db (MAX_MCBS * 8) dup (0)
exit_mcb_cnt    dw 0
exit_mcbs       db (MAX_MCBS * 8) dup (0)
exit_code       dw 0FFFFh

MAX_ALLOC_LOG   EQU 48
alloc_log_cnt   dw 0
alloc_log       db (MAX_ALLOC_LOG * 10) dup (0)

tmp_ah          db 0
tmp_bx_in       dw 0
tmp_es_in       dw 0
tmp_ax_out      dw 0
tmp_bx_out      dw 0
tmp_flags       dw 0

; ----------------------------------------------------------------------------
; Internal helper: snapshot MCB chain into CS:DI, returns entry count in CX.
; Clobbers AX, BX, CX, DX, DI, ES.
; ----------------------------------------------------------------------------
snap_mcb_chain PROC NEAR
        xor cx, cx
        mov bx, cs:[first_mcb_seg]
        or bx, bx
        jz snap_done
snap_loop:
        cmp cx, MAX_MCBS
        jae snap_done
        mov es, bx
        mov al, byte ptr es:[0]
        cmp al, 'M'
        je snap_valid
        cmp al, 'Z'
        jne snap_done
snap_valid:
        mov cs:[di], bx             ; mcb_seg
        mov dx, word ptr es:[1]
        mov cs:[di+2], dx           ; owner
        mov dx, word ptr es:[3]
        mov cs:[di+4], dx           ; size_paras
        xor ah, ah
        mov cs:[di+6], ax           ; type ('M' or 'Z')
        add di, 8
        inc cx
        cmp al, 'Z'
        je snap_done
        add bx, dx
        inc bx
        jmp snap_loop
snap_done:
        ret
snap_mcb_chain ENDP

; ----------------------------------------------------------------------------
; INT 1Ch Hook: counts BIOS ticks (18.2 Hz), captures live MCB snapshot when
; INDOS == 0, and injects an optional clean-exit keystroke into the BIOS
; keyboard ring buffer (0040:001Ah..003Eh) at inject_ticks (+ retries).
; ----------------------------------------------------------------------------
int1c_isr PROC FAR
        cmp byte ptr cs:[hook_active], 0
        je int1c_chain

        push ax
        push bx
        push cx
        push dx
        push di
        push es

        inc word ptr cs:[tick_count]
        mov ax, cs:[tick_count]

        ; Capture live MCB snapshot once after ~14 ticks (~0.77s) when INDOS == 0
        cmp word ptr cs:[live_mcb_cnt], 0
        jne int1c_check_key
        cmp ax, 14
        jb int1c_check_key
        mov es, cs:[indos_seg]
        mov bx, cs:[indos_off]
        cmp byte ptr es:[bx], 0
        jne int1c_check_key
        mov di, offset live_mcbs
        call snap_mcb_chain
        mov cs:[live_mcb_cnt], cx

int1c_check_key:
        mov bx, cs:[inject_ticks]
        or bx, bx
        jz int1c_done
        mov ax, cs:[tick_count]
        cmp ax, bx
        jb int1c_done
        sub ax, bx
        cmp ax, 0
        je do_inject
        cmp ax, 18
        je do_inject
        cmp ax, 36
        je do_inject
        cmp ax, 54
        je do_inject
        jmp int1c_done

do_inject:
        mov ax, 0040h
        mov es, ax
        cli
        mov bx, word ptr es:[001Ch] ; tail
        mov dx, bx
        add dx, 2
        cmp dx, word ptr es:[0082h] ; buf_end
        jb inj_nowrap
        mov dx, word ptr es:[0080h] ; buf_start
inj_nowrap:
        cmp dx, word ptr es:[001Ah] ; head (ring buffer full?)
        je inj_full
        mov ax, cs:[inject_key]
        mov word ptr es:[bx], ax
        mov word ptr es:[001Ch], dx
        inc word ptr cs:[inject_sent]
inj_full:
        sti

int1c_done:
        pop es
        pop di
        pop dx
        pop cx
        pop bx
        pop ax

int1c_chain:
        jmp dword ptr cs:[orig_int1c]
int1c_isr ENDP

; ----------------------------------------------------------------------------
; INT 21h Hook: traces AH=48h/49h/4Ah and snapshots live MCBs right at AH=4Ch
; ----------------------------------------------------------------------------
int21_isr PROC FAR
        cmp byte ptr cs:[hook_active], 0
        je int21_chain
        cmp ah, 48h
        je int21_trace_mem
        cmp ah, 49h
        je int21_trace_mem
        cmp ah, 4Ah
        je int21_trace_mem
        cmp ah, 4Ch
        je int21_trace_exit
        cmp ah, 00h
        je int21_trace_exit

int21_chain:
        jmp dword ptr cs:[orig_int21]

int21_trace_exit:
        push ax
        push bx
        push cx
        push dx
        push di
        push es
        xor ah, ah
        mov cs:[exit_code], ax
        mov di, offset exit_mcbs
        call snap_mcb_chain
        mov cs:[exit_mcb_cnt], cx
        pop es
        pop di
        pop dx
        pop cx
        pop bx
        pop ax
        jmp dword ptr cs:[orig_int21]

int21_trace_mem:
        mov cs:[tmp_ah], ah
        mov cs:[tmp_bx_in], bx
        mov cs:[tmp_es_in], es
        pushf
        call dword ptr cs:[orig_int21]
        ; Preserve exact return flags, AX, BX before logging
        mov cs:[tmp_ax_out], ax
        mov cs:[tmp_bx_out], bx
        pushf
        pop cs:[tmp_flags]

        push cx
        push dx
        push si
        push di

        mov cx, cs:[alloc_log_cnt]
        cmp cx, MAX_ALLOC_LOG
        jae log_done
        mov si, cx
        shl si, 1                   ; *2
        mov di, si
        shl si, 1                   ; *4
        shl si, 1                   ; *8
        add di, si                  ; *10
        add di, offset alloc_log

        mov al, cs:[tmp_ah]
        mov dx, cs:[tmp_flags]
        mov ah, dl
        and ah, 1                   ; bit 0 of high byte = CF
        mov cs:[di], ax             ; ah_cf
        mov ax, cs:[tmp_bx_in]
        mov cs:[di+2], ax           ; bx_in
        mov ax, cs:[tmp_es_in]
        mov cs:[di+4], ax           ; es_in
        mov ax, cs:[tmp_ax_out]
        mov cs:[di+6], ax           ; ax_out
        mov ax, cs:[tmp_bx_out]
        mov cs:[di+8], ax           ; bx_out
        inc word ptr cs:[alloc_log_cnt]

log_done:
        pop di
        pop si
        pop dx
        pop cx
        mov ax, cs:[tmp_ax_out]
        mov bx, cs:[tmp_bx_out]
        push cs:[tmp_flags]
        popf
        retf 2
int21_isr ENDP

; ----------------------------------------------------------------------------
; void __cdecl mb_install_hooks(uint16_t first_mcb, uint16_t indos_seg,
;                               uint16_t indos_off, uint16_t inj_ticks,
;                               uint16_t inj_key)
; ----------------------------------------------------------------------------
        PUBLIC _mb_install_hooks
_mb_install_hooks PROC NEAR
        push bp
        mov bp, sp
        push bx
        push dx
        push es
        push ds

        mov ax, [bp+4]
        mov cs:[first_mcb_seg], ax
        mov ax, [bp+6]
        mov cs:[indos_seg], ax
        mov ax, [bp+8]
        mov cs:[indos_off], ax
        mov ax, [bp+10]
        mov cs:[inject_ticks], ax
        mov ax, [bp+12]
        mov cs:[inject_key], ax

        mov word ptr cs:[tick_count], 0
        mov word ptr cs:[inject_sent], 0
        mov word ptr cs:[live_mcb_cnt], 0
        mov word ptr cs:[exit_mcb_cnt], 0
        mov word ptr cs:[alloc_log_cnt], 0
        mov word ptr cs:[exit_code], 0FFFFh
        mov byte ptr cs:[hook_active], 0

        cmp byte ptr cs:[hooks_installed], 0
        jne hooks_already

        ; Save & hook INT 21h
        mov ax, 3521h
        int 21h
        mov word ptr cs:[orig_int21], bx
        mov word ptr cs:[orig_int21+2], es

        ; Save & hook INT 1Ch
        mov ax, 351Ch
        int 21h
        mov word ptr cs:[orig_int1c], bx
        mov word ptr cs:[orig_int1c+2], es

        push cs
        pop ds
        mov dx, offset int21_isr
        mov ax, 2521h
        int 21h

        mov dx, offset int1c_isr
        mov ax, 251Ch
        int 21h

        mov byte ptr cs:[hooks_installed], 1

hooks_already:
        pop ds
        pop es
        pop dx
        pop bx
        pop bp
        ret
_mb_install_hooks ENDP

; ----------------------------------------------------------------------------
; void __cdecl mb_uninstall_hooks(void)
; ----------------------------------------------------------------------------
        PUBLIC _mb_uninstall_hooks
_mb_uninstall_hooks PROC NEAR
        push dx
        push ds
        mov byte ptr cs:[hook_active], 0
        cmp byte ptr cs:[hooks_installed], 0
        je un_done
        mov byte ptr cs:[hooks_installed], 0

        lds dx, dword ptr cs:[orig_int1c]
        mov ax, 251Ch
        int 21h

        lds dx, dword ptr cs:[orig_int21]
        mov ax, 2521h
        int 21h
un_done:
        pop ds
        pop dx
        ret
_mb_uninstall_hooks ENDP

; ----------------------------------------------------------------------------
; uint16_t __cdecl mb_exec_child(const char *path, void *epb)
; Returns 0 on success, or (0x8000 | dos_err) on EXEC failure.
; Activates hooks right before INT 21h AH=4Bh and deactivates immediately after.
; ----------------------------------------------------------------------------
        PUBLIC _mb_exec_child
_mb_exec_child PROC NEAR
        push bp
        mov bp, sp
        push bx
        push cx
        push dx
        push si
        push di
        push es
        push ds

        mov dx, [bp+4]              ; DS:DX = path
        mov bx, [bp+6]              ; ES:BX = epb (ES = DS = DGROUP)
        push ds
        pop es

        mov cs:[saved_ds], ds
        mov cs:[saved_ss], ss
        mov cs:[saved_sp], sp

        mov byte ptr cs:[hook_active], 1
        mov ax, 4B00h
        int 21h
        ; Immediately deactivate hooks & restore SS:SP/DS
        mov byte ptr cs:[hook_active], 0
        cli
        mov ss, cs:[saved_ss]
        mov sp, cs:[saved_sp]
        sti
        mov ds, cs:[saved_ds]
        jc exec_fail
        xor ax, ax
        jmp exec_ret
exec_fail:
        or ax, 8000h
exec_ret:
        pop ds
        pop es
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop bp
        ret
_mb_exec_child ENDP

; ----------------------------------------------------------------------------
; void __cdecl mb_read_stats(void *dst_struct)
; Copies:
;   tick_count(2), inject_sent(2), exit_code(2),
;   live_mcb_cnt(2), exit_mcb_cnt(2), alloc_log_cnt(2),
;   live_mcbs(256), exit_mcbs(256), alloc_log(480)
; Total = 1004 bytes
; ----------------------------------------------------------------------------
        PUBLIC _mb_read_stats
_mb_read_stats PROC NEAR
        push bp
        mov bp, sp
        push cx
        push si
        push di
        push es
        push ds

        mov di, [bp+4]
        push ds
        pop es                      ; ES:DI = DGROUP:dst_struct
        push cs
        pop ds                      ; DS = CS
        cld

        mov ax, cs:[tick_count]
        stosw
        mov ax, cs:[inject_sent]
        stosw
        mov ax, cs:[exit_code]
        stosw
        mov ax, cs:[live_mcb_cnt]
        stosw
        mov ax, cs:[exit_mcb_cnt]
        stosw
        mov ax, cs:[alloc_log_cnt]
        stosw

        mov si, offset live_mcbs
        mov cx, (MAX_MCBS * 8) / 2
        rep movsw

        mov si, offset exit_mcbs
        mov cx, (MAX_MCBS * 8) / 2
        rep movsw

        mov si, offset alloc_log
        mov cx, (MAX_ALLOC_LOG * 10) / 2
        rep movsw

        pop ds
        pop es
        pop di
        pop si
        pop cx
        pop bp
        ret
_mb_read_stats ENDP

_TEXT   ENDS
        END
