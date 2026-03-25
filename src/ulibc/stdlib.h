/*
 * stdlib.h - Subset of the C standard library for GatOS
 *
 * Author: Claude Code
 */

#pragma once

#include <stddef.h>

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
