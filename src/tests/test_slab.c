/*
 * slab_tests.c - Slab Allocator Validation Suite
 *
 * This suite verifies the correctness, stability, and security of the
 * Slab Allocator. It operates on the live kernel and verifies cache logic,
 * object alignment, slab growth/shrinking, and corruption detection.
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#pragma region Configuration & Types

#define MAX_TRACKED_ITEMS 4096

typedef enum { 
    TRACK_CACHE,    // slab_cache_t*
    TRACK_OBJECT    // void* object
} track_type_t;

typedef struct {
    track_type_t type;
    void* ptr;
    slab_cache_t* owner; // Valid only for TRACK_OBJECT
    bool active;
} slab_tracker_t;

// Mirror internal header structure for white-box testing
typedef struct {
    uint32_t magic;
    uint32_t cache_id;
    uint64_t alloc_timestamp;
} slab_test_header_t;

// Mirror internal free object structure for white-box testing
typedef struct slab_test_free_obj {
    uint32_t magic;
    uint32_t red_zone_pre;
    struct slab_test_free_obj* next;
    uint32_t red_zone_post;
} slab_test_free_obj_t;

static slab_tracker_t g_tracker[MAX_TRACKED_ITEMS];
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
        g_tracker[i].owner = NULL;
    }
    g_tracker_idx = 0;
}

// Registers a cache to be automatically destroyed during cleanup.
static void tracker_add_cache(slab_cache_t* cache) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx].type = TRACK_CACHE;
        g_tracker[g_tracker_idx].ptr = (void*)cache;
        g_tracker[g_tracker_idx].owner = NULL;
        g_tracker[g_tracker_idx].active = true;
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] Slab Tracker full.\n");
    }
}

// Registers an object allocation to be automatically freed during cleanup.
static void tracker_add_obj(slab_cache_t* cache, void* obj) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx].type = TRACK_OBJECT;
        g_tracker[g_tracker_idx].ptr = obj;
        g_tracker[g_tracker_idx].owner = cache;
        g_tracker[g_tracker_idx].active = true;
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] Slab Tracker full.\n");
    }
}

// Frees all tracked objects and destroys tracked caches.
static void tracker_cleanup(void) {
    // 1. Free all objects first
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        if (g_tracker[i].active && g_tracker[i].type == TRACK_OBJECT) {
            slab_free(g_tracker[i].owner, g_tracker[i].ptr);
            g_tracker[i].active = false;
        }
    }
    // 2. Destroy created caches
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        if (g_tracker[i].active && g_tracker[i].type == TRACK_CACHE) {
            slab_cache_destroy((slab_cache_t*)g_tracker[i].ptr);
            g_tracker[i].active = false;
        }
    }
    g_tracker_idx = 0;
}

#pragma endregion

#pragma region Basic Allocation Tests

// Verifies that the slab allocator initialization state is reported correctly.
static bool test_init_check(void) {
    if (!slab_is_initialized()) {
        TEST_ASSERT_STATUS(slab_init(), SLAB_OK);
    } else {
        TEST_ASSERT_STATUS(slab_init(), SLAB_ERR_ALREADY_INIT);
    }
    TEST_ASSERT(slab_is_initialized());
    return true;
}

#pragma endregion

#pragma region Cache Management Tests

// Validates parameter validation during cache creation (alignment, name, size).
static bool test_cache_create_validate(void) {
    tracker_reset();

    // Valid Creation
    slab_cache_t* c1 = slab_cache_create("test_valid", 64, 8);
    TEST_ASSERT(c1 != NULL);
    tracker_add_cache(c1);

    TEST_ASSERT(strcmp(slab_cache_name(c1), "test_valid") == 0);
    TEST_ASSERT(slab_cache_obj_size(c1) == 64);

    // Invalid Alignment (Not Power of 2)
    slab_cache_t* c2 = slab_cache_create("test_bad_align", 64, 7);
    TEST_ASSERT(c2 == NULL);

    // Duplicate Name
    slab_cache_t* c3 = slab_cache_create("test_valid", 32, 8);
    TEST_ASSERT(c3 == NULL);

    // Zero Size
    slab_cache_t* c4 = slab_cache_create("test_zero", 0, 8);
    TEST_ASSERT(c4 == NULL);

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Allocation & Alignment Tests

// Performs a basic allocation and free cycle to verify memory write access.
static bool test_alloc_free_basic(void) {
    tracker_reset();
    
    slab_cache_t* c = slab_cache_create("test_basic", 128, 8);
    tracker_add_cache(c);

    void* obj;
    TEST_ASSERT_STATUS(slab_alloc(c, &obj), SLAB_OK);
    tracker_add_obj(c, obj);

    // Verify memory is writeable
    memset(obj, 0xAA, 128);
    volatile uint8_t* bytes = (volatile uint8_t*)obj;
    TEST_ASSERT(bytes[0] == 0xAA);
    TEST_ASSERT(bytes[127] == 0xAA);

    // Free
    TEST_ASSERT_STATUS(slab_free(c, obj), SLAB_OK);
    g_tracker[1].active = false; 

    slab_cache_stats_t stats;
    slab_cache_stats(c, &stats);
    TEST_ASSERT(stats.active_objects == 0);
    TEST_ASSERT(stats.total_allocs == 1);
    TEST_ASSERT(stats.total_frees == 1);

    tracker_cleanup();
    return true;
}

// Verifies that objects adhere to strict alignment constraints.
static bool test_alignment_strictness(void) {
    tracker_reset();

    // Test large alignment requirement (e.g., AVX-512 friendly)
    size_t align = 64;
    slab_cache_t* c = slab_cache_create("test_align", 32, align);
    tracker_add_cache(c);

    void* obj1;
    void* obj2;

    TEST_ASSERT_STATUS(slab_alloc(c, &obj1), SLAB_OK);
    tracker_add_obj(c, obj1);
    TEST_ASSERT_STATUS(slab_alloc(c, &obj2), SLAB_OK);
    tracker_add_obj(c, obj2);

    // Check pointer arithmetic
    TEST_ASSERT(((uintptr_t)obj1 & (align - 1)) == 0);
    TEST_ASSERT(((uintptr_t)obj2 & (align - 1)) == 0);

    tracker_cleanup();
    return true;
}

// Ensures that allocated objects are zero-initialized (if guaranteed by impl).
static bool test_zero_initialization(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_zero", 64, 8);
    tracker_add_cache(c);

    void* obj;
    TEST_ASSERT_STATUS(slab_alloc(c, &obj), SLAB_OK);
    tracker_add_obj(c, obj);

    // Dirty it
    memset(obj, 0xFF, 64);
    
    // Free it
    slab_free(c, obj);
    g_tracker[1].active = false;

    // Re-alloc (LIFO behavior usually returns same slot)
    void* obj2;
    TEST_ASSERT_STATUS(slab_alloc(c, &obj2), SLAB_OK);
    tracker_add_obj(c, obj2);

    // Verify it is zeroed by the allocator
    uint8_t* p = (uint8_t*)obj2;
    for(int i=0; i<64; i++) {
        if(p[i] != 0) {
            LOGF("[FAIL] Object memory not zeroed at byte %d\n", i);
            return false;
        }
    }

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Scaling & Logic Tests

// Tests the allocator's ability to grow the pool via multiple pages.
static bool test_slab_growth(void) {
    tracker_reset();

    // Object size 512. Page 4096. Overhead ~64. Capacity ~7 objects per page.
    slab_cache_t* c = slab_cache_create("test_growth", 512, 8);
    tracker_add_cache(c);

    // Alloc 20 objects. Should force 3 slabs (7 + 7 + 6).
    #define GROWTH_COUNT 20
    void* ptrs[GROWTH_COUNT];

    for(int i=0; i<GROWTH_COUNT; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
        tracker_add_obj(c, ptrs[i]);
    }

    slab_cache_stats_t stats;
    slab_cache_stats(c, &stats);
    
    TEST_ASSERT(stats.active_objects == GROWTH_COUNT);
    TEST_ASSERT(stats.slab_count >= 3); 

    TEST_ASSERT(slab_verify_integrity());

    tracker_cleanup();
    return true;
}

// Verifies that the allocator releases empty slabs back to the system.
static bool test_culling_shrink(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_shrink", 512, 8);
    tracker_add_cache(c);

    // Inflate
    #define SHRINK_COUNT 30
    void* ptrs[SHRINK_COUNT];
    for(int i=0; i<SHRINK_COUNT; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
        tracker_add_obj(c, ptrs[i]);
    }

    uint64_t peak_slabs;
    slab_cache_stats_t stats;
    slab_cache_stats(c, &stats);
    peak_slabs = stats.slab_count;
    TEST_ASSERT(peak_slabs >= 4);

    // Free all
    for(int i=0; i<SHRINK_COUNT; i++) {
        TEST_ASSERT_STATUS(slab_free(c, ptrs[i]), SLAB_OK);
        for(int t=0; t<g_tracker_idx; t++) 
            if(g_tracker[t].ptr == ptrs[i]) g_tracker[t].active = false;
    }

    // Check if we shrunk (allocator retains 1 empty slab)
    slab_cache_stats(c, &stats);
    
    if (stats.slab_count >= peak_slabs) {
        LOGF("[FAIL] Cache did not shrink. Slabs: %lu (Peak: %lu)\n", stats.slab_count, peak_slabs);
        return false;
    }
    TEST_ASSERT(stats.empty_slabs <= 1);

    tracker_cleanup();
    return true;
}

// Verifies that the allocator prioritizes filling partial slabs over new ones.
static bool test_partial_slab_priority(void) {
    tracker_reset();
    
    // Create cache: 128 byte objects. ~30 objects per page.
    slab_cache_t* c = slab_cache_create("test_prio", 128, 8);
    tracker_add_cache(c);

    // 1. Allocate enough to fill ONE slab (Slab A) completely
    void* objs_a[40]; 
    int count_a = 0;
    
    while(1) {
        slab_alloc(c, &objs_a[count_a]);
        tracker_add_obj(c, objs_a[count_a]);
        count_a++;
        
        slab_cache_stats_t s;
        slab_cache_stats(c, &s);
        if (s.slab_count > 1) break; 
        if (count_a >= 40) return false; 
    }

    // 2. Free ONE object from Slab A. Both A and B are now partial.
    void* obj_to_free = objs_a[0];
    slab_free(c, obj_to_free);
    
    for(int i=0; i<g_tracker_idx; i++) 
        if(g_tracker[i].ptr == obj_to_free) g_tracker[i].active = false;

    // 3. Allocate NEW object.
    void* new_obj;
    slab_alloc(c, &new_obj);
    tracker_add_obj(c, new_obj);

    // 4. Check if new_obj is on the same page as obj_to_free (refilled hole)
    uintptr_t page_a = (uintptr_t)obj_to_free & ~(PAGE_SIZE - 1);
    uintptr_t page_new = (uintptr_t)new_obj & ~(PAGE_SIZE - 1);

    if (page_a != page_new) {
        LOGF("[WARN] Allocator did not refill the hole in the previous slab.\n");
    }

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Error Handling & Security Tests

// Checks if the allocator detects double-free attempts via header corruption.
static bool test_double_free(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_df", 32, 8);
    tracker_add_cache(c);

    void* obj;
    slab_alloc(c, &obj);
    tracker_add_obj(c, obj);

    // First free: OK
    TEST_ASSERT_STATUS(slab_free(c, obj), SLAB_OK);
    g_tracker[1].active = false;

    // Second free: Should detect corruption
    slab_status_t status = slab_free(c, obj);
    TEST_ASSERT_STATUS(status, SLAB_ERR_CORRUPTION);

    tracker_cleanup();
    return true;
}

// Ensures that freeing an object to the wrong cache is detected.
static bool test_cross_cache_free(void) {
    tracker_reset();
    
    slab_cache_t* c1 = slab_cache_create("test_cc_1", 32, 8);
    tracker_add_cache(c1);
    
    slab_cache_t* c2 = slab_cache_create("test_cc_2", 32, 8);
    tracker_add_cache(c2);

    void* obj1;
    slab_alloc(c1, &obj1);
    tracker_add_obj(c1, obj1);

    // Try to free object from C1 using C2 handle
    slab_status_t status = slab_free(c2, obj1);

    if (status == SLAB_OK) {
        LOGF("[FAIL] Allowed freeing object to wrong cache\n");
        return false;
    }
    TEST_ASSERT(status == SLAB_ERR_NOT_FOUND || status == SLAB_ERR_CORRUPTION);

    tracker_cleanup();
    return true;
}

// Verifies that freeing an invalid pointer results in an error.
static bool test_bad_pointer_free(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_badptr", 32, 8);
    tracker_add_cache(c);

    void* obj;
    slab_alloc(c, &obj);
    tracker_add_obj(c, obj);

    // Pass a pointer in the middle of an object
    void* bad_ptr = (void*)((uintptr_t)obj + 16);

    // Attempt free; allocator should read garbage at calculated header offset
    slab_status_t status = slab_free(c, bad_ptr);
    TEST_ASSERT_STATUS(status, SLAB_ERR_CORRUPTION);

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Stress Tests

// Performs randomized allocation and deallocation to stress-test lists.
static bool test_churn_stress(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_churn", 64, 8);
    tracker_add_cache(c);

    #define CHURN_ITERS 1000
    #define CHURN_POOL 50
    void* pool[CHURN_POOL];
    memset(pool, 0, sizeof(pool));

    uint32_t seed = 12345;

    for(int i=0; i<CHURN_ITERS; i++) {
        // LCG Random
        seed = seed * 1103515245 + 12345;
        uint32_t rand_val = (seed / 65536) % 32768;
        int idx = rand_val % CHURN_POOL;
        
        if (pool[idx] == NULL) {
            if (slab_alloc(c, &pool[idx]) != SLAB_OK) return false;
        } else {
            if (slab_free(c, pool[idx]) != SLAB_OK) return false;
            pool[idx] = NULL;
        }
    }

    // Manual cleanup of pool
    for(int i=0; i<CHURN_POOL; i++) {
        if (pool[i]) slab_free(c, pool[i]);
    }

    TEST_ASSERT(slab_verify_integrity());
    
    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Advanced / Functional Tests

// Tests fragmentation handling by creating holes and verifying reuse.
static bool test_swiss_cheese_reuse(void) {
    tracker_reset();
    
    slab_cache_t* c = slab_cache_create("test_holes", 128, 8);
    tracker_add_cache(c);

    #define HOLE_ITEMS 64 
    void* ptrs[HOLE_ITEMS];

    // 1. Fill
    for (int i = 0; i < HOLE_ITEMS; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
        tracker_add_obj(c, ptrs[i]);
    }

    slab_cache_stats_t stats_peak;
    slab_cache_stats(c, &stats_peak);
    uint64_t peak_slabs = stats_peak.slab_count;

    // 2. Punch holes (Free every even index)
    for (int i = 0; i < HOLE_ITEMS; i += 2) {
        TEST_ASSERT_STATUS(slab_free(c, ptrs[i]), SLAB_OK);
        for(int t=0; t<g_tracker_idx; t++) 
             if(g_tracker[t].ptr == ptrs[i]) g_tracker[t].active = false;
        ptrs[i] = NULL;
    }

    // 3. Re-fill the holes
    for (int i = 0; i < HOLE_ITEMS; i += 2) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
        tracker_add_obj(c, ptrs[i]);
    }

    // 4. Verify Slab Count (should not have grown)
    slab_cache_stats_t stats_final;
    slab_cache_stats(c, &stats_final);

    if (stats_final.slab_count > peak_slabs) {
        LOGF("[FAIL] Allocator grew instead of filling holes. Slabs: %lu -> %lu\n", 
             peak_slabs, stats_final.slab_count);
        return false;
    }

    tracker_cleanup();
    return true;
}

// Simulates a buffer underflow to corrupt the object header.
static bool test_header_corruption(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_smash", 32, 8);
    tracker_add_cache(c);

    void* obj;
    slab_alloc(c, &obj);
    tracker_add_obj(c, obj);

    // Simulate buffer underflow: overwriting the header
    uint32_t* header_magic_ptr = (uint32_t*)((uintptr_t)obj - sizeof(slab_test_header_t));
    
    // Sanity check
    if (*header_magic_ptr != 0xA110C8ED) {
        LOGF("[FAIL] Test assumption wrong: Header magic 0x%x not found at -offset\n", *header_magic_ptr);
        return false;
    }

    // Corrupt the header
    *header_magic_ptr = 0xDEADBEEF;

    // Try to free
    slab_status_t status = slab_free(c, obj);
    TEST_ASSERT_STATUS(status, SLAB_ERR_CORRUPTION);

    // Manual cleanup
    g_tracker[1].active = false;
    slab_cache_destroy(c);
    g_tracker[0].active = false;

    return true;
}

// Detects use-after-free by checking integrity markers in the free list.
static bool test_use_after_free_detection(void) {
    tracker_reset();
    slab_cache_t* c = slab_cache_create("test_poison", 64, 8);
    tracker_add_cache(c);

    void* obj;
    slab_alloc(c, &obj);
    
    // 1. Free it (It goes to freelist)
    slab_free(c, obj);

    // 2. Corrupt it (Use After Free)
    memset(obj, 0xCC, 64);

    // 3. Verify Integrity (Expected to fail)
    bool integrity = slab_verify_integrity();

    if (integrity) {
        LOGF("[FAIL] Integrity check failed to detect corrupted free list node\n");
        return false;
    }

    // Repair the object header in memory so we can safely destroy 
    // the cache without causing a kernel panic during cleanup.
    
    slab_test_free_obj_t* free_head = (slab_test_free_obj_t*)obj;
    
    // Restore Magic and Red Zones
    free_head->magic = 0xFEEDF00D;        // SLAB_FREE_MAGIC
    free_head->red_zone_pre = 0xDEADFA11; // SLAB_RED_ZONE
    free_head->red_zone_post = 0xDEADFA11;// SLAB_RED_ZONE
    
    // Terminate list (since we overwrote the 'next' pointer)
    free_head->next = NULL; 

    // 4. Clean Destruction
    slab_cache_destroy(c);
    
    // Mark tracker as inactive since we manually destroyed it
    g_tracker[0].active = false; 

    return true;
}

// Ensures that destroying a cache with active objects releases memory (no leak).
static bool test_dirty_destroy_leak(void) {
    tracker_reset();

    // Snapshot PMM
    pmm_stats_t pmm_start;
    pmm_get_stats(&pmm_start);
    uint64_t free_bytes_start = 0; 
    
    for(int i=0; i < PMM_MAX_ORDERS; i++) {
        free_bytes_start += (pmm_start.free_blocks[i] * (1ULL << i) * PAGE_SIZE);
    }

    // Create cache and allocate but DO NOT free
    slab_cache_t* c = slab_cache_create("test_leak", 128, 8);
    void* p;
    for(int i=0; i<50; i++) slab_alloc(c, &p);

    // Destroy cache immediately
    slab_cache_destroy(c);

    // Snapshot PMM again
    pmm_stats_t pmm_end;
    pmm_get_stats(&pmm_end);
    uint64_t free_bytes_end = 0;
    
    for(int i=0; i < PMM_MAX_ORDERS; i++) {
        free_bytes_end += (pmm_end.free_blocks[i] * (1ULL << i) * PAGE_SIZE);
    }

    int64_t delta = (int64_t)free_bytes_start - (int64_t)free_bytes_end;
    
    // Allow small delta for alignment/structure overhead, but large leaks imply failure
    if (delta > 8192) {
        LOGF("[FAIL] Dirty destroy leaked memory. Delta: %ld bytes\n", delta);
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
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

void test_slab(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN SLAB ALLOCATOR TEST ---\n");
    
    run_test("Initialization Check", test_init_check);
    run_test("Cache Creation Params", test_cache_create_validate);
    run_test("Basic Alloc/Free Cycle", test_alloc_free_basic);
    run_test("Alignment Enforcement", test_alignment_strictness);
    run_test("Zero-Init Guarantee", test_zero_initialization);
    run_test("Slab Growth (Multi-page)", test_slab_growth);
    run_test("Slab Shrinking (Culling)", test_culling_shrink);
    run_test("Partial Slab Priority", test_partial_slab_priority);
    run_test("Double Free Detection", test_double_free);
    run_test("Cross-Cache Free Prevention", test_cross_cache_free);
    run_test("Invalid Pointer Free", test_bad_pointer_free);
    run_test("Random Alloc/Free Churn", test_churn_stress);
    run_test("Hole Filling (Fragmentation)", test_swiss_cheese_reuse);
    run_test("Header Corruption (Underflow)", test_header_corruption);
    run_test("Use-After-Free Detection", test_use_after_free_detection);
    run_test("Dirty Cache Destruction (Leak)", test_dirty_destroy_leak);

    // Final global integrity check
    if (!slab_verify_integrity()) {
        LOGF("[FAIL] Final System-wide Integrity Check Failed\n");
    } else {
        LOGF("[INFO] Final System-wide Integrity Check Passed\n");
    }

    LOGF("--- END SLAB ALLOCATOR TEST ---\n");
    LOGF("Slab Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/vga_console.h>
    #include <kernel/drivers/vga_stdio.h>
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