/*
 * mem.c - Basic heap memory management for GatOS
 *
 * This file provides a simple bump allocator implementation for dynamic memory
 * allocation within the kernel. The allocator assumes a flat memory model and 
 * uses linker-defined symbols to locate the end of the BSS section as the starting 
 * point of the heap.
 * 
 * Author: u/ApparentlyPlus
 */

#include "mem.h"

static char* heap_start = 0;
static char* heap_end = 0;
static char* current_brk = 0;

/*
 * heap_init - Initializes the heap for dynamic allocation
 * @heap_size: Number of bytes to reserve for heap memory
 *
 * Sets the start and end of the heap based on the BSS section end symbol.
 */
void heap_init(size_t heap_size) {
	heap_start = &__bss_end; // Start of the heap is right after the BSS section
	heap_end   = heap_start + heap_size;
	current_brk = heap_start;
}

/*
 * malloc - Allocates a block of memory from the heap
 * @size: Number of bytes to allocate
 *
 * Returns a pointer to the allocated memory, or NULL if out of heap space.
 * Simple bump allocator (no deallocation or reuse yet).
 */
void* malloc(size_t size) {
	if (current_brk + size > heap_end) {
		// Out of memory
		return NULL;
	}

	void* ptr = current_brk;
	current_brk += size;
	return ptr;
}

/*
 * free - Placeholder for memory deallocation
 * @ptr: Pointer to memory previously allocated by malloc
 *
 * Currently does nothing. Will be implemented in the future.
 */
void free(void* ptr) {
	// No-op for now, I'll implement this soon.
}
