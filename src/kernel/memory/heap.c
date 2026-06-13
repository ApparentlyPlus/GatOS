/*
 * heap.c - Multi-Arena Kernel Heap Manager Implementation
 * 
 * This implementation provides a robust heap manager supporting multiple arenas,
 * block coalescing, and comprehensive integrity checking. Each heap maintains
 * separate arenas with free lists sorted by size for efficient allocation.
 * The global kernel heap is automatically initialized on first use.
 * 
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/panic.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Magic numbers for validation
#define HEAP_MAGIC 0xF005BA11
#define ARENA_MAGIC 0x1CEB00DA
#define BLOCK_MAGIC_USED 0xABADCAFE
#define BLOCK_MAGIC_FREE 0xA110CA7E
#define BLOCK_RED_ZONE 0x8BADF00D

// Block alignment
#define BLOCK_ALIGN 16
#define MIN_BLOCK_SIZE 32

// Arena management
#define MIN_ARENA_SIZE (64 * 1024)
#define HEAP_EXPAND_FACTOR 2
#define HEAP_SHRINK_THRESHOLD 4

// Forward declarations
typedef struct arena arena_t;
typedef struct blk_hdr blk_hdr_t;

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

// Arena structure
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

// Heap structure
struct heap {
    uint32_t magic;
    vmm_t* vmm;

    arena_t* arenas;
    blk_hdr_t* free_list;

    size_t min_arena_size;
    size_t max_size;
    size_t current_size;

    uint32_t flags;
    bool is_kernel;

    size_t total_allocated;
    size_t total_free;
    size_t allocation_count;
    size_t arena_count;

    spinlock_t lock;
};

// Global kernel heap
static heap_t* kheap = NULL;
static bool kheap_initing = false;
static spinlock_t heap_lock = {0}; // Static zero init for first boot

// Slab caches
static slab_cache_t* heap_cache = NULL;
static slab_cache_t* arena_cache = NULL;

#pragma region Utility Functions

/*
 * heap_align_size - Align up to BLOCK_ALIGN
 */
size_t heap_align_size(size_t size) {
    if (size > SIZE_MAX - (BLOCK_ALIGN - 1)) return 0;
    return align_up(size, BLOCK_ALIGN);
}

/*
 * get_footer - Return the footer of a block
 */
static inline blk_foot_t* get_footer(blk_hdr_t* header) {
    // Footer sits right after header + user data
    return (blk_foot_t*)((uint8_t*)header + sizeof(blk_hdr_t) + header->size);
}

/*
 * get_user_ptr - Return a pointer to the user data of a block
 */
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
 * heap_validate_blk - Validate a single block's integrity
 */
bool heap_validate_blk(blk_hdr_t* header) {
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
 * freelist_remove - Unlink a block from the heap's free list
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
        heap->free_list = block->next_free;
    }

    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }

    // clear pointers to reduce accidental reuse bugs later
    block->next_free = NULL;
    block->prev_free = NULL;
}

/*
 * freelist_insert - Insert block into sorted-by-size free list
 */
static void freelist_insert(heap_t* heap, blk_hdr_t* block) {
    if (!heap || !block) return;

    block->next_free = NULL;
    block->prev_free = NULL;

    if (!heap->free_list) {
        heap->free_list = block;
        return;
    }

    if (block->size <= heap->free_list->size) {
        block->next_free = heap->free_list;
        heap->free_list->prev_free = block;
        heap->free_list = block;
        return;
    }

    blk_hdr_t* cursor = heap->free_list;
    while (cursor->next_free && cursor->next_free->size < block->size) {
        cursor = cursor->next_free;
    }

    block->next_free = cursor->next_free;
    block->prev_free = cursor;

    if (cursor->next_free) {
        cursor->next_free->prev_free = block;
    }

    cursor->next_free = block;
}

#pragma endregion

#pragma region Arena Management

/*
 * find_arena - Find which arena an address belongs to
 */
static arena_t* find_arena(heap_t* heap, uintptr_t addr) {
    if (!heap) return NULL;

    arena_t* a = heap->arenas;
    while (a) {
        if (!arena_validate(a)) {
            LOGF("[HEAP ERROR] Corrupted arena in list\n");
            return NULL;
        }

        if (addr >= a->start && addr < a->end) {
            return a;
        }

        a = a->next;
    }

    return NULL;
}

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
 * create_arena - Allocate and initialize an arena of at least given size
 */
static arena_t* create_arena(heap_t* heap, size_t size) {

    // This is the crux of the kheap design
    // Because arenas are separate contiguous virtual memory regions, we can easily
    // create/destroy them as needed to expand/contract the heap, and we can also
    // maintain separate metadata for each arena which simplifies integrity checking

    if (!heap || size == 0) return NULL;

    if (size < heap->min_arena_size) {
        size = heap->min_arena_size;
    }

    size = align_up(size, PAGE_SIZE);

    if (heap->current_size + size > heap->max_size) {
        LOGF("[HEAP] Cannot create arena: would exceed max heap size\n");
        return NULL;
    }

    void* arena_struct_mem;
    slab_status_t slab_status = slab_alloc(arena_cache, &arena_struct_mem);
    if (slab_status != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate arena structure\n");
        return NULL;
    }

    arena_t* arena = (arena_t*)arena_struct_mem;

    // conservative initialization
    kmemset(arena, 0, sizeof(arena_t));

    void* arena_region = NULL;
    size_t vmm_flags = VM_FLAG_WRITE | (heap->is_kernel ? 0 : VM_FLAG_USER);
    if (heap->flags & HEAP_FLAG_EXECUTABLE) {
        vmm_flags |= VM_FLAG_EXEC;
    }

    vmm_status_t vmm_status = vmm_alloc(
        heap->vmm, size, vmm_flags,
        NULL, &arena_region);

    if (vmm_status != VMM_OK) {
        LOGF("[HEAP] Failed to allocate arena memory: vmm error %d\n",
             vmm_status);
        slab_free(arena_cache, arena_struct_mem);
        return NULL;
    }

    // initialize arena metadata and first big free block
    arena->magic = ARENA_MAGIC;
    arena->start = (uintptr_t)arena_region;
    arena->end = arena->start + size;
    arena->size = size;
    arena->next = NULL;
    arena->prev = NULL;

    blk_hdr_t* initial_block = (blk_hdr_t*)arena->start;
    size_t block_payload_size = size - sizeof(blk_hdr_t) - sizeof(blk_foot_t);

    // initialize the single big free block that spans the entire arena
    initial_block->magic = BLOCK_MAGIC_FREE;
    initial_block->red_zone_pre = BLOCK_RED_ZONE;
    initial_block->red_zone_post = BLOCK_RED_ZONE;
    initial_block->size = block_payload_size;
    initial_block->total_size = size;
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

    LOGF("[HEAP] Created arena at 0x%lx - 0x%lx (size: %zu bytes)\n", arena->start, arena->end, size);

    return arena;
}

/*
 * destroy_arena - Free the arena's memory and remove it from the heap
 */
static void destroy_arena(heap_t* heap, arena_t* arena) {
    if (!heap || !arena) return;
    if (!arena_validate(arena)) return;

    // find and remove any blocks from the free list that belong to this arena
    blk_hdr_t* cur = heap->free_list;
    while (cur) {
        blk_hdr_t* next = cur->next_free;

        if (cur->arena == arena) {
            freelist_remove(heap, cur);
        }

        cur = next;
    }

    vmm_status_t status = vmm_free(heap->vmm, (void*)arena->start);
    if (status != VMM_OK) {
        LOGF("[HEAP WARNING] Failed to free arena memory: vmm error %d\n",
             status);
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
    slab_free(arena_cache, arena);
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
    if (arena->total_free + sizeof(blk_hdr_t) +
            sizeof(blk_foot_t) >=
        arena->size) {
        LOGF("[HEAP] Destroying empty arena at 0x%lx - 0x%lx\n", arena->start,
             arena->end);
        destroy_arena(heap, arena);
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
    if (!heap_validate_blk(block)) return block;

    // try merging with next block first (arbitrary choice, could do prev first or both iteratively)
    blk_hdr_t* next = next_block(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_blk(next)) {
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
    if (prev && prev->magic == BLOCK_MAGIC_FREE && heap_validate_blk(prev)) {
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
 * find_free_block - Search the free list for a block that fits requested size
 */
static blk_hdr_t* find_free_block(heap_t* heap, size_t size) {
    if (!heap) return NULL;

    blk_hdr_t* cur = heap->free_list;

    while (cur) {
        if (!heap_validate_blk(cur)) {
            LOGF("[HEAP ERROR] Corrupted block in free list\n");
            return NULL;
        }

        if (cur->size >= size) {
            return cur;
        }

        cur = cur->next_free;
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
 * heap_malloc_internal - Core allocation path used by kernel/user wrappers
 * Author note: ASSUMES LOCK IS HELD!
 */
static void* heap_malloc_internal(heap_t* heap, size_t size, bool zero, bool urgent) {
    if (!heap || size == 0) {
        if (urgent) {
            panicf("[HEAP] Invalid malloc parameters: heap=%p, size=%zu", heap,
                   size);
        }
        return NULL;
    }

    // sanity check heap structure before we do anything
    if (!heap_validate(heap)) {
        if (urgent) {
            panicf("[HEAP] Corrupted heap structure at %p", heap);
        }
        return NULL;
    }

    bool should_zero = zero || (heap->flags & HEAP_FLAG_ZERO);

    size_t orig_size = size;
    size = heap_align_size(size);
    if (size == 0 && orig_size > 0) {
        if (urgent) {
            panicf("[HEAP] Size overflow: %zu", orig_size);
        }
        return NULL;
    }
    
    if (size < MIN_BLOCK_SIZE) {
        size = MIN_BLOCK_SIZE;
    }

    blk_hdr_t* block = find_free_block(heap, size);

    // if we couldn't find a big enough block, we need to create a new arena and try again
    if (!block) {
        size_t needed = size + sizeof(blk_hdr_t) + sizeof(blk_foot_t);
        size_t arena_size = heap->min_arena_size;

        if (needed > arena_size) {
            arena_size = align_up(needed, PAGE_SIZE);
        }

        arena_t* new_arena = create_arena(heap, arena_size);
        if (!new_arena) {
            if (urgent) {
                panicf("[HEAP] Failed to create arena: needed %zu bytes",
                       arena_size);
            }
            return NULL;
        }

        block = find_free_block(heap, size);
        if (!block) {
            if (urgent) {
                panicf("[HEAP] No free block found after arena creation");
            }
            return NULL;
        }
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

    if (should_zero) {
        kmemset(user_ptr, 0, block->size);
    }

    return user_ptr;
}

/*
 * heap_free_internal - Core free logic used by kernel/user wrappers
 * Author note: ASSUMES LOCK IS HELD!
 */
static void heap_free_internal(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return;
    if (!heap_validate(heap)) return;

    blk_hdr_t* block = get_header_from_ptr(ptr);

    if (!heap_validate_blk(block)) {
        LOGF("[HEAP ERROR] Attempted to free invalid block at %p\n", ptr);
        return;
    }

    if (block->magic != BLOCK_MAGIC_USED) {
        LOGF("[HEAP ERROR] Double free or invalid free at %p (magic: 0x%x)\n",
             ptr, block->magic);
        return;
    }

    block->magic = BLOCK_MAGIC_FREE;
    block->next_free = NULL;
    block->prev_free = NULL;

    blk_foot_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_FREE;

    stat_mark_free(heap, block);

    freelist_insert(heap, block);

    coalesce_blocks(heap, block);

    if (block->arena) {
        try_shrink_arena(heap, block->arena);
    }
}

#pragma endregion

#pragma region Kernel Heap Management

/*
 * heap_kernel_init - Initialize the global kernel heap
 */
heap_status_t heap_kernel_init(void) {
    bool init_flags = spinlock_acquire(&heap_lock);

    if (kheap) {
        spinlock_release(&heap_lock, init_flags);
        return HEAP_ERR_ALREADY_INIT;
    }
    if (kheap_initing) {
        spinlock_release(&heap_lock, init_flags);
        return HEAP_ERR_ALREADY_INIT;
    }

    kheap_initing = true;

    vmm_t* kernel_vmm = vmm_kernel_get();
    if (!kernel_vmm) {
        LOGF("[HEAP] Kernel VMM not initialized, initializing now...\n");

        uintptr_t alloc_base = get_kend(true) + PAGE_SIZE;
        uintptr_t alloc_end = 0xFFFFFFFFFFFFF000;

        vmm_status_t vmm_status = vmm_kernel_init(alloc_base, alloc_end);
        if (vmm_status != VMM_OK) {
            LOGF("[HEAP] Failed to initialize kernel VMM: error %d\n",
                 vmm_status);
            kheap_initing = false;
            spinlock_release(&heap_lock, init_flags);
            return HEAP_ERR_NOT_INIT;
        }

        kernel_vmm = vmm_kernel_get();
        if (!kernel_vmm) {
            LOGF("[HEAP] Kernel VMM still NULL after initialization\n");
            kheap_initing = false;
            spinlock_release(&heap_lock, init_flags);
            return HEAP_ERR_NOT_INIT;
        }
    }

    if (!heap_cache) {
        heap_cache =
            slab_cache_create("heap_t", sizeof(heap_t), _Alignof(heap_t));
        if (!heap_cache) {
            LOGF("[HEAP] Failed to create heap slab cache\n");
            kheap_initing = false;
            spinlock_release(&heap_lock, init_flags);
            return HEAP_ERR_OOM;
        }
    }

    if (!arena_cache) {
        arena_cache = slab_cache_create("arena_t", sizeof(arena_t),
                                          _Alignof(arena_t));
        if (!arena_cache) {
            LOGF("[HEAP] Failed to create arena slab cache\n");
            kheap_initing = false;
            spinlock_release(&heap_lock, init_flags);
            return HEAP_ERR_OOM;
        }
    }

    // allocate heap structure from slab cache
    void* heap_mem;
    slab_status_t slab_status = slab_alloc(heap_cache, &heap_mem);
    if (slab_status != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate heap structure: slab error %d\n",
             slab_status);
        kheap_initing = false;
        spinlock_release(&heap_lock, init_flags);
        return HEAP_ERR_OOM;
    }

    heap_t* heap = (heap_t*)heap_mem;
    kmemset(heap, 0, sizeof(heap_t));

    // big boy heap initialization
    heap->magic = HEAP_MAGIC;
    heap->vmm = kernel_vmm;
    heap->is_kernel = true;
    heap->flags = HEAP_FLAG_URGENT | HEAP_FLAG_ZERO;
    heap->min_arena_size = MIN_ARENA_SIZE;
    heap->max_size = SIZE_MAX;
    heap->current_size = 0;
    heap->arenas = NULL;
    heap->free_list = NULL;
    heap->total_allocated = 0;
    heap->total_free = 0;
    heap->allocation_count = 0;
    heap->arena_count = 0;
    spinlock_init(&heap->lock, "kernel_heap");

    // create the initial arena to get things rolling
    arena_t* initial_arena = create_arena(heap, MIN_ARENA_SIZE);
    if (!initial_arena) {
        LOGF("[HEAP] Failed to create initial arena\n");
        slab_free(heap_cache, heap_mem);
        kheap_initing = false;
        spinlock_release(&heap_lock, init_flags);
        return HEAP_ERR_VMM_FAIL;
    }

    kheap = heap;
    kheap_initing = false;

    LOGF("[HEAP] Kernel heap initialized with arena at 0x%lx - 0x%lx\n", initial_arena->start, initial_arena->end);

    spinlock_release(&heap_lock, init_flags);
    return HEAP_OK;
}

/*
 * heap_kernel_get - Return pointer to the global kernel heap, lazily initializing it
 */
heap_t* heap_kernel_get(void) {
    if (!kheap && !kheap_initing) {
        heap_status_t status = heap_kernel_init();
        if (status != HEAP_OK) {
            LOGF("[HEAP] Auto-initialization failed: error %d\n", status);
            return NULL;
        }
    }
    return kheap;
}

/*
 * kmalloc - Kernel wrapper around heap_malloc_internal
 */
void* kmalloc(size_t size) {
    heap_t* heap = heap_kernel_get();
    if (!heap) {
        LOGF("[HEAP] kmalloc: kernel heap not available\n");
        return NULL;
    }

    // kmalloc is always urgent and zeroed for safety/performance reasons
    bool flags = spinlock_acquire(&heap->lock);
    void* result = heap_malloc_internal(heap, size, true, true);
    spinlock_release(&heap->lock, flags);
    return result;
}

/*
 * kfree - Kernel wrapper around heap_free_internal
 */
void kfree(void* ptr) {
    if (!ptr) return;

    heap_t* heap = heap_kernel_get();
    if (!heap) {
        LOGF("[HEAP] kfree: kernel heap not available\n");
        return;
    }

    bool flags = spinlock_acquire(&heap->lock);
    heap_free_internal(heap, ptr);
    spinlock_release(&heap->lock, flags);
}

/*
 * krealloc - Kernel realloc; preserves original behavior and API
 */
void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    heap_t* heap = heap_kernel_get();
    if (!heap) {
        LOGF("[HEAP] krealloc: kernel heap not available\n");
        return NULL;
    }

    bool flags = spinlock_acquire(&heap->lock);

    blk_hdr_t* block = get_header_from_ptr(ptr);
    if (!heap_validate_blk(block)) {
        LOGF("[HEAP] krealloc: invalid block at %p\n", ptr);
        spinlock_release(&heap->lock, flags);
        return NULL;
    }
    if (block->magic != BLOCK_MAGIC_USED) {
        LOGF("[HEAP] krealloc: block at %p is not in use\n", ptr);
        spinlock_release(&heap->lock, flags);
        return NULL;
    }

    // if the current block is already big enough, just return the same pointer 
    // (after maybe splitting off a new free block if it's much bigger than needed)
    size_t aligned_size = heap_align_size(size);
    if (aligned_size == 0 && size > 0) {
        spinlock_release(&heap->lock, flags);
        return NULL;
    }
    if (aligned_size <= block->size) {
        if (block->size - aligned_size >= MIN_BLOCK_SIZE + sizeof(blk_hdr_t) + sizeof(blk_foot_t)) {
            split_block(heap, block, aligned_size);
        }
        spinlock_release(&heap->lock, flags);
        return ptr;
    }

    // try to expand into adjacent free block if possible before resorting to allocating a new block and copying
    blk_hdr_t* next = next_block(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_blk(next)) {
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

            block->size = combined_size;
            block->total_size += next->total_size;

            blk_foot_t* footer = get_footer(block);
            footer->header = block;
            footer->magic = BLOCK_MAGIC_USED;
            footer->red_zone_pre = BLOCK_RED_ZONE;
            footer->red_zone_post = BLOCK_RED_ZONE;

            split_block(heap, block, aligned_size);

            spinlock_release(&heap->lock, flags);
            return ptr;
        }
    }

    // Author note: heap_malloc_internal avoids reacquiring the lock
    void* new_ptr = heap_malloc_internal(heap, size, false, false);
    if (!new_ptr) {
        LOGF("[HEAP] krealloc: failed to allocate %zu bytes\n", size);
        spinlock_release(&heap->lock, flags);
        return NULL;
    }

    kmemcpy(new_ptr, ptr, block->size < size ? block->size : size);
    heap_free_internal(heap, ptr);

    spinlock_release(&heap->lock, flags);
    return new_ptr;
}

/*
 * kcalloc - Kernel calloc wrapper
 */
void* kcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        LOGF("[HEAP] kcalloc: overflow detected (nmemb=%zu, size=%zu)\n", nmemb,
             size);
        return NULL;
    }

    heap_t* heap = heap_kernel_get();
    if (!heap) {
        LOGF("[HEAP] kcalloc: kernel heap not available\n");
        return NULL;
    }

    bool flags = spinlock_acquire(&heap->lock);
    void* result = heap_malloc_internal(heap, total, true, false);
    spinlock_release(&heap->lock, flags);
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

    bool lock_flags = spinlock_acquire(&heap->lock);

    size_t calculated_free = 0;
    size_t calculated_used = 0;
    size_t free_blocks = 0;
    size_t used_blocks = 0;
    size_t arena_count = 0;

    arena_t* arena = heap->arenas;
    while (arena) {
        if (!arena_validate(arena)) {
            LOGF("[HEAP INTEGRITY] Arena validation failed at %p\n", arena);
            spinlock_release(&heap->lock, lock_flags);
            return HEAP_ERR_CORRUPTED;
        }

        arena_count++;

        size_t arena_calculated_free = 0;
        size_t arena_calculated_used = 0;

        uintptr_t current_addr = arena->start;

        while (current_addr < arena->end) {
            blk_hdr_t* block = (blk_hdr_t*)current_addr;

            if (!heap_validate_blk(block)) {
                LOGF(
                    "[HEAP INTEGRITY] Block validation failed at 0x%lx in "
                    "arena %p\n",
                    current_addr, arena);
                spinlock_release(&heap->lock, lock_flags);
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
            spinlock_release(&heap->lock, lock_flags);
            return HEAP_ERR_CORRUPTED;
        }

        arena = arena->next;
    }

    if (calculated_free != heap->total_free || calculated_used != heap->total_allocated) {
        LOGF("[HEAP INTEGRITY] Heap statistics mismatch\n");
        spinlock_release(&heap->lock, lock_flags);
        return HEAP_ERR_CORRUPTED;
    }

    spinlock_release(&heap->lock, lock_flags);
    return HEAP_OK;
}

/*
 * heap_dump - Basic human-friendly heap dump for debugging
 */
void heap_dump(heap_t* heap) {
    if (!heap_validate(heap)) {
        LOGF("[HEAP DUMP] Invalid heap\n");
        return;
    }

    bool lock_flags = spinlock_acquire(&heap->lock);

    LOGF("\n=== HEAP DUMP ===\n");
    LOGF("Heap at %p (magic: 0x%x, is_kernel: %d)\n", heap, heap->magic,
         heap->is_kernel);
    LOGF("Total size: %zu bytes across %zu arenas\n", heap->current_size,
         heap->arena_count);
    LOGF("Allocated: %zu bytes in %zu blocks\n", heap->total_allocated,
         heap->allocation_count);
    LOGF("Free: %zu bytes\n", heap->total_free);

    LOGF("\nArenas:\n");
    arena_t* a = heap->arenas;
    int arena_num = 0;

    while (a) {
        if (!arena_validate(a)) {
            LOGF("  [CORRUPTED ARENA]\n");
            break;
        }

        LOGF("  [%d] 0x%lx - 0x%lx (size: %zu, free: %zu, used: %zu)\n",
             arena_num, a->start, a->end, a->size, a->total_free,
             a->total_allocated);

        uintptr_t current_addr = a->start;
        int block_num = 0;

        // only print first few blocks so the klog doesn't explode
        while (current_addr < a->end && block_num < 10) {
            blk_hdr_t* block = (blk_hdr_t*)current_addr;

            if (!heap_validate_blk(block)) {
                LOGF("      [CORRUPTED BLOCK]\n");
                break;
            }

            LOGF("      Block %d: %s, size=%zu\n", block_num,
                 block->magic == BLOCK_MAGIC_FREE ? "FREE" : "USED",
                 block->size);

            current_addr += block->total_size;
            block_num++;
        }

        if (current_addr < a->end) {
            LOGF("      ... (more blocks)\n");
        }

        a = a->next;
        arena_num++;
    }

    LOGF("=================\n");
    spinlock_release(&heap->lock, lock_flags);
}

/*
 * heap_stats - Return basic heap statistics
 */
void heap_stats(heap_t* heap, size_t* total, size_t* used, size_t* free, size_t* overhead) {
    if (!heap_validate(heap)) return;

    bool lock_flags = spinlock_acquire(&heap->lock);

    if (total) *total = heap->current_size;
    if (used) *used = heap->total_allocated;
    if (free) *free = heap->total_free;
    if (overhead) {
        *overhead =
            heap->current_size - heap->total_allocated - heap->total_free;
    }

    spinlock_release(&heap->lock, lock_flags);
}

/*
 * heap_alloc_sz - Get allocation size of a pointer in a heap
 */
size_t heap_alloc_sz(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return 0;
    if (!heap_validate(heap)) return 0;

    bool lock_flags = spinlock_acquire(&heap->lock);

    blk_hdr_t* block = get_header_from_ptr(ptr);

    if (!heap_validate_blk(block)) {
        spinlock_release(&heap->lock, lock_flags);
        return 0;
    }
    if (block->magic != BLOCK_MAGIC_USED) {
        spinlock_release(&heap->lock, lock_flags);
        return 0;
    }

    size_t size = block->size;
    spinlock_release(&heap->lock, lock_flags);
    return size;
}

#pragma endregion