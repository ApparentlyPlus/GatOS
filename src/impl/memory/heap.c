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

#include <debug.h>
#include <libc/string.h>
#include <memory/heap.h>
#include <memory/paging.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <memory/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/panic.h>

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
typedef struct heap_arena heap_arena_t;
typedef struct heap_block_header heap_block_header_t;

// Block header (placed before user data)
struct heap_block_header {
    uint32_t magic;
    uint32_t red_zone_pre;
    size_t size;
    size_t total_size;
    heap_arena_t* arena;

    struct heap_block_header* next_free;
    struct heap_block_header* prev_free;

    uint32_t red_zone_post;

} __attribute__((aligned(16)));

// Block footer (placed after user data)
typedef struct {
    uint32_t red_zone_pre;
    heap_block_header_t* header;
    uint32_t magic;
    uint32_t red_zone_post;
} heap_block_footer_t;

// Arena structure
struct heap_arena {
    uint32_t magic;
    heap_arena_t* next;
    heap_arena_t* prev;

    uintptr_t start;
    uintptr_t end;
    size_t size;

    heap_block_header_t* first_block;

    size_t total_free;
    size_t total_allocated;
};

// Heap structure
struct heap {
    uint32_t magic;
    vmm_t* vmm;

    heap_arena_t* arenas;
    heap_block_header_t* free_list;

    size_t min_arena_size;
    size_t max_size;
    size_t current_size;

    uint32_t flags;
    bool is_kernel;

    size_t total_allocated;
    size_t total_free;
    size_t allocation_count;
    size_t arena_count;
};

// Global kernel heap
static heap_t* g_kernel_heap = NULL;
static bool g_kernel_heap_initializing = false;

// Slab caches
static slab_cache_t* g_heap_cache = NULL;
static slab_cache_t* g_arena_cache = NULL;

#pragma region Utility Functions

/*
 * heap_align_size - Align up to BLOCK_ALIGN
 */
size_t heap_align_size(size_t size) {
    return align_up(size, BLOCK_ALIGN);
}

/*
 * get_footer - Return the footer of a block
 */
static inline heap_block_footer_t* get_footer(heap_block_header_t* header) {
    // Footer sits right after header + user data
    return (heap_block_footer_t*)((uint8_t*)header +
                                  sizeof(heap_block_header_t) + header->size);
}

/*
 * get_user_ptr - Return a pointer to the user data of a block
 */
static inline void* get_user_ptr(heap_block_header_t* header) {
    // Pointer returned to caller (first byte of user data)
    return (void*)((uint8_t*)header + sizeof(heap_block_header_t));
}

/*
 * get_header_from_ptr - Self explanatory
 */
static inline heap_block_header_t* get_header_from_ptr(void* ptr) {
    // If ptr NULL, bail early
    if (!ptr) return NULL;
    return (heap_block_header_t*)((uint8_t*)ptr - sizeof(heap_block_header_t));
}

/*
 * heap_validate_block - Validate a single block's integrity
 */
bool heap_validate_block(heap_block_header_t* header) {
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

    heap_block_footer_t* footer = get_footer(header);

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
static inline bool arena_validate(heap_arena_t* arena) {
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
 * stats_block_mark_used - Update stats when marking a block as used
 */
static inline void stats_block_mark_used(heap_t* heap, heap_block_header_t* block) {
    if (!block->arena) return;

    // Arena level
    block->arena->total_free -= block->size;
    block->arena->total_allocated += block->size;

    // Heap level
    heap->total_free -= block->size;
    heap->total_allocated += block->size;
    heap->allocation_count++;
}

/*
 * stats_block_mark_free - Update stats when marking a block as free
 */
static inline void stats_block_mark_free(heap_t* heap, heap_block_header_t* block) {
    if (!block->arena) return;

    // Arena level
    block->arena->total_allocated -= block->size;
    block->arena->total_free += block->size;

    // Heap level
    heap->total_allocated -= block->size;
    heap->total_free += block->size;
    heap->allocation_count--;
}

/*
 * stats_block_absorb - Update stats when one block absorbs another
 */
static inline void stats_block_absorb(heap_t* heap, heap_block_header_t* survivor, heap_block_header_t* absorbed) {
    // The absorbed block's header and footer are reclaimed as usable space.
    // This overhead is ADDED to the total free space.
    size_t overhead = sizeof(heap_block_header_t) + sizeof(heap_block_footer_t);

    if (survivor->arena) {
        // Add the reclaimed overhead to the arena's free total.
        survivor->arena->total_free += overhead;
    }
    // Add the reclaimed overhead to the heap's free total.
    heap->total_free += overhead;
}

/*
 * stats_block_split - Update stats when splitting a block
 */
static inline void stats_block_split(heap_t* heap, heap_block_header_t* block) {
    size_t overhead = sizeof(heap_block_header_t) + sizeof(heap_block_footer_t);

    if (block->arena) {
        block->arena->total_free -= overhead;
    }
    heap->total_free -= overhead;
}

/*
 * stats_arena_add - Update stats when adding a new arena
 */
static inline void stats_arena_add(heap_t* heap, heap_arena_t* arena,
                                   size_t usable_size) {
    arena->total_free = usable_size;
    arena->total_allocated = 0;

    heap->current_size += arena->size;
    heap->total_free += usable_size;
    heap->arena_count++;
}

/*
 * stats_arena_remove - Update stats when removing an arena
 */
static inline void stats_arena_remove(heap_t* heap, heap_arena_t* arena) {
    heap->current_size -= arena->size;
    heap->total_free -= arena->total_free;
    heap->arena_count--;
}

#pragma endregion

#pragma region Free List Management

/*
 * remove_from_free_list - Unlink a block from the heap's free list
 */
static void remove_from_free_list(heap_t* heap, heap_block_header_t* block) {
    if (!heap || !block) return;

    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        // block was head of free list
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
 * insert_into_free_list - Insert block into sorted-by-size free list
 */
static void insert_into_free_list(heap_t* heap, heap_block_header_t* block) {
    if (!heap || !block) return;

    // reset the freelist pointers before insertion
    block->next_free = NULL;
    block->prev_free = NULL;

    if (!heap->free_list) {
        heap->free_list = block;
        return;
    }

    // If smaller or equal than first element, insert at head
    if (block->size <= heap->free_list->size) {
        block->next_free = heap->free_list;
        heap->free_list->prev_free = block;
        heap->free_list = block;
        return;
    }

    // Walk the list until we find a place to insert (first fit by size sort)
    heap_block_header_t* cursor = heap->free_list;
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
 * find_arena_for_address - Find which arena an address belongs to
 */
static heap_arena_t* find_arena_for_address(heap_t* heap, uintptr_t addr) {
    if (!heap) return NULL;

    heap_arena_t* a = heap->arenas;
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
 * get_next_block_in_arena - Get the block immediately after given block
 */
static heap_block_header_t* get_next_block_in_arena(
    heap_block_header_t* block) {
    if (!block || !block->arena) return NULL;

    uintptr_t next_addr = (uintptr_t)block + block->total_size;

    if (next_addr >= block->arena->end) {
        return NULL;
    }

    return (heap_block_header_t*)next_addr;
}

/*
 * get_prev_block_in_arena - Get the block immediately preceding given block
 */
static heap_block_header_t* get_prev_block_in_arena(
    heap_block_header_t* block) {
    if (!block || !block->arena) return NULL;

    uintptr_t prev_footer_addr = (uintptr_t)block - sizeof(heap_block_footer_t);

    if (prev_footer_addr < block->arena->start) {
        return NULL;
    }

    heap_block_footer_t* prev_footer = (heap_block_footer_t*)prev_footer_addr;

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
static heap_arena_t* create_arena(heap_t* heap, size_t size) {
    if (!heap || size == 0) return NULL;

    // enforce minimum arena size
    if (size < heap->min_arena_size) {
        size = heap->min_arena_size;
    }

    // page-align the arena size
    size = align_up(size, PAGE_SIZE);

    // ensure we won't exceed heap's configured max_size
    if (heap->current_size + size > heap->max_size) {
        LOGF("[HEAP] Cannot create arena: would exceed max heap size\n");
        return NULL;
    }

    // allocate a structure for the arena from the slab cache
    void* arena_struct_mem;
    slab_status_t slab_status = slab_alloc(g_arena_cache, &arena_struct_mem);
    if (slab_status != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate arena structure\n");
        return NULL;
    }

    heap_arena_t* arena = (heap_arena_t*)arena_struct_mem;
    // zero it out - conservative initialization
    memset(arena, 0, sizeof(heap_arena_t));

    // Now allocate the address space for the arena via VMM
    void* arena_region = NULL;
    vmm_status_t vmm_status = vmm_alloc(
        heap->vmm, size, VM_FLAG_WRITE | (heap->is_kernel ? 0 : VM_FLAG_USER),
        NULL, &arena_region);

    if (vmm_status != VMM_OK) {
        LOGF("[HEAP] Failed to allocate arena memory: vmm error %d\n",
             vmm_status);
        slab_free(g_arena_cache, arena_struct_mem);
        return NULL;
    }

    // populate arena metadata
    arena->magic = ARENA_MAGIC;
    arena->start = (uintptr_t)arena_region;
    arena->end = arena->start + size;
    arena->size = size;
    arena->next = NULL;
    arena->prev = NULL;

    // Create initial free block covering the whole arena
    heap_block_header_t* initial_block = (heap_block_header_t*)arena->start;
    size_t block_payload_size =
        size - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);

    initial_block->magic = BLOCK_MAGIC_FREE;
    initial_block->red_zone_pre = BLOCK_RED_ZONE;
    initial_block->red_zone_post = BLOCK_RED_ZONE;
    initial_block->size = block_payload_size;
    initial_block->total_size = size;
    initial_block->arena = arena;
    initial_block->next_free = NULL;
    initial_block->prev_free = NULL;

    heap_block_footer_t* initial_footer = get_footer(initial_block);
    initial_footer->red_zone_pre = BLOCK_RED_ZONE;
    initial_footer->red_zone_post = BLOCK_RED_ZONE;
    initial_footer->header = initial_block;
    initial_footer->magic = BLOCK_MAGIC_FREE;

    arena->first_block = initial_block;

    // append the arena to the heap's arena list
    if (!heap->arenas) {
        heap->arenas = arena;
    } else {
        heap_arena_t* tail = heap->arenas;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = arena;
        arena->prev = tail;
    }

    // update accounting
    stats_arena_add(heap, arena, block_payload_size);

    // Add the initial block to the free list
    insert_into_free_list(heap, initial_block);

    LOGF("[HEAP] Created arena at 0x%lx - 0x%lx (size: %zu bytes)\n",
         arena->start, arena->end, size);

    return arena;
}

/*
 * destroy_arena - Free the arena's memory and remove it from the heap
 */
static void destroy_arena(heap_t* heap, heap_arena_t* arena) {
    if (!heap || !arena) return;
    if (!arena_validate(arena)) return;

    // Remove any freelist nodes belonging to this arena
    heap_block_header_t* cur = heap->free_list;
    while (cur) {
        heap_block_header_t* next = cur->next_free;

        if (cur->arena == arena) {
            remove_from_free_list(heap, cur);
        }

        cur = next;
    }

    // Release VMM memory
    vmm_status_t status = vmm_free(heap->vmm, (void*)arena->start);
    if (status != VMM_OK) {
        LOGF("[HEAP WARNING] Failed to free arena memory: vmm error %d\n",
             status);
    }

    // Adjust stats
    stats_arena_remove(heap, arena);

    // unlink from arena list
    if (arena->prev) {
        arena->prev->next = arena->next;
    } else {
        heap->arenas = arena->next;
    }

    if (arena->next) {
        arena->next->prev = arena->prev;
    }

    // poison the magic then free structure
    arena->magic = 0;
    slab_free(g_arena_cache, arena);
}

/*
 * try_shrink_arena - Attempt to destroy an arena if it's unused/empty
 */
static void try_shrink_arena(heap_t* heap, heap_arena_t* arena) {
    if (!heap || !arena) return;
    if (!arena_validate(arena)) return;

    // don't remove the last arena
    if (heap->arena_count <= 1) return;
    // only consider entirely empty arenas
    if (arena->total_allocated > 0) return;
    // simple heuristic: only shrink when free >> allocated
    if (heap->total_free < heap->total_allocated * HEAP_SHRINK_THRESHOLD)
        return;

    // check if the arena is entirely free (plus header/footer overhead)
    if (arena->total_free + sizeof(heap_block_header_t) +
            sizeof(heap_block_footer_t) >=
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
static heap_block_header_t* coalesce_blocks(heap_t* heap,
                                            heap_block_header_t* block) {
    if (!heap || !block) return block;
    if (!heap_validate_block(block)) return block;

    // try coalescing with next block repeatedly (merge forwards first)
    heap_block_header_t* next = get_next_block_in_arena(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_block(next)) {
        // unlink both from free list before touching sizes
        remove_from_free_list(heap, block);
        remove_from_free_list(heap, next);

        // update stats to reflect reclaimed overhead
        stats_block_absorb(heap, block, next);

        // increase sizes; total_size already includes header/footer of next
        block->size += next->total_size;
        block->total_size += next->total_size;

        // refresh footer to point at new, larger block
        heap_block_footer_t* footer = get_footer(block);
        footer->header = block;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;

        // re-insert the merged block
        insert_into_free_list(heap, block);

        // try again (tail recursion converted into loop by repetition)
        return coalesce_blocks(heap, block);
    }

    // try coalescing with previous block (merge backwards)
    heap_block_header_t* prev = get_prev_block_in_arena(block);
    if (prev && prev->magic == BLOCK_MAGIC_FREE && heap_validate_block(prev)) {
        remove_from_free_list(heap, block);
        remove_from_free_list(heap, prev);

        // stats: prev absorbs block
        stats_block_absorb(heap, prev, block);

        prev->size += block->total_size;
        prev->total_size += block->total_size;

        // refresh footer
        heap_block_footer_t* footer = get_footer(prev);
        footer->header = prev;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;

        insert_into_free_list(heap, prev);

        return coalesce_blocks(heap, prev);
    }

    // nothing to merge; return the block (possibly unchanged)
    return block;
}

#pragma endregion

#pragma region Allocation/Deallocation

/*
 * find_free_block - Search the free list for a block that fits requested size
 */
static heap_block_header_t* find_free_block(heap_t* heap, size_t size) {
    if (!heap) return NULL;

    heap_block_header_t* cur = heap->free_list;

    while (cur) {
        if (!heap_validate_block(cur)) {
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
static void split_block(heap_t* heap, heap_block_header_t* block, size_t size) {
    if (!block || !heap) return;

    size_t remaining = block->size - size;

    // Require room for another minimally sized block (payload + header/footer)
    if (remaining < MIN_BLOCK_SIZE + sizeof(heap_block_header_t) +
                        sizeof(heap_block_footer_t)) {
        // not enough room to split; keep the block as-is.
        return;
    }

    // record whether block was already in free list (we'll re-insert
    // conditionally)
    bool was_in_free_list = (block->magic == BLOCK_MAGIC_FREE);

    if (was_in_free_list) {
        remove_from_free_list(heap, block);
    }

    // account for new header/footer created by the split
    stats_block_split(heap, block);

    // shrink current block to requested size
    block->size = size;
    block->total_size =
        sizeof(heap_block_header_t) + size + sizeof(heap_block_footer_t);

    // refresh footer for the first (now smaller) block
    heap_block_footer_t* footer = get_footer(block);
    footer->red_zone_pre = BLOCK_RED_ZONE;
    footer->red_zone_post = BLOCK_RED_ZONE;
    footer->header = block;
    footer->magic = block->magic;

    // create a new block at the end to represent the leftover free portion
    heap_block_header_t* new_block =
        (heap_block_header_t*)((uint8_t*)block + block->total_size);

    new_block->magic = BLOCK_MAGIC_FREE;
    new_block->red_zone_pre = BLOCK_RED_ZONE;
    new_block->red_zone_post = BLOCK_RED_ZONE;
    // The new block's payload size is remaining minus its own header/footer
    new_block->size =
        remaining - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);
    new_block->total_size = remaining;
    new_block->arena = block->arena;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;

    // footer for the new block
    heap_block_footer_t* new_footer = get_footer(new_block);
    new_footer->red_zone_pre = BLOCK_RED_ZONE;
    new_footer->red_zone_post = BLOCK_RED_ZONE;
    new_footer->header = new_block;
    new_footer->magic = BLOCK_MAGIC_FREE;

    // put new block onto free list
    insert_into_free_list(heap, new_block);

    // if the original was free, re-insert the (now smaller) block too
    if (was_in_free_list) {
        insert_into_free_list(heap, block);
    }
}

/*
 * heap_malloc_internal - Core allocation path used by kernel/user wrappers
 */
static void* heap_malloc_internal(heap_t* heap, size_t size, bool zero, bool urgent) {
    if (!heap || size == 0) {
        if (urgent) {
            panicf("[HEAP] Invalid malloc parameters: heap=%p, size=%zu", heap,
                   size);
        }
        return NULL;
    }

    if (!heap_validate(heap)) {
        if (urgent) {
            panicf("[HEAP] Corrupted heap structure at %p", heap);
        }
        return NULL;
    }

    bool should_zero = zero || (heap->flags & HEAP_FLAG_ZERO);

    // Normalize size to alignment and enforce minimum payload
    size = heap_align_size(size);
    if (size < MIN_BLOCK_SIZE) {
        size = MIN_BLOCK_SIZE;
    }

    // First, try to find a free block
    heap_block_header_t* block = find_free_block(heap, size);

    // If none, create a new arena large enough to contain the requested
    // allocation
    if (!block) {
        size_t needed =
            size + sizeof(heap_block_header_t) + sizeof(heap_block_footer_t);
        size_t arena_size = heap->min_arena_size;

        if (needed > arena_size) {
            arena_size = align_up(needed, PAGE_SIZE);
        }

        heap_arena_t* new_arena = create_arena(heap, arena_size);
        if (!new_arena) {
            if (urgent) {
                panicf("[HEAP] Failed to create arena: needed %zu bytes",
                       arena_size);
            }
            return NULL;
        }

        // after adding an arena, try finding a block again
        block = find_free_block(heap, size);
        if (!block) {
            if (urgent) {
                panicf("[HEAP] No free block found after arena creation");
            }
            return NULL;
        }
    }

    // Try to split the block so we don't waste space
    split_block(heap, block, size);

    // remove from free list (we're going to hand it to the caller)
    remove_from_free_list(heap, block);

    // mark it used
    block->magic = BLOCK_MAGIC_USED;
    heap_block_footer_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_USED;

    // stats update occurs here (FREE -> USED)
    stats_block_mark_used(heap, block);

    void* user_ptr = get_user_ptr(block);

    if (should_zero) {
        // zero user payload on allocation if requested (or heap flag)
        memset(user_ptr, 0, block->size);
    }

    return user_ptr;
}

/*
 * heap_free_internal - Core free logic used by kernel/user wrappers
 */
static void heap_free_internal(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return;
    if (!heap_validate(heap)) return;

    heap_block_header_t* block = get_header_from_ptr(ptr);

    if (!heap_validate_block(block)) {
        LOGF("[HEAP ERROR] Attempted to free invalid block at %p\n", ptr);
        return;
    }

    if (block->magic != BLOCK_MAGIC_USED) {
        LOGF("[HEAP ERROR] Double free or invalid free at %p (magic: 0x%x)\n",
             ptr, block->magic);
        return;
    }

    // Mark block as free, clear freelist pointers for safety
    block->magic = BLOCK_MAGIC_FREE;
    block->next_free = NULL;
    block->prev_free = NULL;

    heap_block_footer_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_FREE;

    // stats update (USED -> FREE)
    stats_block_mark_free(heap, block);

    // insert into free list and attempt coalescing to reduce fragmentation
    insert_into_free_list(heap, block);

    coalesce_blocks(heap, block);

    // maybe the arena is now empty — try to free it
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
    if (g_kernel_heap) return HEAP_ERR_ALREADY_INIT;
    if (g_kernel_heap_initializing) return HEAP_ERR_ALREADY_INIT;

    g_kernel_heap_initializing = true;

    vmm_t* kernel_vmm = vmm_kernel_get();
    if (!kernel_vmm) {
        LOGF("[HEAP] Kernel VMM not initialized, initializing now...\n");

        uintptr_t alloc_base = get_kend(true) + PAGE_SIZE;
        uintptr_t alloc_end = 0xFFFFFFFFFFFFF000;

        vmm_status_t vmm_status = vmm_kernel_init(alloc_base, alloc_end);
        if (vmm_status != VMM_OK) {
            LOGF("[HEAP] Failed to initialize kernel VMM: error %d\n",
                 vmm_status);
            g_kernel_heap_initializing = false;
            return HEAP_ERR_NOT_INIT;
        }

        kernel_vmm = vmm_kernel_get();
        if (!kernel_vmm) {
            LOGF("[HEAP] Kernel VMM still NULL after initialization\n");
            g_kernel_heap_initializing = false;
            return HEAP_ERR_NOT_INIT;
        }
    }

    // create slab caches lazily if needed
    if (!g_heap_cache) {
        g_heap_cache =
            slab_cache_create("heap_t", sizeof(heap_t), _Alignof(heap_t));
        if (!g_heap_cache) {
            LOGF("[HEAP] Failed to create heap slab cache\n");
            g_kernel_heap_initializing = false;
            return HEAP_ERR_OOM;
        }
    }

    if (!g_arena_cache) {
        g_arena_cache = slab_cache_create("heap_arena_t", sizeof(heap_arena_t),
                                          _Alignof(heap_arena_t));
        if (!g_arena_cache) {
            LOGF("[HEAP] Failed to create arena slab cache\n");
            g_kernel_heap_initializing = false;
            return HEAP_ERR_OOM;
        }
    }

    // allocate heap structure from slab
    void* heap_mem;
    slab_status_t slab_status = slab_alloc(g_heap_cache, &heap_mem);
    if (slab_status != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate heap structure: slab error %d\n",
             slab_status);
        g_kernel_heap_initializing = false;
        return HEAP_ERR_OOM;
    }

    heap_t* heap = (heap_t*)heap_mem;
    memset(heap, 0, sizeof(heap_t));

    // initialize heap metadata
    heap->magic = HEAP_MAGIC;
    heap->vmm = kernel_vmm;
    heap->is_kernel = true;
    heap->flags = HEAP_FLAG_NONE;
    heap->min_arena_size = MIN_ARENA_SIZE;
    heap->max_size = SIZE_MAX;
    heap->current_size = 0;
    heap->arenas = NULL;
    heap->free_list = NULL;
    heap->total_allocated = 0;
    heap->total_free = 0;
    heap->allocation_count = 0;
    heap->arena_count = 0;

    // create the initial arena
    heap_arena_t* initial_arena = create_arena(heap, MIN_ARENA_SIZE);
    if (!initial_arena) {
        LOGF("[HEAP] Failed to create initial arena\n");
        slab_free(g_heap_cache, heap_mem);
        g_kernel_heap_initializing = false;
        return HEAP_ERR_VMM_FAIL;
    }

    g_kernel_heap = heap;
    g_kernel_heap_initializing = false;

    LOGF("[HEAP] Kernel heap initialized with arena at 0x%lx - 0x%lx\n",
         initial_arena->start, initial_arena->end);

    return HEAP_OK;
}

/*
 * heap_kernel_get - Return pointer to the global kernel heap, lazily initializing it
 */
heap_t* heap_kernel_get(void) {
    if (!g_kernel_heap && !g_kernel_heap_initializing) {
        heap_status_t status = heap_kernel_init();
        if (status != HEAP_OK) {
            LOGF("[HEAP] Auto-initialization failed: error %d\n", status);
            return NULL;
        }
    }
    return g_kernel_heap;
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

    return heap_malloc_internal(heap, size, false, false);
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

    heap_free_internal(heap, ptr);
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

    heap_block_header_t* block = get_header_from_ptr(ptr);
    if (!heap_validate_block(block)) {
        LOGF("[HEAP] krealloc: invalid block at %p\n", ptr);
        return NULL;
    }
    if (block->magic != BLOCK_MAGIC_USED) {
        LOGF("[HEAP] krealloc: block at %p is not in use\n", ptr);
        return NULL;
    }

    size_t aligned_size = heap_align_size(size);
    if (aligned_size <= block->size) {
        // shrink in place, maybe split to avoid wasting space
        if (block->size - aligned_size >= MIN_BLOCK_SIZE +
                                              sizeof(heap_block_header_t) +
                                              sizeof(heap_block_footer_t)) {
            split_block(heap, block, aligned_size);
        }
        return ptr;
    }

    // try to expand into adjacent free block
    heap_block_header_t* next = get_next_block_in_arena(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_block(next)) {
        size_t combined_size = block->size + next->total_size;
        if (combined_size >= aligned_size) {
            remove_from_free_list(heap, next);

            // account stats for absorption
            stats_block_absorb(heap, block, next);

            block->size = combined_size;
            block->total_size += next->total_size;

            heap_block_footer_t* footer = get_footer(block);
            footer->header = block;
            footer->magic = BLOCK_MAGIC_USED;
            footer->red_zone_pre = BLOCK_RED_ZONE;
            footer->red_zone_post = BLOCK_RED_ZONE;

            // now possibly split to exact requested size
            split_block(heap, block, aligned_size);

            return ptr;
        }
    }

    // fallback: allocate a new region and copy data
    void* new_ptr = kmalloc(size);
    if (!new_ptr) {
        LOGF("[HEAP] krealloc: failed to allocate %zu bytes\n", size);
        return NULL;
    }

    // careful copy: only copy the min of old/new sizes
    memcpy(new_ptr, ptr, block->size < size ? block->size : size);
    kfree(ptr);

    return new_ptr;
}

/*
 * kcalloc - Kernel calloc wrapper
 */
void* kcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    // check multiplication overflow
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

    // request zeroed memory
    return heap_malloc_internal(heap, total, true, false);
}

#pragma endregion

#pragma region User Heap Management

/*
 * heap_create - Create a user (or non-kernel) heap backed by a VMM instance
 */
heap_t* heap_create(vmm_t* vmm, size_t min_size, size_t max_size, uint32_t flags) {
    if (!vmm) {
        LOGF("[HEAP] heap_create: NULL vmm parameter\n");
        return NULL;
    }
    if (min_size == 0) min_size = MIN_ARENA_SIZE;
    if (max_size < min_size) {
        LOGF("[HEAP] heap_create: max_size (%zu) < min_size (%zu)\n", max_size,
             min_size);
        return NULL;
    }

    min_size = align_up(min_size, PAGE_SIZE);
    max_size = align_up(max_size, PAGE_SIZE);

    // ensure slab caches exist
    if (!g_heap_cache) {
        g_heap_cache =
            slab_cache_create("heap_t", sizeof(heap_t), _Alignof(heap_t));
        if (!g_heap_cache) {
            LOGF("[HEAP] heap_create: failed to create slab cache\n");
            return NULL;
        }
    }

    if (!g_arena_cache) {
        g_arena_cache = slab_cache_create("heap_arena_t", sizeof(heap_arena_t),
                                          _Alignof(heap_arena_t));
        if (!g_arena_cache) {
            LOGF("[HEAP] heap_create: failed to create arena slab cache\n");
            return NULL;
        }
    }

    // allocate heap struct
    void* heap_mem;
    slab_status_t slab_status = slab_alloc(g_heap_cache, &heap_mem);
    if (slab_status != SLAB_OK) {
        LOGF(
            "[HEAP] heap_create: failed to allocate heap structure: slab error "
            "%d\n",
            slab_status);
        return NULL;
    }

    heap_t* hh = (heap_t*)heap_mem;
    memset(hh, 0, sizeof(heap_t));

    hh->magic = HEAP_MAGIC;
    hh->vmm = vmm;
    hh->is_kernel = false;
    hh->flags = flags;
    hh->min_arena_size = min_size;
    hh->max_size = max_size;
    hh->current_size = 0;
    hh->arenas = NULL;
    hh->free_list = NULL;
    hh->total_allocated = 0;
    hh->total_free = 0;
    hh->allocation_count = 0;
    hh->arena_count = 0;

    heap_arena_t* initial_arena = create_arena(hh, min_size);
    if (!initial_arena) {
        LOGF("[HEAP] heap_create: failed to create initial arena\n");
        slab_free(g_heap_cache, heap_mem);
        return NULL;
    }

    return hh;
}

/*
 * heap_destroy - Tear down a heap and free resources (non-kernel only)
 */
void heap_destroy(heap_t* heap) {
    if (!heap) return;
    if (!heap_validate(heap)) {
        LOGF("[HEAP] heap_destroy: invalid heap at %p\n", heap);
        return;
    }

    if (heap == g_kernel_heap) {
        LOGF("[HEAP ERROR] Cannot destroy kernel heap\n");
        return;
    }

    heap_arena_t* arena = heap->arenas;
    while (arena) {
        heap_arena_t* next = arena->next;

        vmm_status_t status = vmm_free(heap->vmm, (void*)arena->start);
        if (status != VMM_OK) {
            LOGF("[HEAP WARNING] Failed to free arena memory: vmm error %d\n",
                 status);
        }

        arena->magic = 0;
        slab_free(g_arena_cache, arena);

        arena = next;
    }

    heap->magic = 0;
    slab_free(g_heap_cache, heap);
}

/*
 * heap_malloc - Public allocator for an arbitrary heap
 */
void* heap_malloc(heap_t* heap, size_t size) {
    if (!heap) return NULL;
    bool urgent = (heap->flags & HEAP_FLAG_URGENT) != 0;
    return heap_malloc_internal(heap, size, false, urgent);
}

/*
 * heap_free - Public free wrapper
 */
void heap_free(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return;
    heap_free_internal(heap, ptr);
}

/*
 * heap_realloc - Public realloc wrapper
 */
void* heap_realloc(heap_t* heap, void* ptr, size_t size) {
    if (!heap) return NULL;
    if (!ptr) return heap_malloc(heap, size);
    if (size == 0) {
        heap_free(heap, ptr);
        return NULL;
    }

    bool urgent = (heap->flags & HEAP_FLAG_URGENT) != 0;

    heap_block_header_t* block = get_header_from_ptr(ptr);
    if (!heap_validate_block(block)) {
        if (urgent) {
            panicf("[HEAP] heap_realloc: invalid block at %p", ptr);
        }
        return NULL;
    }
    if (block->magic != BLOCK_MAGIC_USED) {
        if (urgent) {
            panicf("[HEAP] heap_realloc: block at %p is not in use", ptr);
        }
        return NULL;
    }

    size_t aligned_size = heap_align_size(size);
    if (aligned_size <= block->size) {
        if (block->size - aligned_size >= MIN_BLOCK_SIZE +
                                              sizeof(heap_block_header_t) +
                                              sizeof(heap_block_footer_t)) {
            split_block(heap, block, aligned_size);
        }
        return ptr;
    }

    // try to absorb next block if it's free
    heap_block_header_t* next = get_next_block_in_arena(block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_block(next)) {
        size_t combined_size = block->size + next->total_size;
        if (combined_size >= aligned_size) {
            remove_from_free_list(heap, next);

            // update stats for absorption
            stats_block_absorb(heap, block, next);

            block->size = combined_size;
            block->total_size += next->total_size;

            heap_block_footer_t* footer = get_footer(block);
            footer->header = block;
            footer->magic = BLOCK_MAGIC_USED;
            footer->red_zone_pre = BLOCK_RED_ZONE;
            footer->red_zone_post = BLOCK_RED_ZONE;

            split_block(heap, block, aligned_size);
            return ptr;
        }
    }

    // allocate new and copy old data
    void* new_ptr = heap_malloc(heap, size);
    if (!new_ptr) {
        if (urgent) {
            panicf("[HEAP] heap_realloc: failed to allocate %zu bytes", size);
        }
        return NULL;
    }

    memcpy(new_ptr, ptr, block->size < size ? block->size : size);
    heap_free(heap, ptr);

    return new_ptr;
}

/*
 * heap_calloc - Public calloc wrapper
 */
void* heap_calloc(heap_t* heap, size_t nmemb, size_t size) {
    if (!heap || nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        LOGF("[HEAP] heap_calloc: overflow detected (nmemb=%zu, size=%zu)\n",
             nmemb, size);
        return NULL;
    }

    bool urgent = (heap->flags & HEAP_FLAG_URGENT) != 0;
    return heap_malloc_internal(heap, total, true, urgent);
}

#pragma endregion

#pragma region Introspection and Debugging

/*
 * heap_check_integrity - Verify heap data structures and consistency
 */
heap_status_t heap_check_integrity(heap_t* heap) {
    if (!heap_validate(heap)) {
        return HEAP_ERR_INVALID;
    }

    size_t calculated_free = 0;
    size_t calculated_used = 0;
    size_t free_blocks = 0;
    size_t used_blocks = 0;
    size_t arena_count = 0;

    heap_arena_t* arena = heap->arenas;
    while (arena) {
        if (!arena_validate(arena)) {
            LOGF("[HEAP INTEGRITY] Arena validation failed at %p\n", arena);
            return HEAP_ERR_CORRUPTED;
        }

        arena_count++;

        size_t arena_calculated_free = 0;
        size_t arena_calculated_used = 0;

        uintptr_t current_addr = arena->start;

        // Walk blocks in this arena summing free/used counts
        while (current_addr < arena->end) {
            heap_block_header_t* block = (heap_block_header_t*)current_addr;

            if (!heap_validate_block(block)) {
                LOGF(
                    "[HEAP INTEGRITY] Block validation failed at 0x%lx in "
                    "arena %p\n",
                    current_addr, arena);
                return HEAP_ERR_CORRUPTED;
            }

            if (block->arena != arena) {
                LOGF("[HEAP INTEGRITY] Block arena pointer mismatch at 0x%lx\n",
                     current_addr);
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
            } else {
                LOGF("[HEAP INTEGRITY] Invalid magic 0x%x at 0x%lx\n",
                     block->magic, current_addr);
                return HEAP_ERR_CORRUPTED;
            }

            current_addr += block->total_size;
        }

        if (current_addr != arena->end) {
            LOGF("[HEAP INTEGRITY] Arena walk ended at 0x%lx, expected 0x%lx\n",
                 current_addr, arena->end);
            return HEAP_ERR_CORRUPTED;
        }

        if (arena_calculated_free != arena->total_free) {
            LOGF(
                "[HEAP INTEGRITY] Arena %p free mismatch: calculated %zu, "
                "stored %zu\n",
                arena, arena_calculated_free, arena->total_free);
            return HEAP_ERR_CORRUPTED;
        }

        if (arena_calculated_used != arena->total_allocated) {
            LOGF(
                "[HEAP INTEGRITY] Arena %p used mismatch: calculated %zu, "
                "stored %zu\n",
                arena, arena_calculated_used, arena->total_allocated);
            return HEAP_ERR_CORRUPTED;
        }

        arena = arena->next;
    }

    if (arena_count != heap->arena_count) {
        LOGF(
            "[HEAP INTEGRITY] Arena count mismatch: calculated %zu, stored "
            "%zu\n",
            arena_count, heap->arena_count);
        return HEAP_ERR_CORRUPTED;
    }

    if (calculated_free != heap->total_free) {
        LOGF("[HEAP INTEGRITY] Free mismatch: calculated %zu, stored %zu\n",
             calculated_free, heap->total_free);
        return HEAP_ERR_CORRUPTED;
    }

    if (calculated_used != heap->total_allocated) {
        LOGF("[HEAP INTEGRITY] Used mismatch: calculated %zu, stored %zu\n",
             calculated_used, heap->total_allocated);
        return HEAP_ERR_CORRUPTED;
    }

    if (used_blocks != heap->allocation_count) {
        LOGF("[HEAP INTEGRITY] Count mismatch: calculated %zu, stored %zu\n",
             used_blocks, heap->allocation_count);
        return HEAP_ERR_CORRUPTED;
    }

    // Verify free list consistency
    size_t free_list_count = 0;
    size_t free_list_size = 0;
    heap_block_header_t* free_block = heap->free_list;
    heap_block_header_t* prev_free = NULL;

    while (free_block) {
        if (!heap_validate_block(free_block)) {
            LOGF("[HEAP INTEGRITY] Free list contains invalid block\n");
            return HEAP_ERR_CORRUPTED;
        }

        if (free_block->magic != BLOCK_MAGIC_FREE) {
            LOGF("[HEAP INTEGRITY] Free list contains non-free block\n");
            return HEAP_ERR_CORRUPTED;
        }

        if (free_block->prev_free != prev_free) {
            LOGF("[HEAP INTEGRITY] Free list prev pointer mismatch\n");
            return HEAP_ERR_CORRUPTED;
        }

        if (prev_free && prev_free->size > free_block->size) {
            LOGF("[HEAP INTEGRITY] Free list not sorted by size\n");
            return HEAP_ERR_CORRUPTED;
        }

        free_list_count++;
        free_list_size += free_block->size;
        prev_free = free_block;
        free_block = free_block->next_free;
    }

    if (free_list_count != free_blocks) {
        LOGF("[HEAP INTEGRITY] Free list count mismatch: %zu vs %zu\n",
             free_list_count, free_blocks);
        return HEAP_ERR_CORRUPTED;
    }

    if (free_list_size != calculated_free) {
        LOGF("[HEAP INTEGRITY] Free list size mismatch: %zu vs %zu\n",
             free_list_size, calculated_free);
        return HEAP_ERR_CORRUPTED;
    }

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

    LOGF("=== HEAP DUMP ===\n");
    LOGF("Heap at %p (magic: 0x%x, is_kernel: %d)\n", heap, heap->magic,
         heap->is_kernel);
    LOGF("Total size: %zu bytes across %zu arenas\n", heap->current_size,
         heap->arena_count);
    LOGF("Allocated: %zu bytes in %zu blocks\n", heap->total_allocated,
         heap->allocation_count);
    LOGF("Free: %zu bytes\n", heap->total_free);

    LOGF("\nArenas:\n");
    heap_arena_t* a = heap->arenas;
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

        // only print first few blocks so the log doesn't explode
        while (current_addr < a->end && block_num < 10) {
            heap_block_header_t* block = (heap_block_header_t*)current_addr;

            if (!heap_validate_block(block)) {
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
}

/*
 * heap_stats - Return basic heap statistics
 */
void heap_stats(heap_t* heap, size_t* total, size_t* used, size_t* free, size_t* overhead) {
    if (!heap_validate(heap)) return;

    if (total) *total = heap->current_size;
    if (used) *used = heap->total_allocated;
    if (free) *free = heap->total_free;
    if (overhead) {
        *overhead =
            heap->current_size - heap->total_allocated - heap->total_free;
    }
}

/*
 * heap_get_alloc_size - Get allocation size of a pointer in a heap
 */
size_t heap_get_alloc_size(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return 0;
    if (!heap_validate(heap)) return 0;

    heap_block_header_t* block = get_header_from_ptr(ptr);

    if (!heap_validate_block(block)) return 0;
    if (block->magic != BLOCK_MAGIC_USED) return 0;

    return block->size;
}

#pragma endregion