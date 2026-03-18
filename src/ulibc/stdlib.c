/*
 * stdlib.c - Subset of the C standard library for GatOS
 *
 * Boundary-tag allocator with arena per mmap design.
 * Mirrors kernel heap.c but uses syscall_mmap/syscall_munmap instead of the VMM
 * and embeds the arena header at the start of each mmap'd region.
 */

#include <ulibc/stdlib.h>
#include <ulibc/syscalls.h>
#include <ulibc/string.h>
#include <ulibc/spinlock.h>
#include <stdint.h>
#include <stdbool.h>

// Magic numbers
#define HEAP_MAGIC       0xF005BA11u
#define ARENA_MAGIC      0x1CEB00DAu
#define BLOCK_MAGIC_USED 0xABADCAFEu
#define BLOCK_MAGIC_FREE 0xA110CA7Eu
#define BLOCK_RED_ZONE   0x8BADF00Du

// Layout constants
#define BLOCK_ALIGN      16
#define MIN_BLOCK_SIZE   32
#define MIN_ARENA_BODY   (64 * 1024)
#define PAGE_SIZE        4096
#define SHRINK_THRESHOLD 4
#define MMAP_RW          1u  // VM_FLAG_WRITE

#define align_up(x, a) (((x) + (size_t)(a) - 1) & ~((size_t)(a) - 1))

// Forward declarations
typedef struct arena arena_t;
typedef struct block block_t;
typedef struct bfooter bfooter_t;

// Block header
struct block {
    uint32_t magic;
    uint32_t rz_pre;
    size_t   size;
    size_t   total_size;
    arena_t *arena;
    block_t *next_free;
    block_t *prev_free;
    uint32_t rz_post;
} __attribute__((aligned(16)));

// Block footer
struct bfooter {
    uint32_t rz_pre;
    block_t *header;
    uint32_t magic;
    uint32_t rz_post;
} __attribute__((aligned(16)));

// Arena
struct arena {
    uint32_t magic;
    arena_t *next;
    arena_t *prev;
    uintptr_t start;
    uintptr_t end;
    size_t    size;
    block_t  *first_block;
    size_t    total_free;
    size_t    total_alloc;
};

// Heap
typedef struct {
    uint32_t magic;
    arena_t *arenas;
    block_t *free_list;
    size_t   total_free;
    size_t   total_alloc;
    size_t   alloc_count;
    size_t   arena_count;
} heap_t;

static heap_t g_heap;
static ulock_t g_heap_lock; // zero-initialized in .user_bss (unlocked)

#pragma region Utility

static inline bfooter_t *get_footer(block_t *b) {
    return (bfooter_t *)((uint8_t *)b + sizeof(block_t) + b->size);
}

static inline void *get_user_ptr(block_t *b) {
    return (void *)((uint8_t *)b + sizeof(block_t));
}

static inline block_t *get_header(void *ptr) {
    if (!ptr) return NULL;
    return (block_t *)((uint8_t *)ptr - sizeof(block_t));
}

static inline bool block_valid(block_t *b) {
    if (!b) return false;
    if (b->magic != BLOCK_MAGIC_USED && b->magic != BLOCK_MAGIC_FREE)
        return false;
    if (b->rz_pre != BLOCK_RED_ZONE || b->rz_post != BLOCK_RED_ZONE)
        return false;
    bfooter_t *f = get_footer(b);
    if (f->magic != b->magic)                                    return false;
    if (f->rz_pre != BLOCK_RED_ZONE || f->rz_post != BLOCK_RED_ZONE) return false;
    if (f->header != b)                                          return false;
    return true;
}

#pragma endregion

#pragma region Free List

static void fl_remove(block_t *b) {
    if (b->prev_free && b->prev_free->next_free != b) {
        // Crash loudly, no kernel panic available here
        *(volatile int*)0 = 0;
    }
    if (b->next_free && b->next_free->prev_free != b) {
        *(volatile int*)0 = 0;
    }

    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else              g_heap.free_list = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->next_free = b->prev_free = NULL;
}

static void fl_insert(block_t *b) {
    b->next_free = b->prev_free = NULL;
    if (!g_heap.free_list) { g_heap.free_list = b; return; }
    if (b->size <= g_heap.free_list->size) {
        b->next_free = g_heap.free_list;
        g_heap.free_list->prev_free = b;
        g_heap.free_list = b;
        return;
    }
    block_t *cur = g_heap.free_list;
    while (cur->next_free && cur->next_free->size < b->size)
        cur = cur->next_free;
    b->next_free = cur->next_free;
    b->prev_free = cur;
    if (cur->next_free) cur->next_free->prev_free = b;
    cur->next_free = b;
}

#pragma endregion

#pragma region Block navigation

static block_t *next_block(block_t *b) {
    if (!b || !b->arena) return NULL;
    uintptr_t addr = (uintptr_t)b + b->total_size;
    if (addr >= b->arena->end) return NULL;
    return (block_t *)addr;
}

static block_t *prev_block(block_t *b) {
    if (!b || !b->arena) return NULL;
    uintptr_t pf_addr = (uintptr_t)b - sizeof(bfooter_t);
    if (pf_addr < b->arena->start) return NULL;
    bfooter_t *pf = (bfooter_t *)pf_addr;
    if (pf->rz_pre != BLOCK_RED_ZONE || pf->rz_post != BLOCK_RED_ZONE)
        return NULL;
    return pf->header;
}

#pragma endregion

#pragma region Coalescing

static block_t *coalesce(block_t *b) {
    if (!block_valid(b)) return b;

    block_t *nxt = next_block(b);
    if (nxt && nxt->magic == BLOCK_MAGIC_FREE && block_valid(nxt)) {
        fl_remove(b);
        fl_remove(nxt);
        // reclaim the absorbed block's header+footer as usable free space
        size_t oh = sizeof(block_t) + sizeof(bfooter_t);
        if (b->arena) b->arena->total_free += oh;
        g_heap.total_free += oh;
        b->size       += nxt->total_size;
        b->total_size += nxt->total_size;
        bfooter_t *f = get_footer(b);
        f->header = b; f->magic = BLOCK_MAGIC_FREE;
        f->rz_pre = f->rz_post = BLOCK_RED_ZONE;
        fl_insert(b);
        return coalesce(b);
    }

    block_t *prv = prev_block(b);
    if (prv && prv->magic == BLOCK_MAGIC_FREE && block_valid(prv)) {
        fl_remove(b);
        fl_remove(prv);
        size_t oh = sizeof(block_t) + sizeof(bfooter_t);
        if (prv->arena) prv->arena->total_free += oh;
        g_heap.total_free += oh;
        prv->size       += b->total_size;
        prv->total_size += b->total_size;
        bfooter_t *f = get_footer(prv);
        f->header = prv; f->magic = BLOCK_MAGIC_FREE;
        f->rz_pre = f->rz_post = BLOCK_RED_ZONE;
        fl_insert(prv);
        return coalesce(prv);
    }

    return b;
}

#pragma endregion

#pragma region Split

static void split_block(block_t *b, size_t size) {
    size_t oh        = sizeof(block_t) + sizeof(bfooter_t);
    size_t remaining = b->size - size;
    if (remaining < MIN_BLOCK_SIZE + oh) return;

    bool was_free = (b->magic == BLOCK_MAGIC_FREE);

    if (was_free) {
        fl_remove(b);
        // the new header+footer overhead is consumed from free space
        if (b->arena) b->arena->total_free -= oh;
        g_heap.total_free -= oh;
    } else {
        // splitting a used block (realloc shrink): carve out new free payload
        size_t new_free = remaining - oh;
        if (b->arena) {
            b->arena->total_alloc -= remaining;
            b->arena->total_free  += new_free;
        }
        g_heap.total_alloc -= remaining;
        g_heap.total_free  += new_free;
    }

    b->size       = size;
    b->total_size = sizeof(block_t) + size + sizeof(bfooter_t);

    bfooter_t *f = get_footer(b);
    f->rz_pre = f->rz_post = BLOCK_RED_ZONE;
    f->header = b; f->magic = b->magic;

    block_t *nb = (block_t *)((uint8_t *)b + b->total_size);
    nb->magic      = BLOCK_MAGIC_FREE;
    nb->rz_pre     = nb->rz_post = BLOCK_RED_ZONE;
    nb->size       = remaining - oh;
    nb->total_size = remaining;
    nb->arena      = b->arena;
    nb->next_free  = nb->prev_free = NULL;

    bfooter_t *nf = get_footer(nb);
    nf->rz_pre = nf->rz_post = BLOCK_RED_ZONE;
    nf->header = nb; nf->magic = BLOCK_MAGIC_FREE;

    fl_insert(nb);
    if (was_free) fl_insert(b);
}

#pragma endregion

#pragma region Arena

// Offset from mmap base to first block, aligned to BLOCK_ALIGN
#define ARENA_HDR_SIZE align_up(sizeof(arena_t), BLOCK_ALIGN)

static arena_t *arena_create(size_t min_body) {
    size_t total = align_up(ARENA_HDR_SIZE + min_body, PAGE_SIZE);

    void *region = syscall_mmap(NULL, total, MMAP_RW);
    if (!region || region == (void *)(uintptr_t)-1) return NULL;

    arena_t *arena = (arena_t *)region;
    memset(arena, 0, sizeof(arena_t));

    arena->magic = ARENA_MAGIC;
    arena->start = (uintptr_t)region + ARENA_HDR_SIZE;
    arena->end   = (uintptr_t)region + total;
    arena->size  = total;

    // Initial free block covers the entire body
    size_t body    = total - ARENA_HDR_SIZE;
    size_t payload = body - sizeof(block_t) - sizeof(bfooter_t);

    block_t *b = (block_t *)arena->start;
    b->magic      = BLOCK_MAGIC_FREE;
    b->rz_pre     = b->rz_post = BLOCK_RED_ZONE;
    b->size       = payload;
    b->total_size = body;
    b->arena      = arena;
    b->next_free  = b->prev_free = NULL;

    bfooter_t *f = get_footer(b);
    f->rz_pre = f->rz_post = BLOCK_RED_ZONE;
    f->header = b; f->magic = BLOCK_MAGIC_FREE;

    arena->first_block = b;
    arena->total_free  = payload;
    arena->total_alloc = 0;

    // Append to arena list
    if (!g_heap.arenas) {
        g_heap.arenas = arena;
    } else {
        arena_t *tail = g_heap.arenas;
        while (tail->next) tail = tail->next;
        tail->next = arena;
        arena->prev = tail;
    }

    g_heap.total_free += payload;
    g_heap.arena_count++;

    fl_insert(b);
    return arena;
}

static void arena_destroy(arena_t *arena) {
    if (!arena || arena->magic != ARENA_MAGIC) return;

    // Remove all of this arena's blocks from the global free list
    block_t *cur = g_heap.free_list;
    while (cur) {
        block_t *nxt = cur->next_free;
        if (cur->arena == arena) fl_remove(cur);
        cur = nxt;
    }

    g_heap.total_free -= arena->total_free;
    g_heap.arena_count--;

    if (arena->prev) arena->prev->next = arena->next;
    else             g_heap.arenas = arena->next;
    if (arena->next) arena->next->prev = arena->prev;

    arena->magic = 0;
    syscall_munmap((void *)arena);   // arena IS the mmap base
}

static void try_shrink(arena_t *arena) {
    if (!arena || arena->magic != ARENA_MAGIC) return;
    if (g_heap.arena_count <= 1) return;
    if (arena->total_alloc > 0) return;
    if (g_heap.total_free < g_heap.total_alloc * SHRINK_THRESHOLD) return;

    // Arena is empty when its free payload equals body - block overhead
    size_t body = arena->size - ARENA_HDR_SIZE;
    size_t oh   = sizeof(block_t) + sizeof(bfooter_t);
    if (arena->total_free + oh >= body)
        arena_destroy(arena);
}

#pragma endregion

#pragma region Heap init

static void heap_init(void) {
    if (g_heap.magic == HEAP_MAGIC) return;
    g_heap.magic = HEAP_MAGIC;
    arena_create(MIN_ARENA_BODY);
}

#pragma endregion

#pragma region Core alloc/free

static void *heap_alloc(size_t size, bool zero) {
    heap_init();

    size_t orig_size = size;
    size = align_up(size, BLOCK_ALIGN);
    if (size < orig_size) return NULL; // Overflow
    
    if (size < MIN_BLOCK_SIZE) size = MIN_BLOCK_SIZE;

    // First-fit from sorted free list
    block_t *b = g_heap.free_list;
    while (b && b->size < size) b = b->next_free;

    if (!b) {
        // No block fits; expand the heap with a new arena
        size_t needed = size + sizeof(block_t) + sizeof(bfooter_t);
        size_t body   = needed > MIN_ARENA_BODY ? needed : MIN_ARENA_BODY;
        if (!arena_create(body)) return NULL;
        b = g_heap.free_list;
        while (b && b->size < size) b = b->next_free;
        if (!b) return NULL;
    }

    split_block(b, size);
    fl_remove(b);

    b->magic = BLOCK_MAGIC_USED;
    get_footer(b)->magic = BLOCK_MAGIC_USED;

    if (b->arena) {
        b->arena->total_free  -= b->size;
        b->arena->total_alloc += b->size;
    }
    g_heap.total_free  -= b->size;
    g_heap.total_alloc += b->size;
    g_heap.alloc_count++;

    void *ptr = get_user_ptr(b);
    if (zero) memset(ptr, 0, b->size);
    return ptr;
}

static void heap_free(void *ptr) {
    if (!ptr) return;

    block_t *b = get_header(ptr);
    if (!block_valid(b) || b->magic != BLOCK_MAGIC_USED) return;

    b->magic = BLOCK_MAGIC_FREE;
    b->next_free = b->prev_free = NULL;
    get_footer(b)->magic = BLOCK_MAGIC_FREE;

    if (b->arena) {
        b->arena->total_alloc -= b->size;
        b->arena->total_free  += b->size;
    }
    g_heap.total_alloc -= b->size;
    g_heap.total_free  += b->size;
    g_heap.alloc_count--;

    fl_insert(b);
    b = coalesce(b);
    if (b->arena) try_shrink(b->arena);
}

#pragma endregion   

#pragma region Public API

void *malloc(size_t size) {
    if (!size) return NULL;
    ulock_acquire(&g_heap_lock);
    void *p = heap_alloc(size, false);
    ulock_release(&g_heap_lock);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    ulock_acquire(&g_heap_lock);
    heap_free(ptr);
    ulock_release(&g_heap_lock);
}

void *calloc(size_t nmemb, size_t size) {
    if (!nmemb || !size) return NULL;
    size_t total = nmemb * size;
    if (total / nmemb != size) return NULL;  // overflow
    ulock_acquire(&g_heap_lock);
    void *p = heap_alloc(total, true);
    ulock_release(&g_heap_lock);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }

    ulock_acquire(&g_heap_lock);

    block_t *b = get_header(ptr);
    if (!block_valid(b) || b->magic != BLOCK_MAGIC_USED) {
        ulock_release(&g_heap_lock);
        return NULL;
    }

    size_t aligned = align_up(size, BLOCK_ALIGN);
    if (aligned < size) {
        ulock_release(&g_heap_lock);
        return NULL;
    }
    if (aligned < MIN_BLOCK_SIZE) aligned = MIN_BLOCK_SIZE;

    // Shrink in place
    if (aligned <= b->size) {
        size_t oh = sizeof(block_t) + sizeof(bfooter_t);
        if (b->size - aligned >= MIN_BLOCK_SIZE + oh)
            split_block(b, aligned);
        ulock_release(&g_heap_lock);
        return ptr;
    }

    // Try to absorb the adjacent free block
    block_t *nxt = next_block(b);
    if (nxt && nxt->magic == BLOCK_MAGIC_FREE && block_valid(nxt)) {
        size_t combined = b->size + nxt->total_size;
        if (combined >= aligned) {
            fl_remove(nxt);
            size_t oh = sizeof(block_t) + sizeof(bfooter_t);
            if (b->arena) {
                b->arena->total_free  -= nxt->size;
                b->arena->total_alloc += nxt->size + oh;
            }
            g_heap.total_free  -= nxt->size;
            g_heap.total_alloc += nxt->size + oh;
            b->size       = combined;
            b->total_size += nxt->total_size;
            bfooter_t *f = get_footer(b);
            f->header = b; f->magic = BLOCK_MAGIC_USED;
            f->rz_pre = f->rz_post = BLOCK_RED_ZONE;
            split_block(b, aligned);
            ulock_release(&g_heap_lock);
            return ptr;
        }
    }

    // Fallback: allocate elsewhere, copy, free old.
    // Save copy_size before releasing the lock (b->size must not be read after).
    size_t copy_size = b->size < size ? b->size : size;
    void *new_ptr = heap_alloc(size, false); // called while lock is held — fine
    ulock_release(&g_heap_lock);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, copy_size);
    ulock_acquire(&g_heap_lock);
    heap_free(ptr);
    ulock_release(&g_heap_lock);
    return new_ptr;
}

#pragma endregion
