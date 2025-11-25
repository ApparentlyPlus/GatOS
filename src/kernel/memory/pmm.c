/*
 * pmm.c - Range-based physical memory manager (buddy allocator)
 *
 * This implementation uses an explicit [start, end) physical range stored
 * in g_range_start/g_range_end. The public init takes a range (start,end)
 * and rounds/aligns it to the chosen minimum block size.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Magic number for validating free block headers
#define PMM_FREE_BLOCK_MAGIC 0xFEEDBEEF

static bool g_inited = false;
static uint64_t g_range_start = 0;
static uint64_t g_range_end   = 0;
static uint64_t g_min_block = PMM_MIN_ORDER_PAGE_SIZE;
static uint32_t g_max_order = 0;
static uint32_t g_order_count = 0;

// Free list heads per order. Store physical address of first free block, or EMPTY_SENTINEL for empty.
static uint64_t g_free_heads[PMM_MAX_ORDERS];
static const uint64_t EMPTY_SENTINEL = UINT64_MAX;

// Statistics
static pmm_stats_t g_stats;

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
 * validate_block_in_range - Check if a block is within managed range
 */
static inline bool validate_block_in_range(uint64_t block_phys, uint32_t order) {
    uint64_t block_size = order_to_size(order);
    
    if (block_phys < g_range_start) {
        LOGF("[PMM ERROR] Block 0x%lx below managed range (start: 0x%lx)\n",
               block_phys, g_range_start);
        return false;
    }
    
    if (block_phys + block_size > g_range_end) {
        LOGF("[PMM ERROR] Block 0x%lx + 0x%lx exceeds managed range (end: 0x%lx)\n",
               block_phys, block_size, g_range_end);
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
        g_stats.corruption_detected++;
        return false;
    }
    
    if (header->order != expected_order) {
        LOGF("[PMM ERROR] Order mismatch at 0x%lx: %u (expected %u)\n",
               block_phys, header->order, expected_order);
        g_stats.corruption_detected++;
        return false;
    }
    
    // Validate next pointer
    if (header->next_phys != EMPTY_SENTINEL && header->next_phys != 0) {
        if (header->next_phys < g_range_start || header->next_phys >= g_range_end) {
            LOGF("[PMM ERROR] Invalid next pointer at 0x%lx: 0x%lx (range: 0x%lx-0x%lx)\n",
                   block_phys, header->next_phys, g_range_start, g_range_end);
            g_stats.corruption_detected++;
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
        // Corruption detected - return EMPTY_SENTINEL to break the chain safely
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
 * clear_free_header - Clear header when allocating (detect use-after-free)
 */
static inline void clear_free_header(uint64_t block_phys) {
    pmm_free_header_t *header = (pmm_free_header_t *)PHYSMAP_P2V(block_phys);
    header->magic = 0;
    header->order = 0xFFFFFFFF;
    header->next_phys = 0xDEADBEEFDEADBEEF;
}

/*
 * pop_head - pop a block from the free list for given order, or EMPTY_SENTINEL if empty
 */
static uint64_t pop_head(uint32_t order) {
    uint64_t head = g_free_heads[order];
    if (head == EMPTY_SENTINEL) return EMPTY_SENTINEL;
    
    uint64_t next = read_next_word(head, order);
    g_free_heads[order] = (next == EMPTY_SENTINEL || next == 0) ? EMPTY_SENTINEL : next;
    
    // Clear the header to detect use-after-free
    clear_free_header(head);
    
    // Update statistics
    g_stats.free_blocks[order]--;
    
    return head;
}

/* 
 * push_head - push a block onto the free list for given order
 */
static void push_head(uint32_t order, uint64_t block_phys) {
    uint64_t head = g_free_heads[order];
    write_next_word(block_phys, (head == EMPTY_SENTINEL) ? EMPTY_SENTINEL : head, order);
    g_free_heads[order] = block_phys;
    
    // Update statistics
    g_stats.free_blocks[order]++;
}

/*
 * remove_specific - remove a specific block from the free list for given order
 */
static bool remove_specific(uint32_t order, uint64_t target_phys) {
    uint64_t prev = EMPTY_SENTINEL;
    uint64_t cur = g_free_heads[order];
    
    while (cur != EMPTY_SENTINEL) {
        // Validate before reading next
        if (!validate_free_header(cur, order)) {
            LOGF("[PMM] Corruption in remove_specific at 0x%lx\n", cur);
            return false;
        }
        
        uint64_t next = read_next_word(cur, order);
        
        if (cur == target_phys) {
            // Found it, remove from list
            if (prev == EMPTY_SENTINEL) {
                // Removing head
                g_free_heads[order] = (next == 0 || next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
            } else {
                // Removing middle/end - update previous node
                write_next_word(prev, next, order);
            }
            
            // Clear the removed block's header
            clear_free_header(cur);
            
            // Update statistics
            g_stats.free_blocks[order]--;
            
            return true;
        }
        
        prev = cur;
        cur = (next == 0 || next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
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
    if (min_block_size < sizeof(pmm_free_header_t)) {
        LOGF("[PMM] min_block_size (%lu) too small for header (%lu)\n",
               min_block_size, sizeof(pmm_free_header_t));
        return PMM_ERR_INVALID;
    }

    g_min_block = min_block_size;

    // Align start up to min_block and end down to min_block
    uint64_t start_aligned = (uint64_t)align_up(range_start_phys, g_min_block);
    uint64_t end_aligned = (uint64_t)align_down(range_end_phys, g_min_block);

    if (end_aligned <= start_aligned) {
        LOGF("[PMM] After alignment, range is empty\n");
        return PMM_ERR_INVALID;
    }

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

    // Initialize free lists
    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i)
        g_free_heads[i] = EMPTY_SENTINEL;

    // Initialize statistics
    memset(&g_stats, 0, sizeof(pmm_stats_t));

    // Partition the range into blocks
    partition_range_into_blocks(g_range_start, g_range_end);
    
    g_inited = true;

    LOGF("[PMM] PMM initialized, managing 0x%lx - 0x%lx (%zu MiB)\n",
           pmm_managed_base(), pmm_managed_end(), pmm_managed_size() / (1024 * 1024));

    return PMM_OK;
}

/*
 * pmm_shutdown - Reset state so pmm_init may be called again.
 */
void pmm_shutdown(void) {
    if (!g_inited) return;

    // Clear all free block headers in the managed range
    uint8_t *ptr = (uint8_t *)PHYSMAP_P2V(g_range_start);
    uint64_t size = pmm_managed_size();
    memset(ptr, 0, size);

    g_inited = false;
    g_range_start = 0;
    g_range_end = 0;
    g_min_block = PMM_MIN_ORDER_PAGE_SIZE;
    g_max_order = 0;
    g_order_count = 0;
    
    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i) 
        g_free_heads[i] = EMPTY_SENTINEL;
    
    memset(&g_stats, 0, sizeof(pmm_stats_t));

    LOGF("[PMM] PMM Shutdown\n");
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
    if (block == EMPTY_SENTINEL) return PMM_ERR_OOM;

    // split until we reach requested order
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

/*
 * pmm_alloc - Allocate a block large enough to satisfy size_bytes.
 */
pmm_status_t pmm_alloc(size_t size_bytes, uint64_t *out_phys) {
    if (out_phys == NULL) return PMM_ERR_INVALID;
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (size_bytes == 0) return PMM_ERR_INVALID;

    // Round up to multiple of g_min_block
    uint64_t rounded = (uint64_t)size_bytes;
    if (rounded & (g_min_block - 1)) rounded = align_up(rounded, g_min_block);

    uint32_t order = size_to_order(rounded);
    if (order > g_max_order) return PMM_ERR_OOM;

    g_stats.alloc_calls++;
    return alloc_block_of_order(order, out_phys);
}

/*
 * pmm_free - Free an allocation previously returned by pmm_alloc.
 */
pmm_status_t pmm_free(uint64_t phys, size_t size_bytes) {
    if (!g_inited) return PMM_ERR_NOT_INIT;
    if (size_bytes == 0) return PMM_ERR_INVALID;

    // Basic range check against [g_range_start, g_range_end)
    if (phys < g_range_start) {
        LOGF("[PMM ERROR] Free: address 0x%lx below managed range\n", phys);
        return PMM_ERR_OUT_OF_RANGE;
    }
    if (phys >= g_range_end) {
        LOGF("[PMM ERROR] Free: address 0x%lx above managed range\n", phys);
        return PMM_ERR_OUT_OF_RANGE;
    }

    // Round size in the same manner as allocation
    uint64_t rounded = (uint64_t)size_bytes;
    if (rounded & (g_min_block - 1)) rounded = align_up(rounded, g_min_block);

    uint32_t order = size_to_order(rounded);
    if (order > g_max_order) return PMM_ERR_INVALID;

    uint64_t block_addr = phys;
    uint64_t block_size = order_to_size(order);

    if ((block_addr & (block_size - 1)) != 0) {
        LOGF("[PMM ERROR] Free: address 0x%lx not aligned to size 0x%lx\n",
               block_addr, block_size);
        return PMM_ERR_NOT_ALIGNED;
    }

    g_stats.free_calls++;

    // Coalesce upwards where possible
    while (order < g_max_order) {
        uint64_t buddy = buddy_of(block_addr, order);
        uint64_t buddy_size = order_to_size(order);
        
        // Check if buddy is valid
        if (buddy < g_range_start || (buddy + buddy_size) > g_range_end) {
            break;
        }

        // If buddy is free at this order, remove it and coalesce
        bool found = remove_specific(order, buddy);
        if (!found) {
            // Buddy not free, push current block and exit
            push_head(order, block_addr);
            return PMM_OK;
        }

        // Buddy removed successfully - coalesce
        g_stats.coalesce_success++;
        
        // Merged block is min(block_addr, buddy)
        if (buddy < block_addr) block_addr = buddy;
        ++order;
    }

    // Push the resulting (possibly coalesced) block
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

    // Clamp to managed range
    if (start < g_range_start) start = g_range_start;
    if (end > g_range_end) end = g_range_end;
    if (start >= end) return PMM_ERR_INVALID;

    // Align to min block for safe handling
    uint64_t orig_start = start, orig_end = end;
    start = align_down(start, g_min_block);
    end   = align_up(end, g_min_block);
    
    if (start != orig_start || end != orig_end) {
        LOGF("[PMM] Adjusted reserved range [0x%lx, 0x%lx) to [0x%lx, 0x%lx)\n",
               orig_start, orig_end, start, end);
    }

    // For each order from max to min, scan free lists and remove overlapping blocks
    for (int32_t o = (int32_t)g_max_order; o >= 0; --o) {
        uint64_t block_size = order_to_size((uint32_t)o);
        uint64_t cur = g_free_heads[o];

        while (cur != EMPTY_SENTINEL) {
            uint64_t next = read_next_word(cur, (uint32_t)o);
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

            cur = (next == 0 || next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
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

    // Clamp to managed range
    if (start < g_range_start) start = g_range_start;
    if (end > g_range_end) end = g_range_end;
    if (start >= end) return PMM_ERR_INVALID;

    // Round start up and end down to min_block
    uint64_t orig_start = start, orig_end = end;
    start = align_up(start, g_min_block);
    end = align_down(end, g_min_block);
    
    if (start >= end) {
        LOGF("[PMM] After alignment, free range [0x%lx, 0x%lx) became empty\n",
               orig_start, orig_end);
        return PMM_ERR_INVALID;
    }
    
    if (start != orig_start || end != orig_end) {
        LOGF("[PMM] Adjusted free range [0x%lx, 0x%lx) to [0x%lx, 0x%lx)\n",
               orig_start, orig_end, start, end);
    }

    partition_range_into_blocks(start, end);
    return PMM_OK;
}

/*
 * pmm_get_stats - Get current PMM statistics
 */
void pmm_get_stats(pmm_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_stats;
}

/*
 * pmm_dump_stats - Print detailed PMM statistics
 */
void pmm_dump_stats(void) {
    if (!g_inited) {
        LOGF("[PMM] Not initialized\n");
        return;
    }
    
    LOGF("=== PMM Statistics ===\n");
    LOGF("Managed range: [0x%lx - 0x%lx) (0x%lx bytes, %.2f MiB)\n",
           g_range_start, g_range_end, g_range_end - g_range_start,
           (g_range_end - g_range_start) / (1024.0 * 1024.0));
    LOGF("Min block size: 0x%lx, Max order: %u\n", g_min_block, g_max_order);
    
    LOGF("\nOperation counts:\n");
    LOGF("  Allocations:      %lu\n", g_stats.alloc_calls);
    LOGF("  Frees:            %lu\n", g_stats.free_calls);
    LOGF("  Coalesces:        %lu\n", g_stats.coalesce_success);
    LOGF("  Corruptions:      %lu\n", g_stats.corruption_detected);
    
    LOGF("\nFree block distribution:\n");
    LOGF("Order  Size         Free Blocks\n");
    LOGF("-----  -----------  -----------\n");
    
    uint64_t total_free_bytes = 0;
    bool has_free_blocks = false;
    
    for (uint32_t o = 0; o <= g_max_order; o++) {
        uint64_t size = order_to_size(o);
        uint64_t count = g_stats.free_blocks[o];
        
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
}

/*
 * pmm_verify_integrity - Verify free-list integrity
 * Returns true if all checks pass, false otherwise
 */
bool pmm_verify_integrity(void) {
    if (!g_inited) {
        LOGF("[PMM] Not initialized\n");
        return false;
    }
    
    LOGF("[PMM] Checking free-list integrity...\n");
    
    bool all_ok = true;
    uint64_t counted_free[PMM_MAX_ORDERS] = {0};
    
    // Walk each free list and verify
    for (uint32_t order = 0; order <= g_max_order; order++) {
        uint64_t cur = g_free_heads[order];
        int count = 0;
        uint64_t size = order_to_size(order);
        
        while (cur != EMPTY_SENTINEL) {
            count++;
            counted_free[order]++;
            
            // Prevent infinite loops
            if (count > 100000) {
                LOGF("[PMM] Order %u: Possible infinite loop detected\n", order);
                all_ok = false;
                break;
            }
            
            // Validate the header
            if (!validate_free_header(cur, order)) {
                LOGF("[PMM] Order %u: Invalid header at block 0x%lx\n", order, cur);
                all_ok = false;
                break;
            }
            
            // Check alignment relative to block size
            // A block is properly aligned if its offset from rangeStart is aligned
            uint64_t offset = cur - g_range_start;
            if ((offset & (size - 1)) != 0) {
                LOGF("[PMM] Order %u: Block 0x%lx offset 0x%lx not aligned to size 0x%lx\n",
                       order, cur, offset, size);
                all_ok = false;
            }
            
            // Move to next
            uint64_t next = read_next_word(cur, order);
            cur = (next == 0 || next == EMPTY_SENTINEL) ? EMPTY_SENTINEL : next;
        }
    }
    
    // Compare counted blocks with statistics
    for (uint32_t order = 0; order <= g_max_order; order++) {
        if (counted_free[order] != g_stats.free_blocks[order]) {
            LOGF("[PMM] Order %u: Statistics mismatch (counted: %lu, stats: %lu)\n",
                   order, counted_free[order], g_stats.free_blocks[order]);
            all_ok = false;
        }
    }
    
    if (all_ok) {
        LOGF("[PMM] All checks passed\n");
    } else {
        LOGF("[PMM] FAILED - integrity compromised!\n");
    }
    
    return all_ok;
}