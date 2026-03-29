/*
 * pmm.c - Range-based physical memory manager (buddy allocator)
 *
 * This implementation uses an explicit [start, end) physical range stored
 * in range_start/range_end. The public init takes a range (start,end)
 * and rounds/aligns it to the chosen minimum block size.
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

static bool inited = false;
static uint64_t range_start = 0;
static uint64_t range_end = 0;
static uint64_t min_block = PMM_MIN_ORDER_PAGE_SIZE;
static uint32_t max_order = 0;
static uint32_t order_count = 0;

static spinlock_t pmm_lock;

// Forward declarations
static pmm_status_t pmm_mark_free_range(uint64_t start, uint64_t end);

// Free list heads per order. Store physical address of first free block, or EMPTY_SENTINEL for empty.
static uint64_t free_heads[PMM_MAX_ORDERS];
static const uint64_t EMPTY_SENTINEL = UINT64_MAX;

static pmm_stats_t stats;

// We're declaring an exclusion table
// that we populate before marking free ranges
#define PMM_MAX_EXCLUSIONS 8

typedef struct {
    uint64_t start;
    uint64_t end;
} pmm_exclusion_t;

static pmm_exclusion_t exclusions[PMM_MAX_EXCLUSIONS];
static uint32_t exclusion_count = 0;

/*
 * order_to_size - convert order to block size in bytes 
 */
static inline uint64_t order_to_size(uint32_t order) {
    return min_block << order;
}

/*
 * validate_block_in_range - Check if a block is within managed range
 */
static inline bool validate_block_in_range(uint64_t block_phys, uint32_t order) {
    uint64_t block_size = order_to_size(order);
    
    if (block_phys < range_start) {
        LOGF("[PMM ERROR] Block 0x%lx below managed range (start: 0x%lx)\n",
               block_phys, range_start);
        return false;
    }

    if (block_phys + block_size > range_end) {
        LOGF("[PMM ERROR] Block 0x%lx + 0x%lx exceeds managed range (end: 0x%lx)\n",
               block_phys, block_size, range_end);
        return false;
    }
    
    return true;
}

/*
 * validate_free_header - Validate free block header for corruption
 */
static inline bool validate_free_header(uint64_t block_phys, uint32_t expected_order) {
    if (!validate_block_in_range(block_phys, expected_order)) {
        return false;
    }
    
    pmm_free_header_t *header = (pmm_free_header_t *)PHYSMAP_P2V(block_phys);
    
    if (header->magic != PMM_FREE_BLOCK_MAGIC) {
        LOGF("[PMM ERROR] Invalid magic at 0x%lx: 0x%x (expected 0x%x)\n",
               block_phys, header->magic, PMM_FREE_BLOCK_MAGIC);
        stats.corruption_detected++;
        return false;
    }

    if (header->order != expected_order) {
        LOGF("[PMM ERROR] Order mismatch at 0x%lx: %u (expected %u)\n",
               block_phys, header->order, expected_order);
        stats.corruption_detected++;
        return false;
    }

    if (header->next_phys != EMPTY_SENTINEL) {
        if (header->next_phys < range_start || header->next_phys >= range_end) {
            LOGF("[PMM ERROR] Invalid next pointer at 0x%lx: 0x%lx (range: 0x%lx-0x%lx)\n",
                   block_phys, header->next_phys, range_start, range_end);
            stats.corruption_detected++;
            return false;
        }
    }

    if (header->prev_phys != EMPTY_SENTINEL) {
        if (header->prev_phys < range_start || header->prev_phys >= range_end) {
            LOGF("[PMM ERROR] Invalid prev pointer at 0x%lx: 0x%lx (range: 0x%lx-0x%lx)\n",
                   block_phys, header->prev_phys, range_start, range_end);
            stats.corruption_detected++;
            return false;
        }
    }

    return true;
}

/* 
 * read_next_word - read the next pointer stored at the start of a free block
 * Always use the PHYSMAP_P2V macro to access physical memory.
 */
static inline uint64_t read_next_word(uint64_t block_phys, uint32_t order) {
    if (!validate_free_header(block_phys, order)) {
        // Return EMPTY_SENTINEL to break the chain safely on corruption
        return EMPTY_SENTINEL;
    }
    
    pmm_free_header_t *header = (pmm_free_header_t *)PHYSMAP_P2V(block_phys);
    return header->next_phys;
}

/* 
 * write_next_word - write the next pointer stored at the start of a free block
 * Always use the PHYSMAP_P2V macro to access physical memory.
 */
static inline void write_next_word(uint64_t block_phys, uint64_t next_phys, uint32_t order) {
    pmm_free_header_t *header = (pmm_free_header_t *)PHYSMAP_P2V(block_phys);
    header->magic = PMM_FREE_BLOCK_MAGIC;
    header->order = order;
    header->next_phys = next_phys;
}

/* 
 * clear_free_header - Clear header when allocating
 */
static inline void clear_free_header(uint64_t block_phys) {
    pmm_free_header_t *header = (pmm_free_header_t *)PHYSMAP_P2V(block_phys);
    header->magic = 0;
    header->order = 0xFFFFFFFF;
    header->next_phys = 0xDEADBEEFDEADBEEF;
    header->prev_phys = 0xDEADBEEFDEADBEEF;
}

/*
 * pop_head - pop a block from the free list for given order, or EMPTY_SENTINEL if empty
 */
static uint64_t pop_head(uint32_t order) {
    uint64_t head = free_heads[order];
    if (head == EMPTY_SENTINEL) return EMPTY_SENTINEL;

    if (!validate_free_header(head, order)) return EMPTY_SENTINEL;

    pmm_free_header_t* hdr = (pmm_free_header_t*)PHYSMAP_P2V(head);
    uint64_t next = hdr->next_phys;
    free_heads[order] = (next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;

    if (next != EMPTY_SENTINEL)
        ((pmm_free_header_t*)PHYSMAP_P2V(next))->prev_phys = EMPTY_SENTINEL;

    clear_free_header(head);
    stats.free_blocks[order]--;
    return head;
}

/*
 * push_head - push a block onto the free list for given order
 */
static void push_head(uint32_t order, uint64_t block_phys) {
    uint64_t old_head = free_heads[order];
    pmm_free_header_t* hdr = (pmm_free_header_t*)PHYSMAP_P2V(block_phys);
    hdr->magic = PMM_FREE_BLOCK_MAGIC;
    hdr->order = order;
    hdr->next_phys = (old_head == EMPTY_SENTINEL) ? EMPTY_SENTINEL : old_head;
    hdr->prev_phys = EMPTY_SENTINEL;

    if (old_head != EMPTY_SENTINEL)
        ((pmm_free_header_t*)PHYSMAP_P2V(old_head))->prev_phys = block_phys;

    free_heads[order] = block_phys;
    stats.free_blocks[order]++;
}

/*
 * remove_specific - O(1) removal of a specific block using its prev/next pointers
 */
static bool remove_specific(uint32_t order, uint64_t target_phys) {
    if (!validate_block_in_range(target_phys, order)) return false;

    pmm_free_header_t* hdr = (pmm_free_header_t*)PHYSMAP_P2V(target_phys);

    // Magic/order mismatch means allocated or different order - not corruption
    if (hdr->magic != PMM_FREE_BLOCK_MAGIC || hdr->order != order)
        return false;

    if (!validate_free_header(target_phys, order)) {
        LOGF("[PMM] Corruption in remove_specific at 0x%lx\n", target_phys);
        return false;
    }

    uint64_t prev = hdr->prev_phys;
    uint64_t next = hdr->next_phys;

    if (prev == EMPTY_SENTINEL) {
        free_heads[order] = (next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
    } else {
        ((pmm_free_header_t*)PHYSMAP_P2V(prev))->next_phys = next;
    }

    if (next != EMPTY_SENTINEL)
        ((pmm_free_header_t*)PHYSMAP_P2V(next))->prev_phys = prev;

    clear_free_header(target_phys);
    stats.free_blocks[order]--;
    return true;
}

/* 
 * buddy_of - compute buddy address of a block at given order
 */
static inline uint64_t buddy_of(uint64_t addr, uint32_t order) {
    uint64_t size = order_to_size(order);
    return (addr ^ size);
}

/*
 * size_to_order - convert size in bytes to minimum order that fits it
 */
static uint32_t size_to_order(uint64_t size_bytes) {
    if (size_bytes == 0) return 0;
    uint64_t need = min_block;
    uint32_t order = 0;
    while (need < size_bytes) {
        need <<= 1;
        ++order;
    }
    return order;
}

/* 
 * partition_range_into_blocks - Partition an arbitrary aligned range [start,end) 
 * into largest possible aligned blocks and push them into freelists (classic greedy partition).
 * Assumes 'start' is aligned to min_block and 'end' is multiple of min_block.
 */
static void partition_range_into_blocks(uint64_t range_start, uint64_t range_end) {
    uint64_t cur = range_start;

    while (cur < range_end) {
        uint64_t remain = range_end - cur;
        
        /* choose largest order o such that order_to_size(o) <= remain and cur is aligned to that size */
        uint32_t chosen = 0;
        for (int32_t o = (int32_t)max_order; o >= 0; --o) {
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

/*
 * pmm_is_initialized - Returns whether the PMM has been initialized
 */
bool pmm_is_initialized(void) {
    return inited;
}

/*
 * pmm_managed_base - Returns the start of the managed physical memory range
 */
uint64_t pmm_managed_base(void) {
    return range_start;
}

/*
 * pmm_managed_end - Returns the end of the managed physical memory range
 */
uint64_t pmm_managed_end(void) {
    return range_end;
}

/*
 * pmm_managed_size - Returns the size of the managed physical memory range
 */
uint64_t pmm_managed_size(void) {
    return range_end - range_start;
}

/*
 * pmm_min_block_size - Returns the minimum block size (order 0) in bytes
 */
uint64_t pmm_min_block_size(void) {
    return min_block;
}

/*
 * pmm_init - Initialize the physical memory manager.
 *
 * Sets up the buddy allocator data structures for the managed range
 * [range_start_phys, range_end_phys). 
 * 
 * Freelists are now empty by default and the caller must register 
 * exclusions via pmm_exclude_range(), then populate free memory via pmm_populate()
 */
pmm_status_t pmm_init(uint64_t range_start_phys, uint64_t range_end_phys, uint64_t min_block_size) {
    spinlock_init(&pmm_lock, "pmm_global");
    bool flags = spinlock_acquire(&pmm_lock);

    if (inited) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_ALREADY_INIT;
    }
    if (range_end_phys <= range_start_phys) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }
    if (min_block_size == 0 || !is_pow2_u64(min_block_size)) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }
    if (min_block_size < sizeof(pmm_free_header_t)) {
        LOGF("[PMM] min_block_size (%lu) too small for header (%lu)\n",
               min_block_size, sizeof(pmm_free_header_t));
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    min_block = min_block_size;

    uint64_t start_aligned = (uint64_t)align_up(range_start_phys, min_block);
    uint64_t end_aligned = (uint64_t)align_down(range_end_phys, min_block);

    if (end_aligned <= start_aligned) {
        LOGF("[PMM] After alignment, range is empty\n");
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    range_start = start_aligned;
    range_end = end_aligned;

    uint64_t span_trimmed = pmm_managed_size();
    uint64_t blocks = span_trimmed / min_block;

    uint32_t mo = 0;
    uint64_t tmp = blocks;
    while (tmp > 1) { tmp >>= 1; ++mo; }
    if (mo >= PMM_MAX_ORDERS) mo = PMM_MAX_ORDERS - 1;

    max_order = mo;
    order_count = max_order + 1;

    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i)
        free_heads[i] = EMPTY_SENTINEL;

    kmemset(&stats, 0, sizeof(pmm_stats_t));
    exclusion_count = 0;

    inited = true;

    LOGF("[PMM] PMM initialized, managing 0x%lx - 0x%lx (%zu MiB)\n",
           pmm_managed_base(), pmm_managed_end(), pmm_managed_size() / (1024 * 1024));

    spinlock_release(&pmm_lock, flags);
    return PMM_OK;
}

/*
 * pmm_exclude_range - Register a physical range [start, end) that must never
 * be allocated or written to
 */
pmm_status_t pmm_exclude_range(uint64_t start, uint64_t end) {
    bool flags = spinlock_acquire(&pmm_lock);
    if (!inited) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_NOT_INIT;
    }
    if (end <= start) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }
    if (exclusion_count >= PMM_MAX_EXCLUSIONS) {
        LOGF("[PMM] Exclusion table full (max %d)\n", PMM_MAX_EXCLUSIONS);
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    exclusions[exclusion_count].start = start;
    exclusions[exclusion_count].end = end;
    exclusion_count++;

    LOGF("[PMM] Registered exclusion [0x%lx, 0x%lx)\n", start, end);
    spinlock_release(&pmm_lock, flags);
    return PMM_OK;
}

/*
 * pmm_shutdown - Reset state so pmm_init may be called again
 */
void pmm_shutdown(void) {
    bool flags = spinlock_acquire(&pmm_lock);
    if (!inited) {
        spinlock_release(&pmm_lock, flags);
        return;
    }

    // Zero only free blocks
    for (uint32_t order = 0; order <= max_order; order++) {
        uint64_t cur = free_heads[order];
        while (cur != EMPTY_SENTINEL) {
            pmm_free_header_t* hdr = (pmm_free_header_t*)PHYSMAP_P2V(cur);
            uint64_t next = hdr->next_phys;
            kmemset(hdr, 0, order_to_size(order));
            cur = (next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
        }
    }

    inited = false;
    range_start = 0;
    range_end = 0;
    min_block = PMM_MIN_ORDER_PAGE_SIZE;
    max_order = 0;
    order_count = 0;
    exclusion_count = 0;

    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i)
        free_heads[i] = EMPTY_SENTINEL;

    kmemset(&stats, 0, sizeof(pmm_stats_t));

    LOGF("[PMM] PMM Shutdown\n");
    spinlock_release(&pmm_lock, flags);
}

/*
 * alloc_block_of_order - internal allocation helper: find a free block at >= req_order and split down
 */
static pmm_status_t alloc_block_of_order(uint32_t req_order, uint64_t *out_phys) {
    if (!inited) return PMM_ERR_NOT_INIT;
    if (req_order > max_order) return PMM_ERR_OOM;

    for (uint32_t o = req_order; o <= max_order; ++o) {
        while (free_heads[o] != EMPTY_SENTINEL) {
            uint64_t block = pop_head(o);
            if (block == EMPTY_SENTINEL) {
                break;
            }

            while (o > req_order) {
                --o;
                uint64_t half = order_to_size(o);
                uint64_t buddy = block + half;

                // Push buddy into freelist at order o
                push_head(o, buddy);
            }

            *out_phys = block;
            return PMM_OK;
        }
    }

    return PMM_ERR_OOM;
}

/*
 * pmm_alloc - Allocate a block large enough to satisfy size_bytes
 */
pmm_status_t pmm_alloc(size_t size_bytes, uint64_t *out_phys) {
    bool flags = spinlock_acquire(&pmm_lock);
    if (out_phys == NULL) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }
    if (!inited) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_NOT_INIT;
    }
    if (size_bytes == 0) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    uint64_t rounded = (uint64_t)size_bytes;
    if (rounded & (min_block - 1)) rounded = align_up(rounded, min_block);

    uint32_t order = size_to_order(rounded);
    if (order > max_order) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_OOM;
    }

    stats.alloc_calls++;
    pmm_status_t status = alloc_block_of_order(order, out_phys);
    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * pmm_free - Free an allocation previously returned by pmm_alloc
 */
pmm_status_t pmm_free(uint64_t phys, size_t size_bytes) {
    bool flags = spinlock_acquire(&pmm_lock);
    if (!inited) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_NOT_INIT;
    }
    if (size_bytes == 0) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    if (phys < range_start) {
        LOGF("[PMM ERROR] Free: address 0x%lx below managed range\n", phys);
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_OUT_OF_RANGE;
    }
    if (phys >= range_end) {
        LOGF("[PMM ERROR] Free: address 0x%lx above managed range\n", phys);
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_OUT_OF_RANGE;
    }

    // Round size up to nearest block size to determine order and alignment requirements
    uint64_t rounded = (uint64_t)size_bytes;
    if (rounded & (min_block - 1)) rounded = align_up(rounded, min_block);

    uint32_t order = size_to_order(rounded);
    if (order > max_order) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    uint64_t block_addr = phys;
    uint64_t block_size = order_to_size(order);

    if ((block_addr & (block_size - 1)) != 0) {
        LOGF("[PMM ERROR] Free: address 0x%lx not aligned to size 0x%lx\n",
               block_addr, block_size);
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_NOT_ALIGNED;
    }

    stats.free_calls++;

    // Coalesce like a pro heheh
    while (order < max_order) {
        uint64_t buddy = buddy_of(block_addr, order);
        uint64_t buddy_size = order_to_size(order);

        if (buddy < range_start || (buddy + buddy_size) > range_end) {
            break;
        }

        bool found = remove_specific(order, buddy);
        if (!found) {
            push_head(order, block_addr);
            spinlock_release(&pmm_lock, flags);
            return PMM_OK;
        }

        stats.coalesce_success++;

        // Merged block starts at the lower address
        if (buddy < block_addr) block_addr = buddy;
        ++order;
    }

    push_head(order, block_addr);
    spinlock_release(&pmm_lock, flags);
    return PMM_OK;
}

/*
 * pmm_mark_reserved - mark [start,end) as reserved
 * This handles partial overlaps and ensures free-lists remain consistent.
 */
pmm_status_t pmm_mark_reserved(uint64_t start, uint64_t end) {
    bool flags = spinlock_acquire(&pmm_lock);
    if (!inited) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_NOT_INIT;
    }
    if (end <= start) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    if (start < range_start) start = range_start;
    if (end > range_end) end = range_end;
    if (start >= end) {
        spinlock_release(&pmm_lock, flags);
        return PMM_ERR_INVALID;
    }

    uint64_t orig_start = start, orig_end = end;
    start = align_down(start, min_block);
    end = align_up(end, min_block);

    if (start != orig_start || end != orig_end) {
        LOGF("[PMM] Adjusted reserved range [0x%lx, 0x%lx) to [0x%lx, 0x%lx)\n",
               orig_start, orig_end, start, end);
    }

    for (int32_t o = (int32_t)max_order; o >= 0; --o) {
        uint64_t block_size = order_to_size((uint32_t)o);
        uint64_t cur = free_heads[o];

        while (cur != EMPTY_SENTINEL) {
            uint64_t next = read_next_word(cur, (uint32_t)o);
            uint64_t block_start = cur;
            uint64_t block_end = cur + block_size;

            if (!(block_end <= start || block_start >= end)) {
                remove_specific((uint32_t)o, cur);

                if (block_start < start) {
                    pmm_mark_free_range(block_start, start);
                }
                if (block_end > end) {
                    pmm_mark_free_range(end, block_end);
                }
            }

            cur = (next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
        }
    }

    spinlock_release(&pmm_lock, flags);
    return PMM_OK;
}

/*
 * pmm_mark_free_range - manually mark a physical range [start,end) as free
 */
static pmm_status_t pmm_mark_free_range(uint64_t start, uint64_t end) {
    if (!inited) return PMM_ERR_NOT_INIT;
    if (end <= start) return PMM_ERR_INVALID;

    if (start < range_start) start = range_start;
    if (end > range_end) end = range_end;
    if (start >= end) return PMM_ERR_INVALID;

    uint64_t orig_start = start, orig_end = end;
    start = align_up(start, min_block);
    end = align_down(end, min_block);

    if (start >= end) {
        LOGF("[PMM] After alignment, free range [0x%lx, 0x%lx) became empty\n",
               orig_start, orig_end);
        return PMM_ERR_INVALID;
    }

    if (start != orig_start || end != orig_end) {
        LOGF("[PMM] Adjusted free range [0x%lx, 0x%lx) to [0x%lx, 0x%lx)\n",
               orig_start, orig_end, start, end);
    }

    // Clip the range against all registered exclusions
    // Author's Note: This is a simple O(n) loop since we expect very few exclusions. 
    // For large exclusion counts, a more efficient data structure would be needed.
    for (uint32_t i = 0; i < exclusion_count; i++) {
        uint64_t ex_start = exclusions[i].start;
        uint64_t ex_end = exclusions[i].end;
        if (start < ex_end && ex_start < end) {
            if (start < ex_start) {
                pmm_mark_free_range(start, ex_start);
            }
            if (ex_end < end) {
                pmm_mark_free_range(ex_end, end);
            }
            return PMM_OK;
        }
    }

    partition_range_into_blocks(start, end);
    return PMM_OK;
}

/*
 * pmm_populate - public version
 */
pmm_status_t pmm_populate(uint64_t start, uint64_t end) {
    bool flags = spinlock_acquire(&pmm_lock);
    pmm_status_t status = pmm_mark_free_range(start, end);
    spinlock_release(&pmm_lock, flags);
    return status;
}

/*
 * pmm_get_stats - Get current PMM statistics
 */
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
    bool flags = spinlock_acquire(&pmm_lock);
    if (!inited) {
        LOGF("[PMM] Not initialized\n");
        spinlock_release(&pmm_lock, flags);
        return;
    }

    LOGF("\n=== PMM Statistics ===\n");
    LOGF("Managed range: [0x%lx - 0x%lx) (0x%lx bytes, %.2f MiB)\n",
           range_start, range_end, range_end - range_start,
           (range_end - range_start) / (1024.0 * 1024.0));
    LOGF("Min block size: 0x%lx, Max order: %u\n", min_block, max_order);

    LOGF("\nOperation counts:\n");
    LOGF("  Allocations:      %lu\n", stats.alloc_calls);
    LOGF("  Frees:            %lu\n", stats.free_calls);
    LOGF("  Coalesces:        %lu\n", stats.coalesce_success);
    LOGF("  Corruptions:      %lu\n", stats.corruption_detected);

    LOGF("\nFree block distribution:\n");
    LOGF("Order  Size         Free Blocks\n");
    LOGF("-----  -----------  -----------\n");

    uint64_t total_free_bytes = 0;
    bool has_free_blocks = false;

    for (uint32_t o = 0; o <= max_order; o++) {
        uint64_t size = order_to_size(o);
        uint64_t count = stats.free_blocks[o];
        
        if (count > 0) {
            uint64_t bytes = count * size;
            total_free_bytes += bytes;
            LOGF("%-5u  0x%-9lx  %-5lu\n", o, size, count);
            has_free_blocks = true;
        }
    }
    
    if (!has_free_blocks) {
        LOGF("  (no free blocks - all memory allocated)\n");
    }
    
    uint64_t total_managed = pmm_managed_size();
    uint64_t used_bytes = total_managed - total_free_bytes;
    
    LOGF("\nMemory summary:\n");
    LOGF("  Total managed: %lu bytes (%.2f MiB)\n", 
           total_managed, total_managed / (1024.0 * 1024.0));
    LOGF("  Free:          %lu bytes (%.2f MiB)\n",
           total_free_bytes, total_free_bytes / (1024.0 * 1024.0));
    LOGF("  Used:          %lu bytes (%.2f MiB)\n",
           used_bytes, used_bytes / (1024.0 * 1024.0));
    LOGF("  Utilization:   %.1f%%\n",
           total_managed > 0 ? (double)used_bytes / total_managed * 100.0 : 0.0);
    LOGF("======================\n");
    spinlock_release(&pmm_lock, flags);
}

/*
 * pmm_verify_integrity - Verify free-list integrity
 * Returns true if all checks pass, false otherwise
 */
bool pmm_verify_integrity(void) {
    bool flags = spinlock_acquire(&pmm_lock);
    if (!inited) {
        LOGF("[PMM] Not initialized\n");
        spinlock_release(&pmm_lock, flags);
        return false;
    }

    LOGF("[PMM] Checking free-list integrity...\n");

    bool all_ok = true;
    uint64_t counted_free[PMM_MAX_ORDERS] = {0};

    for (uint32_t order = 0; order <= max_order; order++) {
        uint64_t cur = free_heads[order];
        int count = 0;
        uint64_t size = order_to_size(order);

        while (cur != EMPTY_SENTINEL) {
            count++;
            counted_free[order]++;

            if (count > 100000) {
                LOGF("[PMM] Order %u: Possible infinite loop detected\n", order);
                all_ok = false;
                break;
            }

            if (!validate_free_header(cur, order)) {
                LOGF("[PMM] Order %u: Invalid header at block 0x%lx\n", order, cur);
                all_ok = false;
                break;
            }

            if ((cur & (size - 1)) != 0) {
                LOGF("[PMM] Order %u: Block 0x%lx not naturally aligned to size 0x%lx\n",
                        order, cur, size);
                all_ok = false;
            }

            uint64_t next = read_next_word(cur, order);
            cur = (next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
        }
    }

    for (uint32_t order = 0; order <= max_order; order++) {
        if (counted_free[order] != stats.free_blocks[order]) {
            LOGF("[PMM] Order %u: Statistics mismatch (counted: %lu, stats: %lu)\n",
                   order, counted_free[order], stats.free_blocks[order]);
            all_ok = false;
        }
    }
    
    if (all_ok) {
        LOGF("[PMM] All checks passed\n");
    } else {
        LOGF("[PMM] FAILED - integrity compromised!\n");
    }
    
    spinlock_release(&pmm_lock, flags);
    return all_ok;
}