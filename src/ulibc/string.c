/*
 * string.c - Standard C string implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <ulibc/string.h>
#include <stdint.h>

void* memset(void *dest, int c, size_t n) {
    void* d = dest;
    uint64_t fill = (uint8_t)c;
    fill |= fill << 8; fill |= fill << 16; fill |= fill << 32;
    size_t q = n >> 3;
    size_t r = n & 7;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(fill) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(r) : "a"(fill) : "memory");
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    void* d = dest;
    const void* s = src;
    size_t q = n >> 3;
    size_t r = n & 7;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(r) :: "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dest;
    if (d < s) {
        return memcpy(dest, src, n);
    } else {
        uint8_t* d8 = d + n;
        const uint8_t* s8 = s + n;
        size_t r = n & 7;
        while (r--) *(--d8) = *(--s8);
        size_t q = n >> 3;
        uint64_t* dq = (uint64_t*)d8;
        const uint64_t* sq = (const uint64_t*)s8;
        while (q--) *(--dq) = *(--sq);
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = s1, *b = s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (*str++) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*src != '\0')) {
        *d++ = *src++;
        n--;
    }
    while (n--) *d++ = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)((c == 0) ? s : last);
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*src != '\0')) *d++ = *src++;
    *d = '\0';
    return dest;
}

bool isspace(int c) {
    return (c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v');
}

bool isdigit(int c) {
    return (c >= '0' && c <= '9');
}

/*
 * digit_val - Value of a digit in bases up to 36, or 99 for anything that isn't one
 */
static int digit_val(int c) {
    if (isdigit(c)) return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/*
 * parse_ul - Shared core of strtoul and strtol. Returns the magnitude, sets *neg for a leading
 * minus and *ovf if the number did not fit, and points *endptr past the last digit it used
 * (or back at nptr if there were none, which is how callers tell "0" from "no number")
 */
static unsigned long parse_ul(const char *nptr, char **endptr, int base, bool *neg, bool *ovf) {
    const char *s = nptr;
    unsigned long acc = 0;
    bool any = false;
    *neg = false;
    *ovf = false;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-' || *s == '+') {
        *neg = (*s == '-');
        s++;
    }

    // A 0x prefix only counts if a hex digit follows it. In "0x" or "0xg", the 0 is the whole number
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && digit_val((unsigned char)s[2]) < 16) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    for (;; s++) {
        int d = digit_val((unsigned char)*s);
        if (d >= base) break;
        if (acc > (~0UL - (unsigned long)d) / (unsigned long)base) *ovf = true;
        else acc = acc * base + d;
        any = true;
    }

    if (endptr) *endptr = (char *)(any ? s : nptr);
    return acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    bool neg, ovf;
    unsigned long v = parse_ul(nptr, endptr, base, &neg, &ovf);
    if (ovf) return ~0UL;
    return neg ? -v : v; // the standard really does say to negate an unsigned result
}

long strtol(const char *nptr, char **endptr, int base) {
    bool neg, ovf;
    unsigned long v = parse_ul(nptr, endptr, base, &neg, &ovf);
    unsigned long lim = (unsigned long)__LONG_MAX__;

    if (neg) {
        if (ovf || v > lim + 1) return -__LONG_MAX__ - 1;
        return (v == lim + 1) ? -__LONG_MAX__ - 1 : -(long)v;
    }
    return (ovf || v > lim) ? __LONG_MAX__ : (long)v;
}
