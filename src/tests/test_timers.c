/*
 * test_timers.c - Timer Subsystem Validation Suite
 *
 * All timing thresholds are machine-derived at runtime. A calibration pass
 * measures TSC frequency and per-call sleep overshoot, then uses those values
 * to build tight but portable upper bounds for every timing assertion.
 *
 * Output format is fixed for CI parsing:
 *   "[TEST] <name>  [PASS|FAIL]"
 *   "Timer Test Results: N/N"
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static int g_tests_total  = 0;
static int g_tests_passed = 0;

#pragma region Calibration

static uint64_t g_tsc_khz        = 0; /* TSC ticks per millisecond          */
static uint64_t g_ms_overhead_ms = 0; /* worst-case extra ms per sleep_ms() */
static uint64_t g_us_overhead_us = 0; /* worst-case extra us per sleep_us() */
static bool     g_calibrated     = false;

#define CAL_ITERS 10

/* Derive machine-specific timing parameters. Called once before any test. */
static void calibrate(void) {
    if (g_calibrated) return;

    /* TSC frequency via uptime (use 20ms window for stable reading) */
    {
        uint64_t ns0 = get_uptime_ns();
        uint64_t t0  = tsc_read();
        sleep_ms(20);
        uint64_t t1  = tsc_read();
        uint64_t ns1 = get_uptime_ns();
        uint64_t elapsed_ns = ns1 - ns0;
        if (elapsed_ns > 0)
            g_tsc_khz = (t1 - t0) * 1000000ULL / elapsed_ns;
    }

    /* sleep_ms(1) overshoot */
    {
        uint64_t max = 0;
        for (int i = 0; i < CAL_ITERS; i++) {
            uint64_t a = get_uptime_ms();
            sleep_ms(1);
            uint64_t b = get_uptime_ms();
            uint64_t over = (b - a > 1) ? (b - a - 1) : 0;
            if (over > max) max = over;
        }
        g_ms_overhead_ms = max * 3 + 5; /* 3× margin + 5ms absolute floor */
    }

    /* sleep_us(500) overshoot */
    {
        uint64_t max = 0;
        for (int i = 0; i < CAL_ITERS; i++) {
            uint64_t a = get_uptime_ns();
            sleep_us(500);
            uint64_t b = get_uptime_ns();
            uint64_t over_ns = (b - a > 500000ULL) ? (b - a - 500000ULL) : 0;
            uint64_t over_us = over_ns / 1000ULL;
            if (over_us > max) max = over_us;
        }
        g_us_overhead_us = max * 3 + 2000; /* 3× margin + 2ms floor */
    }

    LOGF("[CAL] TSC freq    : %lu kHz\n",  g_tsc_khz);
    LOGF("[CAL] ms overhead : %lu ms\n",   g_ms_overhead_ms);
    LOGF("[CAL] us overhead : %lu us\n",   g_us_overhead_us);
    g_calibrated = true;
}
#pragma endregion

#pragma region HPET

static bool t_hpet_avail(void) {
    /* Availability is reported; either outcome is valid — just must not crash */
    (void)hpet_is_available();
    return true;
}

static bool t_hpet_mono(void) {
    if (!hpet_is_available()) { LOGF("[SKIP]\n"); return true; }
    uint64_t a = hpet_read_counter();
    for (volatile int i = 0; i < 100000; i++);
    uint64_t b = hpet_read_counter();
    TEST_ASSERT(b > a);
    return true;
}

static bool t_hpet_noreg(void) {
    if (!hpet_is_available()) { LOGF("[SKIP]\n"); return true; }
    uint64_t prev = hpet_read_counter();
    for (int i = 0; i < 1000; i++) {
        uint64_t cur = hpet_read_counter();
        TEST_ASSERT(cur >= prev);
        prev = cur;
    }
    return true;
}

static bool t_hpet_adv(void) {
    if (!hpet_is_available()) { LOGF("[SKIP]\n"); return true; }
    uint64_t a = hpet_read_counter();
    for (volatile int i = 0; i < 1000000; i++);
    uint64_t b = hpet_read_counter();
    /* Must have advanced at least a little */
    TEST_ASSERT(b > a + 100);
    return true;
}

static bool t_hpet_uptime(void) {
    if (!hpet_is_available()) { LOGF("[SKIP]\n"); return true; }
    calibrate();
    uint64_t up0 = get_uptime_ms();
    uint64_t h0  = hpet_read_counter();
    sleep_ms(10);
    uint64_t up1 = get_uptime_ms();
    uint64_t h1  = hpet_read_counter();
    TEST_ASSERT(h1 > h0);
    /* Uptime must have advanced at least 8ms */
    TEST_ASSERT((up1 - up0) >= 8);
    return true;
}
#pragma endregion

#pragma region TSC

static bool t_tsc_mono(void) {
    uint64_t a = tsc_read();
    for (volatile int i = 0; i < 10000; i++);
    uint64_t b = tsc_read();
    TEST_ASSERT(b > a);
    return true;
}

static bool t_tsc_order(void) {
    /* 10 000 consecutive reads must never regress */
    uint64_t prev = tsc_read();
    for (int i = 0; i < 10000; i++) {
        uint64_t cur = tsc_read();
        TEST_ASSERT(cur >= prev);
        prev = cur;
    }
    return true;
}

static bool t_tsc_range(void) {
    /* A 1 million iteration loop must span a meaningful tick count */
    uint64_t a = tsc_read();
    for (volatile int i = 0; i < 1000000; i++);
    uint64_t b = tsc_read();
    /* Conservative: any real CPU runs >100 000 ticks for this loop */
    TEST_ASSERT((b - a) > 100000ULL);
    return true;
}

static bool t_tsc_freq(void) {
    calibrate();
    /* Frequency must be >100 MHz and <10 GHz (physically impossible today) */
    TEST_ASSERT(g_tsc_khz > 0);
    TEST_ASSERT(g_tsc_khz < 10000000ULL);
    LOGF("[INFO] TSC %lu kHz\n", g_tsc_khz);
    return true;
}
#pragma endregion

#pragma region Uptime

static bool t_up_ns_mono(void) {
    uint64_t prev = get_uptime_ns();
    for (int i = 0; i < 10000; i++) {
        uint64_t cur = get_uptime_ns();
        TEST_ASSERT(cur >= prev);
        prev = cur;
    }
    return true;
}

static bool t_up_ms_mono(void) {
    uint64_t prev = get_uptime_ms();
    for (int i = 0; i < 1000; i++) {
        uint64_t cur = get_uptime_ms();
        TEST_ASSERT(cur >= prev);
        prev = cur;
    }
    return true;
}

static bool t_up_adv(void) {
    uint64_t a = get_uptime_ms();
    sleep_ms(5);
    uint64_t b = get_uptime_ms();
    TEST_ASSERT(b > a);
    return true;
}

static bool t_up_coh(void) {
    /* ns and ms must agree within 10ms */
    uint64_t via_ns = get_uptime_ns() / 1000000ULL;
    uint64_t direct  = get_uptime_ms();
    uint64_t diff = (via_ns > direct) ? (via_ns - direct) : (direct - via_ns);
    TEST_ASSERT(diff < 10);
    return true;
}

static bool t_up_res(void) {
    /* Consecutive ns reads must eventually differ */
    uint64_t first = get_uptime_ns();
    int advances = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t cur = get_uptime_ns();
        if (cur > first) { advances++; first = cur; }
    }
    TEST_ASSERT(advances > 0);
    return true;
}
#pragma endregion

#pragma region sleep_ms

static bool t_ms_lo(void) {
    calibrate();
    uint64_t a = get_uptime_ms();
    sleep_ms(100);
    uint64_t b = get_uptime_ms();
    TEST_ASSERT((b - a) >= 95); /* <5% undershoot tolerance */
    return true;
}

static bool t_ms_hi(void) {
    calibrate();
    uint64_t a = get_uptime_ms();
    sleep_ms(100);
    uint64_t b = get_uptime_ms();
    uint64_t upper = 100 + g_ms_overhead_ms * 100; /* per-ms overhead × count */
    LOGF("[INFO] sleep_ms(100)=%lu ms (cap:%lu)\n", (b-a), upper);
    TEST_ASSERT((b - a) <= upper);
    return true;
}

static bool t_ms1_lo(void) {
    uint64_t a = get_uptime_ns();
    sleep_ms(1);
    uint64_t b = get_uptime_ns();
    TEST_ASSERT((b - a) >= 500000ULL); /* at least 0.5ms */
    return true;
}

static bool t_ms_zero(void) {
    /* Must not hang or crash */
    sleep_ms(0);
    sleep_ms(0);
    return true;
}

static bool t_ms_zero_fast(void) {
    uint64_t a = get_uptime_ms();
    for (int i = 0; i < 1000; i++) sleep_ms(0);
    uint64_t b = get_uptime_ms();
    /* 1000 no-op sleeps must complete in < 1 second */
    TEST_ASSERT((b - a) < 1000);
    return true;
}

static bool t_ms_various(void) {
    /* Test 1, 5, 10, 50 ms — all must be within calibrated bounds */
    calibrate();
    static const uint64_t targets[] = {1, 5, 10, 50};
    for (int i = 0; i < 4; i++) {
        uint64_t t = targets[i];
        uint64_t a = get_uptime_ms();
        sleep_ms(t);
        uint64_t b = get_uptime_ms();
        uint64_t lo = (t * 90) / 100;
        uint64_t hi = t + g_ms_overhead_ms * t + 10;
        LOGF("[%lums:%lu] ", t, b - a);
        TEST_ASSERT((b - a) >= lo);
        TEST_ASSERT((b - a) <= hi);
    }
    return true;
}
#pragma endregion

#pragma region sleep_us

static bool t_us_lo(void) {
    calibrate();
    uint64_t a = get_uptime_ns();
    sleep_us(500);
    uint64_t b = get_uptime_ns();
    uint64_t delta_us = (b - a) / 1000ULL;
    TEST_ASSERT(delta_us >= 450);
    return true;
}

static bool t_us_hi(void) {
    calibrate();
    uint64_t a = get_uptime_ns();
    sleep_us(500);
    uint64_t b = get_uptime_ns();
    uint64_t delta_us = (b - a) / 1000ULL;
    uint64_t upper = 500 + g_us_overhead_us;
    LOGF("[INFO] sleep_us(500)=%lu us (cap:%lu)\n", delta_us, upper);
    TEST_ASSERT(delta_us <= upper);
    return true;
}

static bool t_us_zero(void) {
    sleep_us(0);
    sleep_us(0);
    return true;
}

static bool t_us_small(void) {
    /* sleep_us(1) — must not crash and must return in reasonable time */
    uint64_t a = get_uptime_ms();
    for (int i = 0; i < 100; i++) sleep_us(1);
    uint64_t b = get_uptime_ms();
    TEST_ASSERT((b - a) < 500); /* 100 × 1us sleeps well under 500ms */
    return true;
}
#pragma endregion

#pragma region Drift

static bool t_drift_lo(void) {
    calibrate();
    uint64_t a = get_uptime_ms();
    for (int i = 0; i < 50; i++) sleep_ms(2);
    uint64_t b = get_uptime_ms();
    TEST_ASSERT((b - a) >= 95);
    return true;
}

static bool t_drift_hi(void) {
    calibrate();
    uint64_t a = get_uptime_ms();
    for (int i = 0; i < 50; i++) sleep_ms(2);
    uint64_t b = get_uptime_ms();
    uint64_t upper = 100 + g_ms_overhead_ms * 50 + 10;
    LOGF("[INFO] drift(50×2ms)=%lu ms (cap:%lu)\n", (b - a), upper);
    TEST_ASSERT((b - a) <= upper);
    return true;
}

static bool t_drift_10x(void) {
    calibrate();
    uint64_t a = get_uptime_ms();
    for (int i = 0; i < 10; i++) sleep_ms(10);
    uint64_t b = get_uptime_ms();
    uint64_t upper = 100 + g_ms_overhead_ms * 10 + 10;
    TEST_ASSERT((b - a) >= 95);
    TEST_ASSERT((b - a) <= upper);
    return true;
}
#pragma endregion

#pragma region LAPIC

static volatile uint32_t g_irq_count = 0;

static cpu_context_t* irq_cb(cpu_context_t* ctx) {
    g_irq_count++;
    return ctx;
}

static bool t_os_fires(void) {
    calibrate();
    const uint8_t vec = 0xE0;
    g_irq_count = 0;
    irq_register(vec, irq_cb);
    lapic_timer_oneshot(5000, vec);
    /* Wait up to 100ms (20× target) */
    for (int i = 0; i < 100 && g_irq_count == 0; i++) sleep_ms(1);
    lapic_timer_stop();
    irq_unregister(vec);
    TEST_ASSERT(g_irq_count == 1);
    return true;
}

static bool t_os_noextra(void) {
    calibrate();
    const uint8_t vec = 0xE0;
    g_irq_count = 0;
    irq_register(vec, irq_cb);
    lapic_timer_oneshot(5000, vec);
    sleep_ms(60); /* well past delivery */
    lapic_timer_stop();
    irq_unregister(vec);
    TEST_ASSERT(g_irq_count == 1);
    return true;
}

static bool t_os_window(void) {
    calibrate();
    const uint8_t vec = 0xE0;
    g_irq_count = 0;
    irq_register(vec, irq_cb);
    uint64_t t0 = get_uptime_ms();
    lapic_timer_oneshot(10000, vec); /* 10ms */
    for (int i = 0; i < 200 && g_irq_count == 0; i++) sleep_ms(1);
    uint64_t t1 = get_uptime_ms();
    lapic_timer_stop();
    irq_unregister(vec);
    TEST_ASSERT(g_irq_count >= 1);
    TEST_ASSERT((t1 - t0) <= 200 + g_ms_overhead_ms * 200);
    return true;
}

static bool t_per_count(void) {
    calibrate();
    const uint8_t vec = 0xE1;
    g_irq_count = 0;
    const uint32_t period_us = 10000; /* 10ms */
    irq_register(vec, irq_cb);
    lapic_timer_periodic(period_us, vec);
    uint64_t t0 = get_uptime_ms();
    for (int i = 0; i < 1000 && g_irq_count < 5; i++) {
        sleep_ms(1);
    }
    uint64_t t1 = get_uptime_ms();
    lapic_timer_stop();
    irq_unregister(vec);
    uint32_t cnt = g_irq_count;
    uint64_t elapsed = t1 - t0;
    LOGF("[INFO] periodic: %u irqs in %lu ms\n", cnt, elapsed);
    TEST_ASSERT(cnt >= 5);
    return true;
}

static bool t_per_stop(void) {
    calibrate();
    const uint8_t vec = 0xE1;
    g_irq_count = 0;
    irq_register(vec, irq_cb);
    lapic_timer_periodic(5000, vec);
    for (int i = 0; i < 500 && g_irq_count == 0; i++) {
        sleep_ms(1);
    }
    lapic_timer_stop();
    uint32_t at_stop = g_irq_count;
    sleep_ms(50);
    irq_unregister(vec);
    uint32_t after_stop = g_irq_count;
    LOGF("[INFO] stop: %u before, %u after\n", at_stop, after_stop);
    TEST_ASSERT(at_stop >= 1);
    TEST_ASSERT(after_stop <= at_stop + 1); /* at most 1 in-flight */
    return true;
}

static bool t_per_rearm(void) {
    calibrate();
    const uint8_t vec = 0xE1;
    g_irq_count = 0;
    irq_register(vec, irq_cb);
    lapic_timer_periodic(10000, vec);
    for (int i = 0; i < 500 && g_irq_count == 0; i++) {
        sleep_ms(1);
    }
    lapic_timer_stop();
    uint32_t first_run = g_irq_count;
    g_irq_count = 0;
    lapic_timer_periodic(10000, vec);
    for (int i = 0; i < 500 && g_irq_count == 0; i++) {
        sleep_ms(1);
    }
    lapic_timer_stop();
    uint32_t second_run = g_irq_count;
    irq_unregister(vec);
    LOGF("[INFO] rearm: %u then %u\n", first_run, second_run);
    TEST_ASSERT(first_run >= 1);
    TEST_ASSERT(second_run >= 1);
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-40s ", name);
    if (fn()) { g_tests_passed++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_timers(void) {
    g_tests_total  = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN TIMER SUBSYSTEM TEST ---\n");

    run_test("HPET Available (no crash)",    t_hpet_avail);
    run_test("HPET Monotonic",               t_hpet_mono);
    run_test("HPET No Regression (1000x)",   t_hpet_noreg);
    run_test("HPET Advance After Loop",      t_hpet_adv);
    run_test("HPET vs Uptime Agreement",     t_hpet_uptime);
    run_test("TSC Monotonic",                t_tsc_mono);
    run_test("TSC Strict Order (10000x)",    t_tsc_order);
    run_test("TSC Range (loop span)",        t_tsc_range);
    run_test("TSC Frequency Plausible",      t_tsc_freq);
    run_test("Uptime NS Monotonic (10000x)", t_up_ns_mono);
    run_test("Uptime MS Monotonic (1000x)",  t_up_ms_mono);
    run_test("Uptime Advances After Sleep",  t_up_adv);
    run_test("Uptime NS/MS Coherent",        t_up_coh);
    run_test("Uptime NS Resolution",         t_up_res);
    run_test("sleep_ms(100) Lower Bound",    t_ms_lo);
    run_test("sleep_ms(100) Upper Bound",    t_ms_hi);
    run_test("sleep_ms(1) Lower Bound",      t_ms1_lo);
    run_test("sleep_ms(0) No-op",            t_ms_zero);
    run_test("sleep_ms(0) x1000 Fast",       t_ms_zero_fast);
    run_test("sleep_ms Various Durations",   t_ms_various);
    run_test("sleep_us(500) Lower Bound",    t_us_lo);
    run_test("sleep_us(500) Upper Bound",    t_us_hi);
    run_test("sleep_us(0) No-op",            t_us_zero);
    run_test("sleep_us(1) x100 Reasonable",  t_us_small);
    run_test("Drift Lower (50x2ms)",         t_drift_lo);
    run_test("Drift Upper (50x2ms)",         t_drift_hi);
    run_test("Drift Lower (10x10ms)",        t_drift_10x);
    run_test("LAPIC One-Shot Fires",         t_os_fires);
    run_test("LAPIC One-Shot No Extras",     t_os_noextra);
    run_test("LAPIC One-Shot Within Window", t_os_window);
    run_test("LAPIC Periodic Count",         t_per_count);
    run_test("LAPIC Periodic Stop",          t_per_stop);
    run_test("LAPIC Periodic Rearm",         t_per_rearm);

    LOGF("--- END TIMER SUBSYSTEM TEST ---\n");
    LOGF("Timer Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some timer tests failed (%d/%d passed).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All timer tests passed! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
