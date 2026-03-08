/*
 * string.h - Standard C string and memory operations
 */
 
#pragma once

#include <stddef.h>
#include <stdbool.h>

void *kmemset(void *dest, int c, size_t n);
void *kmemcpy(void *dest, const void *src, size_t n);
void *kmemmove(void *dest, const void *src, size_t n);
int kmemcmp(const void *s1, const void *s2, size_t n);

size_t kstrlen(const char *str);
char *kstrcpy(char *dest, const char *src);
char *kstrncpy(char *dest, const char *src, size_t n);
int kstrcmp(const char *s1, const char *s2);
int kstrncmp(const char *s1, const char *s2, size_t n);
char *kstrchr(const char *s, int c);
char *kstrrchr(const char *s, int c);
char *kstrcat(char *dest, const char *src);
char *kstrncat(char *dest, const char *src, size_t n);

// Helpers for scanf
bool kisspace(int c);
bool kisdigit(int c);
long kstrtol(const char *nptr, char **endptr, int base);
unsigned long kstrtoul(const char *nptr, char **endptr, int base);
