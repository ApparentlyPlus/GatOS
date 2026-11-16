/*
 * heap.c - Multi-Arena Kernel Heap Manager Implementation
 *
 * Arenas come from the kernel VMM (their structs from a slab cache), every allocation comes
 * zeroed, and kmalloc panics when it can't deliver.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/panic.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Dead weight without a heap to manage, see GATA_CAP_MEM in caps.h.
#ifdef GATA_CAP_MEM

// The kernel heap, live once heap_kernel_init ran
static heap_t kheap_s;
static heap_t* kheap = NULL;
static spinlock_t kheap_lock = {0};

static slab_cache_t* arena_cache = NULL;

/*
 * arena_get - A kernel VMM region for an arena, its struct from the slab
 */
static arena_t* arena_get(size_t size) {
    void* mem;
    if (slab_alloc(arena_cache, &mem) != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate arena structure\n");
        return NULL;
    }

    arena_t* arena = (arena_t*)mem;
    size = align_up(size, PAGE_SIZE);

    void* region = NULL;
    vmm_status_t status = vmm_alloc(NULL, size, VM_FLAG_WRITE, NULL, &region);
    if (status != VMM_OK) {
        LOGF("[HEAP] Failed to allocate arena memory: vmm error %d\n", status);
        slab_free(arena_cache, mem);
        return NULL;
    }

    arena->start = (uintptr_t)region;
    arena->end = arena->start + size;
    arena->size = size;
    return arena;
}

/*
 * arena_put - Gives an arena's region and struct back
 */
static void arena_put(arena_t* arena) {
    vmm_status_t status = vmm_free(NULL, (void*)arena->start);
    if (status != VMM_OK) {
        LOGF("[HEAP WARNING] Failed to free arena memory: vmm error %d\n", status);
    }
    slab_free(arena_cache, arena);
}

/*
 * align_size - Align up to HEAP_MIN_ALIGN, 0 on overflow
 */
static size_t align_size(size_t size) {
    if (size > SIZE_MAX - (HEAP_MIN_ALIGN - 1)) return 0;
    return (size + HEAP_MIN_ALIGN - 1) & ~(size_t)(HEAP_MIN_ALIGN - 1);
}

static inline blk_foot_t* get_footer(blk_hdr_t* header) {
    // Footer sits right after header + user data
    return (blk_foot_t*)((uint8_t*)header + sizeof(blk_hdr_t) + header->size);
}

static inline void* get_user_ptr(blk_hdr_t* header) {
    // Pointer returned to caller (first byte of user data)
    return (void*)((uint8_t*)header + sizeof(blk_hdr_t));
}

/*
 * get_header_from_ptr - Self explanatory
 */
static inline blk_hdr_t* get_header_from_ptr(void* ptr) {
    // If ptr NULL, bail early
    if (!ptr) return NULL;
    return (blk_hdr_t*)((uint8_t*)ptr - sizeof(blk_hdr_t));
}

/*
 * blk_ok - Validate a single block's integrity
 */
static bool blk_ok(blk_hdr_t* header) {
    if (!header) return false;

    // quick magic check (accept either used or free magic)
    if (header->magic != BLOCK_MAGIC_USED &&
        header->magic != BLOCK_MAGIC_FREE) {
        LOGF("[HEAP ERROR] Invalid block magic: 0x%x at %p\n", header->magic,
             header);
        return false;
    }

    // check red zones in the header
    if (header->red_zone_pre != BLOCK_RED_ZONE ||
        header->red_zone_post != BLOCK_RED_ZONE) {
        LOGF("[HEAP ERROR] Block red-zone corrupted at %p\n", header);
        return false;
    }

    blk_foot_t* footer = get_footer(header);

    // footer magic must match header magic (this is a cheap integrity test)
    if (footer->magic != header->magic) {
        LOGF("[HEAP ERROR] Footer magic mismatch at %p\n", header);
        return false;
    }

    // footer red zones
    if (footer->red_zone_pre != BLOCK_RED_ZONE ||
        footer->red_zone_post != BLOCK_RED_ZONE) {
        LOGF("[HEAP ERROR] Footer red-zone corrupted at %p\n", header);
        return false;
    }

    // footer should point back to the header we expected
    if (footer->header != header) {
        LOGF("[HEAP ERROR] Footer header pointer mismatch at %p\n", header);
        return false;
    }

    return true;
}

/*
 * heap_validate - Validate whether a heap blob has the correct magic
 */
static inline bool heap_validate(heap_t* heap) {
    if (!heap) return false;

    if (heap->magic != HEAP_MAGIC) {
        LOGF("[HEAP ERROR] Invalid heap magic: 0x%x\n", heap->magic);
        return false;
    }

    return true;
}

/*
 * arena_validate - Validate whether an arena blob has the correct magic
 */
static inline bool arena_validate(arena_t* arena) {
    if (!arena) return false;

    if (arena->magic != ARENA_MAGIC) {
        LOGF("[HEAP ERROR] Invalid arena magic: 0x%x\n", arena->magic);
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Statistics Management

/*
 * stat_mark_used - Update stats when marking a block as used
 */
static inline void stat_mark_used(heap_t* heap, blk_hdr_t* block) {
    if (!block->arena) return;

    block->arena->total_free -= block->size;
    block->arena->total_allocated += block->size;

    heap->total_free -= block->size;
    heap->total_allocated += block->size;
    heap->allocation_count++;
}

/*
 * stat_mark_free - Update stats when marking a block as free
 */
static inline void stat_mark_free(heap_t* heap, blk_hdr_t* block) {
    if (!block->arena) return;

    block->arena->total_allocated -= block->size;
    block->arena->total_free += block->size;

    heap->total_allocated -= block->size;
    heap->total_free += block->size;
    heap->allocation_count--;
}

/*
 * stat_absorb - Update stats when one block absorbs another
 */
static inline void stat_absorb(heap_t* heap, blk_hdr_t* survivor, blk_hdr_t* absorbed) {
    // The absorbed block's header and footer are reclaimed as usable space.
    // This overhead is ADDED to the total free space.
    size_t overhead = sizeof(blk_hdr_t) + sizeof(blk_foot_t);

    if (survivor->arena) {
        survivor->arena->total_free += overhead;
    }
    heap->total_free += overhead;
}

/*
 * stat_split - Update stats when splitting a block
 */
static inline void stat_split(heap_t* heap, blk_hdr_t* block) {
    size_t overhead = sizeof(blk_hdr_t) + sizeof(blk_foot_t);

    if (block->arena) {
        block->arena->total_free -= overhead;
    }
    heap->total_free -= overhead;
}

/*
 * stat_arena_add - Update stats when adding a new arena
 */
static inline void stat_arena_add(heap_t* heap, arena_t* arena, size_t usable_size) {
    arena->total_free = usable_size;
    arena->total_allocated = 0;

    heap->current_size += arena->size;
    heap->total_free += usable_size;
    heap->arena_count++;
}

/*
 * stat_arena_rm - Update stats when removing an arena
 */
static inline void stat_arena_rm(heap_t* heap, arena_t* arena) {
    heap->current_size -= arena->size;
    heap->total_free -= arena->total_free;
    heap->arena_count--;
}

#pragma endregion

#pragma region Free List Management

/*
 * bin_index - Map a block size to its segregated bin
 */
static inline uint32_t bin_index(size_t size) {
    if (size < HEAP_MIN_ALIGN) size = HEAP_MIN_ALIGN;
    uint32_t i = (uint32_t)(63 - __builtin_clzll((uint64_t)(size >> 4)));
    return i < HEAP_BIN_COUNT ? i : HEAP_BIN_COUNT - 1;
}

/*
 * freelist_remove - Unlink a block from its size bin
 */
static void freelist_remove(heap_t* heap, blk_hdr_t* block) {
    if (!heap || !block) return;

    if (block->prev_free && block->prev_free->next_free != block) {
        panic("heap: safe-unlink violation (prev->next != block)");
    }
    if (block->next_free && block->next_free->prev_free != block) {
        panic("heap: safe-unlink violation (next->prev != block)");
    }

    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        heap->bins[bin_index(block->size)] = block->next_free;
    }

    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }

    // clear pointers to reduce accidental reuse bugs later
    block->next_free = NULL;
    block->prev_free = NULL;
}

/*
 * freelist_insert - Push block onto its size bin (LIFO, O(1))
 */
static void freelist_insert(heap_t* heap, blk_hdr_t* block) {
    if (!heap || !block) return;

    uint32_t bin = bin_index(block->size);
    block->prev_free = NULL;
    block->next_free = heap->bins[bin];

    if (heap->bins[bin]) {
        heap->bins[bin]->prev_free = block;
    }

    heap->bins[bin] = block;
}

#pragma endregion

#pragma region Arena Management

/*
 * next_block - Get the block immediately after given block
 */
static blk_hdr_t* next_block(
    blk_hdr_t* block) {
    if (!block || !block->arena) return NULL;

    uintptr_t next_addr = (uintptr_t)block + block->total_size;

    if (next_addr >= block->arena->end) {
        return NULL;
    }

    return (blk_hdr_t*)next_addr;
}

/*
 * prev_block - Get the block immediately preceding given block
 */
static blk_hdr_t* prev_block(
    blk_hdr_t* block) {
    if (!block || !block->arena) return NULL;

    uintptr_t prev_footer_addr = (uintptr_t)block - sizeof(blk_foot_t);

    if (prev_footer_addr < block->arena->start) {
        return NULL;
    }

    blk_foot_t* prev_footer = (blk_foot_t*)prev_footer_addr;

    // quick sanity check on red zones in footer
    if (prev_footer->red_zone_pre != BLOCK_RED_ZONE ||
        prev_footer->red_zone_post != BLOCK_RED_ZONE) {
        return NULL;
    }

    return prev_footer->header;
}

/*
 * arena_add - Turns a fresh arena into one big free block and puts it on the heap.
 */
static arena_t* arena_add(heap_t* heap, size_t size) {
    arena_t* arena = arena_get(size);
    if (!arena) return NULL;

    arena->magic = ARENA_MAGIC;
    arena->next = NULL;
    arena->prev = NULL;

    blk_hdr_t* initial_block = (blk_hdr_t*)arena->start;
    size_t block_payload_size = (arena->end - arena->start) - sizeof(blk_hdr_t) - sizeof(blk_foot_t);

    // initialize the single big free block that spans the entire arena
    initial_block->magic = BLOCK_MAGIC_FREE;
    initial_block->red_zone_pre = BLOCK_RED_ZONE;
    initial_block->red_zone_post = BLOCK_RED_ZONE;
    initial_block->size = block_payload_size;
    initial_block->total_size = arena->end - arena->start;
    initial_block->arena = arena;
    initial_block->next_free = NULL;
    initial_block->prev_free = NULL;

    // set up the footer for this block
    blk_foot_t* initial_footer = get_footer(initial_block);
    initial_footer->red_zone_pre = BLOCK_RED_ZONE;
    initial_footer->red_zone_post = BLOCK_RED_ZONE;
    initial_footer->header = initial_block;
    initial_footer->magic = BLOCK_MAGIC_FREE;

    arena->first_block = initial_block;

    if (!heap->arenas) {
        heap->arenas = arena;
    } else {
        arena_t* tail = heap->arenas;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = arena;
        arena->prev = tail;
    }

    // update stats and add initial block to free list
    stat_arena_add(heap, arena, block_payload_size);
    freelist_insert(heap, initial_block);

    LOGF("[HEAP] Created arena at 0x%lx - 0x%lx (size: %zu bytes)\n", arena->start, arena->end, arena->size);
    return arena;
}

/*
 * arena_drop - Takes an empty arena off the heap and hands it back through arena_put
 */
static void arena_drop(heap_t* heap, arena_t* arena) {
    if (!heap || !arena) return;
    if (!arena_validate(arena)) return;

    // find and remove any blocks from the free bins that belong to this arena
    for (uint32_t bin = 0; bin < HEAP_BIN_COUNT; bin++) {
        blk_hdr_t* cur = heap->bins[bin];
        while (cur) {
            blk_hdr_t* next = cur->next_free;

            if (cur->arena == arena) {
                freelist_remove(heap, cur);
            }

            cur = next;
        }
    }

    stat_arena_rm(heap, arena);

    if (arena->prev) {
        arena->prev->next = arena->next;
    } else {
        heap->arenas = arena->next;
    }

    if (arena->next) {
        arena->next->prev = arena->prev;
    }

    arena->magic = 0;
    arena_put(arena);
}

/*
 * try_shrink_arena - Attempt to destroy an arena if it's unused/empty
 */
static void try_shrink_arena(heap_t* heap, arena_t* arena) {
    if (!heap || !arena) return;
    if (!arena_validate(arena)) return;

    // don't remove the last arena
    if (heap->arena_count <= 1) return;

    // only consider entirely empty arenas
    if (arena->total_allocated > 0) return;

    // only shrink when free >> allocated, though this is just a huristic
    if (heap->total_free < heap->total_allocated * HEAP_SHRINK_THRESHOLD)
        return;

    // check if the arena is entirely free (plus header/footer overhead)
    if (arena->total_free + sizeof(blk_hdr_t) + sizeof(blk_foot_t) >= arena->end - arena->start) {
        LOGF("[HEAP] Destroying empty arena at 0x%lx - 0x%lx\n", arena->start, arena->end);
        arena_drop(heap, arena);
    }
}

#pragma endregion

#pragma region Block Coalescing

/*
 * coalesce_blocks - Attempt to merge adjacent free blocks to reduce fragmentation
 */
static blk_hdr_t* coalesce_blocks(heap_t* heap,
                                            blk_hdr_t* block) {
    if (!heap || !block) return block;
    if (!blk_ok(block)) return block;

    // try merging with next block first (arbitrary choice, could do prev first or both iteratively)
    blk_hdr_t* next = next_block(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && blk_ok(next)) {
        freelist_remove(heap, block);
        freelist_remove(heap, next);

        stat_absorb(heap, block, next);

        block->size += next->total_size;
        block->total_size += next->total_size;

        blk_foot_t* footer = get_footer(block);
        footer->header = block;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;

        freelist_insert(heap, block);

        return coalesce_blocks(heap, block);
    }

    // try merging with previous block
    blk_hdr_t* prev = prev_block(block);
    if (prev && prev->magic == BLOCK_MAGIC_FREE && blk_ok(prev)) {
        freelist_remove(heap, block);
        freelist_remove(heap, prev);

        stat_absorb(heap, prev, block);

        prev->size += block->total_size;
        prev->total_size += block->total_size;

        blk_foot_t* footer = get_footer(prev);
        footer->header = prev;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;

        freelist_insert(heap, prev);

        return coalesce_blocks(heap, prev);
    }

    return block;
}

#pragma endregion

#pragma region Allocation/Deallocation

/*
 * find_free_block - Scan the size bins for a fit. The block's own bin can hold smaller blocks so it gets scanned, any higher bin is big enough
 */
static blk_hdr_t* find_free_block(heap_t* heap, size_t size) {
    if (!heap) return NULL;

    for (uint32_t bin = bin_index(size); bin < HEAP_BIN_COUNT; bin++) {
        blk_hdr_t* cur = heap->bins[bin];

        while (cur) {
            if (!blk_ok(cur)) {
                LOGF("[HEAP ERROR] Corrupted block in free list\n");
                return NULL;
            }

            if (cur->size >= size) {
                return cur;
            }

            cur = cur->next_free;
        }
    }

    return NULL;
}

/*
 * split_block - Split block so first part has requested size, remainder as new free block
 */
static void split_block(heap_t* heap, blk_hdr_t* block, size_t size) {
    if (!block || !heap) return;

    size_t remaining = block->size - size;
    size_t overhead = sizeof(blk_hdr_t) + sizeof(blk_foot_t);

    if (remaining < MIN_BLOCK_SIZE + overhead) {
        return;
    }

    bool was_in_free_list = (block->magic == BLOCK_MAGIC_FREE);

    if (was_in_free_list) {
        // Option numero uno, splitting a FREE block
        // We are consuming free space. The overhead reduces the total free space available.
        freelist_remove(heap, block);
        stat_split(heap, block); 
    } else {
        // Option numero dos, splitting a USED block (realloc shrinking path)
        
        // We are creating new free space from used space
        // We decrease allocated by the total chunk size we chopped off (remaining)
        // We increase free by the new payload size (remaining - overhead)
        // The overhead is implicitly accounted for as "neither free nor alloc"
        
        size_t new_free_payload = remaining - overhead;

        if (block->arena) {
            block->arena->total_allocated -= remaining;
            block->arena->total_free += new_free_payload;
        }
        heap->total_allocated -= remaining;
        heap->total_free += new_free_payload;
    }

    // shrink current block to requested size
    block->size = size;
    block->total_size =
        sizeof(blk_hdr_t) + size + sizeof(blk_foot_t);

    // refresh footer for the first (now smaller) block
    blk_foot_t* footer = get_footer(block);
    footer->red_zone_pre = BLOCK_RED_ZONE;
    footer->red_zone_post = BLOCK_RED_ZONE;
    footer->header = block;
    footer->magic = block->magic;

    // create a new block at the end to represent the leftover free portion
    blk_hdr_t* new_block = (blk_hdr_t*)((uint8_t*)block + block->total_size);

    new_block->magic = BLOCK_MAGIC_FREE;
    new_block->red_zone_pre = BLOCK_RED_ZONE;
    new_block->red_zone_post = BLOCK_RED_ZONE;

    // The new block's payload size is remaining minus its own header/footer
    new_block->size = remaining - sizeof(blk_hdr_t) - sizeof(blk_foot_t);
    new_block->total_size = remaining;
    new_block->arena = block->arena;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;

    // footer for the new block
    blk_foot_t* new_footer = get_footer(new_block);
    new_footer->red_zone_pre = BLOCK_RED_ZONE;
    new_footer->red_zone_post = BLOCK_RED_ZONE;
    new_footer->header = new_block;
    new_footer->magic = BLOCK_MAGIC_FREE;

    // put new block onto free list
    freelist_insert(heap, new_block);

    // if the original was free, reinsert the (now smaller) block too
    if (was_in_free_list) {
        freelist_insert(heap, block);
    }
}

/*
 * heap_alloc - Core alloc, NULL when it can't (the reason goes to the debug log). Lock held
 */
static void* heap_alloc(heap_t* heap, size_t size, bool zero) {
    if (!heap || size == 0 || !heap_validate(heap)) return NULL;

    size_t orig_size = size;
    size = align_size(size);
    if (size == 0 && orig_size > 0) {
        LOGF("[HEAP] Size overflow: %zu\n", orig_size);
        return NULL;
    }
    
    if (size < MIN_BLOCK_SIZE) {
        size = MIN_BLOCK_SIZE;
    }

    blk_hdr_t* block = find_free_block(heap, size);

    // if we couldn't find a big enough block, we need to create a new arena and try again
    if (!block) {
        size_t needed = size + sizeof(blk_hdr_t) + sizeof(blk_foot_t);
        if (!arena_add(heap, needed > MIN_ARENA_SIZE ? needed : MIN_ARENA_SIZE)) {
            LOGF("[HEAP] Failed to create an arena for %zu bytes\n", needed);
            return NULL;
        }
        block = find_free_block(heap, size);
        if (!block) return NULL;
    }

    // at this point we have a valid block that can satisfy the request, but it might be larger than needed, so we split it
    split_block(heap, block, size);

    // mark block as used and remove from free list
    freelist_remove(heap, block);

    // update magic and stats
    block->magic = BLOCK_MAGIC_USED;
    blk_foot_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_USED;

    stat_mark_used(heap, block);

    void* user_ptr = get_user_ptr(block);
    if (zero) kmemset(user_ptr, 0, block->size);
    return user_ptr;
}

/*
 * heap_release - Core free. Lock held
 */
static void heap_release(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return;
    if (!heap_validate(heap)) return;

    blk_hdr_t* block = get_header_from_ptr(ptr);

    if (!blk_ok(block)) {
        LOGF("[HEAP ERROR] Attempted to free invalid block at %p\n", ptr);
        return;
    }

    if (block->magic != BLOCK_MAGIC_USED) {
        LOGF("[HEAP ERROR] Double free or invalid free at %p (magic: 0x%x)\n", ptr, block->magic);
        return;
    }

    block->magic = BLOCK_MAGIC_FREE;
    block->next_free = NULL;
    block->prev_free = NULL;

    blk_foot_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_FREE;

    stat_mark_free(heap, block);

    freelist_insert(heap, block);

    block = coalesce_blocks(heap, block);

    if (block->arena) {
        try_shrink_arena(heap, block->arena);
    }
}

/*
 * heap_resize - Core realloc
 */
static void* heap_resize(heap_t* heap, void* ptr, size_t size, bool zero) {
    blk_hdr_t* block = get_header_from_ptr(ptr);
    if (!blk_ok(block) || block->magic != BLOCK_MAGIC_USED) {
        LOGF("[HEAP] realloc: invalid block at %p\n", ptr);
        return NULL;
    }

    // if the current block is already big enough, just return the same pointer 
    // (after maybe splitting off a new free block if it's much bigger than needed)
    size_t aligned_size = align_size(size);
    if (aligned_size == 0) return NULL;
    if (aligned_size < MIN_BLOCK_SIZE) aligned_size = MIN_BLOCK_SIZE;

    if (aligned_size <= block->size) {
        if (block->size - aligned_size >= MIN_BLOCK_SIZE + sizeof(blk_hdr_t) + sizeof(blk_foot_t)) {
            split_block(heap, block, aligned_size);
        }
        return ptr;
    }

    // try to expand into adjacent free block if possible before resorting to allocating a new block and copying
    blk_hdr_t* next = next_block(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && blk_ok(next)) {
        size_t combined_size = block->size + next->total_size;
        if (combined_size >= aligned_size) {
            freelist_remove(heap, next);

            // Manual stats update for Used-Absorbs-Free
            size_t reclaimed_overhead = sizeof(blk_hdr_t) + sizeof(blk_foot_t);
            if (block->arena) {
                block->arena->total_free -= next->size;
                block->arena->total_allocated += (next->size + reclaimed_overhead);
            }
            heap->total_free -= next->size;
            heap->total_allocated += (next->size + reclaimed_overhead);

            size_t old_size = block->size;
            block->size = combined_size;
            block->total_size += next->total_size;

            blk_foot_t* footer = get_footer(block);
            footer->header = block;
            footer->magic = BLOCK_MAGIC_USED;
            footer->red_zone_pre = BLOCK_RED_ZONE;
            footer->red_zone_post = BLOCK_RED_ZONE;

            split_block(heap, block, aligned_size);
            if (zero) kmemset((uint8_t*)ptr + old_size, 0, block->size - old_size);
            return ptr;
        }
    }

    void* new_ptr = heap_alloc(heap, size, zero);
    if (!new_ptr) return NULL;
    kmemcpy(new_ptr, ptr, block->size < size ? block->size : size);
    heap_release(heap, ptr);
    return new_ptr;
}

/*
 * heap_align_size - Align up to HEAP_MIN_ALIGN, 0 on overflow
 */
size_t heap_align_size(size_t size) {
    return align_size(size);
}

/*
 * heap_validate_blk - Validate a single block's integrity
 */
bool heap_validate_blk(blk_hdr_t* header) {
    return blk_ok(header);
}

#pragma region Kernel Heap Management

heap_status_t heap_kernel_init(void) {
    if (kheap) return HEAP_ERR_ALREADY_INIT;

    if (!vmm_kernel_get()) {
        LOGF("[HEAP] The kernel VMM must be online first\n");
        return HEAP_ERR_NOT_INIT;
    }

    arena_cache = slab_cache_create("arena_t", sizeof(arena_t), _Alignof(arena_t));
    if (!arena_cache) {
        LOGF("[HEAP] Failed to create arena slab cache\n");
        return HEAP_ERR_OOM;
    }

    kheap_s.magic = HEAP_MAGIC;

    // create the initial arena to get things rolling
    arena_t* initial_arena = arena_add(&kheap_s, MIN_ARENA_SIZE);
    if (!initial_arena) {
        LOGF("[HEAP] Failed to create initial arena\n");
        return HEAP_ERR_VMM_FAIL;
    }

    kheap = &kheap_s;
    LOGF("[HEAP] Kernel heap initialized with arena at 0x%lx - 0x%lx\n", initial_arena->start, initial_arena->end);
    return HEAP_OK;
}

/*
 * heap_kernel_get - The kernel heap, NULL before heap_kernel_init
 */
heap_t* heap_kernel_get(void) {
    return kheap;
}

/*
 * kmalloc - Zeroed kernel allocation. Running out here is fatal, so it never returns NULL
 */
void* kmalloc(size_t size) {
    if (!kheap) panic("kmalloc before the kernel heap is up");
    if (size == 0) return NULL;

    bool flags = spinlock_acquire(&kheap_lock);
    void* result = heap_alloc(kheap, size, true);
    spinlock_release(&kheap_lock, flags);

    if (!result) panicf("[HEAP] kmalloc(%zu) failed", size);
    return result;
}

/*
 * kfree - Give a kmalloc'd block back
 */
void kfree(void* ptr) {
    if (!ptr || !kheap) return;
    bool flags = spinlock_acquire(&kheap_lock);
    heap_release(kheap, ptr);
    spinlock_release(&kheap_lock, flags);
}

/*
 * krealloc - Kernel realloc, the grown part comes zeroed
 */
void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    if (!kheap) return NULL;

    bool flags = spinlock_acquire(&kheap_lock);
    void* result = heap_resize(kheap, ptr, size, true);
    spinlock_release(&kheap_lock, flags);
    return result;
}

/*
 * kcalloc - Kernel calloc, NULL on overflow or when out of memory
 */
void* kcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || !kheap) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        LOGF("[HEAP] kcalloc: overflow detected (nmemb=%zu, size=%zu)\n", nmemb, size);
        return NULL;
    }

    bool flags = spinlock_acquire(&kheap_lock);
    void* result = heap_alloc(kheap, total, true);
    spinlock_release(&kheap_lock, flags);
    return result;
}

#pragma endregion

#pragma region Introspection and Debugging

/*
 * heap_check - Verify heap data structures and consistency
 */
heap_status_t heap_check(heap_t* heap) {
    if (!heap_validate(heap)) {
        return HEAP_ERR_INVALID;
    }

    bool lock_flags = spinlock_acquire(&kheap_lock);

    size_t calculated_free = 0;
    size_t calculated_used = 0;
    size_t free_blocks = 0;
    size_t used_blocks = 0;
    size_t arena_count = 0;

    arena_t* arena = heap->arenas;
    while (arena) {
        if (!arena_validate(arena)) {
            LOGF("[HEAP INTEGRITY] Arena validation failed at %p\n", arena);
            spinlock_release(&kheap_lock, lock_flags);
            return HEAP_ERR_CORRUPTED;
        }

        arena_count++;

        size_t arena_calculated_free = 0;
        size_t arena_calculated_used = 0;

        uintptr_t current_addr = arena->start;

        while (current_addr < arena->end) {
            blk_hdr_t* block = (blk_hdr_t*)current_addr;

            if (!blk_ok(block)) {
                LOGF(
                    "[HEAP INTEGRITY] Block validation failed at 0x%lx in "
                    "arena %p\n",
                    current_addr, arena);
                spinlock_release(&kheap_lock, lock_flags);
                return HEAP_ERR_CORRUPTED;
            }

            if (block->magic == BLOCK_MAGIC_FREE) {
                calculated_free += block->size;
                arena_calculated_free += block->size;
                free_blocks++;
            } else if (block->magic == BLOCK_MAGIC_USED) {
                calculated_used += block->size;
                arena_calculated_used += block->size;
                used_blocks++;
            }

            current_addr += block->total_size;
        }

        if (arena_calculated_free != arena->total_free || 
            arena_calculated_used != arena->total_allocated) {
            LOGF("[HEAP INTEGRITY] Arena %p statistics mismatch\n", arena);
            spinlock_release(&kheap_lock, lock_flags);
            return HEAP_ERR_CORRUPTED;
        }

        arena = arena->next;
    }

    if (calculated_free != heap->total_free || calculated_used != heap->total_allocated) {
        LOGF("[HEAP INTEGRITY] Heap statistics mismatch\n");
        spinlock_release(&kheap_lock, lock_flags);
        return HEAP_ERR_CORRUPTED;
    }

    spinlock_release(&kheap_lock, lock_flags);
    return HEAP_OK;
}

void heap_dump(heap_t* heap) {
    if (!heap_validate(heap)) {
        LOGF("[HEAP DUMP] Invalid heap\n");
        return;
    }

    bool lock_flags = spinlock_acquire(&kheap_lock);

    LOGF("Kernel heap at %p\n", heap);
    log_indent(1);
    LOGF("size      %zu bytes across %zu arenas\n", heap->current_size, heap->arena_count);
    LOGF("used      %zu bytes in %zu blocks\n", heap->total_allocated, heap->allocation_count);
    LOGF("free      %zu bytes\n", heap->total_free);

    int arena_num = 0;
    for (arena_t* a = heap->arenas; a; a = a->next, arena_num++) {
        if (!arena_validate(a)) {
            LOGF("[corrupted arena]\n");
            break;
        }

        LOGF("arena %d   0x%lx - 0x%lx (%zu bytes, %zu free, %zu used)\n",
             arena_num, a->start, a->end, a->size, a->total_free, a->total_allocated);
        log_indent(1);

        // only print first few blocks so the klog doesn't explode
        uintptr_t current_addr = a->start;
        int block_num = 0;
        for (; current_addr < a->end && block_num < 10; block_num++) {
            blk_hdr_t* block = (blk_hdr_t*)current_addr;
            if (!blk_ok(block)) {
                LOGF("[corrupted block]\n");
                break;
            }
            LOGF("block %-2d  %s %zu bytes\n", block_num,
                 block->magic == BLOCK_MAGIC_FREE ? "free" : "used", block->size);
            current_addr += block->total_size;
        }
        if (current_addr < a->end) LOGF("...\n");
        log_indent(-1);
    }
    log_indent(-1);
    spinlock_release(&kheap_lock, lock_flags);
}

/*
 * heap_stats - Return basic heap statistics
 */
void heap_stats(heap_t* heap, size_t* total, size_t* used, size_t* free, size_t* overhead) {
    if (!heap_validate(heap)) return;

    bool lock_flags = spinlock_acquire(&kheap_lock);

    if (total) *total = heap->current_size;
    if (used) *used = heap->total_allocated;
    if (free) *free = heap->total_free;
    if (overhead) {
        *overhead =
            heap->current_size - heap->total_allocated - heap->total_free;
    }

    spinlock_release(&kheap_lock, lock_flags);
}

/*
 * heap_alloc_sz - Get allocation size of a pointer in a heap
 */
size_t heap_alloc_sz(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return 0;
    if (!heap_validate(heap)) return 0;

    bool lock_flags = spinlock_acquire(&kheap_lock);

    blk_hdr_t* block = get_header_from_ptr(ptr);

    if (!blk_ok(block)) {
        spinlock_release(&kheap_lock, lock_flags);
        return 0;
    }
    if (block->magic != BLOCK_MAGIC_USED) {
        spinlock_release(&kheap_lock, lock_flags);
        return 0;
    }

    size_t size = block->size;
    spinlock_release(&kheap_lock, lock_flags);
    return size;
}

#pragma endregion
#endif // GATA_CAP_MEM
