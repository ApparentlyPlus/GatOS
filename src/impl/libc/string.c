/*
 * string.c - Standard C string implementation
 *
 * Implements memory and string manipulation functions compliant
 * with standard C library specifications.
 *
 * Author: u/ApparentlyPlus
 */

#include "libc/string.h"

/*
 * memset - Fills memory with constant byte
 */
void* memset(void *dest, int c, size_t n) {
    unsigned char *p = dest;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return dest;
}

/*
 * memcpy - Copies memory between non-overlapping regions
 */
void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/*
 * memmove - Copies memory between potentially overlapping regions
 */
void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *(--d) = *(--s);
    }
    return dest;
}

/*
 * memcmp - Compares two memory regions
 */
int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = s1, *b = s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

/*
 * strlen - Calculates length of null-terminated string
 */
size_t strlen(const char *str) {
    size_t len = 0;
    while (*str++) len++;
    return len;
}

/*
 * strcpy - Copies null-terminated string
 */
char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

/*
 * strncpy - Copies fixed-length string
 */
char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*src != '\0')) {
        *d++ = *src++;
        n--;
    }
    while (n--) *d++ = '\0';
    return dest;
}

/*
 * strcmp - Compares two null-terminated strings
 */
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/*
 * strncmp - Compares fixed-length strings
 */
int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/*
 * strchr - Finds first occurrence of character in string
 */
char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

/*
 * strrchr - Finds last occurrence of character in string
 */
char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)((c == 0) ? s : last);
}

/*
 * strcat - Concatenates two strings
 */
char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

/*
 * strncat - Concatenates fixed-length strings
 */
char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*src != '\0')) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}