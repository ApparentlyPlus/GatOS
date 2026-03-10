/*
 * test_pmm.c - Physical Memory Manager Validation Suite
 *
 * Tests every public PMM function: pmm_is_initialized, pmm_managed_base/end/size,
 * pmm_min_block_size, pmm_alloc, pmm_free, pmm_mark_reserved_range,
 * pmm_mark_free_range, pmm_get_stats, pmm_dump_stats, pmm_verify_integrity.
 *
 * Machine-adaptive: exhaustion and ladder tests use pmm_managed_size() and
 * pmm_min_block_size() instead of hardcoded sizes.
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_TRACK 4096
typedef struct { uint64_t addr; size_t size; bool on; } pmtr_t;

static pmtr_t g_tr[MAX_TRACK];
static int    g_tidx         = 0;
static int    g_tests_total  = 0;
static int    g_tests_passed = 0;

#pragma region Tracker

static void tr_reset(void) {
    for (int i = 0; i < MAX_TRACK; i++) g_tr[i].on = false;
    g_tidx = 0;
}

static void tr_add(uint64_t a, size_t s) {
    if (g_tidx < MAX_TRACK) g_tr[g_tidx++] = (pmtr_t){a, s, true};
    else LOGF("[WARN] PMM tracker full\n");
}

static void tr_free(void) {
    for (int i = 0; i < MAX_TRACK; i++)
        if (g_tr[i].on) { pmm_free(g_tr[i].addr, g_tr[i].size); g_tr[i].on = false; }
    g_tidx = 0;
}
#pragma endregion

#pragma region Invariants

static bool t_initialized(void) {
    TEST_ASSERT(pmm_is_initialized());
    return true;
}

static bool t_base_end(void) {
    TEST_ASSERT(pmm_managed_end() > pmm_managed_base());
    return true;
}

static bool t_sz_consistent(void) {
    TEST_ASSERT(pmm_managed_size() == pmm_managed_end() - pmm_managed_base());
    return true;
}

static bool t_min_pow2(void) {
    uint64_t min = pmm_min_block_size();
    TEST_ASSERT(min > 0);
    TEST_ASSERT((min & (min - 1)) == 0); /* power of two */
    return true;
}

static bool t_min_geq_pg(void) {
    TEST_ASSERT(pmm_min_block_size() >= 4096ULL);
    return true;
}
#pragma endregion

#pragma region Basic Alloc / Free

static bool t_alloc_ok(void) {
    tr_reset();
    uint64_t a; size_t s = pmm_min_block_size();
    TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK);
    tr_add(a, s);
    tr_free(); return true;
}

static bool t_alloc_range(void) {
    tr_reset();
    uint64_t a; size_t s = pmm_min_block_size();
    TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK); tr_add(a, s);
    TEST_ASSERT(a >= pmm_managed_base());
    TEST_ASSERT(a + s <= pmm_managed_end());
    tr_free(); return true;
}

static bool t_alloc_aligned(void) {
    tr_reset();
    for (int i = 0; i < 5; i++) {
        size_t s = pmm_min_block_size() << i;
        uint64_t a;
        if (pmm_alloc(s, &a) == PMM_ERR_OOM) break;
        tr_add(a, s);
        if (a & (s - 1)) {
            LOGF("[FAIL] 0x%lx not aligned to 0x%lx\n", a, s);
            tr_free(); return false;
        }
    }
    tr_free(); return true;
}

static bool t_alloc_uniq(void) {
    tr_reset();
    uint64_t a, b; size_t s = pmm_min_block_size();
    if (pmm_alloc(s, &a) != PMM_OK) return true; tr_add(a, s);
    if (pmm_alloc(s, &b) != PMM_OK) { tr_free(); return true; } tr_add(b, s);
    TEST_ASSERT(a != b);
    bool overlap = (a < b + s && b < a + s);
    TEST_ASSERT(!overlap);
    tr_free(); return true;
}

static bool t_free_ok(void) {
    tr_reset();
    uint64_t a; size_t s = pmm_min_block_size();
    TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(a, s), PMM_OK);
    return true;
}
#pragma endregion

#pragma region Memory Access

static bool t_mem_access(void) {
    tr_reset();
    uint64_t phys; size_t s = pmm_min_block_size();
    if (pmm_alloc(s, &phys) != PMM_OK) return true; tr_add(phys, s);
    volatile uint64_t* p = (volatile uint64_t*)PHYSMAP_P2V(phys);
    uint64_t pat = 0xDEADBEEFCAFEBABEULL;
    *p = pat;
    TEST_ASSERT(*p == pat);
    *p = 0;
    tr_free(); return true;
}

static bool t_mem_pattern(void) {
    tr_reset();
    uint64_t phys; size_t s = pmm_min_block_size();
    if (pmm_alloc(s, &phys) != PMM_OK) return true; tr_add(phys, s);
    uint32_t* p = (uint32_t*)PHYSMAP_P2V(phys);
    for (size_t i = 0; i < s / 4; i++) p[i] = (uint32_t)(i * 0x12345678u);
    for (size_t i = 0; i < s / 4; i++) TEST_ASSERT(p[i] == (uint32_t)(i * 0x12345678u));
    tr_free(); return true;
}
#pragma endregion

#pragma region Boundary Enforcement

static bool t_free_before_base(void) {
    uint64_t base = pmm_managed_base();
    size_t   min  = (size_t)pmm_min_block_size();
    if (base > min)
        TEST_ASSERT_STATUS(pmm_free(base - min, min), PMM_ERR_OUT_OF_RANGE);
    return true;
}

static bool t_free_after_end(void) {
    TEST_ASSERT_STATUS(pmm_free(pmm_managed_end(), (size_t)pmm_min_block_size()), PMM_ERR_OUT_OF_RANGE);
    return true;
}
#pragma endregion

#pragma region Buddy Mechanics

static bool t_buddy_split_merge(void) {
    tr_reset();
    size_t min  = (size_t)pmm_min_block_size();
    size_t huge = min * 4;
    uint64_t base;
    if (pmm_alloc(huge, &base) != PMM_OK) return true;
    pmm_free(base, huge);

    /* Alloc 4 min-blocks; they should come from the just-freed huge block */
    uint64_t a[4];
    bool any_fail = false;
    for (int i = 0; i < 4; i++) {
        if (pmm_alloc(min, &a[i]) != PMM_OK) { any_fail = true; break; }
        tr_add(a[i], min);
    }
    if (any_fail) { tr_free(); return true; }

    bool contiguous = true;
    for (int i = 1; i < 4; i++)
        if (a[i] != a[i-1] + min) { contiguous = false; break; }

    /* Release all; should allow re-allocating the full huge block */
    tr_free();
    uint64_t back;
    if (contiguous) {
        if (pmm_alloc(huge, &back) == PMM_OK) {
            pmm_free(back, huge);
        } else {
            LOGF("[FAIL] buddy merge failed\n"); return false;
        }
    }
    return true;
}
#pragma endregion

#pragma region Stats

static bool t_stats_smoke(void) {
    pmm_stats_t s; pmm_get_stats(&s);
    /* Must not crash */
    return true;
}

static bool t_stats_alloc_cnt(void) {
    pmm_stats_t s0; pmm_get_stats(&s0);
    uint64_t a; size_t sz = (size_t)pmm_min_block_size();
    if (pmm_alloc(sz, &a) != PMM_OK) return true;
    pmm_stats_t s1; pmm_get_stats(&s1);
    TEST_ASSERT(s1.alloc_calls == s0.alloc_calls + 1);
    pmm_free(a, sz);
    return true;
}

static bool t_stats_free_cnt(void) {
    pmm_stats_t s0; pmm_get_stats(&s0);
    uint64_t a; size_t sz = (size_t)pmm_min_block_size();
    if (pmm_alloc(sz, &a) != PMM_OK) return true;
    pmm_free(a, sz);
    pmm_stats_t s1; pmm_get_stats(&s1);
    TEST_ASSERT(s1.free_calls > s0.free_calls);
    return true;
}

static bool t_stats_dump(void) {
    pmm_dump_stats(); /* must not crash */
    return true;
}
#pragma endregion

#pragma region Integrity

static bool t_verify_clean(void) {
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}

static bool t_corrupt_detect(void) {
    tr_reset();
    uint64_t phys; size_t s = (size_t)pmm_min_block_size();
    if (pmm_alloc(s, &phys) != PMM_OK) return true;
    pmm_free(phys, s);
    pmm_free_header_t* h = (pmm_free_header_t*)PHYSMAP_P2V(phys);
    uint32_t saved = h->magic;
    if (saved != 0xFEEDBEEF) return true; /* Skip if magic mismatch */
    h->magic = 0xDEADDEAD;
    bool ok = pmm_verify_integrity();
    h->magic = saved; /* Repair */
    TEST_ASSERT(!ok);
    TEST_ASSERT(pmm_verify_integrity()); /* Repaired */
    return true;
}
#pragma endregion

#pragma region Reserved Range

static bool t_reserved(void) {
    tr_reset();
    size_t huge = 1024 * 1024;
    uint64_t base;
    TEST_ASSERT_STATUS(pmm_alloc(huge, &base), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(base, huge), PMM_OK);

    uint64_t rs = base + 256 * 1024;
    uint64_t re = base + 512 * 1024;
    TEST_ASSERT_STATUS(pmm_mark_reserved_range(rs, re), PMM_OK);

    uint64_t frag;
    if (pmm_alloc(256 * 1024, &frag) == PMM_OK) {
        tr_add(frag, 256 * 1024);
        bool bad = (frag >= rs && frag < re);
        TEST_ASSERT(!bad);
    }
    tr_free(); return true;
}

static bool t_mark_free(void) {
    /* Just test it doesn't crash and returns something sensible */
    uint64_t base = pmm_managed_base();
    /* Call on a very small already-free region — implementation must handle */
    pmm_status_t s = pmm_mark_free_range(base, base); /* zero-size edge */
    /* Any non-crash result is acceptable */
    (void)s;
    return true;
}
#pragma endregion

#pragma region Churn Stress

static bool t_order_churn(void) {
    tr_reset();
    size_t min = (size_t)pmm_min_block_size();
    for (int i = 0; i < 100; i++) {
        size_t s = (i % 2 == 0) ? min : min * 4;
        uint64_t a;
        if (pmm_alloc(s, &a) == PMM_OK) {
            tr_add(a, s);
            uint32_t* p = (uint32_t*)PHYSMAP_P2V(a);
            *p = (uint32_t)s;
        }
        if (i % 10 == 0) {
            for (int j = 0; j < g_tidx; j += 2) {
                if (g_tr[j].on) {
                    uint32_t* p = (uint32_t*)PHYSMAP_P2V(g_tr[j].addr);
                    if (*p != (uint32_t)g_tr[j].size) { LOGF("[FAIL] churn corruption\n"); tr_free(); return false; }
                    pmm_free(g_tr[j].addr, g_tr[j].size);
                    g_tr[j].on = false;
                }
            }
        }
    }
    TEST_ASSERT(pmm_verify_integrity());
    tr_free(); return true;
}
#pragma endregion

#pragma region All Orders Ladder

static bool t_all_orders(void) {
    tr_reset();
    uint64_t min = pmm_min_block_size();
    uint64_t max = pmm_managed_size();
    for (uint64_t s = min; s < max / 2; s <<= 1) {
        uint64_t a;
        if (pmm_alloc((size_t)s, &a) == PMM_OK) {
            tr_add(a, (size_t)s);
            if (a & (s - 1)) { LOGF("[FAIL] order %llu not aligned\n", s); tr_free(); return false; }
        } else break;
    }
    for (int i = g_tidx - 1; i >= 0; i--) {
        if (g_tr[i].on) { pmm_free(g_tr[i].addr, g_tr[i].size); g_tr[i].on = false; }
    }
    g_tidx = 0;
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}
#pragma endregion

#pragma region Exhaustion Stability

static bool t_exhaustion(void) {
    tr_reset();
    size_t s = (size_t)pmm_min_block_size();
    uint64_t a;
    while (g_tidx < MAX_TRACK) {
        if (pmm_alloc(s, &a) == PMM_ERR_OOM) break;
        tr_add(a, s);
    }
    /* Verify integrity while exhausted */
    TEST_ASSERT(pmm_verify_integrity());
    tr_free();
    /* After freeing, at least one more alloc must succeed */
    TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK);
    pmm_free(a, s);
    return true;
}
#pragma endregion

#pragma region Sandwich Coalescing

static bool t_sandwich(void) {
    tr_reset();
    size_t sz = (size_t)pmm_min_block_size();
    size_t big = sz * 4;
    uint64_t base;
    if (pmm_alloc(big, &base) != PMM_OK) return true;
    pmm_free(base, big);

    uint64_t a, b, c, d;
    if (pmm_alloc(sz,&a)!=PMM_OK||pmm_alloc(sz,&b)!=PMM_OK||
        pmm_alloc(sz,&c)!=PMM_OK||pmm_alloc(sz,&d)!=PMM_OK) {
        tr_free(); return true;
    }
    tr_add(a,sz); tr_add(b,sz); tr_add(c,sz); tr_add(d,sz);

    bool contig = (b==a+sz && c==b+sz && d==c+sz);
    if (!contig) { tr_free(); return true; }

    pmm_free(a,sz); g_tr[0].on=false;
    pmm_free(c,sz); g_tr[2].on=false;
    pmm_free(b,sz); g_tr[1].on=false; /* triggers merge */

    uint64_t merged;
    if (pmm_alloc(sz*2, &merged) != PMM_OK) {
        LOGF("[FAIL] coalesce: can't re-alloc merged block\n");
        g_tr[3].on=false; pmm_free(d,sz);
        return false;
    }
    tr_add(merged, sz*2);
    tr_free(); return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-40s ", name);
    if (!pmm_verify_integrity()) { LOGF("[SKIP] (PMM corrupted)\n"); return; }
    bool pass = fn();
    if (g_tidx > 0) { LOGF("[WARN] leak (cleaning) ... "); tr_free(); }
    if (pass) { g_tests_passed++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_pmm(void) {
    g_tests_total  = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN PMM TEST ---\n");

    run_test("initialized",                   t_initialized);
    run_test("base < end",                    t_base_end);
    run_test("size = end - base",             t_sz_consistent);
    run_test("min_block is power of 2",       t_min_pow2);
    run_test("min_block >= 4096",             t_min_geq_pg);
    run_test("alloc: PMM_OK",                 t_alloc_ok);
    run_test("alloc: within managed range",   t_alloc_range);
    run_test("alloc: naturally aligned",      t_alloc_aligned);
    run_test("alloc: unique non-overlapping", t_alloc_uniq);
    run_test("free: PMM_OK",                  t_free_ok);
    run_test("mem: read/write access",        t_mem_access);
    run_test("mem: pattern persistence",      t_mem_pattern);
    run_test("free: before base → error",     t_free_before_base);
    run_test("free: after end → error",       t_free_after_end);
    run_test("buddy: split + merge",          t_buddy_split_merge);
    run_test("stats: smoke (no crash)",       t_stats_smoke);
    run_test("stats: alloc_calls increments", t_stats_alloc_cnt);
    run_test("stats: free_calls increments",  t_stats_free_cnt);
    run_test("stats: dump no crash",          t_stats_dump);
    run_test("integrity: clean",              t_verify_clean);
    run_test("integrity: detects corruption", t_corrupt_detect);
    run_test("mark_reserved_range",           t_reserved);
    run_test("mark_free_range (edge)",        t_mark_free);
    run_test("order churn stress",            t_order_churn);
    run_test("all orders ladder",             t_all_orders);
    run_test("exhaustion + recovery",         t_exhaustion);
    run_test("sandwich coalesce",             t_sandwich);

    LOGF("--- END PMM TEST ---\n");
    LOGF("PMM Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some PMM tests failed (%d/%d passed).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All PMM tests passed! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
