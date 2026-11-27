/* cleanroom/dos/dos_hal_io.c
 *
 * Clean-room zero-allocation MS-DOS INT 21h Console, Keyboard & File I/O HAL.
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point,
 * 8086/8088 safe.
 */
#include "dos_hal_io.h"

#if !defined(__WATCOMC__) || !defined(__DOS__)
#include <stdio.h>
#endif

#if defined(__WATCOMC__) && defined(__DOS__)

#define OLNK_DOS_STDOUT_HANDLE 1U
#define OLNK_DOS_INVALID_HND   0xFFFFU
#define OLNK_DOS_WRITE_CHUNK   32768U

/*
 * Branchless, label-free 8086 MS-DOS INT 21h primitives (Small Model: DS=DGROUP):
 *   CF=0 -> AX = handle / bytes written; CF=1 -> AX = 0xFFFF.
 */
static uint16_t watcom_dos_create(const char *path);
#pragma aux watcom_dos_create = \
    "mov ah, 3ch"               \
    "xor cx, cx"                \
    "int 21h"                   \
    "sbb dx, dx"                \
    "or ax, dx"                 \
    parm [dx]                   \
    value [ax]                  \
    modify [ax cx dx];

static uint16_t watcom_dos_write(uint16_t handle, const void *buf, uint16_t count);
#pragma aux watcom_dos_write = \
    "mov ah, 40h"              \
    "int 21h"                  \
    "sbb dx, dx"               \
    "or ax, dx"                \
    parm [bx] [dx] [cx]        \
    value [ax]                 \
    modify [ax dx];

static uint8_t watcom_dos_close(uint16_t handle);
#pragma aux watcom_dos_close = \
    "mov ah, 3eh"              \
    "int 21h"                  \
    "sbb ax, ax"               \
    "inc ax"                   \
    parm [bx]                  \
    value [al]                 \
    modify [ax];

static uint8_t watcom_dos_kbhit_raw(void);
#pragma aux watcom_dos_kbhit_raw = \
    "mov ah, 0bh"                  \
    "int 21h"                      \
    value [al]                     \
    modify [ax];

static uint8_t watcom_dos_getch_raw(void);
#pragma aux watcom_dos_getch_raw = \
    "mov ah, 07h"                  \
    "int 21h"                      \
    value [al]                     \
    modify [ax];

#else

static olnk_dos_stdout_sink_t g_stdout_sink = NULL;
static void *g_stdout_sink_ctx = NULL;

void olnk_dos_io_set_stdout_sink(olnk_dos_stdout_sink_t sink, void *ctx) {
    g_stdout_sink = sink;
    g_stdout_sink_ctx = ctx;
}

#endif /* __WATCOMC__ && __DOS__ */

static void dos_stdout_raw(const char *data, size_t len) {
    if ((data == NULL) || (len == 0U)) {
        return;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        size_t sent = 0U;
        while (sent < len) {
            size_t rem = len - sent;
            uint16_t chunk = (rem > (size_t)OLNK_DOS_WRITE_CHUNK)
                                 ? (uint16_t)OLNK_DOS_WRITE_CHUNK
                                 : (uint16_t)rem;
            uint16_t res = watcom_dos_write(OLNK_DOS_STDOUT_HANDLE, &data[sent], chunk);
            if ((res == OLNK_DOS_INVALID_HND) || (res == 0U)) {
                break;
            }
            sent += (size_t)res;
        }
    }
#else
    if (g_stdout_sink != NULL) {
        g_stdout_sink(data, len, g_stdout_sink_ctx);
    } else {
        if (fwrite(data, 1U, len, stdout) != len) {
            clearerr(stdout);
        }
    }
#endif
}

/* =========================================================================
 * Minimal Freestanding ASCII Helpers
 * ========================================================================= */

bool olnk_ascii_isalpha(char ch) {
    return ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z'));
}

char olnk_ascii_toupper(char ch) {
    if ((ch >= 'a') && (ch <= 'z')) {
        return (char)(ch - ('a' - 'A'));
    }
    return ch;
}

bool olnk_ascii_ieq_prefix(const char *s, const char *prefix) {
    if ((s == NULL) || (prefix == NULL)) {
        return false;
    }
    while (*prefix != '\0') {
        if (olnk_ascii_toupper(*s) != olnk_ascii_toupper(*prefix)) {
            return false;
        }
        ++s;
        ++prefix;
    }
    return true;
}

uint32_t olnk_ascii_parse_u32(const char *s) {
    uint32_t val = 0UL;
    if (s == NULL) {
        return 0UL;
    }
    while ((*s >= '0') && (*s <= '9')) {
        val = (val * 10UL) + (uint32_t)(*s - '0');
        ++s;
    }
    return val;
}

/* =========================================================================
 * Console Output Helpers (INT 21h AH=40h Handle 1 on DOS)
 * ========================================================================= */

void olnk_dos_putc(char ch) {
#if defined(__WATCOMC__) && defined(__DOS__)
    if (ch == '\n') {
        dos_stdout_raw("\r\n", 2U);
        return;
    }
#endif
    dos_stdout_raw(&ch, 1U);
}

void olnk_dos_puts(const char *s) {
    if (s == NULL) {
        return;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        const char *span = s;
        while (*s != '\0') {
            if (*s == '\n') {
                if (s > span) {
                    dos_stdout_raw(span, (size_t)(s - span));
                }
                dos_stdout_raw("\r\n", 2U);
                ++s;
                span = s;
            } else {
                ++s;
            }
        }
        if (s > span) {
            dos_stdout_raw(span, (size_t)(s - span));
        }
    }
#else
    {
        size_t len = 0U;
        while (s[len] != '\0') {
            ++len;
        }
        dos_stdout_raw(s, len);
    }
#endif
}

void olnk_dos_put_u32(uint32_t val) {
    char buf[12];
    size_t pos = sizeof(buf);
    do {
        --pos;
        buf[pos] = (char)('0' + (char)(val % 10UL));
        val /= 10UL;
    } while ((val > 0UL) && (pos > 0U));
    dos_stdout_raw(&buf[pos], sizeof(buf) - pos);
}

static void dos_put_hex_n(uint32_t val, uint8_t nibbles) {
    static const char hex_digits[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
    };
    char buf[8];
    uint8_t i;

    if (nibbles > 8U) {
        nibbles = 8U;
    }
    for (i = 0U; i < nibbles; ++i) {
        uint8_t shift = (uint8_t)((uint8_t)(nibbles - 1U - i) * 4U);
        buf[i] = hex_digits[(size_t)((val >> shift) & 0x0FUL)];
    }
    dos_stdout_raw(buf, (size_t)nibbles);
}

void olnk_dos_put_hex16(uint16_t val) {
    dos_put_hex_n((uint32_t)val, 4U);
}

void olnk_dos_put_hex32(uint32_t val) {
    dos_put_hex_n(val, 8U);
}

void olnk_dos_put_str_pad(const char *s, size_t width) {
    static const char spaces[16] = {
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    size_t len = 0U;

    if (s != NULL) {
        while (s[len] != '\0') {
            ++len;
        }
        if (len > 0U) {
            dos_stdout_raw(s, len);
        }
    }
    while (len < width) {
        size_t pad = width - len;
        if (pad > sizeof(spaces)) {
            pad = sizeof(spaces);
        }
        dos_stdout_raw(spaces, pad);
        len += pad;
    }
}

/* =========================================================================
 * Keyboard Poll Helpers (INT 21h AH=0Bh / AH=07h on DOS)
 * ========================================================================= */

bool olnk_dos_kbhit(void) {
#if defined(__WATCOMC__) && defined(__DOS__)
    return watcom_dos_kbhit_raw() != 0U;
#else
    return false;
#endif
}

uint8_t olnk_dos_getch(void) {
#if defined(__WATCOMC__) && defined(__DOS__)
    return watcom_dos_getch_raw();
#else
    return 0U;
#endif
}

/* =========================================================================
 * File Output Helpers (INT 21h AH=3Ch / 40h / 3Eh on DOS)
 * ========================================================================= */

bool olnk_dos_file_create(olnk_dos_file_t *file, const char *path) {
    if ((file == NULL) || (path == NULL)) {
        return false;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        uint16_t h = watcom_dos_create(path);
        if (h == OLNK_DOS_INVALID_HND) {
            file->handle = OLNK_DOS_INVALID_HND;
            file->is_open = false;
            return false;
        }
        file->handle = h;
        file->is_open = true;
        return true;
    }
#else
    file->fp = fopen(path, "wb");
    return file->fp != NULL;
#endif
}

size_t olnk_dos_file_write(olnk_dos_file_t *file, const void *buf, size_t len) {
    if ((file == NULL) || ((buf == NULL) && (len > 0U))) {
        return 0U;
    }
    if (len == 0U) {
        return 0U;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        const uint8_t *p = (const uint8_t *)buf;
        size_t written = 0U;
        if (!file->is_open) {
            return 0U;
        }
        while (written < len) {
            size_t rem = len - written;
            uint16_t chunk = (rem > (size_t)OLNK_DOS_WRITE_CHUNK)
                                 ? (uint16_t)OLNK_DOS_WRITE_CHUNK
                                 : (uint16_t)rem;
            uint16_t res = watcom_dos_write(file->handle, &p[written], chunk);
            if ((res == OLNK_DOS_INVALID_HND) || (res == 0U)) {
                break;
            }
            written += (size_t)res;
            if (res < chunk) {
                break;
            }
        }
        return written;
    }
#else
    if (file->fp == NULL) {
        return 0U;
    }
    return fwrite(buf, 1U, len, (FILE *)file->fp);
#endif
}

bool olnk_dos_file_close(olnk_dos_file_t *file) {
    if (file == NULL) {
        return false;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    if (!file->is_open) {
        return false;
    }
    file->is_open = false;
    return watcom_dos_close(file->handle) != 0U;
#else
    {
        FILE *fp = (FILE *)file->fp;
        if (fp == NULL) {
            return false;
        }
        file->fp = NULL;
        return fclose(fp) == 0;
    }
#endif
}

int olnk_dos_parse_cmdline(char *cmd_tail, char **argv, int max_argv) {
    static char s_empty_prog[] = "";
    int argc = 0;
    char *r = cmd_tail;

    if ((argv == NULL) || (max_argv <= 0)) {
        return 0;
    }
    if (max_argv == 1) {
        argv[0] = NULL;
        return 0;
    }

    argv[argc++] = s_empty_prog;
    if (r == NULL) {
        argv[argc] = NULL;
        return argc;
    }

    while ((*r != '\0') && (*r != '\r') && (*r != '\n') && (argc < (max_argv - 1))) {
        char *w;
        bool in_quote = false;

        while ((*r == ' ') || (*r == '\t')) {
            ++r;
        }
        if ((*r == '\0') || (*r == '\r') || (*r == '\n')) {
            break;
        }

        argv[argc++] = r;
        w = r;
        while ((*r != '\0') && (*r != '\r') && (*r != '\n')) {
            if (*r == '"') {
                in_quote = !in_quote;
                ++r;
            } else if (!in_quote && ((*r == ' ') || (*r == '\t'))) {
                ++r;
                break;
            } else {
                *w++ = *r++;
            }
        }
        *w = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

#if defined(__WATCOMC__) && defined(__DOS__)
int olnk_dos_exe_run(char *cmd_tail);
#pragma aux olnk_dos_exe_run "_olnk_dos_exe_run" parm [ax] value [ax]

int olnk_dos_exe_run(char *cmd_tail) {
    static char *s_dos_argv[33];
    int argc = olnk_dos_parse_cmdline(cmd_tail, s_dos_argv, 33);
    return olnk_dos_main(argc, s_dos_argv);
}
#endif
