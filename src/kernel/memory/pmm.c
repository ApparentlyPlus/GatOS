/*
 * pmm.c - Physical memory manager (buddy allocator)
 *
 * Buddy allocator over [0, end). The free lists live inside the free blocks themselves,
 * and a page map (one byte per page) records which pages start a free block, and of what order.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/sys/spinlock.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Magic number for validating free block headers
#define PMM_FREE_BLOCK_MAGIC 0xFEEDBEEF

// Page map entry, 0 means "not the start of a free block", otherwise PG_FREE | order
#define PG_FREE 0x80

static bool inited = false;
static uint64_t range_end = 0;
static uint32_t max_order = 0;
static uint8_t* pmap = NULL;

static spinlock_t pmm_lock;

// Free list heads per order. Store physical address of first free block, or EMPTY_SENTINEL for empty.
static uint64_t free_heads[PMM_MAX_ORDERS];
static const uint64_t EMPTY_SENTINEL = UINT64_MAX;

static pmm_stats_t stats;

// We're declaring an exclusion table that we populate before marking free ranges
#define PMM_MAX_EXCLUSIONS 8

typedef struct {
    uint64_t start;
    uint64_t end;
} pmm_exclusion_t;

static pmm_exclusion_t exclusions[PMM_MAX_EXCLUSIONS];
static uint32_t exclusion_count = 0;

static inline uint64_t order_to_size(uint32_t order) {
    return (uint64_t)PAGE_SIZE << order;
}

static inline pmm_free_header_t* hdr_of(uint64_t phys) {
    return (pmm_free_header_t*)PHYSMAP_P2V(phys);
}

static uint32_t size_to_order(uint64_t size) {
    uint32_t order = 0;
    while (order_to_size(order) < size) order++;
    return order;
}

/*
 * valid_hdr - Check the in-block header of a block the page map says is free
 */
static bool valid_hdr(uint64_t phys, uint32_t order) {
    pmm_free_header_t* h = hdr_of(phys);

    if (h->magic != PMM_FREE_BLOCK_MAGIC || h->order != order) {
        LOGF("[PMM ERROR] Bad free header at 0x%lx (magic 0x%x, order %u, expected %u)\n", phys, h->magic, h->order, order);
        stats.corruption_detected++;
        return false;
    }

    if ((h->next_phys != EMPTY_SENTINEL && h->next_phys >= range_end) ||
        (h->prev_phys != EMPTY_SENTINEL && h->prev_phys >= range_end)) {
        LOGF("[PMM ERROR] Bad free list links at 0x%lx\n", phys);
        stats.corruption_detected++;
        return false;
    }

    return true;
}

/*
 * push_head - push a block onto the free list for given order
 */
static void push_head(uint32_t order, uint64_t phys) {
    uint64_t old = free_heads[order];
    pmm_free_header_t* h = hdr_of(phys);

    h->magic = PMM_FREE_BLOCK_MAGIC;
    h->order = order;
    h->next_phys = old;
    h->prev_phys = EMPTY_SENTINEL;

    if (old != EMPTY_SENTINEL) hdr_of(old)->prev_phys = phys;

    free_heads[order] = phys;
    pmap[phys / PAGE_SIZE] = PG_FREE | order;
    stats.free_blocks[order]++;
}

/*
 * unlink - Take a free block out of its list, by its own links
 */
static void unlink(uint32_t order, uint64_t phys) {
    pmm_free_header_t* h = hdr_of(phys);

    if (h->prev_phys == EMPTY_SENTINEL) free_heads[order] = h->next_phys;
    else hdr_of(h->prev_phys)->next_phys = h->next_phys;

    if (h->next_phys != EMPTY_SENTINEL) hdr_of(h->next_phys)->prev_phys = h->prev_phys;

    h->magic = 0;
    pmap[phys / PAGE_SIZE] = 0;
    stats.free_blocks[order]--;
}

/*
 * pop_head - pop a block from the free list for given order, or EMPTY_SENTINEL if empty
 */
static uint64_t pop_head(uint32_t order) {
    uint64_t head = free_heads[order];
    if (head == EMPTY_SENTINEL) return EMPTY_SENTINEL;

    // A broken head would hand out garbage, so we cut the whole list loose instead
    if (!valid_hdr(head, order)) {
        free_heads[order] = EMPTY_SENTINEL;
        return EMPTY_SENTINEL;
    }

    unlink(order, head);
    return head;
}

/*
 * take_buddy - Unlink the buddy if the page map says it's a free block of this exact order
 */
static bool take_buddy(uint32_t order, uint64_t buddy) {
    if (pmap[buddy / PAGE_SIZE] != (PG_FREE | order)) return false;
    if (!valid_hdr(buddy, order)) return false;
    unlink(order, buddy);
    return true;
}

/*
 * free_covering - True if phys lies inside any free block
 */
static bool free_covering(uint64_t phys) {
    for (uint32_t o = 0; o <= max_order; o++) {
        uint64_t head = align_down(phys, order_to_size(o));
        uint8_t m = pmap[head / PAGE_SIZE];
        if ((m & PG_FREE) && (uint32_t)(m & ~PG_FREE) >= o) return true;
    }
    return false;
}

/*
 * free_overlaps - True if any page of [phys, phys + size of order) is already free
 */
static bool free_overlaps(uint64_t phys, uint32_t order) {
    if (free_covering(phys)) return true;
    for (uint64_t p = phys + PAGE_SIZE; p < phys + order_to_size(order); p += PAGE_SIZE)
        if (pmap[p / PAGE_SIZE] & PG_FREE) return true;
    return false;
}

/*
 * split_range - Greedy split of [start, end) into the largest aligned blocks, pushed to the freelists.
 */
static void split_range(uint64_t start, uint64_t end) {
    while (start < end) {
        uint32_t o = max_order;
        while (order_to_size(o) > end - start || (start & (order_to_size(o) - 1)))
            o--;
        push_head(o, start);
        start += order_to_size(o);
    }
}

/*
 * mark_free - Hand [start, end) to the free lists, minus any exclusion zones
 */
static void mark_free(uint64_t start, uint64_t end) {
    if (end > range_end) end = range_end;
    start = align_up(start, PAGE_SIZE);
    end = align_down(end, PAGE_SIZE);
    if (start >= end) return;

    // Clip the range against all registered exclusions
    // Author's Note: This is a simple O(n) loop since we expect very few exclusions.
    for (uint32_t i = 0; i < exclusion_count; i++) {
        uint64_t ex_start = exclusions[i].start;
        uint64_t ex_end = exclusions[i].end;
        if (start < ex_end && ex_start < end) {
            if (start < ex_start) mark_free(start, ex_start);
            if (ex_end < end) mark_free(ex_end, end);
            return;
        }
    }

    split_range(start, end);
}

bool pmm_is_initialized(void) {
    return inited;
}

uint64_t pmm_managed_end(void) {
    return range_end;
}

/*
 * pmm_map_size - Bytes of page map needed to manage [0, end)
 */
uint64_t pmm_map_size(uint64_t end) {
    return end / PAGE_SIZE;
}

/*
 * pmm_init - Set up an empty buddy allocator for [0, end), with its page map at map_phys
 */
pmm_status_t pmm_init(uint64_t end, uint64_t map_phys) {
    if (inited) return PMM_ERR_ALREADY_INIT;
    spinlock_init(&pmm_lock, "pmm_global");

    end = align_down(end, PAGE_SIZE);
    if (end == 0) return PMM_ERR_INVALID;

    range_end = end;
    pmap = (uint8_t*)PHYSMAP_P2V(map_phys);
    kmemset(pmap, 0, pmm_map_size(end));

    max_order = 0;
    while (max_order + 1 < PMM_MAX_ORDERS && order_to_size(max_order + 1) <= end)
        max_order++;

    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i)
        free_heads[i] = EMPTY_SENTINEL;

    kmemset(&stats, 0, sizeof(pmm_stats_t));
    exclusion_count = 0;
    inited = true;

    LOGF("[PMM] PMM initialized, managing 0x0 - 0x%lx (%lu MiB)\n", end, end / MEASUREMENT_UNIT_MB);
    return PMM_OK;
}

/*
 * pmm_exclude_range - Physical range [start, end) that must never be allocated or written
 */
pmm_status_t pmm_exclude_range(uint64_t start, uint64_t end) {
    if (!inited) return PMM_ERR_NOT_INIT;
    if (end <= start) return PMM_ERR_INVALID;

    bool flags = spinlock_acquire(&pmm_lock);
    pmm_status_t status = PMM_OK;

    if (exclusion_count >= PMM_MAX_EXCLUSIONS) {
        LOGF("[PMM] Exclusion table full (max %d)\n", PMM_MAX_EXCLUSIONS);
        status = PMM_ERR_INVALID;
    } else {
        exclusions[exclusion_count++] = (pmm_exclusion_t){ align_down(start, PAGE_SIZE), align_up(end, PAGE_SIZE) };
        LOGF("[PMM] Registered exclusion [0x%lx, 0x%lx)\n", start, end);
    }

    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * pmm_populate - Mark a firmware reported available region as free
 */
pmm_status_t pmm_populate(uint64_t start, uint64_t end) {
    if (!inited) return PMM_ERR_NOT_INIT;
    if (end <= start) return PMM_ERR_INVALID;

    bool flags = spinlock_acquire(&pmm_lock);
    mark_free(start, end);
    spinlock_release(&pmm_lock, flags);
    return PMM_OK;
}

/*
 * alloc_order - Find a free block at >= order and split it down (lock held)
 */
static pmm_status_t alloc_order(uint32_t order, uint64_t* out_phys) {
    for (uint32_t o = order; o <= max_order; o++) {
        uint64_t block = pop_head(o);
        if (block == EMPTY_SENTINEL) continue;

        // Hand the upper halves back until the block is the size we want
        while (o > order) {
            o--;
            push_head(o, block + order_to_size(o));
        }

        *out_phys = block;
        return PMM_OK;
    }
    return PMM_ERR_OOM;
}

/*
 * pmm_alloc - Allocate a block large enough to satisfy size_bytes
 */
pmm_status_t pmm_alloc(size_t size_bytes, uint64_t* out_phys) {
    if (!out_phys || size_bytes == 0) return PMM_ERR_INVALID;
    if (!inited) return PMM_ERR_NOT_INIT;

    uint32_t order = size_to_order(size_bytes);
    if (order > max_order) return PMM_ERR_OOM;

    bool flags = spinlock_acquire(&pmm_lock);
    stats.alloc_calls++;
    pmm_status_t status = alloc_order(order, out_phys);
    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * free_order - Coalesce a block with its buddies and put it back (lock held)
 */
static pmm_status_t free_order(uint64_t phys, uint32_t order) {
    if (free_overlaps(phys, order)) {
        LOGF("[PMM ERROR] Double free at 0x%lx\n", phys);
        return PMM_ERR_DOUBLE_FREE;
    }

    stats.free_calls++;

    // Coalesce like a pro heheh
    while (order < max_order) {
        uint64_t buddy = phys ^ order_to_size(order);
        if (buddy + order_to_size(order) > range_end) break;
        if (!take_buddy(order, buddy)) break;

        stats.coalesce_success++;
        if (buddy < phys) phys = buddy;
        order++;
    }

    push_head(order, phys);
    return PMM_OK;
}

/*
 * pmm_free - Return a block to the allocator
 */
pmm_status_t pmm_free(uint64_t phys, size_t size_bytes) {
    if (!inited) return PMM_ERR_NOT_INIT;
    if (size_bytes == 0) return PMM_ERR_INVALID;
    if (phys >= range_end) {
        LOGF("[PMM ERROR] Free: address 0x%lx above managed range\n", phys);
        return PMM_ERR_OUT_OF_RANGE;
    }

    uint32_t order = size_to_order(size_bytes);
    if (order > max_order) return PMM_ERR_INVALID;
    if (phys + order_to_size(order) > range_end) return PMM_ERR_OUT_OF_RANGE;

    if (phys & (order_to_size(order) - 1)) {
        LOGF("[PMM ERROR] Free: address 0x%lx not aligned to size 0x%lx\n", phys, order_to_size(order));
        return PMM_ERR_NOT_ALIGNED;
    }

    bool flags = spinlock_acquire(&pmm_lock);
    pmm_status_t status = free_order(phys, order);
    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * pmm_alloc_exact - Like pmm_alloc, but the rounding tail goes straight back to the free lists,
 * so exactly align_up(size, PAGE_SIZE) bytes stay allocated
 */
pmm_status_t pmm_alloc_exact(size_t size_bytes, uint64_t* out_phys) {
    if (!out_phys || size_bytes == 0) return PMM_ERR_INVALID;
    if (!inited) return PMM_ERR_NOT_INIT;

    uint32_t order = size_to_order(size_bytes);
    if (order > max_order) return PMM_ERR_OOM;

    bool flags = spinlock_acquire(&pmm_lock);
    stats.alloc_calls++;
    pmm_status_t status = alloc_order(order, out_phys);
    if (status == PMM_OK)
        split_range(*out_phys + align_up(size_bytes, PAGE_SIZE), *out_phys + order_to_size(order));
    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * pmm_free_range - Frees any page aligned range, in the largest aligned blocks that fit
 */
pmm_status_t pmm_free_range(uint64_t phys, size_t size_bytes) {
    if (!inited) return PMM_ERR_NOT_INIT;
    if (phys & (PAGE_SIZE - 1)) return PMM_ERR_NOT_ALIGNED;

    uint64_t end = phys + align_up(size_bytes, PAGE_SIZE);
    if (end > range_end || end < phys) return PMM_ERR_OUT_OF_RANGE;

    bool flags = spinlock_acquire(&pmm_lock);
    pmm_status_t status = PMM_OK;
    while (phys < end && status == PMM_OK) {
        uint32_t o = max_order;
        while (order_to_size(o) > end - phys || (phys & (order_to_size(o) - 1))) o--;
        status = free_order(phys, o);
        phys += order_to_size(o);
    }
    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * pmm_is_free - True if the page at phys is currently free
 */
bool pmm_is_free(uint64_t phys) {
    if (!inited || phys >= range_end) return false;
    bool flags = spinlock_acquire(&pmm_lock);
    bool free = free_covering(phys);
    spinlock_release(&pmm_lock, flags);
    return free;
}

void pmm_get_stats(pmm_stats_t* out_stats) {
    if (!out_stats) return;
    bool flags = spinlock_acquire(&pmm_lock);
    *out_stats = stats;
    spinlock_release(&pmm_lock, flags);
}

/*
 * pmm_dump_stats - Print detailed PMM statistics
 */
void pmm_dump_stats(void) {
    if (!inited) {
        LOGF("[PMM] Not initialized\n");
        return;
    }

    bool flags = spinlock_acquire(&pmm_lock);

    uint64_t free_bytes = 0;
    for (uint32_t o = 0; o <= max_order; o++) free_bytes += stats.free_blocks[o] * order_to_size(o);

    LOGF("PMM\n");
    log_indent(1);
    LOGF("managing  0x0 - 0x%lx (%lu MiB, max order %u)\n", range_end, range_end / MEASUREMENT_UNIT_MB, max_order);
    LOGF("free      %lu KiB of %lu KiB\n", free_bytes / MEASUREMENT_UNIT_KB, range_end / MEASUREMENT_UNIT_KB);
    LOGF("calls     %lu allocs, %lu frees, %lu merges\n", stats.alloc_calls, stats.free_calls, stats.coalesce_success);
    if (stats.corruption_detected) LOGF("errors    %lu corrupt blocks seen\n", stats.corruption_detected);

    LOGF("free blocks\n");
    log_indent(1);
    for (uint32_t o = 0; o <= max_order; o++)
        if (stats.free_blocks[o]) LOGF("order %-2u %8lu KiB  x%lu\n", o, order_to_size(o) / MEASUREMENT_UNIT_KB, stats.free_blocks[o]);
    log_indent(-2);
    spinlock_release(&pmm_lock, flags);
}

/*
 * pmm_verify_integrity - Free lists and page map must agree, false if anything is off
 */
bool pmm_verify_integrity(void) {
    if (!inited) {
        LOGF("[PMM] Not initialized\n");
        return false;
    }

    bool flags = spinlock_acquire(&pmm_lock);
    bool ok = true;
    uint64_t listed = 0;

    for (uint32_t order = 0; order <= max_order && ok; order++) {
        uint64_t count = 0;
        uint64_t prev = EMPTY_SENTINEL;

        for (uint64_t cur = free_heads[order]; cur != EMPTY_SENTINEL; cur = hdr_of(cur)->next_phys) {
            if (cur >= range_end || (cur & (order_to_size(order) - 1)) ||
                pmap[cur / PAGE_SIZE] != (PG_FREE | order) || !valid_hdr(cur, order) ||
                hdr_of(cur)->prev_phys != prev || ++count > stats.free_blocks[order]) {
                LOGF("[PMM] Order %u: broken block at 0x%lx\n", order, cur);
                ok = false;
                break;
            }
            prev = cur;
        }

        if (ok && count != stats.free_blocks[order]) {
            LOGF("[PMM] Order %u: counted %lu blocks, stats say %lu\n", order, count, stats.free_blocks[order]);
            ok = false;
        }
        listed += count;
    }

    // Every free mark in the page map must belong to a listed block
    uint64_t marked = 0;
    for (uint64_t i = 0; ok && i < range_end / PAGE_SIZE; i++)
        if (pmap[i] & PG_FREE) marked++;

    if (ok && marked != listed) {
        LOGF("[PMM] Page map has %lu free marks, lists have %lu blocks\n", marked, listed);
        ok = false;
    }

    if (!ok) LOGF("[PMM] Integrity check failed\n");
    spinlock_release(&pmm_lock, flags);
    return ok;
}
