/* cleanroom/dos/dos_sys_rt.c
 *
 * Minimal freestanding Zero-CRT memory/string primitives (memset, memcpy,
 * memcmp, strcmp) linked into the 16-bit Real-Mode DOS targets (OPENLNK.SYS,
 * OPENSVR.EXE, OPENLNK.EXE) so Open Watcom produces 100% self-contained
 * binaries with zero clibs.lib usage.
 *
 * Strictly ISO C99, zero dynamic allocation, zero floating-point, 8086 safe.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int ch, size_t len) {
    uint8_t *p = (uint8_t *)dst;
    uint8_t val = (uint8_t)ch;
    size_t i;
    for (i = 0U; i < len; ++i) {
        p[i] = val;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0U; i < len; ++i) {
        d[i] = s[i];
    }
    return dst;
}

int memcmp(const void *lhs, const void *rhs, size_t len) {
    const uint8_t *a = (const uint8_t *)lhs;
    const uint8_t *b = (const uint8_t *)rhs;
    size_t i;
    for (i = 0U; i < len; ++i) {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}

int strcmp(const char *lhs, const char *rhs) {
    const uint8_t *a = (const uint8_t *)lhs;
    const uint8_t *b = (const uint8_t *)rhs;
    while ((*a != 0U) && (*a == *b)) {
        ++a;
        ++b;
    }
    if (*a == *b) {
        return 0;
    }
    return (*a < *b) ? -1 : 1;
}
