/*
 * pmm_tests.c - Physical Memory Manager Validation Suite
 *
 * This suite verifies the correctness, stability, and security of the
 * Physical Memory Manager (Buddy Allocator). It operates on the live
 * system memory and verifies allocator logic, boundary enforcement,
 * exhaustion handling, and integrity checks.
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#pragma region Configuration & Types

#define MAX_TRACKED_ITEMS 4096

typedef struct {
    uint64_t addr;
    size_t size;
    bool active;
} pmm_tracker_t;

// Static tracking to maintain isolation
static pmm_tracker_t g_tracker[MAX_TRACKED_ITEMS];
static int g_tracker_idx = 0;

static int g_tests_total = 0;
static int g_tests_passed = 0;

#pragma endregion

#pragma region Harness Helpers

// Resets the internal tracking array before a test begins.
static void tracker_reset(void) {
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        g_tracker[i].active = false;
        g_tracker[i].addr = 0;
        g_tracker[i].size = 0;
    }
    g_tracker_idx = 0;
}

// Registers a memory allocation to be automatically freed during cleanup.
static void tracker_add(uint64_t addr, size_t size) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx].addr = addr;
        g_tracker[g_tracker_idx].size = size;
        g_tracker[g_tracker_idx].active = true;
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] PMM Tracker full. Subsequent allocs may leak on panic.\n");
    }
}

// Frees all tracked allocations.
static void tracker_cleanup(void) {
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        if (g_tracker[i].active) {
            pmm_free(g_tracker[i].addr, g_tracker[i].size);
            g_tracker[i].active = false;
        }
    }
    g_tracker_idx = 0;
}

#pragma endregion

#pragma region Core Allocator Tests

// Checks if the PMM is initialized and reports consistent memory statistics.
static bool test_invariants(void) {
    uint64_t start = pmm_managed_base();
    uint64_t end = pmm_managed_end();
    uint64_t size = pmm_managed_size();
    uint64_t min_block = pmm_min_block_size();

    TEST_ASSERT(pmm_is_initialized());
    TEST_ASSERT(end > start);
    TEST_ASSERT(size == (end - start));
    TEST_ASSERT(min_block > 0);
    TEST_ASSERT((min_block & (min_block - 1)) == 0);
    
    return true;
}

// Verifies that allocations adhere to natural alignment requirements.
static bool test_alignment_contract(void) {
    tracker_reset();
    size_t min = pmm_min_block_size();
    
    for (int i = 0; i < 5; i++) {
        size_t size = min << i;
        uint64_t addr;
        
        pmm_status_t status = pmm_alloc(size, &addr);
        if (status == PMM_ERR_OOM) break;
        
        TEST_ASSERT_STATUS(status, PMM_OK);
        tracker_add(addr, size);

        TEST_ASSERT(addr >= pmm_managed_base());
        TEST_ASSERT(addr + size <= pmm_managed_end());

        if ((addr & (size - 1)) != 0) {
            LOGF("[FAIL] Addr 0x%lx not aligned to size 0x%lx\n", addr, size);
            return false;
        }
    }

    tracker_cleanup();
    return true;
}

// Ensures that attempting to free memory outside the managed range returns errors.
static bool test_boundary_enforcement(void) {
    uint64_t start = pmm_managed_base();
    uint64_t end = pmm_managed_end();
    size_t min = pmm_min_block_size();

    if (start > 0) {
        TEST_ASSERT_STATUS(pmm_free(start - min, min), PMM_ERR_OUT_OF_RANGE);
    }
    TEST_ASSERT_STATUS(pmm_free(end, min), PMM_ERR_OUT_OF_RANGE);

    return true;
}

// Verifies that consecutive allocations return distinct, non-overlapping addresses.
static bool test_uniqueness(void) {
    tracker_reset();
    size_t sz = pmm_min_block_size();
    uint64_t addr1, addr2;

    if (pmm_alloc(sz, &addr1) != PMM_OK) return true;
    tracker_add(addr1, sz);

    if (pmm_alloc(sz, &addr2) != PMM_OK) {
        tracker_cleanup();
        return true; 
    }
    tracker_add(addr2, sz);

    TEST_ASSERT(addr1 != addr2);
    
    bool overlap = (addr1 < (addr2 + sz) && addr2 < (addr1 + sz));
    TEST_ASSERT(!overlap);

    tracker_cleanup();
    return true;
}

// Fills memory to capacity to ensure the allocator handles OOM gracefully.
static bool test_exhaustion_stability(void) {
    tracker_reset();
    size_t sz = pmm_min_block_size();
    uint64_t addr;

    while (g_tracker_idx < MAX_TRACKED_ITEMS) {
        pmm_status_t status = pmm_alloc(sz, &addr);
        if (status == PMM_ERR_OOM) break;
        TEST_ASSERT_STATUS(status, PMM_OK);
        tracker_add(addr, sz);
    }

    pmm_stats_t stats;
    pmm_get_stats(&stats);
    
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        TEST_ASSERT(stats.free_blocks[0] == 0);
    }

    TEST_ASSERT(pmm_verify_integrity());
    tracker_cleanup();
    return true;
}

// Probes the buddy system's ability to split large blocks and merge them back.
static bool test_buddy_mechanics(void) {
    tracker_reset();
    size_t min = pmm_min_block_size();
    uint64_t addr_large;
    size_t size_large = min;
    bool found_large = false;

    // Probe for a block significantly larger than minimum
    for (size_t s = min; s <= (4 * 1024 * 1024); s <<= 1) {
        if (pmm_alloc(s, &addr_large) == PMM_OK) {
            pmm_free(addr_large, s);
            size_large = s;
            found_large = true;
        } else {
            break; 
        }
    }

    if (!found_large || size_large == min) return true;

    // Alloc large, free, alloc two halves
    TEST_ASSERT_STATUS(pmm_alloc(size_large, &addr_large), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(addr_large, size_large), PMM_OK);

    size_t half_size = size_large / 2;
    uint64_t half1, half2;

    TEST_ASSERT_STATUS(pmm_alloc(half_size, &half1), PMM_OK);
    tracker_add(half1, half_size);

    TEST_ASSERT_STATUS(pmm_alloc(half_size, &half2), PMM_OK);
    tracker_add(half2, half_size);

    bool in_range1 = (half1 >= addr_large && half1 < addr_large + size_large);
    bool in_range2 = (half2 >= addr_large && half2 < addr_large + size_large);

    if (!in_range1 || !in_range2) {
        tracker_cleanup();
        return true;
    }

    tracker_cleanup(); // Free halves

    // Re-acquire original large block (verifies merge)
    uint64_t verify_addr;
    TEST_ASSERT_STATUS(pmm_alloc(size_large, &verify_addr), PMM_OK);
    tracker_add(verify_addr, size_large);

    tracker_cleanup();
    return true;
}

// Writes pattern data to allocated memory to ensure it is correctly mapped.
static bool test_memory_access(void) {
    tracker_reset();
    size_t sz = pmm_min_block_size();
    uint64_t phys;

    if (pmm_alloc(sz, &phys) != PMM_OK) return true;
    tracker_add(phys, sz);

    volatile uint64_t *ptr = (volatile uint64_t*)PHYSMAP_P2V(phys);
    uint64_t pattern = 0xCAFEBABE12345678;
    
    *ptr = pattern;
    TEST_ASSERT(*ptr == pattern);
    *ptr = 0;

    tracker_cleanup();
    return true;
}

// Intentionally corrupts a free block header to verify integrity checking.
static bool test_integrity_checks(void) {
    tracker_reset();
    size_t sz = pmm_min_block_size();
    uint64_t phys;

    if (pmm_alloc(sz, &phys) != PMM_OK) return true;
    
    pmm_free(phys, sz);

    pmm_free_header_t *header = (pmm_free_header_t*)PHYSMAP_P2V(phys);
    uint32_t old_magic = header->magic;
    
    if (old_magic == 0xFEEDBEEF) {
        header->magic = 0xDEADDEAD; // Corrupt

        if (pmm_verify_integrity()) {
            header->magic = old_magic; 
            LOGF("[FAIL] pmm_verify_integrity failed to detect corruption.\n");
            return false;
        }

        header->magic = old_magic; // Restore
        TEST_ASSERT(pmm_verify_integrity());
    }

    return true;
}

// Tests coalescing by freeing outer blocks before the middle one.
static bool test_sandwich_coalescing(void) {
    tracker_reset();
    size_t sz = pmm_min_block_size();
    size_t huge_sz = sz * 4;
    uint64_t base;
    
    if (pmm_alloc(huge_sz, &base) != PMM_OK) return true;
    pmm_free(base, huge_sz);
    
    uint64_t a, b, c, d;
    if (pmm_alloc(sz, &a) != PMM_OK) return false; tracker_add(a, sz);
    if (pmm_alloc(sz, &b) != PMM_OK) return false; tracker_add(b, sz);
    if (pmm_alloc(sz, &c) != PMM_OK) return false; tracker_add(c, sz);
    if (pmm_alloc(sz, &d) != PMM_OK) return false; tracker_add(d, sz);
    
    bool contiguous = (b == a + sz) && (c == b + sz) && (d == c + sz);
    if (!contiguous) {
        tracker_cleanup();
        return true;
    }
    
    // Free neighbors first
    pmm_free(a, sz); g_tracker[0].active = false;
    pmm_free(c, sz); g_tracker[2].active = false;
    
    // Free middle (trigger merge)
    pmm_free(b, sz); g_tracker[1].active = false;
    
    // Check if we can allocate merged size
    uint64_t merged_addr;
    if (pmm_alloc(sz * 2, &merged_addr) == PMM_OK) {
        tracker_add(merged_addr, sz * 2);
    } else {
        LOGF("[FAIL] Coalescing failed for sandwich case.\n");
        return false;
    }

    tracker_cleanup();
    return true;
}

// Performs randomized allocations/deallocations to stress-test free lists.
static bool test_order_churn(void) {
    tracker_reset();
    size_t min = pmm_min_block_size();
    
    for (int i = 0; i < 100; i++) {
        size_t size = (i % 2 == 0) ? min : min * 4;
        uint64_t addr;
        
        if (pmm_alloc(size, &addr) == PMM_OK) {
            tracker_add(addr, size);
            uint32_t *p = (uint32_t*)PHYSMAP_P2V(addr);
            *p = (uint32_t)size;
        }
        
        if (i % 10 == 0) {
            for (int j = 0; j < g_tracker_idx; j += 2) {
                if (g_tracker[j].active) {
                    uint32_t *p = (uint32_t*)PHYSMAP_P2V(g_tracker[j].addr);
                    if (*p != (uint32_t)g_tracker[j].size) {
                        LOGF("[FAIL] Memory corruption detected.\n");
                        return false;
                    }
                    pmm_free(g_tracker[j].addr, g_tracker[j].size);
                    g_tracker[j].active = false;
                }
            }
        }
    }
    
    if (!pmm_verify_integrity()) return false;
    tracker_cleanup();
    return true;
}

// Allocates blocks of every possible order to verify split/merge depth.
static bool test_all_orders_ladder(void) {
    tracker_reset();
    uint64_t max_sz = pmm_managed_size();
    uint64_t min_sz = pmm_min_block_size();
    
    for (uint64_t sz = min_sz; sz < max_sz / 2; sz <<= 1) {
        uint64_t addr;
        if (pmm_alloc(sz, &addr) == PMM_OK) {
            tracker_add(addr, sz);
            if ((addr & (sz - 1)) != 0) {
                LOGF("[FAIL] Order size %lu not aligned\n", sz);
                return false;
            }
        } else {
            break;
        }
    }
    
    // Free all manually to verify integrity post-cleanup
    for (int i = g_tracker_idx - 1; i >= 0; i--) {
        if (g_tracker[i].active) {
            pmm_free(g_tracker[i].addr, g_tracker[i].size);
            g_tracker[i].active = false;
        }
    }
    g_tracker_idx = 0; 
    
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}

// Verifies that marking a range as reserved correctly fragments free blocks.
static bool test_reserved_range_slicing(void) {
    tracker_reset();
    
    size_t huge_sz = 1024 * 1024;
    uint64_t base;
    
    TEST_ASSERT_STATUS(pmm_alloc(huge_sz, &base), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(base, huge_sz), PMM_OK);

    // Mark a chunk in the MIDDLE as reserved
    uint64_t res_start = base + (256 * 1024); // +256KB
    uint64_t res_end   = base + (512 * 1024); // +512KB (Length 256KB)
    
    TEST_ASSERT_STATUS(pmm_mark_reserved_range(res_start, res_end), PMM_OK);

    // Verify we cannot allocate the full 1MB anymore
    uint64_t check_addr;
    if (pmm_alloc(huge_sz, &check_addr) == PMM_OK) {
        if (check_addr == base) {
             LOGF("[FAIL] Reserved range was ignored, allocator returned overlapping block\n");
             return false;
        }
        tracker_add(check_addr, huge_sz);
    }

    // Verify we can allocate the fragments before and after
    uint64_t frag1, frag2;
    TEST_ASSERT_STATUS(pmm_alloc(256 * 1024, &frag1), PMM_OK);
    tracker_add(frag1, 256 * 1024);
    
    TEST_ASSERT_STATUS(pmm_alloc(256 * 1024, &frag2), PMM_OK);
    tracker_add(frag2, 256 * 1024);

    bool overlap1 = (frag1 >= res_start && frag1 < res_end);
    bool overlap2 = (frag2 >= res_start && frag2 < res_end);
    
    TEST_ASSERT(!overlap1 && !overlap2);

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
    if (!pmm_verify_integrity()) {
        LOGF("[SKIP] (System Corrupted)\n");
        return;
    }

    bool pass = func();

    if (g_tracker_idx > 0) {
        LOGF("[WARN] Leak detected (cleaning) ... ");
        tracker_cleanup();
    }

    if (pass) {
        g_tests_passed++;
        LOGF("[PASS]\n");
    } else {
        LOGF("[FAIL]\n");
    }
}

void test_pmm(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN PMM TEST ---\n");
    
    run_test("Invariants Check", test_invariants);
    run_test("Alignment Contracts", test_alignment_contract);
    run_test("Boundary Enforcement", test_boundary_enforcement);
    run_test("Uniqueness & Overlap", test_uniqueness);
    run_test("Exhaustion Stability", test_exhaustion_stability);
    run_test("Buddy Mechanics (Probe)", test_buddy_mechanics);
    run_test("Memory R/W Access", test_memory_access);
    run_test("Integrity/Corruption Detect", test_integrity_checks);
    run_test("Sandwich Coalescing", test_sandwich_coalescing);
    run_test("Order Churn Stress", test_order_churn);
    run_test("All Orders Ladder", test_all_orders_ladder);
    run_test("Reserved Range Slicing", test_reserved_range_slicing);
    
    LOGF("--- END PMM TEST ---\n");
    LOGF("PMM Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

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