/*
 * heap.h - Kernel Heap Manager
 *
 * Grows by allocating arenas from the VMM. Segregated size bins, boundary tag coalescing,
 * magic numbers and red zones for corruption detection.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <kernel/memory/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>



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

#pragma region Internals (shared with the white box tests)

// Magic numbers for validation
#define HEAP_MAGIC 0xF005BA11
#define ARENA_MAGIC 0x1CEB00DA
#define BLOCK_MAGIC_USED 0xABADCAFE
#define BLOCK_MAGIC_FREE 0xA110CA7E
#define BLOCK_RED_ZONE 0x8BADF00D

// Minimum allocation alignment (must be power of 2) and block size
#define HEAP_MIN_ALIGN 16
#define MIN_BLOCK_SIZE 32

// Arena management
#define MIN_ARENA_SIZE (64 * 1024)
#define HEAP_SHRINK_THRESHOLD 4

// Segregated free list bins
#define HEAP_BIN_COUNT 16

typedef struct arena arena_t;
typedef struct blk_hdr blk_hdr_t;
typedef struct heap heap_t;

// Block header (placed before user data)
struct blk_hdr {
    uint32_t magic;
    uint32_t red_zone_pre;
    size_t size;
    size_t total_size;
    arena_t* arena;
    struct blk_hdr* next_free;
    struct blk_hdr* prev_free;
    uint32_t red_zone_post;
} __attribute__((aligned(16)));

// Block footer (placed after user data)
typedef struct {
    uint32_t red_zone_pre;
    blk_hdr_t* header;
    uint32_t magic;
    uint32_t red_zone_post;
} __attribute__((aligned(16))) blk_foot_t;

// Arena, one contiguous region carved into blocks. [start, end) holds the blocks
struct arena {
    uint32_t magic;
    arena_t* next;
    arena_t* prev;

    uintptr_t start;
    uintptr_t end;
    size_t size;

    blk_hdr_t* first_block;

    size_t total_free;
    size_t total_allocated;
};

// Heap, a list of arenas and the size bins of their free blocks
struct heap {
    uint32_t magic;
    arena_t* arenas;
    blk_hdr_t* bins[HEAP_BIN_COUNT];
    size_t current_size;
    size_t total_allocated;
    size_t total_free;
    size_t allocation_count;
    size_t arena_count;
};

#pragma endregion

// Kernel heap interface

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