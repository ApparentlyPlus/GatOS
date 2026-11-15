/*
 * test_pmm.c - Physical Memory Manager Validation Suite
 *
 * Author: Claude Code
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
#define PG 4096UL
typedef struct { uint64_t addr; size_t size; bool on; } pmtr_t;

static pmtr_t tr[MAX_TRACK];
static int tidx = 0;
static int ntests = 0;
static int npass = 0;

#pragma region Tracker

static void tr_reset(void) {
    for (int i = 0; i < MAX_TRACK; i++) tr[i].on = false;
    tidx = 0;
}

static void tr_add(uint64_t a, size_t s) {
    if (tidx < MAX_TRACK) tr[tidx++] = (pmtr_t){a, s, true};
    else LOGF("[WARN] PMM tracker full\n");
}

static void tr_free(void) {
    for (int i = 0; i < MAX_TRACK; i++)
        if (tr[i].on) { pmm_free(tr[i].addr, tr[i].size); tr[i].on = false; }
    tidx = 0;
}

static uint64_t free_bytes(void) {
    pmm_stats_t s; pmm_get_stats(&s);
    uint64_t total = 0;
    for (int i = 0; i < PMM_MAX_ORDERS; i++) total += s.free_blocks[i] * (PG << i);
    return total;
}
#pragma endregion

#pragma region Invariants

static bool t_initialized(void) {
    TEST_ASSERT(pmm_is_initialized());
    TEST_ASSERT(pmm_managed_end() == align_down(kmem->ram, PG));
    return true;
}

static bool t_double_init(void) {
    TEST_ASSERT_STATUS(pmm_init(pmm_managed_end(), 0), PMM_ERR_ALREADY_INIT);
    return true;
}
#pragma endregion

#pragma region Basic Alloc / Free

static bool t_alloc_ok(void) {
    tr_reset();
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(PG, &a), PMM_OK); tr_add(a, PG);
    TEST_ASSERT(a + PG <= pmm_managed_end());
    TEST_ASSERT(!pmm_is_free(a));
    tr_free();
    TEST_ASSERT(pmm_is_free(a));
    return true;
}

static bool t_alloc_bad_args(void) {
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(0, &a), PMM_ERR_INVALID);
    TEST_ASSERT_STATUS(pmm_alloc(PG, NULL), PMM_ERR_INVALID);
    TEST_ASSERT_STATUS(pmm_alloc(pmm_managed_end() * 2, &a), PMM_ERR_OOM);
    return true;
}

static bool t_alloc_rounds_up(void) {
    tr_reset();
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(PG + 1, &a), PMM_OK); tr_add(a, PG + 1);
    TEST_ASSERT((a & (2 * PG - 1)) == 0);       // rounded to an 8 KiB block
    TEST_ASSERT(!pmm_is_free(a + PG));          // and the second page is ours too
    tr_free(); return true;
}

static bool t_alloc_aligned(void) {
    tr_reset();
    for (int i = 0; i < 10; i++) {
        size_t s = PG << i;
        uint64_t a;
        TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK); tr_add(a, s);
        if (a & (s - 1)) { LOGF("[FAIL] 0x%lx not aligned to 0x%lx\n", a, s); tr_free(); return false; }
    }
    tr_free(); return true;
}

static bool t_alloc_uniq(void) {
    tr_reset();
    uint64_t a[64];
    for (int i = 0; i < 64; i++) { TEST_ASSERT_STATUS(pmm_alloc(PG, &a[i]), PMM_OK); tr_add(a[i], PG); }
    for (int i = 0; i < 64; i++)
        for (int j = i + 1; j < 64; j++)
            if (a[i] == a[j]) { LOGF("[FAIL] 0x%lx handed out twice\n", a[i]); tr_free(); return false; }
    tr_free(); return true;
}
#pragma endregion

#pragma region Memory Access

static bool t_mem_pattern(void) {
    tr_reset();
    uint64_t phys;
    TEST_ASSERT_STATUS(pmm_alloc(PG, &phys), PMM_OK); tr_add(phys, PG);
    uint32_t* p = (uint32_t*)PHYSMAP_P2V(phys);
    for (size_t i = 0; i < PG / 4; i++) p[i] = (uint32_t)(i * 0x12345678u);
    for (size_t i = 0; i < PG / 4; i++) TEST_ASSERT(p[i] == (uint32_t)(i * 0x12345678u));
    tr_free(); return true;
}
#pragma endregion

#pragma region Bad Frees

static bool t_free_after_end(void) {
    TEST_ASSERT_STATUS(pmm_free(pmm_managed_end(), PG), PMM_ERR_OUT_OF_RANGE);
    TEST_ASSERT_STATUS(pmm_free(pmm_managed_end() - PG, 2 * PG), PMM_ERR_OUT_OF_RANGE);
    return true;
}

static bool t_free_misaligned(void) {
    tr_reset();
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(4 * PG, &a), PMM_OK); tr_add(a, 4 * PG);
    TEST_ASSERT_STATUS(pmm_free(a + PG, 2 * PG), PMM_ERR_NOT_ALIGNED);
    tr_free(); return true;
}

static bool t_double_free(void) {
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(PG, &a), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(a, PG), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(a, PG), PMM_ERR_DOUBLE_FREE);
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}

static bool t_partial_double_free(void) {
    // Free the top half of a block, then the whole block: the top half is already free
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(2 * PG, &a), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(a + PG, PG), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(a, 2 * PG), PMM_ERR_DOUBLE_FREE);
    TEST_ASSERT_STATUS(pmm_free(a, PG), PMM_OK);
    TEST_ASSERT(pmm_is_free(a) && pmm_is_free(a + PG));
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}

static bool t_fake_header(void) {
    // An allocated page whose contents look exactly like a free header must not get merged
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(2 * PG, &a), PMM_OK);

    pmm_free_header_t* fake = (pmm_free_header_t*)PHYSMAP_P2V(a + PG);
    fake->magic = 0xFEEDBEEF;
    fake->order = 0;
    fake->next_phys = UINT64_MAX;
    fake->prev_phys = UINT64_MAX;

    TEST_ASSERT_STATUS(pmm_free(a, PG), PMM_OK);
    TEST_ASSERT(pmm_is_free(a));
    TEST_ASSERT(!pmm_is_free(a + PG));
    TEST_ASSERT(pmm_verify_integrity());

    TEST_ASSERT_STATUS(pmm_free(a + PG, PG), PMM_OK);
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}
#pragma endregion

#pragma region Buddy Mechanics

static bool t_buddy_split_merge(void) {
    tr_reset();
    uint64_t base;
    TEST_ASSERT_STATUS(pmm_alloc(4 * PG, &base), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(base, 4 * PG), PMM_OK);

    // Splitting it back down into single pages
    uint64_t a[4];
    for (int i = 0; i < 4; i++) { TEST_ASSERT_STATUS(pmm_alloc(PG, &a[i]), PMM_OK); tr_add(a[i], PG); }

    // Freeing all four must give the whole block back
    tr_free();
    TEST_ASSERT(pmm_is_free(base));
    uint64_t before = free_bytes();
    uint64_t back;
    TEST_ASSERT_STATUS(pmm_alloc(4 * PG, &back), PMM_OK);
    TEST_ASSERT(free_bytes() == before - 4 * PG);
    TEST_ASSERT_STATUS(pmm_free(back, 4 * PG), PMM_OK);
    return true;
}

static bool t_sandwich(void) {
    tr_reset();
    uint64_t base;
    TEST_ASSERT_STATUS(pmm_alloc(4 * PG, &base), PMM_OK);

    // Free the pages out of order, the middle one has to merge with both sides
    TEST_ASSERT_STATUS(pmm_free(base, PG), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(base + 2 * PG, PG), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(base + PG, PG), PMM_OK);
    TEST_ASSERT(!pmm_is_free(base + 3 * PG));
    TEST_ASSERT_STATUS(pmm_free(base + 3 * PG, PG), PMM_OK);

    TEST_ASSERT(pmm_verify_integrity());
    return true;
}
#pragma endregion

#pragma region Stats

static bool t_stats_counts(void) {
    pmm_stats_t s0; pmm_get_stats(&s0);
    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(PG, &a), PMM_OK);
    pmm_stats_t s1; pmm_get_stats(&s1);
    TEST_ASSERT(s1.alloc_calls == s0.alloc_calls + 1);
    TEST_ASSERT_STATUS(pmm_free(a, PG), PMM_OK);
    pmm_stats_t s2; pmm_get_stats(&s2);
    TEST_ASSERT(s2.free_calls == s1.free_calls + 1);
    pmm_dump_stats();
    return true;
}

static bool t_no_leak(void) {
    tr_reset();
    uint64_t before = free_bytes();
    for (int i = 0; i < 200; i++) {
        uint64_t a; size_t s = PG << (i % 5);
        TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK); tr_add(a, s);
    }
    TEST_ASSERT(free_bytes() < before);
    tr_free();
    TEST_ASSERT(free_bytes() == before);
    return true;
}
#pragma endregion

#pragma region Integrity

static bool t_verify_clean(void) {
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}

static bool t_corrupt_detect(void) {
    // Free only the top page of a 4 page block, its neighbor is still ours, so it can't merge
    uint64_t base;
    TEST_ASSERT_STATUS(pmm_alloc(4 * PG, &base), PMM_OK);
    TEST_ASSERT_STATUS(pmm_free(base + 3 * PG, PG), PMM_OK);

    pmm_free_header_t* h = (pmm_free_header_t*)PHYSMAP_P2V(base + 3 * PG);
    TEST_ASSERT(h->magic == 0xFEEDBEEF);
    h->magic = 0xDEADDEAD;
    bool ok = pmm_verify_integrity();
    h->magic = 0xFEEDBEEF;

    TEST_ASSERT(!ok);
    TEST_ASSERT(pmm_verify_integrity());

    for (int i = 0; i < 3; i++) TEST_ASSERT_STATUS(pmm_free(base + i * PG, PG), PMM_OK);
    return true;
}
#pragma endregion

#pragma region Churn Stress

static bool t_order_churn(void) {
    tr_reset();
    for (int i = 0; i < 100; i++) {
        size_t s = (i % 2 == 0) ? PG : PG * 4;
        uint64_t a;
        TEST_ASSERT_STATUS(pmm_alloc(s, &a), PMM_OK); tr_add(a, s);
        *(uint32_t*)PHYSMAP_P2V(a) = (uint32_t)s;

        if (i % 10 == 0) {
            for (int j = 0; j < tidx; j += 2) {
                if (!tr[j].on) continue;
                if (*(uint32_t*)PHYSMAP_P2V(tr[j].addr) != (uint32_t)tr[j].size) {
                    LOGF("[FAIL] churn corruption\n"); tr_free(); return false;
                }
                TEST_ASSERT_STATUS(pmm_free(tr[j].addr, tr[j].size), PMM_OK);
                tr[j].on = false;
            }
        }
    }
    TEST_ASSERT(pmm_verify_integrity());
    tr_free(); return true;
}

static bool t_all_orders(void) {
    tr_reset();
    uint64_t max = pmm_managed_end();
    for (uint64_t s = PG; s < max / 8; s <<= 1) {
        uint64_t a;
        if (pmm_alloc((size_t)s, &a) != PMM_OK) break;
        tr_add(a, (size_t)s);
        if (a & (s - 1)) { LOGF("[FAIL] order %lu not aligned\n", s); tr_free(); return false; }
    }
    TEST_ASSERT(tidx >= 10);
    for (int i = tidx - 1; i >= 0; i--) {
        TEST_ASSERT_STATUS(pmm_free(tr[i].addr, tr[i].size), PMM_OK);
        tr[i].on = false;
    }
    tidx = 0;
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}
#pragma endregion

#pragma region Exhaustion

static bool t_exhaustion(void) {
    // Drain everything in big blocks, nothing may land in the kernel or below it
    tr_reset();
    uint64_t before = free_bytes();
    uint64_t kend = kmem->kend;

    for (int o = PMM_MAX_ORDERS - 1; o >= 0; o--) {
        uint64_t s = PG << o, a;
        while (tidx < MAX_TRACK && pmm_alloc((size_t)s, &a) == PMM_OK) {
            tr_add(a, (size_t)s);
            if (a < kend) { LOGF("[FAIL] 0x%lx is below KEND (0x%lx)\n", a, kend); tr_free(); return false; }
        }
    }

    uint64_t a;
    TEST_ASSERT_STATUS(pmm_alloc(PG, &a), PMM_ERR_OOM);
    TEST_ASSERT(free_bytes() == 0);
    TEST_ASSERT(pmm_verify_integrity());

    tr_free();
    TEST_ASSERT(free_bytes() == before);
    TEST_ASSERT(pmm_verify_integrity());
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    ntests++;
    LOGF("[TEST] %-40s ", name);
    if (!pmm_verify_integrity()) { LOGF("[SKIP] (PMM corrupted)\n"); return; }
    bool pass = fn();
    if (tidx > 0) { LOGF("[WARN] leak (cleaning) ... "); tr_free(); }
    if (pass) { npass++; LOGF("[PASS]\n"); }
    else { LOGF("[FAIL]\n"); }
}

void test_pmm(void) {
    ntests = 0;
    npass = 0;

    LOGF("\n--- BEGIN PMM TEST ---\n");

    run_test("initialized",                   t_initialized);
    run_test("init: second call rejected",    t_double_init);
    run_test("alloc: PMM_OK + in range",      t_alloc_ok);
    run_test("alloc: bad arguments",          t_alloc_bad_args);
    run_test("alloc: rounds up to a block",   t_alloc_rounds_up);
    run_test("alloc: naturally aligned",      t_alloc_aligned);
    run_test("alloc: unique pages",           t_alloc_uniq);
    run_test("mem: pattern persistence",      t_mem_pattern);
    run_test("free: after end → error",       t_free_after_end);
    run_test("free: misaligned → error",      t_free_misaligned);
    run_test("free: double free caught",      t_double_free);
    run_test("free: partial double free",     t_partial_double_free);
    run_test("free: fake header not merged",  t_fake_header);
    run_test("buddy: split + merge",          t_buddy_split_merge);
    run_test("buddy: sandwich coalesce",      t_sandwich);
    run_test("stats: counters",               t_stats_counts);
    run_test("stats: no leaked bytes",        t_no_leak);
    run_test("integrity: clean",              t_verify_clean);
    run_test("integrity: detects corruption", t_corrupt_detect);
    run_test("order churn stress",            t_order_churn);
    run_test("all orders ladder",             t_all_orders);
    run_test("exhaustion: nothing below KEND",t_exhaustion);

    LOGF("--- END PMM TEST ---\n");
    LOGF("PMM Test Results: %d/%d\n\n", npass, ntests);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (npass != ntests) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some PMM tests failed (%d/%d passed).\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All PMM tests passed! (%d/%d)\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
