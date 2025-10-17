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
    HEAP_ERR_INVALID,
    HEAP_ERR_OOM,
    HEAP_ERR_NOT_INIT,
    HEAP_ERR_ALREADY_INIT,
    HEAP_ERR_VMM_FAIL,
    HEAP_ERR_CORRUPTED,
    HEAP_ERR_NOT_FOUND,
    HEAP_ERR_DOUBLE_FREE,
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