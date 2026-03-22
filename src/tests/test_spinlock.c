/*
 * test_spinlock.c - Spinlock Validation Suite
 *
 * Tests every public spinlock function: init, acquire, try_acquire, release,
 * is_locked. Covers IRQ state saving/restoring, independence of multiple locks,
 * sequential reuse, try-fail semantics, and high-frequency churn stability.
 */

#include <kernel/sys/spinlock.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <stdbool.h>
#include <stdint.h>

static int ntests  = 0;
static int npass = 0;

/* Read RFLAGS.IF */
static inline bool irq_on(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    return (f >> 9) & 1;
}

#pragma region Init

static bool t_init_zero(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_zero");
    TEST_ASSERT(lk.locked == 0);
    return true;
}

static bool t_init_name(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_name");
    TEST_ASSERT(lk.name != NULL);
    return true;
}

static bool t_init_free(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_nolocked");
    TEST_ASSERT(!spinlock_is_locked(&lk));
    return true;
}
#pragma endregion

#pragma region Acquire / Release

static bool t_acq_sets(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_acq");
    bool f = spinlock_acquire(&lk);
    TEST_ASSERT(spinlock_is_locked(&lk));
    spinlock_release(&lk, f);
    return true;
}

static bool t_rel_clears(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_rel");
    bool f = spinlock_acquire(&lk);
    spinlock_release(&lk, f);
    TEST_ASSERT(!spinlock_is_locked(&lk));
    TEST_ASSERT(lk.locked == 0);
    return true;
}

static bool t_is_locked(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_islock");
    TEST_ASSERT(!spinlock_is_locked(&lk));
    bool f = spinlock_acquire(&lk);
    TEST_ASSERT(spinlock_is_locked(&lk));
    spinlock_release(&lk, f);
    TEST_ASSERT(!spinlock_is_locked(&lk));
    return true;
}
#pragma endregion

#pragma region IRQ State

static bool t_irq_acq_en(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_irq_en");
    enable_interrupts();
    bool f = spinlock_acquire(&lk);
    TEST_ASSERT(f == true);
    TEST_ASSERT(!irq_on()); /* IRQs must be off while held */
    spinlock_release(&lk, f);
    return true;
}

static bool t_irq_acq_dis(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_irq_dis");
    disable_interrupts();
    bool f = spinlock_acquire(&lk);
    TEST_ASSERT(f == false);
    TEST_ASSERT(!irq_on());
    spinlock_release(&lk, f);
    enable_interrupts();
    return true;
}

static bool t_irq_rel_on(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_irq_ron");
    enable_interrupts();
    bool f = spinlock_acquire(&lk);
    spinlock_release(&lk, f);
    TEST_ASSERT(irq_on()); /* Must be re-enabled */
    return true;
}

static bool t_irq_rel_off(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_irq_roff");
    disable_interrupts();
    bool f = spinlock_acquire(&lk);
    spinlock_release(&lk, f);
    TEST_ASSERT(!irq_on()); /* Must stay disabled */
    enable_interrupts();
    return true;
}

static bool t_irq_alt(void) {
    /* Enabled → acquire → release → disabled → acquire → release × 5 */
    spinlock_t lk;
    spinlock_init(&lk, "t_alt");
    for (int i = 0; i < 5; i++) {
        enable_interrupts();
        bool f1 = spinlock_acquire(&lk);
        TEST_ASSERT(f1 == true && !irq_on());
        spinlock_release(&lk, f1);
        TEST_ASSERT(irq_on());

        disable_interrupts();
        bool f2 = spinlock_acquire(&lk);
        TEST_ASSERT(f2 == false && !irq_on());
        spinlock_release(&lk, f2);
        TEST_ASSERT(!irq_on());
    }
    enable_interrupts();
    return true;
}
#pragma endregion

#pragma region try_acquire

static bool t_try_free(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_try_free");
    bool was = false;
    bool got = spinlock_try_acquire(&lk, &was);
    TEST_ASSERT(got == true);
    TEST_ASSERT(spinlock_is_locked(&lk));
    spinlock_release(&lk, was);
    TEST_ASSERT(!spinlock_is_locked(&lk));
    return true;
}

static bool t_try_held(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_try_held");
    enable_interrupts();
    bool f = spinlock_acquire(&lk);
    bool was = false;
    bool got = spinlock_try_acquire(&lk, &was);
    TEST_ASSERT(got == false);
    TEST_ASSERT(spinlock_is_locked(&lk));
    spinlock_release(&lk, f);
    return true;
}

static bool t_try_was_on(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_try_wet");
    enable_interrupts();
    bool was = false;
    bool got = spinlock_try_acquire(&lk, &was);
    TEST_ASSERT(got == true);
    TEST_ASSERT(was == true);
    spinlock_release(&lk, was);
    return true;
}

static bool t_try_was_off(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_try_wef");
    disable_interrupts();
    bool was = true;
    bool got = spinlock_try_acquire(&lk, &was);
    TEST_ASSERT(got == true);
    TEST_ASSERT(was == false);
    spinlock_release(&lk, was);
    enable_interrupts();
    return true;
}

static bool t_try_fail_irq(void) {
    /* Failed try must not corrupt IRQ state */
    spinlock_t lk;
    spinlock_init(&lk, "t_try_nirq");
    enable_interrupts();
    bool f = spinlock_acquire(&lk); /* disables IRQ */
    bool was = false;
    bool got = spinlock_try_acquire(&lk, &was);
    TEST_ASSERT(got == false);
    TEST_ASSERT(!irq_on()); /* still inside critical section */
    spinlock_release(&lk, f);
    TEST_ASSERT(irq_on());
    return true;
}

static bool t_try_retry(void) {
    /* try fails → release → try succeeds */
    spinlock_t lk;
    spinlock_init(&lk, "t_try_retry");
    enable_interrupts();
    bool f = spinlock_acquire(&lk);
    bool was = false;
    TEST_ASSERT(!spinlock_try_acquire(&lk, &was));
    spinlock_release(&lk, f);
    TEST_ASSERT(spinlock_try_acquire(&lk, &was));
    spinlock_release(&lk, was);
    TEST_ASSERT(!spinlock_is_locked(&lk));
    return true;
}
#pragma endregion

#pragma region Multiple Locks

static bool t_two_ind(void) {
    spinlock_t a, b;
    spinlock_init(&a, "t_2a"); spinlock_init(&b, "t_2b");
    bool fa = spinlock_acquire(&a);
    TEST_ASSERT(spinlock_is_locked(&a) && !spinlock_is_locked(&b));
    bool fb = spinlock_acquire(&b);
    TEST_ASSERT(spinlock_is_locked(&a) && spinlock_is_locked(&b));
    spinlock_release(&b, fb);
    TEST_ASSERT(spinlock_is_locked(&a) && !spinlock_is_locked(&b));
    spinlock_release(&a, fa);
    TEST_ASSERT(!spinlock_is_locked(&a) && !spinlock_is_locked(&b));
    return true;
}

static bool t_three_ind(void) {
    spinlock_t a, b, c;
    spinlock_init(&a, "t_3a"); spinlock_init(&b, "t_3b"); spinlock_init(&c, "t_3c");
    bool fa = spinlock_acquire(&a);
    bool fb = spinlock_acquire(&b);
    TEST_ASSERT(!spinlock_is_locked(&c));
    bool fc = spinlock_acquire(&c);
    TEST_ASSERT(spinlock_is_locked(&a) && spinlock_is_locked(&b) && spinlock_is_locked(&c));
    spinlock_release(&b, fb);
    TEST_ASSERT(!spinlock_is_locked(&b));
    spinlock_release(&c, fc);
    spinlock_release(&a, fa);
    TEST_ASSERT(!spinlock_is_locked(&a) && !spinlock_is_locked(&c));
    return true;
}
#pragma endregion

#pragma region Sequential Reuse

static bool t_seq_100(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_seq100");
    enable_interrupts();
    for (int i = 0; i < 100; i++) {
        bool f = spinlock_acquire(&lk);
        TEST_ASSERT(spinlock_is_locked(&lk));
        spinlock_release(&lk, f);
        TEST_ASSERT(!spinlock_is_locked(&lk));
    }
    return true;
}

static bool t_seq_1k(void) {
    spinlock_t lk;
    spinlock_init(&lk, "t_seq1k");
    enable_interrupts();
    for (int i = 0; i < 1000; i++) {
        bool f = spinlock_acquire(&lk);
        spinlock_release(&lk, f);
    }
    TEST_ASSERT(!spinlock_is_locked(&lk));
    return true;
}

static bool t_seq_irq(void) {
    /* Verify IRQ state is consistent across 100 sequential acquires */
    spinlock_t lk;
    spinlock_init(&lk, "t_seqirq");
    enable_interrupts();
    for (int i = 0; i < 100; i++) {
        bool f = spinlock_acquire(&lk);
        TEST_ASSERT(!irq_on());
        spinlock_release(&lk, f);
        TEST_ASSERT(irq_on());
    }
    return true;
}
#pragma endregion

#pragma region Churn Stress

static bool t_churn_8x5k(void) {
    #define CL 8
    #define CI 5000
    spinlock_t locks[CL];
    bool flags[CL]; bool held[CL];
    for (int i = 0; i < CL; i++) { spinlock_init(&locks[i], "churn"); held[i] = false; flags[i] = false; }
    uint32_t seed = 0xDEAD1234u;
    for (int it = 0; it < CI; it++) {
        seed = seed * 1664525u + 1013904223u;
        int idx = (int)((seed >> 16) % CL);
        if (!held[idx]) { flags[idx] = spinlock_acquire(&locks[idx]); held[idx] = true; }
        else            { spinlock_release(&locks[idx], flags[idx]);   held[idx] = false; }
    }
    for (int i = 0; i < CL; i++) if (held[i]) spinlock_release(&locks[i], flags[i]);
    for (int i = 0; i < CL; i++) {
        if (spinlock_is_locked(&locks[i])) {
            LOGF("[FAIL] lock %d still held post-churn ", i);
            return false;
        }
    }
    return true;
}

static bool t_churn_free(void) {
    /* Acquire all, release all, verify none locked */
    #define CA 16
    spinlock_t locks[CA];
    bool flags[CA];
    for (int i = 0; i < CA; i++) { spinlock_init(&locks[i], "all"); flags[i] = false; }
    for (int i = 0; i < CA; i++) flags[i] = spinlock_acquire(&locks[i]);
    for (int i = CA - 1; i >= 0; i--) spinlock_release(&locks[i], flags[i]);
    for (int i = 0; i < CA; i++) TEST_ASSERT(!spinlock_is_locked(&locks[i]));
    return true;
}

static bool t_try_churn(void) {
    /* Mix try_acquire and acquire on the same lock */
    spinlock_t lk;
    spinlock_init(&lk, "t_trychurn");
    enable_interrupts();
    for (int i = 0; i < 500; i++) {
        bool was = false;
        if (spinlock_try_acquire(&lk, &was)) {
            TEST_ASSERT(spinlock_is_locked(&lk));
            spinlock_release(&lk, was);
        }
        /* If try failed it means something is wrong (single-threaded) */
        else {
            LOGF("[FAIL] try_acquire failed in single-threaded churn iter %d ", i);
            return false;
        }
    }
    TEST_ASSERT(!spinlock_is_locked(&lk));
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    ntests++;
    LOGF("[TEST] %-40s ", name);
    if (fn()) { npass++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_spinlock(void) {
    ntests  = 0;
    npass = 0;

    LOGF("\n--- BEGIN SPINLOCK TEST ---\n");

    run_test("Init: locked==0",              t_init_zero);
    run_test("Init: name set",               t_init_name);
    run_test("Init: is_locked false",        t_init_free);
    run_test("Acquire: sets locked",         t_acq_sets);
    run_test("Release: clears locked",       t_rel_clears);
    run_test("is_locked accuracy",           t_is_locked);
    run_test("IRQ acquire (enabled→off)",    t_irq_acq_en);
    run_test("IRQ acquire (disabled→off)",   t_irq_acq_dis);
    run_test("IRQ release restores ON",      t_irq_rel_on);
    run_test("IRQ release restores OFF",     t_irq_rel_off);
    run_test("IRQ alternating sequence ×5",  t_irq_alt);
    run_test("try_acquire: free succeeds",   t_try_free);
    run_test("try_acquire: held fails",      t_try_held);
    run_test("try_acquire: was_enabled=T",   t_try_was_on);
    run_test("try_acquire: was_enabled=F",   t_try_was_off);
    run_test("try_fail: IRQ unchanged",      t_try_fail_irq);
    run_test("try_fail → release → retry",   t_try_retry);
    run_test("Two locks independent",        t_two_ind);
    run_test("Three locks independent",      t_three_ind);
    run_test("Sequential reuse ×100",        t_seq_100);
    run_test("Sequential reuse ×1000",       t_seq_1k);
    run_test("Sequential IRQ state ×100",    t_seq_irq);
    run_test("Churn: 8 locks × 5000 iters",  t_churn_8x5k);
    run_test("Churn: all 16 free after",     t_churn_free);
    run_test("try_acquire churn ×500",       t_try_churn);

    LOGF("--- END SPINLOCK TEST ---\n");
    LOGF("Spinlock Test Results: %d/%d\n\n", npass, ntests);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (npass != ntests) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some spinlock tests failed (%d/%d passed).\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All spinlock tests passed! (%d/%d)\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
