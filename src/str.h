/* String helpers for code with no libc: the client (DR exports none to clients; calling one dies at first use) and the
 * static launcher. Freestanding: nothing here may include a libc or DR header. */
#ifndef V3COMPAT_STR_H
#define V3COMPAT_STR_H
#include <stdbool.h>
#include <stddef.h>

static inline size_t slen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline bool starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}

/* Copies s without its terminator; returns the end. */
static inline char *scpy(char *d, const char *s) {
    while (*s) *d++ = *s++;
    return d;
}

/* Writes v in decimal, no terminator; returns the end. */
static inline char *put_dec(char *p, long v) {
    char t[24];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

#endif
