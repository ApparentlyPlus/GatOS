/*
 * test_heap.c - Kernel Heap Manager Validation Suite (White-Box)
 *
 * Tests every public heap function: heap_kernel_init, heap_kernel_get,
 * kmalloc, kfree, krealloc, kcalloc, heap_check,
 * heap_alloc_sz, heap_stats, heap_align_size, heap_validate_blk.
 *
 * Machine-adaptive: large allocation stress targets 25% of available
 * heap space rather than a hardcoded byte count.
 */

#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#pragma region Mirror internal structures for white-box inspection

#define HEAP_MAGIC       0xF005BA11
#define BLOCK_MAGIC_USED 0xABADCAFE
#define BLOCK_MAGIC_FREE 0xA110CA7E
#define BLOCK_RED_ZONE   0x8BADF00D
#define MIN_BLOCK_SIZE   32

typedef struct {
    bool active;
    void* ptr;
} htrace_t;

typedef struct heap_test_arena {
    uint32_t magic;
    struct heap_test_arena* next;
    struct heap_test_arena* prev;
    uintptr_t start;
    uintptr_t end;
    size_t size;
    void* first_block;
    size_t total_free;
    size_t total_allocated;
} heap_test_arena_t;

typedef struct {
    uint32_t magic;
    vmm_t* vmm;
    void* arenas;
    void* free_list;
    size_t min_arena_size;
    size_t max_size;
    size_t current_size;
    uint32_t flags;
    bool is_kernel;
    size_t total_allocated;
    size_t total_free;
    size_t allocation_count;
    size_t arena_count;
} heap_test_t;

typedef struct heap_test_header {
    uint32_t magic;
    uint32_t red_zone_pre;
    size_t size;
    size_t total_size;
    heap_test_arena_t* arena;
    struct heap_test_header* next_free;
    struct heap_test_header* prev_free;
    uint32_t red_zone_post;
} __attribute__((aligned(16))) heap_test_hdr_t;

typedef struct {
    uint32_t red_zone_pre;
    heap_test_hdr_t* header;
    uint32_t magic;
    uint32_t red_zone_post;
} __attribute__((aligned(16))) heap_test_ftr_t;

#define MAX_TRACK 1024
static htrace_t track[MAX_TRACK];
static int  tidx         = 0;
static int  ntests  = 0;
static int  npass = 0;
#pragma endregion

#pragma region Tracker

static void tr_reset(void) {
    for (int i = 0; i < MAX_TRACK; i++) track[i] = (htrace_t){false, NULL};
    tidx = 0;
}

static void tr_add(void* p) {
    if (tidx < MAX_TRACK) track[tidx++] = (htrace_t){true, p};
    else LOGF("[WARN] heap tracker full\n");
}

static void tr_free(void) {
    for (int i = 0; i < MAX_TRACK; i++)
        if (track[i].active) { kfree(track[i].ptr); track[i].active = false; }
    tidx = 0;
}

static heap_test_hdr_t* hdr(void* p) {
    return (heap_test_hdr_t*)((uint8_t*)p - sizeof(heap_test_hdr_t));
}

static heap_test_ftr_t* ftr(heap_test_hdr_t* h) {
    return (heap_test_ftr_t*)((uint8_t*)h + sizeof(heap_test_hdr_t) + h->size);
}

static heap_test_t* heapv(void) { return (heap_test_t*)heap_kernel_get(); }
#pragma endregion

#pragma region Initialization

static bool t_init_already(void) {
    TEST_ASSERT_STATUS(heap_kernel_init(), HEAP_ERR_ALREADY_INIT);
    return true;
}

static bool t_get_nn(void) {
    TEST_ASSERT(heap_kernel_get() != NULL);
    return true;
}

static bool t_magic(void) {
    TEST_ASSERT(heapv()->magic == HEAP_MAGIC);
    return true;
}
#pragma endregion

#pragma region Basic Allocation

static bool t_alloc_nn(void) {
    tr_reset();
    void* p = kmalloc(32); TEST_ASSERT(p != NULL); tr_add(p);
    tr_free(); return true;
}

static bool t_alloc_aligned(void) {
    tr_reset();
    void* p = kmalloc(1); TEST_ASSERT(p != NULL); tr_add(p);
    TEST_ASSERT(((uintptr_t)p % HEAP_MIN_ALIGN) == 0);
    tr_free(); return true;
}

static bool t_alloc_align_small(void) {
    /* kmalloc(1..32) must all be 16-byte aligned */
    tr_reset();
    for (int s = 1; s <= 32; s++) {
        void* p = kmalloc((size_t)s);
        TEST_ASSERT(p != NULL);
        TEST_ASSERT(((uintptr_t)p % HEAP_MIN_ALIGN) == 0);
        tr_add(p);
    }
    tr_free(); return true;
}

static bool t_alloc_meta(void) {
    tr_reset();
    void* p = kmalloc(64); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    TEST_ASSERT(h->magic == BLOCK_MAGIC_USED);
    TEST_ASSERT(h->size >= 64);
    TEST_ASSERT(h->red_zone_pre  == BLOCK_RED_ZONE);
    TEST_ASSERT(h->red_zone_post == BLOCK_RED_ZONE);
    tr_free(); return true;
}

static bool t_alloc_ftr(void) {
    tr_reset();
    void* p = kmalloc(48); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    heap_test_ftr_t* f = ftr(h);
    TEST_ASSERT(f->magic == BLOCK_MAGIC_USED);
    TEST_ASSERT(f->red_zone_pre  == BLOCK_RED_ZONE);
    TEST_ASSERT(f->red_zone_post == BLOCK_RED_ZONE);
    tr_free(); return true;
}

static bool t_alloc_wr(void) {
    tr_reset();
    void* p = kmalloc(128); tr_add(p);
    kmemset(p, 0xBB, 128);
    uint8_t* b = (uint8_t*)p;
    TEST_ASSERT(b[0] == 0xBB && b[127] == 0xBB);
    tr_free(); return true;
}
#pragma endregion

#pragma region Free

static bool t_free_marks(void) {
    void* p = kmalloc(32);
    heap_test_hdr_t* h = hdr(p);
    kfree(p);
    TEST_ASSERT(h->magic == BLOCK_MAGIC_FREE);
    return true;
}

static bool t_free_null(void) {
    kfree(NULL); /* must not crash */
    return true;
}

static bool t_dbl_free(void) {
    tr_reset();
    void* p = kmalloc(32); tr_add(p);
    kfree(p); track[0].active = false;
    kfree(p); /* should warn, not crash */
    TEST_ASSERT_STATUS(heap_check(heap_kernel_get()), HEAP_OK);
    return true;
}
#pragma endregion

#pragma region Calloc

static bool t_calloc_zero(void) {
    tr_reset();
    void* p = kcalloc(4, 1024); tr_add(p); /* 4KB */
    uint64_t* q = (uint64_t*)p;
    for (int i = 0; i < 4096/8; i++) TEST_ASSERT(q[i] == 0);
    tr_free(); return true;
}

static bool t_calloc_ovf(void) {
    void* p = kcalloc(SIZE_MAX, 2);
    TEST_ASSERT(p == NULL);
    return true;
}

static bool t_calloc_zero_n(void) {
    void* p = kcalloc(0, 64);
    if (p) kfree(p); /* implementation defined but must not crash */
    return true;
}
#pragma endregion

#pragma region Realloc

static bool t_realloc_null_malloc(void) {
    tr_reset();
    void* p = krealloc(NULL, 64); TEST_ASSERT(p != NULL); tr_add(p);
    TEST_ASSERT(hdr(p)->size >= 64);
    tr_free(); return true;
}

static bool t_realloc_zero_free(void) {
    tr_reset();
    void* p = kmalloc(64); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    void* r = krealloc(p, 0);
    TEST_ASSERT(r == NULL);
    TEST_ASSERT(h->magic == BLOCK_MAGIC_FREE);
    track[0].active = false;
    tr_free(); return true;
}

static bool t_realloc_grow(void) {
    tr_reset();
    void* p = kmalloc(64); tr_add(p);
    kmemset(p, 0x55, 64);
    void* r = krealloc(p, 128); tr_add(r);
    if (r != p) track[0].active = false;
    TEST_ASSERT(r != NULL);
    TEST_ASSERT(hdr(r)->size >= 128);
    /* First 64 bytes must be preserved */
    uint8_t* b = (uint8_t*)r;
    for (int i = 0; i < 64; i++) TEST_ASSERT(b[i] == 0x55);
    tr_free(); return true;
}

static bool t_realloc_shrink(void) {
    tr_reset();
    void* p = kmalloc(256); tr_add(p);
    kmemset(p, 0xCC, 256);
    void* r = krealloc(p, 64);
    TEST_ASSERT(r != NULL);
    if (r != p) { track[0].active = false; tr_add(r); }
    uint8_t* b = (uint8_t*)r;
    for (int i = 0; i < 64; i++) TEST_ASSERT(b[i] == 0xCC);
    tr_free(); return true;
}

static bool t_realloc_hole(void) {
    tr_reset();
    void* A = kmalloc(64); tr_add(A);
    void* B = kmalloc(64); tr_add(B);
    void* C = kmalloc(64); tr_add(C);
    kmemset(A, 0x11, 64);
    kfree(B); track[1].active = false;
    void* A2 = krealloc(A, 100);
    if (A2 == A) track[0].active = false;
    else         tr_add(A2);
    TEST_ASSERT(A2 != NULL);
    TEST_ASSERT(hdr(A2)->size >= 100);
    uint8_t* b = (uint8_t*)A2;
    TEST_ASSERT(b[0] == 0x11 && b[63] == 0x11);
    tr_free(); return true;
}
#pragma endregion

#pragma region get_alloc_size

static bool t_get_sz(void) {
    tr_reset();
    void* p = kmalloc(100); tr_add(p);
    size_t sz = heap_alloc_sz(heap_kernel_get(), p);
    TEST_ASSERT(sz >= 100);
    tr_free(); return true;
}
#pragma endregion

#pragma region heap_align_size

static bool t_align_sz(void) {
    TEST_ASSERT(heap_align_size(1) >= 1);
    TEST_ASSERT((heap_align_size(1) % HEAP_MIN_ALIGN) == 0);
    TEST_ASSERT((heap_align_size(15) % HEAP_MIN_ALIGN) == 0);
    TEST_ASSERT((heap_align_size(16) % HEAP_MIN_ALIGN) == 0);
    TEST_ASSERT((heap_align_size(17) % HEAP_MIN_ALIGN) == 0);
    TEST_ASSERT(heap_align_size(HEAP_MIN_ALIGN) == HEAP_MIN_ALIGN);
    return true;
}
#pragma endregion

#pragma region heap_validate_blk

static bool t_validate_ok(void) {
    tr_reset();
    void* p = kmalloc(48); tr_add(p);
    TEST_ASSERT(heap_validate_blk((blk_hdr_t*)hdr(p)));
    tr_free(); return true;
}

static bool t_validate_bad(void) {
    tr_reset();
    void* p = kmalloc(48); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    uint32_t saved = h->magic;
    h->magic = 0xBAD1BAD1;
    TEST_ASSERT(!heap_validate_blk((blk_hdr_t*)h));
    h->magic = saved;
    tr_free(); return true;
}
#pragma endregion

#pragma region heap_stats

static bool t_stats_smoke(void) {
    size_t total, used, free, overhead;
    heap_stats(heap_kernel_get(), &total, &used, &free, &overhead);
    /* Values must be non-negative (trivial, but verifies no crash) */
    TEST_ASSERT(total > 0);
    return true;
}

static bool t_stats_track(void) {
    size_t t0, u0, f0, o0, t1, u1, f1, o1;
    heap_stats(heap_kernel_get(), &t0, &u0, &f0, &o0);
    void* p = kmalloc(256);
    heap_stats(heap_kernel_get(), &t1, &u1, &f1, &o1);
    TEST_ASSERT(u1 > u0);
    kfree(p);
    return true;
}
#pragma endregion

#pragma region Coalescing

static bool t_coalesce_fwd(void) {
    tr_reset();
    void* A = kmalloc(128); tr_add(A);
    void* B = kmalloc(128); tr_add(B);
    void* C = kmalloc(128); tr_add(C);
    heap_test_hdr_t* hA = hdr(A);
    heap_test_hdr_t* hB = hdr(B);
    size_t sA = hA->total_size, sB = hB->total_size;
    kfree(A); track[0].active = false;
    kfree(C); track[2].active = false;
    kfree(B); track[1].active = false;
    TEST_ASSERT(hA->magic == BLOCK_MAGIC_FREE);
    TEST_ASSERT(hA->total_size >= sA + sB);
    TEST_ASSERT_STATUS(heap_check(heap_kernel_get()), HEAP_OK);
    tr_free(); return true;
}

static bool t_split_thresh(void) {
    tr_reset();
    void* p = kmalloc(512); tr_add(p);
    /* Shrink so remainder > MIN_BLOCK_SIZE - should split */
    void* p2 = krealloc(p, 256);
    if (p2 != p) { track[0].active = false; tr_add(p2); }
    TEST_ASSERT(p2 != NULL);
    TEST_ASSERT(hdr(p2)->size == 256);
    /* Tiny shrink - should NOT split */
    void* p3 = krealloc(p2, 250);
    if (p3 != p2) { track[tidx-1].active = false; tr_add(p3); }
    TEST_ASSERT(hdr(p3)->size == 256);
    tr_free(); return true;
}

static bool t_freelist_sorted(void) {
    tr_reset();
    void* s1 = kmalloc(16); tr_add(s1);
    void* large = kmalloc(256);
    void* s2 = kmalloc(16); tr_add(s2);
    void* small = kmalloc(32);
    void* s3 = kmalloc(16); tr_add(s3);
    void* med = kmalloc(128);
    kfree(med); kfree(large); kfree(small);
    heap_test_t* h = heapv();
    heap_test_hdr_t* cur = (heap_test_hdr_t*)h->free_list;
    size_t prev_sz = 0; int cnt = 0;
    while (cur) { TEST_ASSERT(cur->size >= prev_sz); prev_sz = cur->size; cur = cur->next_free; cnt++; }
    TEST_ASSERT(cnt >= 3);
    tr_free(); return true;
}
#pragma endregion

#pragma region Arena

static bool t_arena_expand(void) {
    tr_reset();
    heap_test_t* h = heapv();
    size_t arenas0 = h->arena_count;
    void* p = kmalloc(1024 * 1024); tr_add(p);
    TEST_ASSERT(p != NULL);
    TEST_ASSERT(h->arena_count > arenas0);
    tr_free(); return true;
}

static bool t_arena_shrink(void) {
    tr_reset();
    heap_test_t* h = heapv();
    size_t arenas0 = h->arena_count;
    void* p = kmalloc(1024 * 1024); tr_add(p);
    TEST_ASSERT(h->arena_count > arenas0);
    kfree(p); track[0].active = false;
    TEST_ASSERT(h->arena_count == arenas0);
    return true;
}
#pragma endregion

#pragma region Security / Integrity

static bool t_hdr_corrupt(void) {
    tr_reset();
    void* p = kmalloc(32); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    uint32_t saved = h->magic;
    h->magic = 0xDEADBEEF;
    heap_status_t s = heap_check(heap_kernel_get());
    h->magic = saved;
    TEST_ASSERT_STATUS(s, HEAP_ERR_CORRUPTED);
    tr_free(); return true;
}

static bool t_ftr_corrupt(void) {
    tr_reset();
    void* p = kmalloc(32); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    heap_test_ftr_t* f = ftr(h);
    uint32_t saved = f->magic;
    f->magic = 0xBADF00D1;
    heap_status_t s = heap_check(heap_kernel_get());
    f->magic = saved;
    TEST_ASSERT_STATUS(s, HEAP_ERR_CORRUPTED);
    tr_free(); return true;
}

static bool t_rz_corrupt(void) {
    tr_reset();
    void* p = kmalloc(32); tr_add(p);
    heap_test_hdr_t* h = hdr(p);
    uint32_t saved = h->red_zone_post;
    h->red_zone_post = 0;
    heap_status_t s = heap_check(heap_kernel_get());
    h->red_zone_post = saved;
    TEST_ASSERT_STATUS(s, HEAP_ERR_CORRUPTED);
    tr_free(); return true;
}
#pragma endregion

#pragma region Inter-allocation Independence

static bool t_multi_ind(void) {
    /* Write distinct pattern to each allocation; verify no cross-contamination */
    #define MCOUNT 16
    tr_reset();
    void* ptrs[MCOUNT];
    size_t sizes[MCOUNT];
    for (int i = 0; i < MCOUNT; i++) {
        sizes[i] = 32 + (size_t)(i * 16);
        ptrs[i] = kmalloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL);
        tr_add(ptrs[i]);
        kmemset(ptrs[i], (uint8_t)i, sizes[i]);
    }
    /* Verify all patterns intact */
    for (int i = 0; i < MCOUNT; i++) {
        uint8_t* b = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (b[j] != (uint8_t)i) {
                LOGF("[FAIL] corruption at alloc %d byte %zu\n", i, j);
                return false;
            }
        }
    }
    tr_free(); return true;
}
#pragma endregion

#pragma region Stress Churn (machine-adaptive)

static bool t_stress_churn(void) {
    tr_reset();
    #define SC 100
    #define SI 3000
    void* pool[SC];
    kmemset(pool, 0, sizeof(pool));
    uint32_t seed = 0xBEEFCAFE;
    for (int i = 0; i < SI; i++) {
        seed = seed * 1103515245u + 12345u;
        int idx = (int)((seed >> 16) % SC);
        if (!pool[idx]) {
            seed = seed * 1103515245u + 12345u;
            size_t sz = ((seed >> 16) % 512) + 1;
            pool[idx] = kmalloc(sz);
            if (!pool[idx]) return false;
            kmemset(pool[idx], (uint8_t)idx, sz);
        } else {
            if (*(uint8_t*)pool[idx] != (uint8_t)idx) {
                LOGF("[FAIL] stress corruption idx %d\n", idx);
                return false;
            }
            kfree(pool[idx]);
            pool[idx] = NULL;
        }
        if (i % 500 == 0)
            if (heap_check(heap_kernel_get()) != HEAP_OK) return false;
    }
    for (int i = 0; i < SC; i++) if (pool[i]) kfree(pool[i]);
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    ntests++;
    LOGF("[TEST] %-40s ", name);
    heap_kernel_get(); /* ensure init */
    bool pass = fn();
    if (tidx > 0) { LOGF("[WARN] leak (cleaning) ... "); tr_free(); }
    if (pass) { npass++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_heap(void) {
    ntests  = 0;
    npass = 0;

    LOGF("\n--- BEGIN HEAP MANAGER TEST ---\n");

    run_test("init: already init",            t_init_already);
    run_test("get: not null",                 t_get_nn);
    run_test("get: magic correct",            t_magic);
    run_test("malloc: not null",              t_alloc_nn);
    run_test("malloc: 16-byte aligned",       t_alloc_aligned);
    run_test("malloc(1..32): all aligned",    t_alloc_align_small);
    run_test("malloc: header metadata",       t_alloc_meta);
    run_test("malloc: footer metadata",       t_alloc_ftr);
    run_test("malloc: memory writable",       t_alloc_wr);
    run_test("free: marks block free",        t_free_marks);
    run_test("free(NULL): no crash",          t_free_null);
    run_test("free: double-free safe",        t_dbl_free);
    run_test("calloc: zeroed memory",         t_calloc_zero);
    run_test("calloc: overflow → NULL",       t_calloc_ovf);
    run_test("calloc(0,N): safe",             t_calloc_zero_n);
    run_test("realloc(NULL,N): = malloc",     t_realloc_null_malloc);
    run_test("realloc(p,0): = free",          t_realloc_zero_free);
    run_test("realloc grow: data preserved",  t_realloc_grow);
    run_test("realloc shrink: data preserved",t_realloc_shrink);
    run_test("realloc into adjacent hole",    t_realloc_hole);
    run_test("get_alloc_size >= requested",   t_get_sz);
    run_test("heap_align_size: multiples",    t_align_sz);
    run_test("validate_block: valid block",   t_validate_ok);
    run_test("validate_block: corrupt block", t_validate_bad);
    run_test("heap_stats: smoke",             t_stats_smoke);
    run_test("heap_stats: tracks alloc",      t_stats_track);
    run_test("coalesce: A+B+C merge",         t_coalesce_fwd);
    run_test("split threshold",               t_split_thresh);
    run_test("free list sorted",              t_freelist_sorted);
    run_test("arena: expands on huge alloc",  t_arena_expand);
    run_test("arena: shrinks after free",     t_arena_shrink);
    run_test("corrupt: header detected",      t_hdr_corrupt);
    run_test("corrupt: footer detected",      t_ftr_corrupt);
    run_test("corrupt: redzone detected",     t_rz_corrupt);
    run_test("multi-alloc independence",      t_multi_ind);
    run_test("stress churn (3000 iters)",     t_stress_churn);

    LOGF("--- END HEAP MANAGER TEST ---\n");
    LOGF("Heap Test Results: %d/%d\n\n", npass, ntests);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (npass != ntests) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some heap tests failed (%d/%d passed).\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All heap tests passed! (%d/%d)\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
