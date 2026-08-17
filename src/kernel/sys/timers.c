/*
 * timers.c - Kernel Timer Subsystem Implementation
 *
 * This module implements the core timing functionality for GatOS.
 * It handles hardware discovery for PIT and HPET, performs calibration
 * of the Local APIC and TSC, and provides high-level sleep and uptime APIs.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/drivers/serial.h>
#include <kernel/sys/scheduler.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/sys/panic.h>
#include <arch/x86_64/cpu/io.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/acpi.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <klibc/stdio.h>

#pragma region Internal Globals

static hpet_regs_t* hpet = NULL;
static uint32_t hpet_period = 0; // Femtoseconds per tick

static uint64_t tsc_tpm = 0;
static uint64_t boot_tsc = 0;

static volatile uint64_t ticks = 0;

// Set to true when TSC Deadline mode is active (tickless)
// When false, we fall back to a 10ms periodic LAPIC timer for compatibility
static bool tsc_deadline_mode = false;

#pragma endregion

#pragma region PIT Implementation

/*
 * pit_set_oneshot - Sets PIT channel 0 to one-shot mode (Mode 0)
 */
void pit_set_oneshot(uint16_t ticks) {
    // Channel 0, Access Mode lobyte/hibyte, Mode 0 (Interrupt on Terminal Count), Binary
    outb(0x43, 0x30); 
    outb(0x40, (uint8_t)(ticks & 0xFF));
    outb(0x40, (uint8_t)((ticks >> 8) & 0xFF));
}

/*
 * pit_latch - Latches and reads channel 0's current count. The latch command
 * freezes the count so the two byte reads see one consistent value.
 */
static uint16_t pit_latch(void) {
    outb(0x43, 0x00);
    uint8_t low = inb(0x40);
    uint8_t high = inb(0x40);
    return (uint16_t)(((uint16_t)high << 8) | low);
}

/*
 * pit_prepare_sleep - Helper for short delays during early boot
 */
void pit_prepare_sleep(uint32_t ms) {
    uint32_t total_ticks = (PIT_FREQUENCY / 1000) * ms;
    
    // The PIT counter is 16-bit, max 54ms per wrap
    // For longer sleeps, we'd need a loop
    if (total_ticks > 0xFFFF) total_ticks = 0xFFFF;

    pit_set_oneshot((uint16_t)total_ticks);
}

/*
 * pit_wait - Spins until PIT Channel 0 reaches 0
 */
static void pit_wait(void) {
    // In Mode 0, the OUT pin goes high when count reaches 0
    // This is magic numbers galore again, blame x86 hardware design
    uint16_t last_val = 0xFFFF;
    while (1) {
        outb(0x43, 0x00); // Latch channel 0
        uint8_t low = inb(0x40);
        uint8_t high = inb(0x40);
        uint16_t val = (high << 8) | low;
        
        if (val > last_val) break; // Wrapped around
        last_val = val;
    }
}

#pragma endregion

#pragma region HPET Implementation

/*
 * hpet_init - Discovers and initializes the HPET from ACPI
 */
static void hpet_init(void) {
    struct HpetSdt {
        ACPISDTHeader header;
        uint32_t event_timer_block_id;
        uint8_t address_space_id;
        uint8_t register_bit_width;
        uint8_t register_bit_offset;
        uint8_t reserved0;
        uint64_t address;
        uint8_t hpet_number;
        uint16_t minimum_tick;
        uint8_t page_protection;
    } __attribute__((packed))*hpet_table;

    hpet_table = (struct HpetSdt*)acpi_find_table("HPET");
    if (!hpet_table) {
        LOGF("[TIMER] HPET not found in ACPI tables.\n");
        return;
    }

    uint64_t phys_addr = hpet_table->address;
    void* virt_addr = NULL;

    // Map HPET registers
    if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_FOREIGN | VM_FLAG_DEVICE, (void*)phys_addr, &virt_addr) != VMM_OK) {
        LOGF("[TIMER] Failed to map HPET registers.\n");
        return;
    }

    hpet = (hpet_regs_t*)virt_addr;
    hpet_period = hpet->capabilities_high;

    // Enable HPET (Set bit 0 of General Config)
    // Also clear bit 1 (Legacy Replacement) to use it cleanly
    // Jesus christ, HPET, why do you have to be like this
    hpet->configuration |= 0x01;
    hpet->configuration &= ~0x02;

    LOGF("[TIMER] HPET initialized. Period: %u fs (%u MHz)\n", 
         hpet_period, (uint32_t)(FEMTOSECONDS_PER_SECOND / hpet_period / 1000000));
}

/*
 * hpet_is_available - Checks if the HPET has been successfully initialized
 */
bool hpet_is_available(void) {
    return hpet != NULL;
}

/*
 * hpet_read_counter - Reads the current value of the HPET main counter
 */
uint64_t hpet_read_counter(void) {
    if (!hpet) return 0;
    return hpet->main_counter;
}

#pragma endregion

#pragma region Calibration Logic

/*
 * timer_handler - Periodic timer interrupt handler
 */
static cpu_context_t* timer_handler(cpu_context_t* ctx) {
    ticks++;
    
    // Call the scheduler to perform a context switch
    return sched_schedule(ctx);
}

/*
 * tsc_hz_declared - The TSC frequency the hardware reports, or 0 if it does not
 */
static uint64_t tsc_hz_declared(void) {
    uint32_t a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    bool is_amd = (b == 0x68747541); // "Auth"

    if (!is_amd && max_leaf >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);
        if (a != 0 && b != 0 && c != 0) return ((uint64_t)c * b) / a;
    }
    if (!is_amd && max_leaf >= 0x16) {
        cpuid(0x16, 0, &a, &b, &c, &d);
        if ((a & 0xFFFF) != 0) return (uint64_t)(a & 0xFFFF) * 1000000ULL;
    }
    if (is_amd) {
        cpuid(0x80000000, 0, &a, &b, &c, &d);
        if (a >= 0x80000007) {
            cpuid(0x80000007, 0, &a, &b, &c, &d);
            if (d & (1u << 8)) { // invariant TSC
                uint64_t p0 = read_msr(MSR_PSTATE_0);
                uint64_t fid = p0 & 0xFF;
                uint64_t did = (p0 >> 8) & 0x3F;
                if (did != 0 && fid != 0) return (fid * 200000000ULL) / did;
            }
        }
    }
    return 0;
}

/*
 * hpet_counter_advances - True if the main counter is actually running
 */
static bool hpet_counter_advances(void) {
    if (!hpet) return false;

    if (!(hpet->capabilities_low & (1u << 13))) {
        LOGF("[TIMER] HPET main counter is 32-bit, not using it for calibration.\n");
        return false;
    }
    if (hpet_period == 0 || hpet_period > 100000000UL) {
        LOGF("[TIMER] HPET period %u fs is out of spec, not using it.\n", hpet_period);
        return false;
    }

    uint64_t first = hpet_read_counter();
    uint64_t deadline = tsc_read() + 100000;
    while (tsc_read() < deadline) {
        if (hpet_read_counter() != first) return true;
    }
    LOGF("[TIMER] HPET main counter is not advancing; not using it.\n");
    return false;
}

/*
 * calibrate_once - One window against the given reference
 */
static bool calibrate_once(bool use_hpet, uint64_t* out_tsc, uint64_t* out_lapic) {
    lapic_write(LAPIC_TDCR, 0x03);       // Divisor 16
    lapic_write(LAPIC_TICR, 0xFFFFFFFF); // Max count
    uint64_t abort_at = tsc_read() + (uint64_t)CALIBRATE_MS * 100 * CALIBRATE_MIN_TSC;
    bool complete = false;

    uint64_t lapic_start, tsc_start;

    if (use_hpet) {
        uint64_t target = ((uint64_t)CALIBRATE_MS * 1000000000000ULL) / hpet_period;
        uint64_t start = hpet_read_counter();

        lapic_start = lapic_read(LAPIC_TCCR);
        tsc_start = tsc_read();

        while (tsc_read() < abort_at) {
            if (hpet_read_counter() - start >= target) { complete = true; break; }
        }
    } else {
        pit_set_oneshot(0xFFFF);
        uint64_t settle = tsc_read() + 10000;
        while (tsc_read() < settle) { }

        uint16_t start_val = pit_latch();
        uint16_t target = (uint16_t)((PIT_FREQUENCY / 1000) * CALIBRATE_MS);

        lapic_start = lapic_read(LAPIC_TCCR);
        tsc_start = tsc_read();

        while (tsc_read() < abort_at) {
            uint16_t elapsed = (uint16_t)(start_val - pit_latch());
            if (elapsed >= target) { complete = true; break; }
        }
    }

    uint64_t lapic_end = lapic_read(LAPIC_TCCR);
    uint64_t tsc_end = tsc_read();

    if (!complete) {
        LOGF("[TIMER] %s window never completed.\n", use_hpet ? "HPET" : "PIT");
        return false;
    }

    uint64_t dtsc = tsc_end - tsc_start;
    if (dtsc < CALIBRATE_MIN_TSC) {
        LOGF("[TIMER] %s window closed after only %lu TSC ticks; rejecting.\n",
             use_hpet ? "HPET" : "PIT", dtsc);
        return false;
    }

    *out_tsc = dtsc;
    *out_lapic = lapic_start - lapic_end;
    return true;
}

/*
 * median3 - The middle of three, so one bad sample cannot carry the result.
 */
static uint64_t median3(uint64_t x, uint64_t y, uint64_t z) {
    if (x > y) { uint64_t t = x; x = y; y = t; }
    if (y > z) { uint64_t t = y; y = z; z = t; }
    if (x > y) { uint64_t t = x; x = y; y = t; }
    return y;
}

/*
 * timer_calibrate_all - Establishes tsc_tpm and the LAPIC tick rate.
 */
static void timer_calibrate_all(void) {
    LOGF("[TIMER] Calibrating high-precision timers...\n");

    bool use_hpet = hpet_is_available() && hpet_counter_advances();
    const char* src = use_hpet ? "HPET" : "PIT";

    uint64_t tsc_s[3], lapic_s[3];
    int good = 0;
    for (int i = 0; i < 3; i++) {
        uint64_t dt, dl;
        if (calibrate_once(use_hpet, &dt, &dl)) {
            tsc_s[good] = dt;
            lapic_s[good] = dl;
            good++;
        }
    }

    if (good == 0 && use_hpet) {
        LOGF("[TIMER] HPET produced no usable sample; falling back to the PIT.\n");
        use_hpet = false;
        src = "PIT";
        for (int i = 0; i < 3; i++) {
            uint64_t dt, dl;
            if (calibrate_once(false, &dt, &dl)) {
                tsc_s[good] = dt;
                lapic_s[good] = dl;
                good++;
            }
        }
    }

    if (good == 0) {
        panic("timer: no reference clock produced a usable calibration window");
    }

    uint64_t dtsc = (good == 3) ? median3(tsc_s[0], tsc_s[1], tsc_s[2]) : tsc_s[0];
    uint64_t dlapic = (good == 3) ? median3(lapic_s[0], lapic_s[1], lapic_s[2]) : lapic_s[0];

    uint64_t hz = (dtsc / CALIBRATE_MS) * 1000ULL;
    if (hz < TSC_HZ_MIN || hz > TSC_HZ_MAX) {
        LOGF("[TIMER] Calibrated TSC of %lu Hz is not a real frequency.\n", hz);
        panic("timer: TSC calibration produced an impossible frequency");
    }

    tsc_tpm = dtsc / CALIBRATE_MS;
    lapic_set_tpm(dlapic / CALIBRATE_MS);

    LOGF("[TIMER] %s: %d/3 samples, TSC %lu ticks/ms (%lu MHz), LAPIC %lu ticks/ms\n",
         src, good, tsc_tpm, hz / 1000000ULL, dlapic / CALIBRATE_MS);

    uint64_t declared = tsc_hz_declared();
    if (declared >= TSC_HZ_MIN && declared <= TSC_HZ_MAX) {
        uint64_t lo = hz < declared ? hz : declared;
        uint64_t hi = hz < declared ? declared : hz;
        LOGF("[TIMER] Hardware reports %lu MHz; measured %lu MHz.\n",
             declared / 1000000ULL, hz / 1000000ULL);
        if ((hi - lo) * 10 > hi) {
            LOGF("[TIMER] WARNING: measured and reported TSC differ by more than 10%%.\n");
            kprintf("[TIMER] WARNING: TSC calibration disagrees with the hardware "
                    "(%lu MHz measured, %lu MHz reported). Timings are suspect.\n",
                    hz / 1000000ULL, declared / 1000000ULL);
        }
    } else {
        LOGF("[TIMER] Hardware reports no TSC frequency; measurement stands alone.\n");
    }
}

/*
 * timer_tsc_hz - The calibrated TSC frequency
 */
uint64_t timer_tsc_hz(void) {
    return tsc_tpm * 1000ULL;
}

#pragma endregion

#pragma region Public Control API

/*
 * timer_init - Initializes the timer subsystem and determines TSC/LAPIC frequency
 */
void timer_init(void) {
    boot_tsc = tsc_read();
    
    hpet_init();
    timer_calibrate_all();

    // Register handler and unmask IRQ 0 (System Timer)
    irq_register(INT_FIRST_INTERRUPT, (irq_handler_t)timer_handler);
    ioapic_unmask(0);

    // Enable TSC Deadline tickless mode if supported
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1u << 24)) {
        tsc_deadline_mode = true;
        LOGF("[TIMER] TSC-Deadline tickless mode enabled.\n");
    } else {
        lapic_timer_periodic(10000, INT_FIRST_INTERRUPT);
        LOGF("[TIMER] TSC-Deadline not available; using 10ms periodic timer.\n");
    }
}

/*
 * sleep_ms - Blocks execution for at least the specified number of milliseconds
 */
void sleep_ms(uint64_t ms) {
    if (ms == 0) return;

    if (sched_current() != NULL) {
        sched_sleep(ms);
        return;
    }

    if (tsc_tpm > 0) {
        uint64_t target = tsc_read() + (ms * tsc_tpm);
        while (tsc_read() < target) __asm__ volatile("pause");
    } else if (hpet_is_available()) {
        uint64_t q = 1000000000000ULL / hpet_period;
        uint64_t r = 1000000000000ULL % hpet_period;
        uint64_t target = hpet_read_counter() + ms * q + (ms * r) / hpet_period;
        while (hpet_read_counter() < target) __asm__ volatile("pause");
    } else {
        for (uint64_t i = 0; i < ms; i++) {
            pit_prepare_sleep(1);
            pit_wait();
        }
    }
}

/*
 * sleep_us - Blocks execution for at least the specified number of microseconds
 */
void sleep_us(uint64_t us) {
    if (tsc_tpm > 0) {
        uint64_t target = tsc_read() + (us * tsc_tpm / 1000);
        while (tsc_read() < target) __asm__ volatile("pause");
    } else if (hpet_is_available()) {
        uint64_t target = hpet_read_counter() + (us * 1000000000ULL / hpet_period);
        while (hpet_read_counter() < target) __asm__ volatile("pause");
    } else {
        uint32_t ticks = (PIT_FREQUENCY * us) / 1000000;
        if (ticks == 0) ticks = 1;
        pit_set_oneshot((uint16_t)ticks);
        pit_wait();
    }
}

/*
 * get_uptime_ms - Returns the number of milliseconds since the kernel booted
 */
uint64_t get_uptime_ms(void) {
    if (tsc_tpm == 0) return 0;
    return (tsc_read() - boot_tsc) / tsc_tpm;
}

/*
 * get_uptime_ns - Returns the number of nanoseconds since the kernel booted
 */
uint64_t get_uptime_ns(void) {
    if (tsc_tpm == 0) return 0;
    uint64_t delta = tsc_read() - boot_tsc;
    uint64_t whole = delta / tsc_tpm;
    uint64_t rem = delta % tsc_tpm;
    return whole * 1000000 + (rem * 1000000) / tsc_tpm;
}

/*
 * timer_arm_next - Arms the LAPIC timer for the next scheduler event
 */
void timer_arm_next(bool going_idle) {
    if (!tsc_deadline_mode) return;

    // this is a cool function for efficiency
    uint64_t now_tsc = tsc_read();

    if (going_idle) {
        uint64_t next_ms = sched_next_wake();

        if (next_ms == UINT64_MAX) {
            lapic_timer_stop();
            return;
        }

        uint64_t now_ms = get_uptime_ms();
        if (next_ms <= now_ms) {
            lapic_tsc_arm(now_tsc, INT_FIRST_INTERRUPT);
            return;
        }
        uint64_t deadline_tsc = now_tsc + ((next_ms - now_ms) * tsc_tpm);
        lapic_tsc_arm(deadline_tsc, INT_FIRST_INTERRUPT);

    } else {
        uint64_t quantum_tsc = now_tsc + ((uint64_t)SCHED_QUANTUM_MS * tsc_tpm);

        uint64_t next_ms = sched_next_wake();
        if (next_ms != UINT64_MAX) {
            uint64_t now_ms = get_uptime_ms();
            if (next_ms <= now_ms) {
                lapic_tsc_arm(now_tsc, INT_FIRST_INTERRUPT);
                return;
            }
            uint64_t sleep_tsc = now_tsc + ((next_ms - now_ms) * tsc_tpm);
            if (sleep_tsc < quantum_tsc)
                quantum_tsc = sleep_tsc;
        }

        lapic_tsc_arm(quantum_tsc, INT_FIRST_INTERRUPT);
    }
}

#pragma endregion
