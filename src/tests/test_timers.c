/*
 * test_timers.c - Timer Subsystem Validation Suite
 *
 * Verifies hardware timer functionality, calibration accuracy, 
 * polled sleep precision, and interrupt delivery.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static int g_tests_total = 0;
static int g_tests_passed = 0;

#pragma region Core Timer Tests

/*
 * test_hpet_functionality - Verifies HPET counter increment and mapping
 */
static bool test_hpet_functionality(void) {
    if (!hpet_is_available()) {
        LOGF("[INFO] HPET not available, skipping.\n");
        return true;
    }

    uint64_t c1 = hpet_read_counter();
    for (volatile int i = 0; i < 10000; i++);
    uint64_t c2 = hpet_read_counter();

    TEST_ASSERT(c2 > c1);
    return true;
}

/*
 * test_tsc_functionality - Verifies TSC counter increment
 */
static bool test_tsc_functionality(void) {
    uint64_t t1 = tsc_read();
    for (volatile int i = 0; i < 10000; i++);
    uint64_t t2 = tsc_read();

    TEST_ASSERT(t2 > t1);
    return true;
}

/*
 * test_sleep_accuracy - Verifies polled sleep precision across scales
 */
static bool test_sleep_accuracy(void) {
    // Millisecond Scale
    uint64_t start_ms = get_uptime_ms();
    sleep_ms(100);
    uint64_t end_ms = get_uptime_ms();
    uint64_t delta_ms = end_ms - start_ms;
    TEST_ASSERT(delta_ms >= 95 && delta_ms <= 110);

    // Microsecond Scale (High Precision)
    uint64_t start_ns = get_uptime_ns();
    sleep_us(500); // 0.5ms
    uint64_t end_ns = get_uptime_ns();
    uint64_t delta_us = (end_ns - start_ns) / 1000;
    TEST_ASSERT(delta_us >= 450 && delta_us <= 600);

    // Zero/Boundary Check
    sleep_ms(0);
    sleep_us(0);
    
    return true;
}

/*
 * test_uptime_monotonicity - Ensures uptime never regresses or stalls
 */
static bool test_uptime_monotonicity(void) {
    uint64_t last_ns = get_uptime_ns();
    
    for (int i = 0; i < 1000; i++) {
        uint64_t current_ns = get_uptime_ns();
        TEST_ASSERT(current_ns >= last_ns);
        last_ns = current_ns;
    }
    
    return true;
}

static volatile uint32_t g_test_irq_count = 0;

/*
 * test_irq_handler - Internal handler for delivery validation
 */
static void test_irq_handler(cpu_context_t* ctx) {
    (void)ctx;
    g_test_irq_count++;
}

/*
 * test_lapic_timer_oneshot - Validates single interrupt delivery
 */
static bool test_lapic_timer_oneshot(void) {
    const uint8_t vector = 0xE0;
    g_test_irq_count = 0;

    register_interrupt_handler(vector, test_irq_handler);
    lapic_timer_oneshot(5000, vector); // 5ms

    // Timeout after 50ms
    for (int i = 0; i < 50 && g_test_irq_count == 0; i++) {
        sleep_ms(1);
    }

    unregister_interrupt_handler(vector);
    TEST_ASSERT(g_test_irq_count == 1);

    return true;
}

/*
 * test_lapic_timer_periodic - Validates periodic interrupt cadence
 */
static bool test_lapic_timer_periodic(void) {
    const uint8_t vector = 0xE1;
    g_test_irq_count = 0;

    register_interrupt_handler(vector, test_irq_handler);
    lapic_timer_periodic(10000, vector); // 10ms period

    // Wait for 105ms (should see ~10 interrupts)
    sleep_ms(105);

    lapic_timer_stop();
    unregister_interrupt_handler(vector);

    uint32_t final_count = g_test_irq_count;
    LOGF("[INFO] Periodic count (105ms): %u\n", final_count);
    
    // Expect 10 interrupts, allow margin for VM scheduling jitter
    TEST_ASSERT(final_count >= 9 && final_count <= 12);

    return true;
}

/*
 * test_drift_accumulation - Checks for cumulative error over many short sleeps
 */
static bool test_drift_accumulation(void) {
    uint64_t start_ms = get_uptime_ms();
    
    // 50 x 2ms sleeps = 100ms total
    for (int i = 0; i < 50; i++) {
        sleep_ms(2);
    }
    
    uint64_t end_ms = get_uptime_ms();
    uint64_t total_delta = end_ms - start_ms;
    
    LOGF("[INFO] Cumulative delta (50x2ms): %lu ms\n", total_delta);
    
    // Cumulative drift should be managed (VM context switches might bloat this)
    TEST_ASSERT(total_delta >= 100 && total_delta <= 150);
    
    return true;
}

#pragma endregion

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

void test_timers(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN TIMER SUBSYSTEM TEST ---\n");

    run_test("HPET Functionality", test_hpet_functionality);
    run_test("TSC Functionality", test_tsc_functionality);
    run_test("Sleep Accuracy", test_sleep_accuracy);
    run_test("Uptime Monotonicity", test_uptime_monotonicity);
    run_test("LAPIC Timer One-Shot", test_lapic_timer_oneshot);
    run_test("LAPIC Timer Periodic", test_lapic_timer_periodic);
    run_test("Drift Accumulation", test_drift_accumulation);

    LOGF("--- END TIMER SUBSYSTEM TEST ---\n");
    LOGF("Timer Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <kernel/drivers/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[-] Some timer tests failed (%d/%d). Check debug log.\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        printf("[+] All timer tests passed successfully! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}

#pragma endregion

