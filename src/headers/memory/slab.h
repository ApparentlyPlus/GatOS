/*
 * slab.h - Slab Allocator for Small Object Allocation
 * 
 * This allocator manages fixed-size object caches (slabs) to efficiently
 * allocate small structures without wasting PMM pages. 
 * 
 * VMM
 * ├─→ Slab Allocator (for small objects < PAGE_SIZE/8)
 * │      └─→ PMM (for backing pages)
 * └─→ PMM (for large allocations >= PAGE_SIZE/8)
 * 
 * A warning is emitted in QEMU serial if the PAGE_SIZE/8 
 * constraint is violated.
 * 
 * This allocator must be initialized directly after the PMM is online,
 * and before the VMM becomes online.
 * 
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Constants
#ifndef SLAB_MAX_CACHES
#define SLAB_MAX_CACHES 16
#endif

#ifndef SLAB_CACHE_NAME_LEN
#define SLAB_CACHE_NAME_LEN 32
#endif

// Return codes
typedef enum {
    SLAB_OK = 0,
    SLAB_ERR_INVALID,        // Invalid arguments
    SLAB_ERR_NO_MEMORY,      // Failed to allocate from PMM
    SLAB_ERR_NOT_INIT,       // Slab allocator not initialized
    SLAB_ERR_ALREADY_INIT,   // Slab allocator already initialized
    SLAB_ERR_CACHE_FULL,     // Maximum number of caches reached
    SLAB_ERR_NOT_FOUND,      // Cache or object not found
    SLAB_ERR_CORRUPTION,     // Detected memory corruption
    SLAB_ERR_BAD_SIZE,       // Object size too large for slab
} slab_status_t;


typedef struct slab_cache slab_cache_t;
typedef struct slab slab_t;

// Slab cache statistics
typedef struct {
    uint64_t total_allocs;       // Total allocations from this cache
    uint64_t total_frees;        // Total frees to this cache
    uint64_t active_objects;     // Currently allocated objects
    uint64_t slab_count;         // Number of slabs in cache
    uint64_t partial_slabs;      // Slabs with some free objects
    uint64_t full_slabs;         // Slabs with no free objects
    uint64_t empty_slabs;        // Slabs with all objects free
} slab_cache_stats_t;

// Global slab allocator statistics
typedef struct {
    uint64_t total_slabs;           // Total slabs allocated from PMM
    uint64_t total_pmm_bytes;       // Total bytes requested from PMM
    uint64_t cache_count;           // Number of active caches
    uint64_t corruption_detected;   // Corruption events detected
} slab_stats_t;

// Initialization and Shutdown

slab_status_t slab_init(void);
void slab_shutdown(void);
bool slab_is_initialized(void);

// Cache Management

slab_cache_t* slab_cache_create(const char* name, size_t obj_size, size_t align);
void slab_cache_destroy(slab_cache_t* cache);
slab_cache_t* slab_cache_find(const char* name);

// Allocation/Deallocation

slab_status_t slab_alloc(slab_cache_t* cache, void** out_obj);
slab_status_t slab_free(slab_cache_t* cache, void* obj);

// Statistics and Debugging

void slab_cache_stats(slab_cache_t* cache, slab_cache_stats_t* out_stats);
void slab_get_stats(slab_stats_t* out_stats);
void slab_dump_stats(void);
void slab_cache_dump(slab_cache_t* cache);
void slab_dump_all_caches(void);
bool slab_verify_integrity(void);

// Introspection

size_t slab_cache_obj_size(slab_cache_t* cache);
const char* slab_cache_name(slab_cache_t* cache);