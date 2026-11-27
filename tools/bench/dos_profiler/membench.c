/* tools/bench/dos_profiler/membench.c
 *
 * Real MS-DOS 6.22 Foreground Executable Conventional RAM Profiler.
 *
 * Measures exact conventional memory behavior of any DOS .EXE:
 *   1. Live MCB chain snapshot during execution (INT 1Ch when INDOS == 0)
 *      and right before child termination (INT 21h AH=4Ch / 00h hook).
 *   2. Full INT 21h AH=48h/49h/4Ah dynamic allocation & resize trace.
 *   3. Sentinel dirty-memory scan (0xA5 fill across free conventional RAM
 *      before EXEC, paragraph-by-paragraph scan after exit -> exact
 *      High-Water Mark and Touched Working Set / RSS).
 *   4. Optional /LIMIT:<KB> ballast allocator to experimentally prove the
 *      exact minimum free conventional RAM required to start and run.
 *
 * Usage:
 *   MEMBENCH [options] <exe_path> [exe_args...]
 * Options:
 *   /KEY:ALTF4      Inject Alt+F4 (0x6B00) after /TICKS (e.g. INTERSVR.EXE)
 *   /KEY:ESC        Inject ESC    (0x011B) after /TICKS (e.g. OPENSVR.EXE)
 *   /TICKS:<N>      BIOS ticks before key injection (default 36 = ~2.0s; 0=none)
 *   /LIMIT:<KB>     Leave only <KB> KiB free conventional RAM (ballast test)
 *   /QUIET          Suppress verbose MCB/alloc dump (compact summary only)
 *   /COM2           Mirror summary output directly to COM2 (0x2F8 polled)
 */
#include <conio.h>
#include <dos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MCBS      32
#define MAX_ALLOC_LOG 48
#define SENTINEL_BYTE 0xA5U
#define COM2_BASE     0x2F8U

typedef struct {
    uint16_t seg;
    uint16_t owner;
    uint16_t size_paras;
    uint16_t type;
} mcb_entry_t;

typedef struct {
    uint16_t ah_cf;
    uint16_t bx_in;
    uint16_t es_in;
    uint16_t ax_out;
    uint16_t bx_out;
} alloc_ev_t;

typedef struct {
    uint16_t tick_count;
    uint16_t inject_sent;
    uint16_t exit_code;
    uint16_t live_mcb_cnt;
    uint16_t exit_mcb_cnt;
    uint16_t alloc_log_cnt;
    mcb_entry_t live_mcbs[MAX_MCBS];
    mcb_entry_t exit_mcbs[MAX_MCBS];
    alloc_ev_t alloc_log[MAX_ALLOC_LOG];
} mb_stats_t;

typedef struct {
    uint16_t env_seg;
    uint16_t cmd_off;
    uint16_t cmd_seg;
    uint16_t fcb1_off;
    uint16_t fcb1_seg;
    uint16_t fcb2_off;
    uint16_t fcb2_seg;
} exec_pb_t;

extern void __cdecl mb_install_hooks(
    uint16_t first_mcb,
    uint16_t indos_seg,
    uint16_t indos_off,
    uint16_t inj_ticks,
    uint16_t inj_key
);
extern void __cdecl mb_uninstall_hooks(void);
extern uint16_t __cdecl mb_exec_child(const char *path, void *epb);
extern void __cdecl mb_read_stats(void *dst_struct);

static int g_use_com2 = 0;
static mb_stats_t g_stats;
static mcb_entry_t g_pre_mcbs[MAX_MCBS];
static uint16_t g_pre_cnt = 0;
static char g_linebuf[256];

static void com2_init(void) {
    outp(COM2_BASE + 1U, 0x00U);
    outp(COM2_BASE + 3U, 0x80U);
    outp(COM2_BASE + 0U, 0x01U); /* 115200 bps */
    outp(COM2_BASE + 1U, 0x00U);
    outp(COM2_BASE + 3U, 0x03U); /* 8N1 */
    outp(COM2_BASE + 2U, 0xC7U);
    outp(COM2_BASE + 4U, 0x03U);
}

static void com2_putc(unsigned char ch) {
    unsigned long spin = 0UL;
    while (((inp(COM2_BASE + 5U) & 0x20U) == 0U) && (spin < 200000UL)) {
        ++spin;
    }
    outp(COM2_BASE, ch);
}

static void out_str(const char *s) {
    fputs(s, stdout);
    if (g_use_com2) {
        while (*s != '\0') {
            if (*s == '\n') {
                com2_putc('\r');
            }
            com2_putc((unsigned char)*s++);
        }
    }
}

static uint16_t get_first_mcb(void) {
    union REGS r;
    struct SREGS sr;
    r.h.ah = 0x52;
    int86x(0x21, &r, &r, &sr);
    return *(uint16_t far *)MK_FP(sr.es, r.x.bx - 2);
}

static void get_indos_ptr(uint16_t *seg, uint16_t *off) {
    union REGS r;
    struct SREGS sr;
    r.h.ah = 0x34;
    int86x(0x21, &r, &r, &sr);
    *seg = sr.es;
    *off = r.x.bx;
}

static uint16_t snapshot_mcbs_now(uint16_t first_mcb, mcb_entry_t *out) {
    uint16_t seg = first_mcb;
    uint16_t cnt = 0;
    while (cnt < MAX_MCBS && seg != 0) {
        uint8_t far *p = (uint8_t far *)MK_FP(seg, 0);
        uint8_t ty = p[0];
        if (ty != 'M' && ty != 'Z') {
            break;
        }
        out[cnt].seg = seg;
        out[cnt].owner = *(uint16_t far *)(p + 1);
        out[cnt].size_paras = *(uint16_t far *)(p + 3);
        out[cnt].type = ty;
        ++cnt;
        if (ty == 'Z') {
            break;
        }
        seg = (uint16_t)(seg + out[cnt - 1].size_paras + 1U);
    }
    return cnt;
}

/* Fill free block payload (seg_start .. seg_start + paras - 1) with 0xA5 */
static void fill_sentinel(uint16_t seg_start, uint16_t paras) {
    uint16_t p;
    for (p = 0; p < paras; ++p) {
        uint8_t far *ptr = (uint8_t far *)MK_FP((uint16_t)(seg_start + p), 0);
        uint16_t i;
        for (i = 0; i < 16; ++i) {
            ptr[i] = SENTINEL_BYTE;
        }
    }
}

/* Scan region [seg_start .. seg_start + paras - 1] for non-0xA5 paragraphs */
static void scan_dirty_region(
    uint16_t seg_start,
    uint16_t paras,
    uint16_t child_psp,
    int quiet
) {
    uint32_t dirty_paras = 0;
    uint32_t dirty_bytes = 0;
    uint32_t hwm_from_start = 0;
    uint32_t hwm_from_psp = 0;
    uint16_t p;
    int in_run = 0;
    uint16_t run_start = 0;
    int runs_printed = 0;

    for (p = 0; p < paras; ++p) {
        uint16_t cur_seg = (uint16_t)(seg_start + p);
        uint8_t far *ptr = (uint8_t far *)MK_FP(cur_seg, 0);
        uint16_t i;
        int is_dirty = 0;
        for (i = 0; i < 16; ++i) {
            if (ptr[i] != SENTINEL_BYTE) {
                is_dirty = 1;
                ++dirty_bytes;
            }
        }
        if (is_dirty) {
            ++dirty_paras;
            hwm_from_start = (uint32_t)(p + 1U) * 16UL;
            if (child_psp != 0 && cur_seg >= child_psp) {
                hwm_from_psp = (uint32_t)(cur_seg - child_psp + 1U) * 16UL;
            }
            if (!in_run) {
                in_run = 1;
                run_start = p;
            }
        } else if (in_run) {
            if (!quiet && runs_printed < 12) {
                sprintf(
                    g_linebuf,
                    "  DIRTY_RUN seg=%04X..%04X off=0x%05lX..0x%05lX len=%lu B\n",
                    (unsigned)(seg_start + run_start),
                    (unsigned)(seg_start + p - 1U),
                    (unsigned long)run_start * 16UL,
                    (unsigned long)p * 16UL,
                    (unsigned long)(p - run_start) * 16UL
                );
                out_str(g_linebuf);
                ++runs_printed;
            }
            in_run = 0;
        }
    }
    if (in_run && !quiet && runs_printed < 12) {
        sprintf(
            g_linebuf,
            "  DIRTY_RUN seg=%04X..%04X off=0x%05lX..0x%05lX len=%lu B\n",
            (unsigned)(seg_start + run_start),
            (unsigned)(seg_start + paras - 1U),
            (unsigned long)run_start * 16UL,
            (unsigned long)paras * 16UL,
            (unsigned long)(paras - run_start) * 16UL
        );
        out_str(g_linebuf);
    }

    sprintf(
        g_linebuf,
        "  DIRTY_SUMMARY: touched_paras=%lu (%lu B), exact_dirty_bytes=%lu B, "
        "hwm_from_free=%lu B, hwm_from_psp=%lu B (child_psp=%04X)\n",
        dirty_paras,
        dirty_paras * 16UL,
        dirty_bytes,
        hwm_from_start,
        hwm_from_psp,
        (unsigned)child_psp
    );
    out_str(g_linebuf);
}

static uint16_t find_child_psp(
    const mcb_entry_t *snap,
    uint16_t snap_cnt,
    uint16_t free_seg_start
) {
    uint16_t i;
    uint16_t best = 0;
    for (i = 0; i < snap_cnt; ++i) {
        uint16_t body_seg = (uint16_t)(snap[i].seg + 1U);
        if (body_seg >= free_seg_start && snap[i].owner != 0 && snap[i].owner != _psp) {
            if (snap[i].owner == body_seg) {
                return body_seg;
            }
            if (best == 0) {
                best = snap[i].owner;
            }
        }
    }
    return best;
}

static void dump_mcb_snapshot(
    const char *tag,
    const mcb_entry_t *snap,
    uint16_t cnt,
    uint16_t free_seg_start,
    uint16_t *out_child_psp
) {
    uint16_t i;
    uint16_t child_psp = find_child_psp(snap, cnt, free_seg_start);
    uint32_t child_total_paras = 0;
    uint32_t child_extra_paras = 0;
    uint16_t child_blocks = 0;

    if (out_child_psp != NULL && child_psp != 0) {
        *out_child_psp = child_psp;
    }

    for (i = 0; i < cnt; ++i) {
        uint16_t body = (uint16_t)(snap[i].seg + 1U);
        int is_child = (child_psp != 0 && snap[i].owner == child_psp);
        if (is_child) {
            child_total_paras += (uint32_t)snap[i].size_paras + 1UL;
            ++child_blocks;
            if (body != child_psp) {
                child_extra_paras += (uint32_t)snap[i].size_paras + 1UL;
            }
        }
        if (body >= free_seg_start || is_child) {
            sprintf(
                g_linebuf,
                "  MCB[%s] %c seg=%04X body=%04X owner=%04X size=%u paras (%lu B)%s\n",
                tag,
                (char)snap[i].type,
                (unsigned)snap[i].seg,
                (unsigned)body,
                (unsigned)snap[i].owner,
                (unsigned)snap[i].size_paras,
                (unsigned long)snap[i].size_paras * 16UL,
                is_child ? ((body == child_psp) ? " [CHILD_MAIN]" : " [CHILD_AUX]") : ""
            );
            out_str(g_linebuf);
        }
    }
    sprintf(
        g_linebuf,
        "  MCB_SUM[%s]: child_psp=%04X blocks=%u total_incl_hdr=%lu B aux=%lu B\n",
        tag,
        (unsigned)child_psp,
        (unsigned)child_blocks,
        child_total_paras * 16UL,
        child_extra_paras * 16UL
    );
    out_str(g_linebuf);
}

int main(int argc, char **argv) {
    uint16_t inj_ticks = 0;
    uint16_t inj_key = 0;
    uint32_t limit_kb = 0;
    int quiet = 0;
    int argi = 1;
    const char *exe_path = NULL;
    static char cmd_tail[130];
    static uint8_t fcb1[37];
    static uint8_t fcb2[37];
    exec_pb_t epb;
    uint16_t first_mcb, indos_seg, indos_off;
    uint16_t ballast_seg = 0;
    uint16_t free_mcb_seg = 0, free_paras = 0;
    uint16_t i, exec_rc;
    uint16_t child_psp = 0;

    while (argi < argc && (argv[argi][0] == '/' || argv[argi][0] == '-')) {
        const char *opt = argv[argi] + 1;
        if (stricmp(opt, "KEY:ALTF4") == 0) {
            inj_key = 0x6B00U;
            if (inj_ticks == 0) {
                inj_ticks = 36;
            }
        } else if (stricmp(opt, "KEY:ESC") == 0) {
            inj_key = 0x011BU;
            if (inj_ticks == 0) {
                inj_ticks = 36;
            }
        } else if (strnicmp(opt, "TICKS:", 6) == 0) {
            inj_ticks = (uint16_t)atoi(opt + 6);
        } else if (strnicmp(opt, "LIMIT:", 6) == 0) {
            limit_kb = (uint32_t)atol(opt + 6);
        } else if (stricmp(opt, "QUIET") == 0) {
            quiet = 1;
        } else if (stricmp(opt, "COM2") == 0) {
            g_use_com2 = 1;
        } else {
            break;
        }
        ++argi;
    }

    if (g_use_com2) {
        com2_init();
    }

    if (argi >= argc) {
        out_str("Usage: MEMBENCH [/KEY:ALTF4|ESC] [/TICKS:N] [/LIMIT:KB] [/QUIET] [/COM2] <exe> [args]\n");
        return 2;
    }

    exe_path = argv[argi++];
    {
        char tmp[126];
        size_t pos = 0;
        tmp[0] = '\0';
        while (argi < argc) {
            size_t alen = strlen(argv[argi]);
            if (pos + 1 + alen < 125) {
                tmp[pos++] = ' ';
                memcpy(tmp + pos, argv[argi], alen);
                pos += alen;
            }
            ++argi;
        }
        cmd_tail[0] = (char)pos;
        if (pos > 0) {
            memcpy(cmd_tail + 1, tmp, pos);
        }
        cmd_tail[pos + 1] = '\r';
        cmd_tail[pos + 2] = '\0';
    }

    first_mcb = get_first_mcb();
    get_indos_ptr(&indos_seg, &indos_off);

    g_pre_cnt = snapshot_mcbs_now(first_mcb, g_pre_mcbs);
    for (i = 0; i < g_pre_cnt; ++i) {
        if (g_pre_mcbs[i].owner == 0 && g_pre_mcbs[i].size_paras > free_paras) {
            free_mcb_seg = g_pre_mcbs[i].seg;
            free_paras = g_pre_mcbs[i].size_paras;
        }
    }

    if (limit_kb > 0) {
        uint32_t want_paras = limit_kb * 64UL;
        if (want_paras + 4UL < (uint32_t)free_paras) {
            uint16_t bal_paras = (uint16_t)((uint32_t)free_paras - want_paras - 1UL);
            unsigned seg_out = 0;
            if (_dos_allocmem(bal_paras, &seg_out) == 0) {
                ballast_seg = (uint16_t)seg_out;
                g_pre_cnt = snapshot_mcbs_now(first_mcb, g_pre_mcbs);
                free_mcb_seg = 0;
                free_paras = 0;
                for (i = 0; i < g_pre_cnt; ++i) {
                    if (g_pre_mcbs[i].owner == 0 && g_pre_mcbs[i].size_paras > free_paras) {
                        free_mcb_seg = g_pre_mcbs[i].seg;
                        free_paras = g_pre_mcbs[i].size_paras;
                    }
                }
            }
        }
    }

    sprintf(
        g_linebuf,
        "=== MEMBENCH target=%s limit_kb=%lu free_before=%u paras (%lu B) key=0x%04X ticks=%u ===\n",
        exe_path,
        limit_kb,
        (unsigned)free_paras,
        (unsigned long)free_paras * 16UL,
        (unsigned)inj_key,
        (unsigned)inj_ticks
    );
    out_str(g_linebuf);

    memset(fcb1, 0, sizeof(fcb1));
    memset(fcb2, 0, sizeof(fcb2));
    epb.env_seg = 0;
    epb.cmd_off = FP_OFF(cmd_tail);
    epb.cmd_seg = FP_SEG(cmd_tail);
    epb.fcb1_off = FP_OFF(fcb1);
    epb.fcb1_seg = FP_SEG(fcb1);
    epb.fcb2_off = FP_OFF(fcb2);
    epb.fcb2_seg = FP_SEG(fcb2);

    if (free_mcb_seg != 0 && free_paras > 0) {
        fill_sentinel((uint16_t)(free_mcb_seg + 1U), free_paras);
    }

    mb_install_hooks(first_mcb, indos_seg, indos_off, inj_ticks, inj_key);
    exec_rc = mb_exec_child(exe_path, &epb);
    mb_uninstall_hooks();
    mb_read_stats(&g_stats);

    sprintf(
        g_linebuf,
        "  EXEC_RESULT: rc=0x%04X exit_code=0x%04X ticks=%u inj_sent=%u alloc_events=%u\n",
        (unsigned)exec_rc,
        (unsigned)g_stats.exit_code,
        (unsigned)g_stats.tick_count,
        (unsigned)g_stats.inject_sent,
        (unsigned)g_stats.alloc_log_cnt
    );
    out_str(g_linebuf);

    if (exec_rc == 0) {
        uint16_t free_body = (uint16_t)(free_mcb_seg + 1U);
        if (!quiet) {
            if (g_stats.live_mcb_cnt > 0) {
                dump_mcb_snapshot("LIVE", g_stats.live_mcbs, g_stats.live_mcb_cnt, free_body, &child_psp);
            }
            if (g_stats.exit_mcb_cnt > 0) {
                dump_mcb_snapshot("EXIT", g_stats.exit_mcbs, g_stats.exit_mcb_cnt, free_body, &child_psp);
            }
            for (i = 0; i < g_stats.alloc_log_cnt; ++i) {
                uint8_t ah = (uint8_t)(g_stats.alloc_log[i].ah_cf & 0xFFU);
                uint8_t cf = (uint8_t)((g_stats.alloc_log[i].ah_cf >> 8U) & 1U);
                sprintf(
                    g_linebuf,
                    "  ALLOC_EV[%02u] AH=%02Xh CF=%u BX_in=%04X(%lu B) ES_in=%04X -> AX=%04X BX_out=%04X\n",
                    (unsigned)i,
                    (unsigned)ah,
                    (unsigned)cf,
                    (unsigned)g_stats.alloc_log[i].bx_in,
                    (unsigned long)g_stats.alloc_log[i].bx_in * 16UL,
                    (unsigned)g_stats.alloc_log[i].es_in,
                    (unsigned)g_stats.alloc_log[i].ax_out,
                    (unsigned)g_stats.alloc_log[i].bx_out
                );
                out_str(g_linebuf);
            }
        } else {
            if (g_stats.live_mcb_cnt > 0) {
                child_psp = find_child_psp(g_stats.live_mcbs, g_stats.live_mcb_cnt, free_body);
            }
            if (child_psp == 0 && g_stats.exit_mcb_cnt > 0) {
                child_psp = find_child_psp(g_stats.exit_mcbs, g_stats.exit_mcb_cnt, free_body);
            }
        }
        if (free_mcb_seg != 0 && free_paras > 0) {
            scan_dirty_region(free_body, free_paras, child_psp, quiet);
        }
    }

    if (ballast_seg != 0) {
        _dos_freemem(ballast_seg);
    }

    if (exec_rc != 0) {
        return 10;
    }
    if (g_stats.exit_code != 0 && g_stats.exit_code != 0xFFFFU) {
        return (int)(g_stats.exit_code & 0xFFU);
    }
    return 0;
}
