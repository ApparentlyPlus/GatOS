/*
 * test_heap.c - Kernel Heap Manager Validation Suite (White Box)
 *
 * This suite verifies the correctness, stability, and security of the
 * Multi-Arena Heap Allocator. It mirrors internal structures to verify 
 * boundary tags, coalescing logic, and protection mechanisms on the 
 * live kernel.
 */

#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#pragma region Configuration & Types

#define MAX_TRACKED_ITEMS 1024

// Constants from heap.c
#define HEAP_MAGIC       0xF005BA11
#define BLOCK_MAGIC_USED 0xABADCAFE
#define BLOCK_MAGIC_FREE 0xA110CA7E
#define BLOCK_RED_ZONE   0x8BADF00D
#define HEAP_MIN_ALIGN   16
#define MIN_BLOCK_SIZE   32

typedef enum {
    TRACK_KERNEL,
    TRACK_USER_HEAP
} track_source_t;

typedef struct {
    bool active;
    track_source_t source;
    void* ptr;
    heap_t* heap_inst;
} heap_tracker_t;

// Mirror of heap_arena_t
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

// Mirror of heap_t
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
} heap_test_struct_t;

// Mirror of heap_block_header_t
typedef struct heap_test_header {
    uint32_t magic;
    uint32_t red_zone_pre;
    size_t size;
    size_t total_size;
    heap_test_arena_t* arena;
    struct heap_test_header* next_free;
    struct heap_test_header* prev_free;
    uint32_t red_zone_post;
} __attribute__((aligned(16))) heap_test_header_t;

// Mirror of heap_block_footer_t
typedef struct {
    uint32_t red_zone_pre;
    heap_test_header_t* header;
    uint32_t magic;
    uint32_t red_zone_post;
} __attribute__((aligned(16))) heap_test_footer_t;

static heap_tracker_t g_tracker[MAX_TRACKED_ITEMS];
static int g_tracker_idx = 0;
static int g_tests_total = 0;
static int g_tests_passed = 0;

#pragma endregion

#pragma region Harness Helpers

// Resets the internal tracking array before a test begins.
static void tracker_reset(void) {
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        g_tracker[i].active = false;
        g_tracker[i].ptr = NULL;
        g_tracker[i].heap_inst = NULL;
    }
    g_tracker_idx = 0;
}

// Registers a kernel heap allocation to be freed during cleanup.
static void tracker_add_k(void* ptr) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx] = (heap_tracker_t){ .active = true, .source = TRACK_KERNEL, .ptr = ptr };
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] Heap Tracker full.\n");
    }
}

// Registers a user heap allocation to be freed during cleanup.
static void tracker_add_u(heap_t* heap, void* ptr) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx] = (heap_tracker_t){ .active = true, .source = TRACK_USER_HEAP, .ptr = ptr, .heap_inst = heap };
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] Heap Tracker full.\n");
    }
}

// Frees all tracked allocations based on their source.
static void tracker_cleanup(void) {
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        if (g_tracker[i].active) {
            if (g_tracker[i].source == TRACK_KERNEL) {
                kfree(g_tracker[i].ptr);
            } else {
                heap_free(g_tracker[i].heap_inst, g_tracker[i].ptr);
            }
            g_tracker[i].active = false;
        }
    }
    g_tracker_idx = 0;
}

static heap_test_header_t* get_header(void* ptr) {
    return (heap_test_header_t*)((uint8_t*)ptr - sizeof(heap_test_header_t));
}

static heap_test_footer_t* get_footer(heap_test_header_t* header) {
    return (heap_test_footer_t*)((uint8_t*)header + sizeof(heap_test_header_t) + header->size);
}

static heap_test_struct_t* access_heap(heap_t* h) {
    return (heap_test_struct_t*)h;
}

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        LOGF("[FAIL] Assertion failed: %s (Line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

#define TEST_ASSERT_STATUS(s, e) do { \
    if ((s) != (e)) { \
        LOGF("[FAIL] Status mismatch: Got %d, Expected %d (Line %d)\n", s, e, __LINE__); \
        return false; \
    } \
} while(0)

#pragma endregion

#pragma region Basic Allocation Tests

// Verifies kernel heap initialization and basic allocation metadata.
static bool test_kernel_init_and_basic_alloc(void) {
    tracker_reset();

    TEST_ASSERT_STATUS(heap_kernel_init(), HEAP_ERR_ALREADY_INIT);
    TEST_ASSERT(heap_kernel_get() != NULL);

    void* p1 = kmalloc(32);
    TEST_ASSERT(p1 != NULL);
    tracker_add_k(p1);

    memset(p1, 0xAA, 32);
    uint8_t* b = (uint8_t*)p1;
    TEST_ASSERT(b[0] == 0xAA && b[31] == 0xAA);

    heap_test_header_t* h = get_header(p1);
    TEST_ASSERT(h->magic == BLOCK_MAGIC_USED);
    TEST_ASSERT(h->size >= 32);
    TEST_ASSERT(h->red_zone_pre == BLOCK_RED_ZONE);
    TEST_ASSERT(h->red_zone_post == BLOCK_RED_ZONE);

    kfree(p1);
    g_tracker[0].active = false;

    // Verify block is now free (kernel heap shouldn't unmap immediately)
    TEST_ASSERT(h->magic == BLOCK_MAGIC_FREE);

    return true;
}

// Checks alignment guarantees and zeroing behavior of calloc.
static bool test_alignment_and_calloc(void) {
    tracker_reset();

    void* p1 = kmalloc(1);
    tracker_add_k(p1);
    
    TEST_ASSERT(((uintptr_t)p1 % HEAP_MIN_ALIGN) == 0);
    
    heap_test_header_t* h = get_header(p1);
    TEST_ASSERT(h->size >= 1);
    TEST_ASSERT((h->size % HEAP_MIN_ALIGN) == 0);

    void* p2 = kcalloc(4, 1024); // 4KB
    tracker_add_k(p2);
    
    uint64_t* check = (uint64_t*)p2;
    for(int i=0; i < (4096/8); i++) {
        if (check[i] != 0) return false;
    }

    tracker_cleanup();
    return true;
}

// Tests reallocation logic when expanding into adjacent free space.
static bool test_realloc_logic(void) {
    tracker_reset();

    void* A = kmalloc(64); tracker_add_k(A);
    void* B = kmalloc(64); tracker_add_k(B);
    void* C = kmalloc(64); tracker_add_k(C);

    memset(A, 0x11, 64);
    
    kfree(B); // Create hole
    g_tracker[1].active = false;

    // Expand A into B's hole
    void* A_new = krealloc(A, 100);
    
    TEST_ASSERT(A_new == A); 
    
    uint8_t* b = (uint8_t*)A_new;
    TEST_ASSERT(b[0] == 0x11 && b[63] == 0x11);
    TEST_ASSERT(get_header(A_new)->size >= 100);

    tracker_cleanup();
    return true;
}

// Verifies standard compliance for realloc with NULL or zero size.
static bool test_realloc_compliance(void) {
    tracker_reset();

    // 1. realloc(NULL, size) -> malloc(size)
    void* p1 = krealloc(NULL, 64);
    TEST_ASSERT(p1 != NULL);
    tracker_add_k(p1);
    
    heap_test_header_t* h = get_header(p1);
    TEST_ASSERT(h->size >= 64);

    // 2. realloc(ptr, 0) -> free(ptr)
    void* p2 = krealloc(p1, 0);
    // Standard varies, but often returns NULL or a specific unique pointer. 
    // This implementation calls kfree and returns NULL.
    TEST_ASSERT(p2 == NULL);
    
    // Verify p1 is actually free
    TEST_ASSERT(h->magic == BLOCK_MAGIC_FREE);
    g_tracker[0].active = false; // Manually untrack p1

    tracker_cleanup();
    return true;
}

// Checks edge case handling for NULL pointers, zero allocation, and overflow.
static bool test_edge_cases(void) {
    tracker_reset();

    // 1. NULL Free (Should be no-op)
    kfree(NULL);

    // 2. Zero-size alloc (Implementation defined, but safe)
    void* p = kmalloc(0);
    if (p) kfree(p);

    // 3. Overflow check
    void* p2 = kcalloc(SIZE_MAX, 2);
    TEST_ASSERT(p2 == NULL);

    // 4. Large aligned realloc to ensure we handle alignment padding + resize
    void* p3 = kmalloc(128);
    tracker_add_k(p3);
    void* p4 = krealloc(p3, 256);
    TEST_ASSERT(p4 != NULL);
    g_tracker[0].ptr = p4; // update tracker

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Core Logic Tests

// Tests coalescing of free blocks (forward and backward merging).
static bool test_coalescing(void) {
    tracker_reset();
    
    void* A = kmalloc(128); tracker_add_k(A);
    void* B = kmalloc(128); tracker_add_k(B);
    void* C = kmalloc(128); tracker_add_k(C);

    heap_test_header_t* hA = get_header(A);
    heap_test_header_t* hB = get_header(B);
    size_t size_A = hA->total_size;
    size_t size_B = hB->total_size;

    kfree(A); g_tracker[0].active = false;
    kfree(C); g_tracker[2].active = false;
    
    // Free B. Should merge Backwards (A) and Forwards (C).
    kfree(B); g_tracker[1].active = false;

    TEST_ASSERT(hA->magic == BLOCK_MAGIC_FREE);
    TEST_ASSERT(hA->total_size >= size_A + size_B);
    TEST_ASSERT_STATUS(heap_check_integrity(heap_kernel_get()), HEAP_OK);
    
    tracker_cleanup();
    return true;
}

// Verifies that blocks are only split if the remainder exceeds the threshold.
static bool test_splitting_threshold(void) {
    tracker_reset();
    
    void* ptr = kmalloc(512);
    tracker_add_k(ptr);

    // Overhead is 96 bytes. Min block 32. Need 128 bytes free to split.
    // 512 - 256 = 256 free. (>128). Should split.
    void* ptr2 = krealloc(ptr, 256); 
    TEST_ASSERT(ptr2 == ptr);
    TEST_ASSERT(get_header(ptr2)->size == 256);

    // 256 - 250 = 6 bytes free. (<128). Should NOT split.
    void* ptr3 = krealloc(ptr2, 250);
    TEST_ASSERT(ptr3 == ptr2);
    TEST_ASSERT(get_header(ptr3)->size == 256);

    tracker_cleanup();
    return true;
}

// Checks that the free list maintains correct sorting order (by size).
static bool test_free_list_sorting(void) {
    tracker_reset();
    
    heap_t* u_heap = heap_create(vmm_kernel_get(), 8192, 1024*1024, HEAP_FLAG_NONE);
    heap_test_struct_t* h_struct = access_heap(u_heap);

    // Alloc blocks with spacers to prevent coalescing upon free
    void* s1 = heap_malloc(u_heap, 16); 
    void* large = heap_malloc(u_heap, 256);
    void* s2 = heap_malloc(u_heap, 16); 
    void* small = heap_malloc(u_heap, 32);
    void* s3 = heap_malloc(u_heap, 16); 
    void* med = heap_malloc(u_heap, 128);

    heap_free(u_heap, med);
    heap_free(u_heap, large);
    heap_free(u_heap, small);

    // Verify sort order: Small -> Med -> Large
    heap_test_header_t* cur = (heap_test_header_t*)h_struct->free_list;
    TEST_ASSERT(cur != NULL);

    size_t prev_size = 0;
    int count = 0;
    while(cur) {
        TEST_ASSERT(cur->size >= prev_size);
        prev_size = cur->size;
        cur = cur->next_free;
        count++;
    }
    TEST_ASSERT(count >= 3);

    heap_destroy(u_heap);
    return true;
}

// Tests that the heap expands by creating new arenas when necessary.
static bool test_arena_expansion(void) {
    tracker_reset();
    heap_t* heap = heap_kernel_get();
    heap_test_struct_t* h_struct = access_heap(heap);

    size_t initial_arenas = h_struct->arena_count;
    
    size_t huge_size = 1024 * 1024;
    void* huge = kmalloc(huge_size);
    TEST_ASSERT(huge != NULL);
    tracker_add_k(huge);

    TEST_ASSERT(h_struct->arena_count > initial_arenas);
    
    uint8_t* ptr = (uint8_t*)huge;
    ptr[0] = 0xAA;
    ptr[huge_size - 1] = 0xBB;
    
    tracker_cleanup();
    return true;
}

// Tests that arenas are released when fully freed.
static bool test_arena_shrinking(void) {
    tracker_reset();
    heap_t* heap = heap_kernel_get();
    heap_test_struct_t* h_struct = access_heap(heap);
    
    size_t start_arenas = h_struct->arena_count;

    void* big = kmalloc(1024 * 1024); // 1MB
    tracker_add_k(big);

    TEST_ASSERT(h_struct->arena_count > start_arenas);

    kfree(big);
    g_tracker[0].active = false;

    // Should drop back down
    TEST_ASSERT(h_struct->arena_count == start_arenas);

    return true;
}

#pragma endregion

#pragma region Security & Integrity Tests

// Checks if double-free attempts are detected without crashing.
static bool test_double_free_protection(void) {
    tracker_reset();
    void* p = kmalloc(32);
    tracker_add_k(p);

    kfree(p);
    g_tracker[0].active = false;

    kfree(p); // Double free (Should warn, not crash)

    TEST_ASSERT_STATUS(heap_check_integrity(heap_kernel_get()), HEAP_OK);
    return true;
}

// Verifies detection of corrupted block headers.
static bool test_header_corruption_detection(void) {
    tracker_reset();
    void* p = kmalloc(32);
    tracker_add_k(p);

    heap_test_header_t* h = get_header(p);
    uint32_t original_magic = h->magic;

    h->magic = 0xDEADBEEF;
    heap_status_t status = heap_check_integrity(heap_kernel_get());
    h->magic = original_magic; // Repair

    if (status != HEAP_ERR_CORRUPTED) return false;

    tracker_cleanup();
    return true;
}

// Verifies detection of corrupted block footers.
static bool test_footer_corruption_detection(void) {
    tracker_reset();
    void* p = kmalloc(32);
    tracker_add_k(p);

    heap_test_header_t* h = get_header(p);
    heap_test_footer_t* f = get_footer(h);

    uint32_t original_magic = f->magic;
    f->magic = 0xBADF00D;
    heap_status_t status = heap_check_integrity(heap_kernel_get());
    f->magic = original_magic; // Repair

    if (status != HEAP_ERR_CORRUPTED) return false;

    tracker_cleanup();
    return true;
}

// Ensures RedZone integrity checks detect buffer overflows.
static bool test_redzone_check(void) {
    tracker_reset();
    void* p = kmalloc(32);
    tracker_add_k(p);

    heap_test_header_t* h = get_header(p);
    h->red_zone_post = 0x00000000;

    heap_status_t status = heap_check_integrity(heap_kernel_get());
    h->red_zone_post = BLOCK_RED_ZONE; // Repair

    TEST_ASSERT_STATUS(status, HEAP_ERR_CORRUPTED);

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Isolation & Stress Tests

// Validates the lifecycle of a separate user-space heap instance.
static bool test_user_heap_lifecycle(void) {
    tracker_reset();

    heap_t* u_heap = heap_create(vmm_kernel_get(), 4096, 1024*1024, HEAP_FLAG_ZERO);
    TEST_ASSERT(u_heap != NULL);

    void* p1 = heap_malloc(u_heap, 64);
    TEST_ASSERT(p1 != NULL);
    tracker_add_u(u_heap, p1);

    uint64_t* check = (uint64_t*)p1;
    TEST_ASSERT(check[0] == 0); // Check ZERO flag

    TEST_ASSERT_STATUS(heap_check_integrity(u_heap), HEAP_OK);
    TEST_ASSERT_STATUS(heap_check_integrity(heap_kernel_get()), HEAP_OK);

    heap_free(u_heap, p1);
    g_tracker[0].active = false;

    heap_destroy(u_heap);
    return true;
}

// Performs randomized allocation/deallocation churn to stress stability.
static bool test_heap_stress_churn(void) {
    tracker_reset();

    #define STRESS_POOL 100
    #define STRESS_ITERS 2000
    void* pool[STRESS_POOL];
    memset(pool, 0, sizeof(pool));

    uint32_t seed = 999;
    
    for (int i = 0; i < STRESS_ITERS; i++) {
        // LCG
        seed = seed * 1103515245 + 12345;
        int idx = (seed / 65536) % STRESS_POOL;
        
        if (pool[idx] == NULL) {
            seed = seed * 1103515245 + 12345;
            size_t sz = ((seed / 65536) % 512) + 1;
            pool[idx] = kmalloc(sz);
            if (!pool[idx]) return false; 
            memset(pool[idx], (uint8_t)idx, sz);
        } else {
            volatile uint8_t* b = (uint8_t*)pool[idx];
            if (*b != (uint8_t)idx) {
                 LOGF("[FAIL] Stress corruption idx %d\n", idx);
                 return false;
            }
            kfree(pool[idx]);
            pool[idx] = NULL;
        }

        if (i % 500 == 0) {
            if (heap_check_integrity(heap_kernel_get()) != HEAP_OK) return false;
        }
    }

    for (int i = 0; i < STRESS_POOL; i++) {
        if (pool[i]) kfree(pool[i]);
    }

    return true;
}

#pragma endregion

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
    heap_kernel_get(); // Ensure init

    bool pass = func();

    if (g_tracker_idx > 0) {
        LOGF("[WARN] Leak/State detected (cleaning) ... ");
        tracker_cleanup();
    }

    if (pass) {
        g_tests_passed++;
        LOGF("[PASS]\n");
    } else {
        LOGF("[FAIL]\n");
    }
}

void test_heap(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN HEAP MANAGER TEST ---\n");

    // Basics
    run_test("Kernel Init & Basic Alloc", test_kernel_init_and_basic_alloc);
    run_test("Alignment & Calloc", test_alignment_and_calloc);
    run_test("Edge Cases (Null/Overflow)", test_edge_cases);
    run_test("Realloc Logic (Grow/Move)", test_realloc_logic);
    run_test("Realloc Compliance (NULL/0)", test_realloc_compliance);
    
    // Core Logic
    run_test("Block Coalescing (Merge)", test_coalescing);
    run_test("Splitting Thresholds", test_splitting_threshold);
    run_test("Free List Sorting", test_free_list_sorting);
    run_test("Arena Expansion (Huge Alloc)", test_arena_expansion);
    run_test("Arena Shrinking (Release)", test_arena_shrinking);
    
    // Security
    run_test("Double Free Protection", test_double_free_protection);
    run_test("Header Corruption Detect", test_header_corruption_detection);
    run_test("Footer Corruption Detect", test_footer_corruption_detection);
    run_test("RedZone Integrity Check", test_redzone_check);
    
    // Isolation
    run_test("User Heap Lifecycle", test_user_heap_lifecycle);
    
    // Stress
    run_test("Randomized Churn Stress", test_heap_stress_churn);

    LOGF("--- END HEAP MANAGER TEST ---\n");
    LOGF("Heap Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <kernel/drivers/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[-] Some tests failed (%d/%d). Please check the debug log for details.\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    else{
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        printf("[+] All tests passed successfully! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}

#pragma endregion