/*
 * test_spinlock.c - Spinlock Validation Suite
 *
 * Verifies basic mutual exclusion and interrupt-state management.
 *
 * Author: ApparentlyPlus
 */

#include <kernel/sys/spinlock.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/debug.h>
#include <tests/tests.h>

static int g_tests_total = 0;
static int g_tests_passed = 0;

/*
 * test_basic_lock - Verifies simple acquire/release
 */
static bool test_basic_lock(void) {
    spinlock_t lock;
    spinlock_init(&lock, "test_basic");

    TEST_ASSERT(!spinlock_is_locked(&lock));

    bool flags = spinlock_acquire(&lock);
    TEST_ASSERT(spinlock_is_locked(&lock));

    spinlock_release(&lock, flags);
    TEST_ASSERT(!spinlock_is_locked(&lock));

    return true;
}

/*
 * test_interrupt_safety - Verifies interrupt state saving/restoring
 */
static bool test_interrupt_safety(void) {
    spinlock_t lock;
    spinlock_init(&lock, "test_irq");

    // 1. Test with interrupts enabled
    enable_interrupts();
    bool flags1 = spinlock_acquire(&lock);
    TEST_ASSERT(flags1 == true);
    // Interrupts should be disabled now
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    TEST_ASSERT(!((rflags >> 9) & 1));

    spinlock_release(&lock, flags1);
    // Interrupts should be enabled again
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    TEST_ASSERT((rflags >> 9) & 1);

    // 2. Test with interrupts already disabled
    disable_interrupts();
    bool flags2 = spinlock_acquire(&lock);
    TEST_ASSERT(flags2 == false);
    
    spinlock_release(&lock, flags2);
    // Interrupts should still be disabled
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    TEST_ASSERT(!((rflags >> 9) & 1));

    enable_interrupts(); // Restore for other tests
    return true;
}

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
    if (func()) {
        g_tests_passed++;
        LOGF("[PASS]\n");
    } else {
        LOGF("[FAIL]\n");
    }
}

void test_spinlock(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN SPINLOCK TEST ---\n");

    run_test("Basic Acquire/Release", test_basic_lock);
    run_test("Interrupt State Management", test_interrupt_safety);

    LOGF("--- END SPINLOCK TEST ---\n");
    LOGF("Spinlock Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <kernel/drivers/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[-] Some spinlock tests failed (%d/%d).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        printf("[+] Spinlock primitives validated! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}

#pragma endregion
