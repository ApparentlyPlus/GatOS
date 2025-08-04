/*
 * string.h - Standard C string and memory operations
 *
 * Declares common string manipulation and memory handling functions
 * compliant with standard C library specifications.
 *
 * Author: u/ApparentlyPlus
 */
 
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

#endif