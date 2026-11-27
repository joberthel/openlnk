/* cleanroom/dos/dos_hal_io.h
 *
 * Clean-room zero-allocation MS-DOS INT 21h Console & File I/O HAL plus
 * minimal freestanding ASCII helpers for 16-bit Real-Mode OPENSVR.EXE and
 * OPENLNK.EXE (eliminates Open Watcom clibs.lib stdio/ctype/conio bloat).
 *
 * Under Open Watcom 16-bit DOS (__WATCOMC__ && __DOS__):
 *   - Console output uses INT 21h / AH=40h on Handle 1 (STDOUT), preserving
 *     standard MS-DOS shell redirection (e.g., OPENLNK /DIR > LIST.TXT).
 *   - File creation/write/close uses INT 21h / AH=3Ch, 40h, 3Eh directly.
 *   - Non-blocking keyboard poll uses INT 21h / AH=0Bh and AH=07h.
 *
 * On non-DOS hosts, falls back cleanly to ISO C99 <stdio.h> (with an optional
 * stdout capture sink for deterministic unit testing).
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point,
 * 8086/8088 safe.
 */
#ifndef OPENLNK_DOS_HAL_IO_H
#define OPENLNK_DOS_HAL_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
#if defined(__WATCOMC__) && defined(__DOS__)
    uint16_t handle;
    bool is_open;
#else
    void *fp;
#endif
} olnk_dos_file_t;

/* Minimal freestanding ASCII helpers (zero <ctype.h> / istable.c dependency) */
bool olnk_ascii_isalpha(char ch);
char olnk_ascii_toupper(char ch);
bool olnk_ascii_ieq_prefix(const char *s, const char *prefix);
uint32_t olnk_ascii_parse_u32(const char *s);

/**
 * Tokenize an in-place NUL/CR-terminated MS-DOS PSP command tail into argv[].
 * Sets argv[0] = "", argv[1..argc-1] = parsed arguments (spaces, tabs, and
 * optional double quotes), and argv[argc] = NULL. Zero heap allocation.
 */
int olnk_dos_parse_cmdline(char *cmd_tail, char **argv, int max_argv);

/* On 16-bit Watcom DOS EXEs, bind entry point to olnk_dos_main (invoked by
 * dos_exe_crt0.asm -> _olnk_dos_exe_run) so wcc does not emit EXTRN _cstart_. */
#if defined(__WATCOMC__) && defined(__DOS__)
int olnk_dos_main(int argc, char **argv);
#define OLNK_DOS_MAIN_FN olnk_dos_main
#else
#define OLNK_DOS_MAIN_FN main
#endif

/* Zero-stdio console output (INT 21h AH=40h Handle 1 on DOS; stdout on host) */
void olnk_dos_putc(char ch);
void olnk_dos_puts(const char *s);
void olnk_dos_put_u32(uint32_t val);
void olnk_dos_put_hex16(uint16_t val);
void olnk_dos_put_hex32(uint32_t val);
void olnk_dos_put_str_pad(const char *s, size_t width);

/* Zero-conio keyboard poll (INT 21h AH=0Bh / AH=07h on DOS) */
bool olnk_dos_kbhit(void);
uint8_t olnk_dos_getch(void);

/* Zero-stdio file output for OPENLNK /GET (INT 21h AH=3Ch / 40h / 3Eh on DOS) */
bool olnk_dos_file_create(olnk_dos_file_t *file, const char *path);
size_t olnk_dos_file_write(olnk_dos_file_t *file, const void *buf, size_t len);
bool olnk_dos_file_close(olnk_dos_file_t *file);

#if !defined(__WATCOMC__) || !defined(__DOS__)
typedef void (*olnk_dos_stdout_sink_t)(const char *data, size_t len, void *ctx);

/**
 * Host-test hook: redirect olnk_dos_put* console output into a test callback.
 * Pass sink = NULL to restore default stdout output.
 */
void olnk_dos_io_set_stdout_sink(olnk_dos_stdout_sink_t sink, void *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_DOS_HAL_IO_H */
