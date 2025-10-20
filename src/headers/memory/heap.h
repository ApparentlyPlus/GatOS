/*
 * heap.h - Kernel Heap Manager
 *
 * Provides a comprehensive heap allocation system for both kernel and userspace.
 * Uses boundary tag coalescing for efficient memory management and maintains
 * extensive metadata for debugging and validation.
 *
 * The heap automatically expands when needed by allocating virtual memory from
 * the VMM. It maintains a sorted free list organized by size for efficient
 * best-fit allocation. Blocks include magic numbers and red zones for corruption
 * detection.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <memory/vmm.h>

// Minimum allocation alignment (must be power of 2)
#define HEAP_MIN_ALIGN 16

// Minimum heap size (in bytes)
#define HEAP_MIN_SIZE (64 * 1024)  // 64KB

// Heap flags
#define HEAP_FLAG_NONE      0
#define HEAP_FLAG_ZERO      (1 << 0)  // Zero memory on allocation
#define HEAP_FLAG_URGENT    (1 << 1)  // Don't fail, panic instead

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
typedef struct heap_block_header heap_block_header_t;

// Kernel heap interface (auto-initialized on first use)

heap_status_t heap_kernel_init(void);
heap_t* heap_kernel_get(void);
void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t size);
void* kcalloc(size_t nmemb, size_t size);

// User heap interface

heap_t* heap_create(vmm_t* vmm, size_t min_size, size_t max_size, uint32_t flags);
void heap_destroy(heap_t* heap);
void* heap_malloc(heap_t* heap, size_t size);
void heap_free(heap_t* heap, void* ptr);
void* heap_realloc(heap_t* heap, void* ptr, size_t size);
void* heap_calloc(heap_t* heap, size_t nmemb, size_t size);

// Heap introspection and debugging

heap_status_t heap_check_integrity(heap_t* heap);
void heap_dump(heap_t* heap);
void heap_stats(heap_t* heap, size_t* total, size_t* used, size_t* free, size_t* overhead);
size_t heap_get_alloc_size(heap_t* heap, void* ptr);

// Utility functions

size_t heap_align_size(size_t size);
bool heap_validate_block(heap_block_header_t* header);

/*

Notes on improving Heap in the future:

1. To catch use-after-free bugs, wed'd need to:

- Poison freed memory - Fill the user data area with a recognizable pattern (like 0xDD) on free
- Check for poison pattern - Verify the pattern is intact when the block is reallocated
- Use guard pages - Unmap freed pages to cause a page fault on access (expensive but catches bugs immediately)
- Track allocation/free backtraces - Store where allocations came from to help debug double-frees

2. We should really reconsider our heap panic philosophy:

Panic when:
* Heap metadata is corrupted (you can't trust anything anymore)
* Double-free detected (indicates serious bug)
* Use-after-free confirmed (memory safety violation)
* Free list or arena chain is broken (heap is unrecoverable)
* Critical allocation fails with URGENT flag set

Return NULL/error when:
* Out of memory in normal allocation (caller should handle)
* Invalid user input (NULL pointers, zero sizes)
* Heap limits reached gracefully

The heap allocator is a trust boundary:

- If its internal structures are corrupted, the entire kernel is compromised. 
- Panicking early and loudly is much better than silently propagating corruption 
            that manifests as a mysterious crash miles away from the actual bug.
*/