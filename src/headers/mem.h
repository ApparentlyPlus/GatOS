#pragma once
#include <stddef.h>

extern char __bss_end; // Heap starts here
extern char __stack_top;
extern char __stack_bottom;

void* malloc(size_t size);
void  free(void* ptr);
void  heap_init(size_t heap_size);