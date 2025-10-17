/*
 * heap.c - Kernel Heap Manager Implementation
 *
 * This implementation provides a robust heap allocator using boundary tag
 * coalescing. Each allocation is tracked with header and footer metadata
 * that includes magic numbers for validation and size information for
 * efficient coalescing and freeing without requiring size parameters.
 *
 * Author: u/ApparentlyPlus
 */

#include <memory/heap.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <memory/paging.h>
#include <sys/panic.h>
#include <libc/string.h>
#include <debug.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Magic numbers for validation
#define HEAP_MAGIC              0x48454150
#define BLOCK_MAGIC_USED        0x55534544
#define BLOCK_MAGIC_FREE        0x46524545
#define BLOCK_RED_ZONE          0xDEADBEEF

// Block alignment
#define BLOCK_ALIGN             16
#define MIN_BLOCK_SIZE          32  // Minimum payload size

// Expansion strategy
#define HEAP_EXPAND_FACTOR      2
#define HEAP_SHRINK_THRESHOLD   4   // Shrink if free space is > 4x used space

// Block header (placed before user data)
struct heap_block_header {
    uint32_t magic;                     // BLOCK_MAGIC_USED or BLOCK_MAGIC_FREE
    uint32_t red_zone_pre;              // BLOCK_RED_ZONE
    size_t size;                        // Size of user data (aligned)
    size_t total_size;                  // Total size including header/footer
    
    // Free list pointers (only valid when magic == BLOCK_MAGIC_FREE)
    struct heap_block_header* next_free;
    struct heap_block_header* prev_free;
    
    uint32_t red_zone_post;             // BLOCK_RED_ZONE
};

// Block footer (placed after user data)
typedef struct {
    uint32_t red_zone_pre;              // BLOCK_RED_ZONE
    heap_block_header_t* header;        // Pointer back to header
    uint32_t magic;                     // BLOCK_MAGIC_USED or BLOCK_MAGIC_FREE
    uint32_t red_zone_post;             // BLOCK_RED_ZONE
} heap_block_footer_t;

// Heap structure
struct heap {
    uint32_t magic;                     // HEAP_MAGIC
    vmm_t* vmm;                         // VMM instance for allocations
    
    heap_block_header_t* free_list;     // Head of free list (sorted by size)
    
    uintptr_t heap_start;               // Start of heap region
    uintptr_t heap_end;                 // End of heap region (current)
    
    size_t min_size;                    // Minimum heap size
    size_t max_size;                    // Maximum heap size
    size_t current_size;                // Current heap size
    
    uint32_t flags;                     // Heap flags
    bool is_kernel;                     // Is this the kernel heap?
    
    // Statistics
    size_t total_allocated;             // Total bytes allocated
    size_t total_free;                  // Total bytes free
    size_t allocation_count;            // Number of active allocations
};

// Global kernel heap
static heap_t* g_kernel_heap = NULL;
static bool g_kernel_heap_initializing = false;

// Slab cache for heap structures
static slab_cache_t* g_heap_cache = NULL;

#pragma region Utility Functions

/*
 * heap_align_size - Align size to BLOCK_ALIGN boundary
 */
size_t heap_align_size(size_t size) {
    return align_up(size, BLOCK_ALIGN);
}

/*
 * get_footer - Get footer from header
 */
static inline heap_block_footer_t* get_footer(heap_block_header_t* header) {
    return (heap_block_footer_t*)((uint8_t*)header + sizeof(heap_block_header_t) + header->size);
}

/*
 * get_user_ptr - Get user data pointer from header
 */
static inline void* get_user_ptr(heap_block_header_t* header) {
    return (void*)((uint8_t*)header + sizeof(heap_block_header_t));
}

/*
 * get_header_from_ptr - Get header from user pointer
 */
static inline heap_block_header_t* get_header_from_ptr(void* ptr) {
    if (!ptr) return NULL;
    return (heap_block_header_t*)((uint8_t*)ptr - sizeof(heap_block_header_t));
}

/*
 * heap_validate_block - Validate block integrity
 */
bool heap_validate_block(heap_block_header_t* header) {
    if (!header) return false;
    
    if (header->magic != BLOCK_MAGIC_USED && header->magic != BLOCK_MAGIC_FREE) {
        LOGF("[HEAP ERROR] Invalid block magic: 0x%x at %p\n", header->magic, header);
        return false;
    }
    
    if (header->red_zone_pre != BLOCK_RED_ZONE) {
        LOGF("[HEAP ERROR] Block pre-red-zone corrupted: 0x%x at %p\n", 
               header->red_zone_pre, header);
        return false;
    }
    
    if (header->red_zone_post != BLOCK_RED_ZONE) {
        LOGF("[HEAP ERROR] Block post-red-zone corrupted: 0x%x at %p\n",
               header->red_zone_post, header);
        return false;
    }
    
    heap_block_footer_t* footer = get_footer(header);
    
    if (footer->magic != header->magic) {
        LOGF("[HEAP ERROR] Footer magic mismatch: header=0x%x footer=0x%x at %p\n",
               header->magic, footer->magic, header);
        return false;
    }
    
    if (footer->red_zone_pre != BLOCK_RED_ZONE || footer->red_zone_post != BLOCK_RED_ZONE) {
        LOGF("[HEAP ERROR] Footer red-zone corrupted at %p\n", header);
        return false;
    }
    
    if (footer->header != header) {
        LOGF("[HEAP ERROR] Footer header pointer mismatch at %p\n", header);
        return false;
    }
    
    return true;
}

/*
 * heap_validate - Validate heap structure
 */
static inline bool heap_validate(heap_t* heap) {
    if (!heap) return false;
    
    if (heap->magic != HEAP_MAGIC) {
        LOGF("[HEAP ERROR] Invalid heap magic: 0x%x\n", heap->magic);
        return false;
    }
    
    return true;
}

#pragma endregion

#pragma region Free List Management

/*
 * remove_from_free_list - Remove block from free list
 */
static void remove_from_free_list(heap_t* heap, heap_block_header_t* block) {
    if (!heap || !block) return;
    
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        heap->free_list = block->next_free;
    }
    
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    
    block->next_free = NULL;
    block->prev_free = NULL;
}

/*
 * insert_into_free_list - Insert block into free list (sorted by size)
 */
static void insert_into_free_list(heap_t* heap, heap_block_header_t* block) {
    if (!heap || !block) return;
    
    block->next_free = NULL;
    block->prev_free = NULL;
    
    // Empty list
    if (!heap->free_list) {
        heap->free_list = block;
        return;
    }
    
    // Insert at beginning if smaller than first block
    if (block->size <= heap->free_list->size) {
        block->next_free = heap->free_list;
        heap->free_list->prev_free = block;
        heap->free_list = block;
        return;
    }
    
    // Find insertion point (keep sorted by size)
    heap_block_header_t* current = heap->free_list;
    while (current->next_free && current->next_free->size < block->size) {
        current = current->next_free;
    }
    
    // Insert after current
    block->next_free = current->next_free;
    block->prev_free = current;
    
    if (current->next_free) {
        current->next_free->prev_free = block;
    }
    
    current->next_free = block;
}

#pragma endregion

#pragma region Block Coalescing

/*
 * get_next_block - Get physically adjacent next block
 */
static heap_block_header_t* get_next_block(heap_t* heap, heap_block_header_t* block) {
    if (!block) return NULL;
    
    uintptr_t next_addr = (uintptr_t)block + block->total_size;
    
    if (next_addr >= heap->heap_end) {
        return NULL;  // No next block
    }
    
    return (heap_block_header_t*)next_addr;
}

/*
 * get_prev_block - Get physically adjacent previous block
 */
static heap_block_header_t* get_prev_block(heap_t* heap, heap_block_header_t* block) {
    if (!block) return NULL;
    
    uintptr_t prev_footer_addr = (uintptr_t)block - sizeof(heap_block_footer_t);
    
    if (prev_footer_addr < heap->heap_start) {
        return NULL;  // No previous block
    }
    
    heap_block_footer_t* prev_footer = (heap_block_footer_t*)prev_footer_addr;
    
    // Validate that this is actually a footer
    if (prev_footer->red_zone_pre != BLOCK_RED_ZONE || 
        prev_footer->red_zone_post != BLOCK_RED_ZONE) {
        return NULL;
    }
    
    return prev_footer->header;
}

/*
 * coalesce_blocks - Coalesce adjacent free blocks
 */
static heap_block_header_t* coalesce_blocks(heap_t* heap, heap_block_header_t* block) {
    if (!heap || !block) return block;
    if (!heap_validate_block(block)) return block;
    
    // Try to coalesce with next block
    heap_block_header_t* next = get_next_block(heap, block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_block(next)) {
        // Merge with next
        remove_from_free_list(heap, block);
        remove_from_free_list(heap, next);
        
        block->size += next->total_size;
        block->total_size += next->total_size;
        
        // Update footer
        heap_block_footer_t* footer = get_footer(block);
        footer->header = block;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;
        
        insert_into_free_list(heap, block);
        
        // Recursively try again
        return coalesce_blocks(heap, block);
    }
    
    // Try to coalesce with previous block
    heap_block_header_t* prev = get_prev_block(heap, block);
    if (prev && prev->magic == BLOCK_MAGIC_FREE && heap_validate_block(prev)) {
        // Merge with previous
        remove_from_free_list(heap, block);
        remove_from_free_list(heap, prev);
        
        prev->size += block->total_size;
        prev->total_size += block->total_size;
        
        // Update footer
        heap_block_footer_t* footer = get_footer(prev);
        footer->header = prev;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;
        
        insert_into_free_list(heap, prev);
        
        // Recursively try again
        return coalesce_blocks(heap, prev);
    }
    
    return block;
}

#pragma endregion

#pragma region Heap Expansion/Contraction

/*
 * expand_heap - Expand heap by allocating more virtual memory
 */
static heap_status_t expand_heap(heap_t* heap, size_t min_increase) {
    if (!heap) return HEAP_ERR_INVALID;
    
    // Calculate new size (double or add min_increase, whichever is larger)
    size_t new_size = heap->current_size * HEAP_EXPAND_FACTOR;
    if (new_size - heap->current_size < min_increase) {
        new_size = heap->current_size + min_increase;
    }
    
    // Align to page size
    new_size = align_up(new_size, PAGE_SIZE);
    
    // Check against max size
    if (new_size > heap->max_size) {
        new_size = heap->max_size;
    }
    
    if (new_size <= heap->current_size) {
        return HEAP_ERR_OOM;  // Can't expand further
    }
    
    size_t increase = new_size - heap->current_size;
    
    // Allocate virtual memory at heap_end
    void* new_region;
    vmm_status_t status = vmm_alloc_at(
        heap->vmm, 
        (void*)heap->heap_end,  // Exact address we need
        increase,
        VM_FLAG_WRITE | (heap->is_kernel ? 0 : VM_FLAG_USER),
        NULL, 
        &new_region
    );
    
    if (status != VMM_OK) {
        LOGF("[HEAP] Failed to expand heap: vmm_alloc returned %d\n", status);
        return HEAP_ERR_VMM_FAIL;
    }
    
    // Verify allocation is contiguous
    if ((uintptr_t)new_region != heap->heap_end) {
        LOGF("[HEAP] Non-contiguous heap expansion: expected 0x%lx, got 0x%lx\n",
               heap->heap_end, (uintptr_t)new_region);
        vmm_status_t free_status = vmm_free(heap->vmm, new_region);
        if (free_status != VMM_OK) {
            LOGF("[HEAP WARNING] Failed to free non-contiguous allocation: %d\n", free_status);
        }
        return HEAP_ERR_VMM_FAIL;
    }
    
    // Create a new free block for the expanded region
    heap_block_header_t* new_block = (heap_block_header_t*)heap->heap_end;
    size_t block_size = increase - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);
    
    new_block->magic = BLOCK_MAGIC_FREE;
    new_block->red_zone_pre = BLOCK_RED_ZONE;
    new_block->red_zone_post = BLOCK_RED_ZONE;
    new_block->size = block_size;
    new_block->total_size = increase;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;
    
    heap_block_footer_t* footer = get_footer(new_block);
    footer->red_zone_pre = BLOCK_RED_ZONE;
    footer->red_zone_post = BLOCK_RED_ZONE;
    footer->header = new_block;
    footer->magic = BLOCK_MAGIC_FREE;
    
    // Update heap metadata
    heap->heap_end += increase;
    heap->current_size = new_size;
    heap->total_free += block_size;
    
    // Add to free list and coalesce
    insert_into_free_list(heap, new_block);
    coalesce_blocks(heap, new_block);
    
    return HEAP_OK;
}

/*
 * try_shrink_heap - Attempt to shrink heap if there's excessive free space
 */
static void try_shrink_heap(heap_t* heap) {
    if (!heap) return;
    
    // Don't shrink below minimum
    if (heap->current_size <= heap->min_size) return;
    
    // Only shrink if we have a lot of free space
    if (heap->total_free < heap->total_allocated * HEAP_SHRINK_THRESHOLD) return;
    
    // Check if last block is free and large enough to warrant shrinking
    heap_block_header_t* last_block = (heap_block_header_t*)(heap->heap_end - sizeof(heap_block_footer_t));
    heap_block_footer_t* last_footer = (heap_block_footer_t*)last_block;
    last_block = last_footer->header;
    
    if (!last_block || last_block->magic != BLOCK_MAGIC_FREE) return;
    if (!heap_validate_block(last_block)) return;
    
    // Calculate shrink amount (must be page-aligned)
    size_t shrink_amount = align_down(last_block->total_size, PAGE_SIZE);
    if (shrink_amount == 0) return;
    
    // Don't shrink below minimum
    if (heap->current_size - shrink_amount < heap->min_size) {
        shrink_amount = heap->current_size - heap->min_size;
        shrink_amount = align_down(shrink_amount, PAGE_SIZE);
    }
    
    if (shrink_amount == 0) return;
    
    // Remove from free list
    remove_from_free_list(heap, last_block);
    
    // Free the virtual memory
    void* shrink_start = (void*)(heap->heap_end - shrink_amount);
    vmm_status_t status = vmm_free(heap->vmm, shrink_start);
    if (status != VMM_OK) {
        LOGF("[HEAP WARNING] Failed to shrink heap: vmm_free returned %d\n", status);
        // Re-add to free list on failure
        insert_into_free_list(heap, last_block);
        return;
    }
    
    // Update heap metadata
    heap->heap_end -= shrink_amount;
    heap->current_size -= shrink_amount;
    heap->total_free -= (shrink_amount - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t));
    
    // If there's still space left in the block, re-add it
    if (last_block->total_size > shrink_amount) {
        last_block->size -= shrink_amount;
        last_block->total_size -= shrink_amount;
        
        heap_block_footer_t* footer = get_footer(last_block);
        footer->header = last_block;
        footer->magic = BLOCK_MAGIC_FREE;
        footer->red_zone_pre = BLOCK_RED_ZONE;
        footer->red_zone_post = BLOCK_RED_ZONE;
        
        insert_into_free_list(heap, last_block);
    }
}

#pragma endregion

#pragma region Allocation/Deallocation

/*
 * find_free_block - Find a free block that fits the requested size
 */
static heap_block_header_t* find_free_block(heap_t* heap, size_t size) {
    if (!heap) return NULL;
    
    // Free list is sorted by size, so first fit is best fit
    heap_block_header_t* current = heap->free_list;
    
    while (current) {
        if (!heap_validate_block(current)) {
            LOGF("[HEAP ERROR] Corrupted block in free list\n");
            return NULL;
        }
        
        if (current->size >= size) {
            return current;
        }
        
        current = current->next_free;
    }
    
    return NULL;
}

/*
 * split_block - Split a block if it's large enough
 */
static void split_block(heap_t* heap, heap_block_header_t* block, size_t size) {
    if (!block || !heap) return;
    
    size_t remaining = block->size - size;
    
    // Only split if remaining space is large enough for another block
    if (remaining < MIN_BLOCK_SIZE + sizeof(heap_block_header_t) + sizeof(heap_block_footer_t)) {
        return;  // Don't split, too small
    }
    
    // Remove old block from free list
    remove_from_free_list(heap, block);
    
    // Update current block
    block->size = size;
    block->total_size = sizeof(heap_block_header_t) + size + sizeof(heap_block_footer_t);
    
    // Create footer for current block
    heap_block_footer_t* footer = get_footer(block);
    footer->red_zone_pre = BLOCK_RED_ZONE;
    footer->red_zone_post = BLOCK_RED_ZONE;
    footer->header = block;
    footer->magic = block->magic;
    
    // Create new free block
    heap_block_header_t* new_block = (heap_block_header_t*)((uint8_t*)block + block->total_size);
    new_block->magic = BLOCK_MAGIC_FREE;
    new_block->red_zone_pre = BLOCK_RED_ZONE;
    new_block->red_zone_post = BLOCK_RED_ZONE;
    new_block->size = remaining - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);
    new_block->total_size = remaining;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;
    
    // Create footer for new block
    heap_block_footer_t* new_footer = get_footer(new_block);
    new_footer->red_zone_pre = BLOCK_RED_ZONE;
    new_footer->red_zone_post = BLOCK_RED_ZONE;
    new_footer->header = new_block;
    new_footer->magic = BLOCK_MAGIC_FREE;
    
    // Update statistics
    heap->total_free -= (sizeof(heap_block_header_t) + sizeof(heap_block_footer_t));
    
    // Add new block to free list
    insert_into_free_list(heap, new_block);
    
    // If original block should remain free, add it back
    if (block->magic == BLOCK_MAGIC_FREE) {
        insert_into_free_list(heap, block);
    }
}

/*
 * heap_malloc_internal - Internal allocation function
 */
static void* heap_malloc_internal(heap_t* heap, size_t size, bool zero, bool urgent) {
    if (!heap || size == 0) {
        if (urgent) {
            panicf("[HEAP] Invalid malloc parameters: heap=%p, size=%zu", heap, size);
        }
        return NULL;
    }
    
    if (!heap_validate(heap)) {
        if (urgent) {
            panicf("[HEAP] Corrupted heap structure at %p", heap);
        }
        return NULL;
    }
    
    // Check if we should zero memory (either explicit request or heap flag)
    bool should_zero = zero || (heap->flags & HEAP_FLAG_ZERO);
    
    // Align size
    size = heap_align_size(size);
    if (size < MIN_BLOCK_SIZE) {
        size = MIN_BLOCK_SIZE;
    }
    
    // Find free block
    heap_block_header_t* block = find_free_block(heap, size);
    
    // Expand if necessary
    if (!block) {
        size_t needed = size + sizeof(heap_block_header_t) + sizeof(heap_block_footer_t);
        heap_status_t status = expand_heap(heap, needed);
        
        if (status != HEAP_OK) {
            if (urgent) {
                panicf("[HEAP] Failed to expand heap: error %d, needed %zu bytes", status, needed);
            }
            return NULL;
        }
        
        block = find_free_block(heap, size);
        if (!block) {
            if (urgent) {
                panicf("[HEAP] No free block found after expansion");
            }
            return NULL;
        }
    }
    
    // Split block if possible
    split_block(heap, block, size);
    
    // Mark as used
    remove_from_free_list(heap, block);
    block->magic = BLOCK_MAGIC_USED;
    
    heap_block_footer_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_USED;
    
    // Update statistics
    heap->total_allocated += block->size;
    heap->total_free -= block->size;
    heap->allocation_count++;
    
    void* ptr = get_user_ptr(block);
    
    // Zero if requested
    if (should_zero) {
        memset(ptr, 0, block->size);
    }
    
    return ptr;
}

/*
 * heap_free_internal - Internal deallocation function
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
    
    // Mark as free
    block->magic = BLOCK_MAGIC_FREE;
    block->next_free = NULL;
    block->prev_free = NULL;
    
    heap_block_footer_t* footer = get_footer(block);
    footer->magic = BLOCK_MAGIC_FREE;
    
    // Update statistics
    heap->total_allocated -= block->size;
    heap->total_free += block->size;
    heap->allocation_count--;
    
    // Add to free list
    insert_into_free_list(heap, block);
    
    // Coalesce with adjacent free blocks
    coalesce_blocks(heap, block);
    
    // Try to shrink heap
    try_shrink_heap(heap);
}

#pragma endregion

#pragma region Kernel Heap Management

/*
 * heap_kernel_init - Initialize the kernel heap
 */
heap_status_t heap_kernel_init(void) {
    if (g_kernel_heap) return HEAP_ERR_ALREADY_INIT;
    if (g_kernel_heap_initializing) return HEAP_ERR_ALREADY_INIT;
    
    g_kernel_heap_initializing = true;
    
    // Get or initialize kernel VMM
    vmm_t* kernel_vmm = vmm_kernel_get();
    if (!kernel_vmm) {
        // Auto-initialize kernel VMM
        LOGF("[HEAP] Kernel VMM not initialized, initializing now...\n");
        
        uintptr_t alloc_base = get_kend(true) + PAGE_SIZE;
        uintptr_t alloc_end = 0xFFFFFFFFFFFFF000;
        
        vmm_status_t vmm_status = vmm_kernel_init(alloc_base, alloc_end);
        if (vmm_status != VMM_OK) {
            LOGF("[HEAP] Failed to initialize kernel VMM: error %d\n", vmm_status);
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
    
    // Create slab cache for heap structures if not already created
    if (!g_heap_cache) {
        g_heap_cache = slab_cache_create("heap_t", sizeof(heap_t), _Alignof(heap_t));
        if (!g_heap_cache) {
            LOGF("[HEAP] Failed to create heap slab cache\n");
            g_kernel_heap_initializing = false;
            return HEAP_ERR_OOM;
        }
    }
    
    // Allocate heap structure
    void* heap_mem;
    slab_status_t slab_status = slab_alloc(g_heap_cache, &heap_mem);
    if (slab_status != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate heap structure: slab error %d\n", slab_status);
        g_kernel_heap_initializing = false;
        return HEAP_ERR_OOM;
    }
    
    heap_t* heap = (heap_t*)heap_mem;
    memset(heap, 0, sizeof(heap_t));
    
    heap->magic = HEAP_MAGIC;
    heap->vmm = kernel_vmm;
    heap->is_kernel = true;
    heap->flags = HEAP_FLAG_NONE;
    heap->min_size = HEAP_MIN_SIZE;
    heap->max_size = SIZE_MAX;  // Unlimited for kernel heap
    
    // Allocate initial heap region
    void* initial_region;
    vmm_status_t vmm_status = vmm_alloc(kernel_vmm, HEAP_MIN_SIZE, VM_FLAG_WRITE, 
                                         NULL, &initial_region);
    
    if (vmm_status != VMM_OK) {
        LOGF("[HEAP] Failed to allocate initial heap region: vmm error %d\n", vmm_status);
        slab_free(g_heap_cache, heap_mem);
        g_kernel_heap_initializing = false;
        return HEAP_ERR_VMM_FAIL;
    }
    
    heap->heap_start = (uintptr_t)initial_region;
    heap->heap_end = heap->heap_start + HEAP_MIN_SIZE;
    heap->current_size = HEAP_MIN_SIZE;
    
    // Create initial free block
    heap_block_header_t* initial_block = (heap_block_header_t*)heap->heap_start;
    size_t initial_block_size = HEAP_MIN_SIZE - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);
    
    initial_block->magic = BLOCK_MAGIC_FREE;
    initial_block->red_zone_pre = BLOCK_RED_ZONE;
    initial_block->red_zone_post = BLOCK_RED_ZONE;
    initial_block->size = initial_block_size;
    initial_block->total_size = HEAP_MIN_SIZE;
    initial_block->next_free = NULL;
    initial_block->prev_free = NULL;
    
    heap_block_footer_t* initial_footer = get_footer(initial_block);
    initial_footer->red_zone_pre = BLOCK_RED_ZONE;
    initial_footer->red_zone_post = BLOCK_RED_ZONE;
    initial_footer->header = initial_block;
    initial_footer->magic = BLOCK_MAGIC_FREE;
    
    heap->free_list = initial_block;
    heap->total_free = initial_block_size;
    heap->total_allocated = 0;
    heap->allocation_count = 0;
    
    g_kernel_heap = heap;
    g_kernel_heap_initializing = false;
    
    LOGF("[HEAP] Kernel heap initialized at 0x%lx - 0x%lx (%zu KiB)\n",
           heap->heap_start, heap->heap_end, heap->current_size / MEASUREMENT_UNIT_KB);
    
    return HEAP_OK;
}

/*
 * heap_kernel_get - Get the kernel heap instance
 */
heap_t* heap_kernel_get(void) {
    // Auto-initialize on first use
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
 * kmalloc - Allocate memory from kernel heap
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
 * kfree - Free memory to kernel heap
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
 * krealloc - Reallocate memory in kernel heap
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
    
    // If new size fits in current block, just return
    size_t aligned_size = heap_align_size(size);
    if (aligned_size <= block->size) {
        // Try to split if much smaller
        if (block->size - aligned_size >= MIN_BLOCK_SIZE + sizeof(heap_block_header_t) + sizeof(heap_block_footer_t)) {
            split_block(heap, block, aligned_size);
        }
        return ptr;
    }
    
    // Try to expand in place by merging with next block if it's free
    heap_block_header_t* next = get_next_block(heap, block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_block(next)) {
        size_t combined_size = block->size + next->total_size;
        if (combined_size >= aligned_size) {
            // Merge with next block
            remove_from_free_list(heap, next);
            
            heap->total_free -= next->size;
            heap->total_allocated += next->total_size;
            
            block->size = combined_size;
            block->total_size += next->total_size;
            
            heap_block_footer_t* footer = get_footer(block);
            footer->header = block;
            footer->magic = BLOCK_MAGIC_USED;
            footer->red_zone_pre = BLOCK_RED_ZONE;
            footer->red_zone_post = BLOCK_RED_ZONE;
            
            // Split if too large
            split_block(heap, block, aligned_size);
            
            return ptr;
        }
    }
    
    // Can't expand in place, allocate new block and copy
    void* new_ptr = kmalloc(size);
    if (!new_ptr) {
        LOGF("[HEAP] krealloc: failed to allocate %zu bytes\n", size);
        return NULL;
    }
    
    memcpy(new_ptr, ptr, block->size < size ? block->size : size);
    kfree(ptr);
    
    return new_ptr;
}

/*
 * kcalloc - Allocate and zero memory from kernel heap
 */
void* kcalloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    
    // Check for overflow
    size_t total = nmemb * size;
    if (total / nmemb != size) {
        LOGF("[HEAP] kcalloc: overflow detected (nmemb=%zu, size=%zu)\n", nmemb, size);
        return NULL;
    }
    
    heap_t* heap = heap_kernel_get();
    if (!heap) {
        LOGF("[HEAP] kcalloc: kernel heap not available\n");
        return NULL;
    }
    
    return heap_malloc_internal(heap, total, true, false);
}

#pragma endregion

#pragma region User Heap Management

/*
 * heap_create - Create a new heap instance
 */
heap_t* heap_create(vmm_t* vmm, size_t min_size, size_t max_size, uint32_t flags) {
    if (!vmm) {
        LOGF("[HEAP] heap_create: NULL vmm parameter\n");
        return NULL;
    }
    if (min_size == 0) min_size = HEAP_MIN_SIZE;
    if (max_size < min_size) {
        LOGF("[HEAP] heap_create: max_size (%zu) < min_size (%zu)\n", max_size, min_size);
        return NULL;
    }
    
    // Align sizes
    min_size = align_up(min_size, PAGE_SIZE);
    max_size = align_up(max_size, PAGE_SIZE);
    
    // Ensure slab cache exists
    if (!g_heap_cache) {
        g_heap_cache = slab_cache_create("heap_t", sizeof(heap_t), _Alignof(heap_t));
        if (!g_heap_cache) {
            LOGF("[HEAP] heap_create: failed to create slab cache\n");
            return NULL;
        }
    }
    
    // Allocate heap structure
    void* heap_mem;
    slab_status_t slab_status = slab_alloc(g_heap_cache, &heap_mem);
    if (slab_status != SLAB_OK) {
        LOGF("[HEAP] heap_create: failed to allocate heap structure: slab error %d\n", slab_status);
        return NULL;
    }
    
    heap_t* heap = (heap_t*)heap_mem;
    memset(heap, 0, sizeof(heap_t));
    
    heap->magic = HEAP_MAGIC;
    heap->vmm = vmm;
    heap->is_kernel = false;
    heap->flags = flags;
    heap->min_size = min_size;
    heap->max_size = max_size;
    
    // Allocate initial heap region
    void* initial_region;
    vmm_status_t vmm_status = vmm_alloc(vmm, min_size, 
                                         VM_FLAG_WRITE | VM_FLAG_USER,
                                         NULL, &initial_region);
    
    if (vmm_status != VMM_OK) {
        LOGF("[HEAP] heap_create: failed to allocate initial region: vmm error %d\n", vmm_status);
        slab_free(g_heap_cache, heap_mem);
        return NULL;
    }
    
    heap->heap_start = (uintptr_t)initial_region;
    heap->heap_end = heap->heap_start + min_size;
    heap->current_size = min_size;
    
    // Create initial free block
    heap_block_header_t* initial_block = (heap_block_header_t*)heap->heap_start;
    size_t initial_block_size = min_size - sizeof(heap_block_header_t) - sizeof(heap_block_footer_t);
    
    initial_block->magic = BLOCK_MAGIC_FREE;
    initial_block->red_zone_pre = BLOCK_RED_ZONE;
    initial_block->red_zone_post = BLOCK_RED_ZONE;
    initial_block->size = initial_block_size;
    initial_block->total_size = min_size;
    initial_block->next_free = NULL;
    initial_block->prev_free = NULL;
    
    heap_block_footer_t* initial_footer = get_footer(initial_block);
    initial_footer->red_zone_pre = BLOCK_RED_ZONE;
    initial_footer->red_zone_post = BLOCK_RED_ZONE;
    initial_footer->header = initial_block;
    initial_footer->magic = BLOCK_MAGIC_FREE;
    
    heap->free_list = initial_block;
    heap->total_free = initial_block_size;
    heap->total_allocated = 0;
    heap->allocation_count = 0;

    LOGF("[HEAP] User heap initialized at 0x%lx - 0x%lx (%zu MiB)\n",
           heap->heap_start, heap->heap_end, heap->current_size / MEASUREMENT_UNIT_MB);
    
    return heap;
}

/*
 * heap_destroy - Destroy a heap instance
 */
void heap_destroy(heap_t* heap) {
    if (!heap) return;
    if (!heap_validate(heap)) {
        LOGF("[HEAP] heap_destroy: invalid heap at %p\n", heap);
        return;
    }
    
    // Can't destroy kernel heap
    if (heap == g_kernel_heap) {
        LOGF("[HEAP ERROR] Cannot destroy kernel heap\n");
        return;
    }
    
    // Free all heap memory
    if (heap->heap_start) {
        vmm_status_t status = vmm_free(heap->vmm, (void*)heap->heap_start);
        if (status != VMM_OK) {
            LOGF("[HEAP WARNING] Failed to free heap memory: vmm error %d\n", status);
        }
    }
    
    // Clear magic
    heap->magic = 0;
    
    // Free heap structure
    slab_free(g_heap_cache, heap);

    LOGF("[HEAP] User heap destroyed\n");
}

/*
 * heap_malloc - Allocate memory from heap
 */
void* heap_malloc(heap_t* heap, size_t size) {
    if (!heap) return NULL;
    bool urgent = (heap->flags & HEAP_FLAG_URGENT) != 0;
    return heap_malloc_internal(heap, size, false, urgent);
}

/*
 * heap_free - Free memory to heap
 */
void heap_free(heap_t* heap, void* ptr) {
    if (!heap || !ptr) return;
    heap_free_internal(heap, ptr);
}

/*
 * heap_realloc - Reallocate memory in heap
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
    
    // If new size fits in current block, just return
    size_t aligned_size = heap_align_size(size);
    if (aligned_size <= block->size) {
        if (block->size - aligned_size >= MIN_BLOCK_SIZE + sizeof(heap_block_header_t) + sizeof(heap_block_footer_t)) {
            split_block(heap, block, aligned_size);
        }
        return ptr;
    }
    
    // Try to expand in place
    heap_block_header_t* next = get_next_block(heap, block);
    if (next && next->magic == BLOCK_MAGIC_FREE && heap_validate_block(next)) {
        size_t combined_size = block->size + next->total_size;
        if (combined_size >= aligned_size) {
            remove_from_free_list(heap, next);
            
            heap->total_free -= next->size;
            heap->total_allocated += next->total_size;
            
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
    
    // Allocate new and copy
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
 * heap_calloc - Allocate and zero memory from heap
 */
void* heap_calloc(heap_t* heap, size_t nmemb, size_t size) {
    if (!heap || nmemb == 0 || size == 0) return NULL;
    
    size_t total = nmemb * size;
    if (total / nmemb != size) {
        LOGF("[HEAP] heap_calloc: overflow detected (nmemb=%zu, size=%zu)\n", nmemb, size);
        return NULL;
    }
    
    bool urgent = (heap->flags & HEAP_FLAG_URGENT) != 0;
    return heap_malloc_internal(heap, total, true, urgent);
}

#pragma endregion

#pragma region Introspection and Debugging

/*
 * heap_check_integrity - Verify heap integrity
 */
heap_status_t heap_check_integrity(heap_t* heap) {
    if (!heap_validate(heap)) {
        return HEAP_ERR_INVALID;
    }
    
    size_t calculated_free = 0;
    size_t calculated_used = 0;
    size_t free_blocks = 0;
    size_t used_blocks = 0;
    
    // Walk through physical memory
    uintptr_t current_addr = heap->heap_start;
    
    while (current_addr < heap->heap_end) {
        heap_block_header_t* block = (heap_block_header_t*)current_addr;
        
        if (!heap_validate_block(block)) {
            LOGF("[HEAP INTEGRITY] Block validation failed at 0x%lx\n", current_addr);
            return HEAP_ERR_CORRUPTED;
        }
        
        if (block->magic == BLOCK_MAGIC_FREE) {
            calculated_free += block->size;
            free_blocks++;
        } else if (block->magic == BLOCK_MAGIC_USED) {
            calculated_used += block->size;
            used_blocks++;
        } else {
            LOGF("[HEAP INTEGRITY] Invalid magic 0x%x at 0x%lx\n", block->magic, current_addr);
            return HEAP_ERR_CORRUPTED;
        }
        
        current_addr += block->total_size;
    }
    
    if (current_addr != heap->heap_end) {
        LOGF("[HEAP INTEGRITY] Heap walk ended at 0x%lx, expected 0x%lx\n",
               current_addr, heap->heap_end);
        return HEAP_ERR_CORRUPTED;
    }
    
    // Verify statistics
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
    
    // Verify free list
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
        
        // Check sorting (should be sorted by size)
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
 * heap_dump - Dump heap state for debugging
 */
void heap_dump(heap_t* heap) {
    if (!heap_validate(heap)) {
        LOGF("[HEAP DUMP] Invalid heap\n");
        return;
    }
    
    LOGF("=== HEAP DUMP ===\n");
    LOGF("Heap at %p (magic: 0x%x, is_kernel: %d)\n",
           heap, heap->magic, heap->is_kernel);
    LOGF("Range: 0x%lx - 0x%lx (current: %zu bytes, min: %zu, max: %zu)\n",
           heap->heap_start, heap->heap_end, heap->current_size,
           heap->min_size, heap->max_size);
    LOGF("Allocated: %zu bytes in %zu blocks\n",
           heap->total_allocated, heap->allocation_count);
    LOGF("Free: %zu bytes\n", heap->total_free);
    LOGF("Overhead: %zu bytes\n",
           heap->current_size - heap->total_allocated - heap->total_free);
    
    LOGF("\nPhysical blocks:\n");
    uintptr_t current_addr = heap->heap_start;
    int block_num = 0;
    
    while (current_addr < heap->heap_end) {
        heap_block_header_t* block = (heap_block_header_t*)current_addr;
        
        if (!heap_validate_block(block)) {
            LOGF("  [%d] CORRUPTED at 0x%lx\n", block_num, current_addr);
            break;
        }
        
        LOGF("  [%d] 0x%lx: %s, size=%zu, total=%zu\n",
               block_num, current_addr,
               block->magic == BLOCK_MAGIC_FREE ? "FREE" : "USED",
               block->size, block->total_size);
        
        current_addr += block->total_size;
        block_num++;
    }
    
    LOGF("\nFree list (sorted by size):\n");
    heap_block_header_t* free_block = heap->free_list;
    int free_num = 0;
    
    while (free_block) {
        LOGF("  [%d] 0x%lx: size=%zu\n",
               free_num, (uintptr_t)free_block, free_block->size);
        free_block = free_block->next_free;
        free_num++;
    }
    
    if (free_num == 0) {
        LOGF("  (no free blocks)\n");
    }
    
    LOGF("=================\n");
}

/*
 * heap_stats - Get heap statistics
 */
void heap_stats(heap_t* heap, size_t* total, size_t* used, size_t* free, size_t* overhead) {
    if (!heap_validate(heap)) return;
    
    if (total) *total = heap->current_size;
    if (used) *used = heap->total_allocated;
    if (free) *free = heap->total_free;
    if (overhead) {
        *overhead = heap->current_size - heap->total_allocated - heap->total_free;
    }
}

/*
 * heap_get_alloc_size - Get the size of an allocation
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