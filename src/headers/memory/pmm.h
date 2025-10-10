/*
 * pmm.h - Physical Memory Manager (Buddy Allocator)
 * 
 * Author: u/ApparentlyPlus
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef PMM_MIN_ORDER_PAGE_SIZE
#define PMM_MIN_ORDER_PAGE_SIZE 4096ULL
#endif

#ifndef PMM_MAX_ORDERS
#define PMM_MAX_ORDERS 32
#endif

// Return type

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

// Public API
pmm_status_t pmm_init(uint64_t base_phys, uint64_t span_bytes, uint64_t min_block_size);
void pmm_shutdown(void);
pmm_status_t pmm_alloc(size_t size_bytes, uint64_t *out_phys);
pmm_status_t pmm_free(uint64_t phys, size_t size_bytes);
pmm_status_t pmm_mark_free_range(uint64_t start, uint64_t end);
pmm_status_t pmm_mark_reserved_range(uint64_t start, uint64_t end);

// Introspection helpers
bool pmm_is_initialized(void);
uint64_t pmm_managed_base(void);
uint64_t pmm_managed_size(void);
uint64_t pmm_managed_end(void);
uint64_t pmm_min_block_size(void);

#endif
