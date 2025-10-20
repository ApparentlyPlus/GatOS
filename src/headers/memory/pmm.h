/*
 * pmm.h - Physical Memory Manager
 *
 * The Physical Memory Manager (PMM) is responsible for managing all physical memory
 * in the system, excluding the kernel region. It tracks free and allocated memory frames,
 * handles allocation requests, and manages memory deallocation. The PMM implements
 * a buddy allocation algorithm, organizing memory into free-lists of power-of-two sized
 * blocks for efficient memory management.
 *
 * Memory blocks are managed through the kernel's PHYSMAP region, which provides direct
 * access to physical memory from the higher-half kernel address space. While internal
 * operations use virtual addresses via PHYSMAP, all public interfaces return physical
 * addresses to maintain abstraction.
 *
 * The PMM *must* be initialized FIRST, before the Slab allocator and the VMM.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef PMM_MIN_ORDER_PAGE_SIZE
#define PMM_MIN_ORDER_PAGE_SIZE 4096ULL
#endif

#ifndef PMM_MAX_ORDERS
#define PMM_MAX_ORDERS 32
#endif

// Return codes
typedef enum {
    PMM_OK = 0,
    PMM_ERR_OOM,           // out of memory (no block large enough)
    PMM_ERR_INVALID,       // invalid arguments 
    PMM_ERR_NOT_INIT,      // pmm not initialized yet
    PMM_ERR_ALREADY_INIT,  // pmm_init called twice without pmm_shutdown
    PMM_ERR_NOT_ALIGNED,   // address/size not aligned to required block size
    PMM_ERR_OUT_OF_RANGE,  // address outside managed range
    PMM_ERR_NOT_FOUND      // expected buddy not found during coalescing (internal)
} pmm_status_t;

typedef struct {
    uint64_t free_blocks[PMM_MAX_ORDERS];
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t coalesce_success;
    uint64_t corruption_detected;
} pmm_stats_t;

// Free block header stored at the start of each free block
typedef struct {
    uint32_t magic; 
    uint32_t order;
    uint64_t next_phys;
} pmm_free_header_t;


// Initialization and shutdown

pmm_status_t pmm_init(uint64_t range_start_phys, uint64_t range_end_phys, uint64_t min_block_size);
void pmm_shutdown(void);

// Allocation/Deallocation

pmm_status_t pmm_alloc(size_t size_bytes, uint64_t *out_phys);
pmm_status_t pmm_free(uint64_t phys, size_t size_bytes);
pmm_status_t pmm_mark_reserved_range(uint64_t start, uint64_t end);
pmm_status_t pmm_mark_free_range(uint64_t start, uint64_t end);

// Introspection helpers

bool pmm_is_initialized(void);
uint64_t pmm_managed_base(void);
uint64_t pmm_managed_end(void);
uint64_t pmm_managed_size(void);
uint64_t pmm_min_block_size(void);

// Stats

void pmm_get_stats(pmm_stats_t* out_stats);
void pmm_dump_stats(void);
bool pmm_verify_integrity(void);


/*

Notes on improving the PMM in the future:

1. Coalescing Could Be More Aggressive

The buddy allocator only coalesces upward during free. Consider checking 
if the block being freed can be merged with its buddy even when the buddy is in a higher-order list.

Can't think of anything else now ~u/ApparentlyPlus

*/