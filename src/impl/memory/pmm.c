/*
 * pmm.c - Range-based physical memory manager (buddy allocator).
 *
 * This implementation uses an explicit [start, end) physical range stored
 * in g_range_start/g_range_end. The public init takes a range (start,end)
 * and rounds/aligns it to the chosen minimum block size.
 *
 * Author: u/ApparentlyPlus
 */

#include <memory/pmm.h>
#include <memory/paging.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <libc/string.h>

static bool g_inited = false;
static uint64_t g_range_start = 0;
static uint64_t g_range_end   = 0;
static uint64_t g_min_block = PMM_MIN_ORDER_PAGE_SIZE;
static uint32_t g_max_order = 0;
static uint32_t g_order_count = 0;

// Free list heads per order. Store physical address of first free block, or EMPTY_SENTINEL for empty.
static uint64_t g_free_heads[PMM_MAX_ORDERS];
static const uint64_t EMPTY_SENTINEL = UINT64_MAX;

/* 
 * is_pow2_u64 - check if x is a power of two
 */
static inline bool is_pow2_u64(uint64_t x) { 
    return x && ((x & (x - 1)) == 0); 
}

/* 
 * order_to_size - convert order to block size in bytes 
 */
static inline uint64_t order_to_size(uint32_t order) { 
    return g_min_block << order; 
}

/* 
 * read_next_word - read the next pointer stored at the start of a free block
 */
static inline uint64_t read_next_word(uint64_t block_phys) {
    uint64_t *ptr = (uint64_t *)(uintptr_t)block_phys;
    return *ptr;
}

/* 
 * write_next_word - write the next pointer stored at the start of a free block
 */
static inline void write_next_word(uint64_t block_phys, uint64_t next_phys) {
    uint64_t *ptr = (uint64_t *)(uintptr_t)block_phys;
    *ptr = next_phys;
}

/*
 * pop_head - pop a block from the free list for given order, or EMPTY_SENTINEL if empty
 */
static uint64_t pop_head(uint32_t order) {
    uint64_t head = g_free_heads[order];
    if (head == EMPTY_SENTINEL) return EMPTY_SENTINEL;
    uint64_t next = read_next_word(head); // next may be 0, which means NULL
    g_free_heads[order] = (next == 0 ? EMPTY_SENTINEL : next);
    return head;
}

/* 
 * push_head - push a block onto the free list for given order
 */
static void push_head(uint32_t order, uint64_t block_phys) {
    uint64_t head = g_free_heads[order];
    // store (head==EMPTY_SENTINEL ? 0 : head) as the next pointer
    write_next_word(block_phys, (head == EMPTY_SENTINEL) ? 0ULL : head);
    g_free_heads[order] = block_phys;
}

/*
 * remove_specific - remove a specific block from the free list for given order
 */
static bool remove_specific(uint32_t order, uint64_t target_phys) {
    uint64_t prev = EMPTY_SENTINEL;
    uint64_t cur = g_free_heads[order];
    while (cur != EMPTY_SENTINEL) {
        uint64_t next = read_next_word(cur); /* 0 means NULL */
        if (cur == target_phys) {
            if (prev == EMPTY_SENTINEL) {
                g_free_heads[order] = (next == 0 ? EMPTY_SENTINEL : next);
            } else {
                // write prev->next = next
                write_next_word(prev, next);
            }
            return true;
        }
        prev = cur;
        cur = (next == 0 ? EMPTY_SENTINEL : next);
    }
    return false;
}

/* 
 * buddy_of - compute buddy address of a block at given order
 */
static inline uint64_t buddy_of(uint64_t addr, uint32_t order) {
    uint64_t size = order_to_size(order);
    return (((addr - g_range_start) ^ size) + g_range_start);
}

/*
 * size_to_order - convert size in bytes to minimum order that fits it
 */
static uint32_t size_to_order(uint64_t size_bytes) {
    if (size_bytes == 0) return 0;
    uint64_t need = g_min_block;
    uint32_t order = 0;
    while (need < size_bytes) {
        need <<= 1;
        ++order;
        // guaranteed to stop because g_max_order bounds allocations
    }
    return order;
}

/* 
 * partition_range_into_blocks - Partition an arbitrary aligned range [start,end) 
 * into largest possible aligned blocks and push them into freelists (classic greedy partition).
 * Assumes 'start' is aligned to g_min_block and 'end' is multiple of g_min_block. 
 */
static void partition_range_into_blocks(uint64_t range_start, uint64_t range_end) {
    uint64_t cur = range_start;

    while (cur < range_end) {
        uint64_t remain = range_end - cur;
        /* choose largest order o such that order_to_size(o) <= remain and cur is aligned to that size */
        uint32_t chosen = 0;
        for (int32_t o = (int32_t)g_max_order; o >= 0; --o) {
            uint64_t bsize = order_to_size((uint32_t)o);
            if (bsize > remain) continue;
            if ((cur & (bsize - 1)) != 0) continue;
            chosen = (uint32_t)o;
            break;
        }
        push_head(chosen, cur);
        cur += order_to_size(chosen);
    }
}

/* ----------------------------------------------------------------- */

/*
 * pmm_is_initialized - Returns whether the PMM has been initialized
 */
bool pmm_is_initialized(void) {
    return g_inited; 
}

/*
 * pmm_managed_base - Returns the start of the managed physical memory range
 */
uint64_t pmm_managed_base(void) { 
    return g_range_start; 
}

/*
 * pmm_managed_end - Returns the end of the managed physical memory range
 */
uint64_t pmm_managed_end(void) { 
    return g_range_end; 
}

/*
 * pmm_managed_size - Returns the size of the managed physical memory range
 */
uint64_t pmm_managed_size(void) { 
    return g_range_end - g_range_start; 
}

/*
 * pmm_min_block_size - Returns the minimum block size (order 0) in bytes
 */
uint64_t pmm_min_block_size(void) { 
    return g_min_block; 
}


/*
 * pmm_init - Initialize the physical memory manager to manage
 * the physical address range [range_start_phys, range_end_phys).
 */
pmm_status_t pmm_init(uint64_t range_start_phys, uint64_t range_end_phys, uint64_t min_block_size) {
    if (g_inited) return PMM_ERR_ALREADY_INIT;
    if (range_end_phys <= range_start_phys) return PMM_ERR_INVALID;
    if (min_block_size == 0 || !is_pow2_u64(min_block_size)) return PMM_ERR_INVALID;
    if (min_block_size < sizeof(uint64_t)) return PMM_ERR_INVALID;

    g_min_block = min_block_size;

    // Align start up to min_block and end down to min_block
    uint64_t start_aligned = (uint64_t)align_up(range_start_phys, g_min_block);
    uint64_t end_aligned = (uint64_t)align_down(range_end_phys, g_min_block);

    if (end_aligned <= start_aligned)
        return PMM_ERR_INVALID;

    g_range_start = start_aligned;
    g_range_end = end_aligned;

    uint64_t span_trimmed = pmm_managed_size();
    uint64_t blocks = span_trimmed / g_min_block;

    uint32_t max_order = 0;
    uint64_t tmp = blocks;
    while (tmp > 1) { tmp >>= 1; ++max_order; }
    if (max_order >= PMM_MAX_ORDERS) max_order = PMM_MAX_ORDERS - 1;

    g_max_order = max_order;
    g_order_count = g_max_order + 1;

    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i)
        g_free_heads[i] = EMPTY_SENTINEL;

    // Partition the range into blocks
    partition_range_into_blocks(g_range_start, g_range_end);

    g_inited = true;
    return PMM_OK;
}

/*
 * pmm_shutdown - Reset state so pmm_init may be called again.
 */
void pmm_shutdown(void) {
    if (!g_inited) return;

    // clear metadata region (we store next pointers at starts of free blocks)
    uint8_t *ptr = (uint8_t *)(uintptr_t)g_range_start;
    uint64_t size = pmm_managed_size();
    for (uint64_t i = 0; i < size; ++i) ptr[i] = 0;

    g_inited = false;
    g_range_start = 0;
    g_range_end = 0;
    g_min_block = PMM_MIN_ORDER_PAGE_SIZE;
    g_max_order = 0;
    g_order_count = 0;
    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i) g_free_heads[i] = EMPTY_SENTINEL;
}

/*
 * alloc_block_of_order - internal allocation helper: find a free block at >= req_order and split down
 */
static pmm_status_t alloc_block_of_order(uint32_t req_order, uint64_t *out_phys) {
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (req_order > g_max_order) return PMM_ERR_OOM;

    uint32_t o = req_order;
    while (o <= g_max_order && g_free_heads[o] == EMPTY_SENTINEL) ++o;
    if (o > g_max_order) return PMM_ERR_OOM;

    uint64_t block = pop_head(o);
    if (block == EMPTY_SENTINEL) return PMM_ERR_OOM; // paranoia

    // split until we reach requested order
    while (o > req_order) {
        --o;
        uint64_t half = order_to_size(o);
        uint64_t buddy = block + half;
        //push buddy into freelist at order o
        push_head(o, buddy);
    }

    *out_phys = block;
    return PMM_OK;
}

/*
 * pmm_alloc - Allocate a block large enough to satisfy size_bytes.
 */
pmm_status_t pmm_alloc(size_t size_bytes, uint64_t *out_phys) {
    if (out_phys == NULL) return PMM_ERR_INVALID;
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (size_bytes == 0) return PMM_ERR_INVALID;

    /* Round up to multiple of g_min_block */
    uint64_t rounded = (uint64_t)size_bytes;
    if (rounded & (g_min_block - 1)) rounded = align_up(rounded, g_min_block);

    uint32_t order = size_to_order(rounded);
    if (order > g_max_order) return PMM_ERR_OOM;

    return alloc_block_of_order(order, out_phys);
}

/*
 * pmm_free - Free an allocation previously returned by pmm_alloc.
 */
pmm_status_t pmm_free(uint64_t phys, size_t size_bytes) {
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (size_bytes == 0) return PMM_ERR_INVALID;

    // basic range check against [g_range_start, g_range_end)
    if (phys < g_range_start) return PMM_ERR_OUT_OF_RANGE;
    if (phys >= g_range_end) return PMM_ERR_OUT_OF_RANGE;

    // round size in the same manner as allocation
    uint64_t rounded = (uint64_t)size_bytes;
    if (rounded & (g_min_block - 1)) rounded = align_up(rounded, g_min_block);

    uint32_t order = size_to_order(rounded);
    if (order > g_max_order) return PMM_ERR_INVALID;

    uint64_t block_addr = phys;
    uint64_t block_size = order_to_size(order);

    if ((block_addr & (block_size - 1)) != 0) return PMM_ERR_NOT_ALIGNED;

    // Coalesce upwards where possible
    while (order < g_max_order) {
        uint64_t buddy = buddy_of(block_addr, order);

        // if buddy is outside managed range, stop
        if (buddy < g_range_start || (buddy + block_size) > g_range_end) {
            break;
        }

        // If buddy is free at this order, remove it and coalesce
        bool found = remove_specific(order, buddy);
        if (!found) {
            push_head(order, block_addr); // buddy not free, push current and exit
            return PMM_OK;
        }

        // buddy removed, merged block is min(block_addr, buddy)
        if (buddy < block_addr) block_addr = buddy;
        ++order;
        block_size <<= 1;
    }

    /* push the resulting (possibly coalesced) block */
    push_head(order, block_addr);
    return PMM_OK;
}

/*
 * pmm_mark_reserved_range - mark [start,end) as reserved.
 * This handles partial overlaps and ensures free-lists remain consistent.
 */
pmm_status_t pmm_mark_reserved_range(uint64_t start, uint64_t end) {
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (end <= start) return PMM_ERR_INVALID;

    // clamp to managed range
    if (start < g_range_start) start = g_range_start;
    if (end > g_range_end) end = g_range_end;
    if (start >= end) return PMM_ERR_INVALID;

    // align to min block for safe handling
    start = align_down(start, g_min_block);
    end   = align_up(end, g_min_block);

    // for each order from max to min, scan free lists and remove overlapping blocks
    for (int32_t o = (int32_t)g_max_order; o >= 0; --o) {
        uint64_t block_size = order_to_size((uint32_t)o);
        uint64_t cur = g_free_heads[o];

        while (cur != EMPTY_SENTINEL) {
            uint64_t next = read_next_word(cur);
            uint64_t block_start = cur;
            uint64_t block_end = cur + block_size;

            // Check if block overlaps the reserved range
            if (!(block_end <= start || block_start >= end)) {
                
                // Remove this block from the free list
                remove_specific((uint32_t)o, cur);

                // If block is partially outside the reserved range, push remaining pieces
                if (block_start < start) {
                    pmm_mark_free_range(block_start, start);
                }
                if (block_end > end) {
                    pmm_mark_free_range(end, block_end);
                }
            }

            cur = next == 0 ? EMPTY_SENTINEL : next;
        }
    }

    return PMM_OK;
}

/*
 * pmm_mark_free_range - manually mark a physical range [start,end) as free.
 * Partitions the range into aligned blocks and pushes them into the free-lists.
 */
pmm_status_t pmm_mark_free_range(uint64_t start, uint64_t end) {
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (end <= start) return PMM_ERR_INVALID;

    // clamp to managed range
    if (start < g_range_start) start = g_range_start;
    if (end > g_range_end) end = g_range_end;
    if (start >= end) return PMM_ERR_INVALID;

    // Round start up and end down to min_block
    start = align_up(start, g_min_block);
    end   = align_down(end, g_min_block);
    if (start >= end) return PMM_ERR_INVALID;

    partition_range_into_blocks(start, end);
    return PMM_OK;
}
