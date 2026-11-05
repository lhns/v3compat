/* DR exports no libc string functions to clients (calling one dies at first use). */
#ifndef V3COMPAT_STR_H
#define V3COMPAT_STR_H
#include "dr_api.h"

static inline bool starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}

#endif
