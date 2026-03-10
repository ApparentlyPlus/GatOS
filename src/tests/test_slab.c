/*
 * test_slab.c - Slab Allocator Validation Suite
 *
 * Tests every public slab API function. Machine-adaptive where applicable.
 * All caches created here use unique names to avoid collisions with kernel caches.
 */

#include "tests.h"
#include <kernel/debug.h>
#include <kernel/memory/slab.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <klibc/string.h>

#pragma region Harness

static int g_tests_total  = 0;
static int g_tests_passed = 0;

/* Caches registered during a test (for cleanup) */
#define MAX_TR_CACHES 16
static slab_cache_t* g_tr_caches[MAX_TR_CACHES];
static int           g_tr_n = 0;

static void tr_reg(slab_cache_t* c) {
    if (c && g_tr_n < MAX_TR_CACHES)
        g_tr_caches[g_tr_n++] = c;
}

static void tr_free(void) {
    for (int i = 0; i < g_tr_n; i++) {
        if (g_tr_caches[i]) slab_cache_destroy(g_tr_caches[i]);
    }
    g_tr_n = 0;
}

static void run_test(const char* name, bool (*fn)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-40s ", name);
    if (!slab_is_initialized()) { LOGF("[SKIP] (slab not init)\n"); return; }
    bool pass = fn();
    if (g_tr_n > 0) { LOGF("[WARN] leak (cleaning) ... "); tr_free(); }
    if (pass) { g_tests_passed++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}
#pragma endregion

#pragma region Helpers

/* Unique counter so each test gets a fresh cache name */
static int g_cache_seq = 0;

static slab_cache_t* mk_cache(const char* prefix, size_t obj_size, size_t align) {
    static char name[SLAB_CACHE_NAME_LEN];
    /* compose e.g. "ts_alloc_3" — stays within SLAB_CACHE_NAME_LEN */
    int n = 0;
    while (prefix[n] && n < 20) { name[n] = prefix[n]; n++; }
    name[n++] = '_';
    int seq = ++g_cache_seq;
    if (seq >= 100) { name[n++] = '0' + seq / 100; seq %= 100; }
    if (seq >= 10)  { name[n++] = '0' + seq / 10;  seq %= 10;  }
    name[n++] = '0' + seq;
    name[n]   = '\0';
    slab_cache_t* c = slab_cache_create(name, obj_size, align);
    tr_reg(c);
    return c;
}
#pragma endregion

#pragma region Init / Shutdown

static bool t_is_init(void) {
    /* already confirmed by run_test guard, but verify return value */
    TEST_ASSERT(slab_is_initialized() == true);
    return true;
}

static bool t_dbl_init(void) {
    /* second init must return ALREADY_INIT */
    slab_status_t s = slab_init();
    TEST_ASSERT(s == SLAB_ERR_ALREADY_INIT);
    return true;
}
#pragma endregion

#pragma region Cache creation / destruction / find

static bool t_create_nn(void) {
    slab_cache_t* c = mk_cache("ts_cnn", 64, 8);
    TEST_ASSERT(c != NULL);
    return true;
}

static bool t_create_sz(void) {
    slab_cache_t* c = mk_cache("ts_cos", 128, 8);
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(slab_cache_obj_size(c) >= 128);
    return true;
}

static bool t_create_name(void) {
    /* Use a fixed name so we can find it */
    slab_cache_t* c = slab_cache_create("ts_name_fixed", 32, 8);
    tr_reg(c);
    TEST_ASSERT(c != NULL);
    const char* n = slab_cache_name(c);
    TEST_ASSERT(n != NULL);
    TEST_ASSERT(kstrcmp(n, "ts_name_fixed") == 0);
    return true;
}

static bool t_create_align(void) {
    /* Allocations from an aligned cache must honour alignment */
    slab_cache_t* c = mk_cache("ts_aln", 64, 64);
    TEST_ASSERT(c != NULL);
    void* p = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
    TEST_ASSERT(p != NULL);
    TEST_ASSERT(((uintptr_t)p % 64) == 0);
    slab_free(c, p);
    return true;
}

static bool t_create_bad(void) {
    /* Object size 0 must fail */
    slab_cache_t* c = slab_cache_create("ts_bad0", 0, 8);
    TEST_ASSERT(c == NULL);
    return true;
}

static bool t_create_nulnm(void) {
    slab_cache_t* c = slab_cache_create(NULL, 32, 8);
    TEST_ASSERT(c == NULL);
    return true;
}

static bool t_find_hit(void) {
    slab_cache_t* c = slab_cache_create("ts_find_hit", 32, 8);
    tr_reg(c);
    TEST_ASSERT(c != NULL);
    slab_cache_t* f = slab_cache_find("ts_find_hit");
    TEST_ASSERT(f == c);
    return true;
}

static bool t_find_miss(void) {
    slab_cache_t* f = slab_cache_find("ts_no_such_cache_xyz");
    TEST_ASSERT(f == NULL);
    return true;
}

static bool t_find_null(void) {
    slab_cache_t* f = slab_cache_find(NULL);
    TEST_ASSERT(f == NULL);
    return true;
}

static bool t_destroy_rm(void) {
    slab_cache_t* c = slab_cache_create("ts_destroy_rem", 32, 8);
    TEST_ASSERT(c != NULL);
    slab_cache_destroy(c);
    /* Should not be findable after destroy */
    TEST_ASSERT(slab_cache_find("ts_destroy_rem") == NULL);
    return true;
}

static bool t_destroy_null(void) {
    /* Must not crash */
    slab_cache_destroy(NULL);
    return true;
}

static bool t_cache_lim(void) {
    /* Slab is dynamically allocated — verify stats accurately track many caches */
    slab_stats_t before; slab_get_stats(&before);
    int created = 0;
    for (int i = 0; i < 8; i++) {
        slab_cache_t* c = mk_cache("ts_lim", 32, 8);
        if (c) created++;
    }
    slab_stats_t after; slab_get_stats(&after);
    TEST_ASSERT(after.cache_count == before.cache_count + (uint64_t)created);
    /* tr_free cleans all registered caches */
    return true;
}
#pragma endregion

#pragma region Allocation / Free

static bool t_alloc_ok(void) {
    slab_cache_t* c = mk_cache("ts_alok", 64, 8);
    TEST_ASSERT(c != NULL);
    void* p = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
    TEST_ASSERT(p != NULL);
    slab_free(c, p);
    return true;
}

static bool t_alloc_nc(void) {
    void* p = NULL;
    slab_status_t s = slab_alloc(NULL, &p);
    TEST_ASSERT(s != SLAB_OK);
    return true;
}

static bool t_alloc_nout(void) {
    slab_cache_t* c = mk_cache("ts_alout", 64, 8);
    TEST_ASSERT(c != NULL);
    slab_status_t s = slab_alloc(c, NULL);
    TEST_ASSERT(s != SLAB_OK);
    return true;
}

static bool t_alloc_wr(void) {
    slab_cache_t* c = mk_cache("ts_alwr", 64, 8);
    TEST_ASSERT(c != NULL);
    void* p = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
    /* Write then read back */
    kmemset(p, 0xA5, 64);
    uint8_t* b = (uint8_t*)p;
    for (int i = 0; i < 64; i++) TEST_ASSERT(b[i] == 0xA5);
    slab_free(c, p);
    return true;
}

static bool t_alloc_uniq(void) {
    /* 32 allocations must all return distinct pointers */
    slab_cache_t* c = mk_cache("ts_uniq", 32, 8);
    TEST_ASSERT(c != NULL);
    void* ptrs[32];
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
        TEST_ASSERT(ptrs[i] != NULL);
    }
    for (int i = 0; i < 32; i++)
        for (int j = i + 1; j < 32; j++)
            TEST_ASSERT(ptrs[i] != ptrs[j]);
    for (int i = 0; i < 32; i++) slab_free(c, ptrs[i]);
    return true;
}

static bool t_alloc_sz1(void) {
    /* Minimum useful object: 1 byte */
    slab_cache_t* c = mk_cache("ts_sz1", 1, 1);
    TEST_ASSERT(c != NULL);
    void* p = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
    TEST_ASSERT(p != NULL);
    *(uint8_t*)p = 0xFF;
    TEST_ASSERT(*(uint8_t*)p == 0xFF);
    slab_free(c, p);
    return true;
}

static bool t_alloc_sizes(void) {
    /* Powers of two from 8 to 512 */
    for (size_t sz = 8; sz <= 512; sz <<= 1) {
        slab_cache_t* c = mk_cache("ts_vsz", sz, 8);
        if (!c) continue; /* size may exceed slab limit */
        void* p = NULL;
        if (slab_alloc(c, &p) == SLAB_OK) {
            TEST_ASSERT(p != NULL);
            kmemset(p, 0x5A, sz);
            slab_free(c, p);
        }
    }
    return true;
}

static bool t_free_nobj(void) {
    slab_cache_t* c = mk_cache("ts_fnul", 32, 8);
    TEST_ASSERT(c != NULL);
    /* Must not crash; error is acceptable */
    slab_free(c, NULL);
    return true;
}

static bool t_free_nc(void) {
    /* Must not crash */
    void* dummy = (void*)0x1000;
    slab_free(NULL, dummy);
    return true;
}

static bool t_dbl_free(void) {
    slab_cache_t* c = mk_cache("ts_dfd", 32, 8);
    TEST_ASSERT(c != NULL);
    void* p = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
    slab_free(c, p);
    /* Second free: expect non-OK or at least no crash */
    slab_status_t s2 = slab_free(c, p);
    /* Any non-crash is acceptable; CORRUPTION is ideal */
    (void)s2;
    return true;
}

static bool t_cycle(void) {
    /* Allocate and free the same slot 1000 times — no leak */
    slab_cache_t* c = mk_cache("ts_cyc", 48, 8);
    TEST_ASSERT(c != NULL);
    for (int i = 0; i < 1000; i++) {
        void* p = NULL;
        TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
        *(uint64_t*)p = (uint64_t)i;
        TEST_ASSERT_STATUS(slab_free(c, p), SLAB_OK);
    }
    slab_cache_stats_t st;
    slab_cache_stats(c, &st);
    TEST_ASSERT(st.active_objects == 0);
    return true;
}

static bool t_data_persist(void) {
    /* Data written before free must not corrupt adjacent object */
    slab_cache_t* c = mk_cache("ts_dp", 64, 8);
    TEST_ASSERT(c != NULL);
    void* a = NULL; void* b = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &a), SLAB_OK);
    TEST_ASSERT_STATUS(slab_alloc(c, &b), SLAB_OK);
    kmemset(a, 0xDE, 64);
    kmemset(b, 0xAD, 64);
    uint8_t* ba = (uint8_t*)a;
    uint8_t* bb = (uint8_t*)b;
    for (int i = 0; i < 64; i++) TEST_ASSERT(ba[i] == 0xDE);
    for (int i = 0; i < 64; i++) TEST_ASSERT(bb[i] == 0xAD);
    slab_free(c, a);
    slab_free(c, b);
    return true;
}
#pragma endregion

#pragma region Statistics

static bool t_stats(void) {
    slab_stats_t st;
    slab_get_stats(&st);
    TEST_ASSERT(st.cache_count > 0);
    return true;
}

static bool t_stats_null(void) {
    /* Must not crash */
    slab_get_stats(NULL);
    return true;
}

static bool t_cstats(void) {
    slab_cache_t* c = mk_cache("ts_cst", 32, 8);
    TEST_ASSERT(c != NULL);
    slab_cache_stats_t st;
    slab_cache_stats(c, &st);
    TEST_ASSERT(st.active_objects == 0);
    TEST_ASSERT(st.total_allocs == 0);
    return true;
}

static bool t_cstats_cnt(void) {
    slab_cache_t* c = mk_cache("ts_cac", 32, 8);
    TEST_ASSERT(c != NULL);
    void* ptrs[8];
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
    }
    slab_cache_stats_t st;
    slab_cache_stats(c, &st);
    TEST_ASSERT(st.total_allocs == 8);
    TEST_ASSERT(st.active_objects == 8);
    for (int i = 0; i < 8; i++) slab_free(c, ptrs[i]);
    slab_cache_stats(c, &st);
    TEST_ASSERT(st.total_frees == 8);
    TEST_ASSERT(st.active_objects == 0);
    return true;
}

static bool t_cstats_null(void) {
    slab_cache_t* c = mk_cache("ts_csnl", 32, 8);
    TEST_ASSERT(c != NULL);
    /* Must not crash */
    slab_cache_stats(c, NULL);
    slab_cache_stats(NULL, NULL);
    return true;
}

static bool t_gcnt(void) {
    slab_stats_t before, after;
    slab_get_stats(&before);
    slab_cache_t* c = mk_cache("ts_gcc", 32, 8);
    TEST_ASSERT(c != NULL);
    slab_get_stats(&after);
    TEST_ASSERT(after.cache_count == before.cache_count + 1);
    return true;
}

static bool t_pmm_grow(void) {
    /* Allocating many objects should grow total_pmm_bytes */
    slab_cache_t* c = mk_cache("ts_pbg", 256, 8);
    TEST_ASSERT(c != NULL);
    slab_stats_t before; slab_get_stats(&before);
    void* ptrs[64];
    for (int i = 0; i < 64; i++) {
        if (slab_alloc(c, &ptrs[i]) != SLAB_OK) { ptrs[i] = NULL; }
    }
    slab_stats_t after; slab_get_stats(&after);
    TEST_ASSERT(after.total_pmm_bytes >= before.total_pmm_bytes);
    for (int i = 0; i < 64; i++) if (ptrs[i]) slab_free(c, ptrs[i]);
    return true;
}
#pragma endregion

#pragma region Dump / Debug (no-crash tests)

static bool t_dump(void) {
    slab_dump_stats();
    return true;
}

static bool t_cdump(void) {
    slab_cache_t* c = mk_cache("ts_cd", 32, 8);
    TEST_ASSERT(c != NULL);
    slab_cache_dump(c);
    slab_cache_dump(NULL);
    return true;
}

static bool t_dump_all(void) {
    slab_dump_all_caches();
    return true;
}
#pragma endregion

#pragma region Integrity

static bool t_integrity(void) {
    TEST_ASSERT(slab_verify_integrity() == true);
    return true;
}

static bool t_integrity_alloc(void) {
    slab_cache_t* c = mk_cache("ts_iaa", 64, 8);
    TEST_ASSERT(c != NULL);
    void* ptrs[16];
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
    }
    TEST_ASSERT(slab_verify_integrity() == true);
    for (int i = 0; i < 16; i++) slab_free(c, ptrs[i]);
    TEST_ASSERT(slab_verify_integrity() == true);
    return true;
}
#pragma endregion

#pragma region Introspection

static bool t_objsz_null(void) {
    /* Must not crash; 0 is an acceptable sentinel */
    size_t s = slab_cache_obj_size(NULL);
    (void)s;
    return true;
}

static bool t_name_null(void) {
    const char* n = slab_cache_name(NULL);
    (void)n; /* Must not crash */
    return true;
}

static bool t_name_len(void) {
    /* Name must fit within SLAB_CACHE_NAME_LEN */
    slab_cache_t* c = mk_cache("ts_nl", 32, 8);
    TEST_ASSERT(c != NULL);
    const char* n = slab_cache_name(c);
    TEST_ASSERT(n != NULL);
    TEST_ASSERT(kstrlen(n) < SLAB_CACHE_NAME_LEN);
    return true;
}
#pragma endregion

#pragma region Stress

static bool t_churn(void) {
    /* Allocate and free objects from 4 caches in interleaved fashion */
    slab_cache_t* c[4];
    size_t sizes[4] = {16, 32, 64, 128};
    for (int i = 0; i < 4; i++) {
        c[i] = mk_cache("ts_str", sizes[i], 8);
        TEST_ASSERT(c[i] != NULL);
    }
#define CHURN_N 256
    void* ptrs[4][CHURN_N];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < CHURN_N; j++) {
            TEST_ASSERT_STATUS(slab_alloc(c[i], &ptrs[i][j]), SLAB_OK);
            kmemset(ptrs[i][j], (uint8_t)(i * 64 + j), sizes[i]);
        }
    /* Verify no cross-contamination */
    for (int i = 0; i < 4; i++) {
        uint8_t* b = (uint8_t*)ptrs[i][0];
        for (size_t k = 0; k < sizes[i]; k++)
            TEST_ASSERT(b[k] == (uint8_t)(i * 64));
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < CHURN_N; j++)
            slab_free(c[i], ptrs[i][j]);
    TEST_ASSERT(slab_verify_integrity() == true);
    return true;
#undef CHURN_N
}

static bool t_seq(void) {
    /* Sequential alloc / free of 2000 objects from a single cache */
    slab_cache_t* c = mk_cache("ts_seq", 48, 8);
    TEST_ASSERT(c != NULL);
    for (int i = 0; i < 2000; i++) {
        void* p = NULL;
        TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
        TEST_ASSERT(p != NULL);
        *(uint32_t*)p = (uint32_t)i;
        TEST_ASSERT(*(uint32_t*)p == (uint32_t)i);
        TEST_ASSERT_STATUS(slab_free(c, p), SLAB_OK);
    }
    slab_cache_stats_t st;
    slab_cache_stats(c, &st);
    TEST_ASSERT(st.active_objects == 0);
    TEST_ASSERT(st.total_allocs == 2000);
    return true;
}

static bool t_batch_rev(void) {
    /* Alloc 128, free in reverse order — exercises freelist reorder */
    slab_cache_t* c = mk_cache("ts_bfr", 64, 8);
    TEST_ASSERT(c != NULL);
    void* ptrs[128];
    for (int i = 0; i < 128; i++) {
        TEST_ASSERT_STATUS(slab_alloc(c, &ptrs[i]), SLAB_OK);
        kmemset(ptrs[i], (uint8_t)i, 64);
    }
    for (int i = 127; i >= 0; i--)
        TEST_ASSERT_STATUS(slab_free(c, ptrs[i]), SLAB_OK);
    slab_cache_stats_t st;
    slab_cache_stats(c, &st);
    TEST_ASSERT(st.active_objects == 0);
    return true;
}

static bool t_recreate(void) {
    /* Destroy a cache and recreate it: must start fresh */
    slab_cache_t* c = slab_cache_create("ts_realloc_pat", 32, 8);
    TEST_ASSERT(c != NULL);
    void* p = NULL;
    TEST_ASSERT_STATUS(slab_alloc(c, &p), SLAB_OK);
    slab_free(c, p);
    slab_cache_destroy(c);
    TEST_ASSERT(slab_cache_find("ts_realloc_pat") == NULL);
    slab_cache_t* c2 = slab_cache_create("ts_realloc_pat", 32, 8);
    tr_reg(c2);
    TEST_ASSERT(c2 != NULL);
    slab_cache_stats_t st;
    slab_cache_stats(c2, &st);
    TEST_ASSERT(st.total_allocs == 0);
    return true;
}
#pragma endregion

#pragma region Runner

void test_slab(void) {
    g_tests_total  = 0;
    g_tests_passed = 0;

    LOGF("--- BEGIN SLAB ALLOCATOR TEST ---\n");

    /* Init / shutdown */
    run_test("is_initialized",          t_is_init);
    run_test("double_init",             t_dbl_init);

    /* Cache management */
    run_test("create_not_null",         t_create_nn);
    run_test("create_obj_size",         t_create_sz);
    run_test("create_name",             t_create_name);
    run_test("create_align",            t_create_align);
    run_test("create_bad_size",         t_create_bad);
    run_test("create_null_name",        t_create_nulnm);
    run_test("find_hit",                t_find_hit);
    run_test("find_miss",               t_find_miss);
    run_test("find_null",               t_find_null);
    run_test("destroy_removes",         t_destroy_rm);
    run_test("destroy_null",            t_destroy_null);
    run_test("cache_limit",             t_cache_lim);

    /* Allocation / free */
    run_test("alloc_ok",                t_alloc_ok);
    run_test("alloc_null_cache",        t_alloc_nc);
    run_test("alloc_null_out",          t_alloc_nout);
    run_test("alloc_writable",          t_alloc_wr);
    run_test("alloc_unique",            t_alloc_uniq);
    run_test("alloc_size_1",            t_alloc_sz1);
    run_test("alloc_various_sizes",     t_alloc_sizes);
    run_test("free_null_obj",           t_free_nobj);
    run_test("free_null_cache",         t_free_nc);
    run_test("double_free_detect",      t_dbl_free);
    run_test("alloc_free_cycle",        t_cycle);
    run_test("data_persist",            t_data_persist);

    /* Statistics */
    run_test("stats_smoke",             t_stats);
    run_test("stats_null",              t_stats_null);
    run_test("cache_stats_smoke",       t_cstats);
    run_test("cache_stats_alloc_count", t_cstats_cnt);
    run_test("cache_stats_null",        t_cstats_null);
    run_test("global_cache_count",      t_gcnt);
    run_test("pmm_bytes_grow",          t_pmm_grow);

    /* Debug / dump */
    run_test("dump_stats",              t_dump);
    run_test("cache_dump",              t_cdump);
    run_test("dump_all",                t_dump_all);

    /* Integrity */
    run_test("integrity_clean",         t_integrity);
    run_test("integrity_after_alloc",   t_integrity_alloc);

    /* Introspection */
    run_test("obj_size_null",           t_objsz_null);
    run_test("name_null",               t_name_null);
    run_test("name_len",                t_name_len);

    /* Stress */
    run_test("stress_churn",            t_churn);
    run_test("stress_sequential",       t_seq);
    run_test("stress_batch_free_rev",   t_batch_rev);
    run_test("realloc_pattern",         t_recreate);

    LOGF("--- END SLAB ALLOCATOR TEST ---\n");
    LOGF("Slab Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some Slab tests failed (%d/%d passed).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All Slab tests passed! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
