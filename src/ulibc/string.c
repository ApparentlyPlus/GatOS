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

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;
    int any = 0;

    while (isspace((unsigned char)*s)) s++;

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
        if (isdigit(c)) c -= '0';
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

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc;
    int neg = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') s++;

    acc = strtoul(s, endptr, base);
    return neg ? -(long)acc : (long)acc;
}
