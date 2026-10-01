/*
 * heap.h - Kernel Heap Manager
 *
 * Grows by allocating from the VMM. Free list sorted by size (best fit), boundary tag
 * coalescing, magic numbers and red zones for corruption detection.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <kernel/memory/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Minimum allocation alignment (must be power of 2)
#define HEAP_MIN_ALIGN 16

// Minimum heap size (in bytes)
#define HEAP_MIN_SIZE (64 * 1024)  // 64KB

// Heap flags
#define HEAP_FLAG_NONE      0
#define HEAP_FLAG_ZERO      (1 << 0)  // Zero memory on allocation
#define HEAP_FLAG_URGENT    (1 << 1)  // Don't fail, panic instead
#define HEAP_FLAG_EXECUTABLE (1 << 2) // Memory is executable

// Return codes
typedef enum {
    HEAP_OK = 0,
    HEAP_ERR_INVALID,       // Invalid arguments 
    HEAP_ERR_OOM,           // Out of memory
    HEAP_ERR_NOT_INIT,      // Heap not initialized
    HEAP_ERR_ALREADY_INIT,  // Heap already initialized
    HEAP_ERR_VMM_FAIL,      // Something went wrong with the VMM
    HEAP_ERR_CORRUPTED,     // Heap corruption detected
} heap_status_t;

// Forward declarations

typedef struct heap heap_t;
typedef struct blk_hdr blk_hdr_t;

// Kernel heap interface (auto initialized on first use)

heap_status_t heap_kernel_init(void);
heap_t* heap_kernel_get(void);
void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t size);
void* kcalloc(size_t nmemb, size_t size);

// Heap introspection and debugging

heap_status_t heap_check(heap_t* heap);
void heap_dump(heap_t* heap);
void heap_stats(heap_t* heap, size_t* total, size_t* used, size_t* free, size_t* overhead);
size_t heap_alloc_sz(heap_t* heap, void* ptr);

// Utility functions

size_t heap_align_size(size_t size);
bool heap_validate_blk(blk_hdr_t* header);

/*
 * Notes on improving the heap in the future:
 *
 * - Use-after-free detection: poison freed memory and check it on realloc, guard pages for
 *   freed pages, alloc/free backtraces for double frees.
 * - Panic when the heap itself is broken (corrupted metadata, double free, broken free list
 *   or arena chain, urgent alloc failing). Return NULL on plain OOM, bad input or hitting the
 *   heap limits. The heap is a trust boundary: if its structures are corrupt the whole kernel
 *   is suspect, so fail early and loud instead of letting it show up as a mystery crash
 *   somewhere else.
 */