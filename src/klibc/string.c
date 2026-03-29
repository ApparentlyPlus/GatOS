/*
 * string.c - Standard C string implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <klibc/string.h>
#include <stdint.h>

void* kmemset(void *dest, int c, size_t n) {
    uint8_t* p = (uint8_t*)dest;
    while (n && ((uintptr_t)p & 7)) { *p++ = (uint8_t)c; n--; }
    uint64_t fill = (uint8_t)c;
    fill |= fill << 8; fill |= fill << 16; fill |= fill << 32;
    uint64_t* q = (uint64_t*)p;
    size_t words = n / 8; n &= 7;
    while (words--) *q++ = fill;
    p = (uint8_t*)q;
    while (n--) *p++ = (uint8_t)c;
    return dest;
}

void *kmemcpy(void *dest, const void *src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
    uint64_t* dq = (uint64_t*)d;
    const uint64_t* sq = (const uint64_t*)s;
    size_t words = n / 8; n &= 7;
    while (words--) *dq++ = *sq++;
    d = (uint8_t*)dq; s = (const uint8_t*)sq;
    while (n--) *d++ = *s++;
    return dest;
}

void *kmemmove(void *dest, const void *src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dest;
    if (d < s) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        uint64_t* dq = (uint64_t*)d; const uint64_t* sq = (const uint64_t*)s;
        size_t words = n / 8; n &= 7;
        while (words--) *dq++ = *sq++;
        d = (uint8_t*)dq; s = (const uint8_t*)sq;
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *(--d) = *(--s);
    }
    return dest;
}

int kmemcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = s1, *b = s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

size_t kstrlen(const char *str) {
    size_t len = 0;
    while (*str++) len++;
    return len;
}

char *kstrcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *kstrncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*src != '\0')) {
        *d++ = *src++;
        n--;
    }
    while (n--) *d++ = '\0';
    return dest;
}

int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int kstrncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *kstrchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *kstrrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)((c == 0) ? s : last);
}

char *kstrcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *kstrncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*src != '\0')) *d++ = *src++;
    *d = '\0';
    return dest;
}

bool kisspace(int c) {
    return (c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v');
}

bool kisdigit(int c) {
    return (c >= '0' && c <= '9');
}

unsigned long kstrtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    int any = 0;

    while (kisspace((unsigned char)*s)) s++;

    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') {
                s++;
                base = 16;
            } else base = 8;
        } else base = 10;
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    }

    for (;; s++) {
        int c = (unsigned char)*s;
        if (kisdigit(c)) c -= '0';
        else if (c >= 'A' && c <= 'Z') c -= 'A' - 10;
        else if (c >= 'a' && c <= 'z') c -= 'a' - 10;
        else break;
        if (c >= base) break;
        acc = acc * base + c;
        any = 1;
    }

    if (endptr != 0) *endptr = (char *)(any ? s : nptr);
    return acc;
}

long kstrtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc;
    int neg = 0;

    while (kisspace((unsigned char)*s)) s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') s++;

    acc = kstrtoul(s, endptr, base);
    return neg ? -(long)acc : (long)acc;
}
