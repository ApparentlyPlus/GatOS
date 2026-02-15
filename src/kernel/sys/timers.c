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
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/acpi.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <libc/string.h>

#pragma region Internal Globals

static hpet_regs_t* g_hpet = NULL;
static uint32_t g_hpet_period = 0; // Femtoseconds per tick

static uint64_t g_tsc_ticks_per_ms = 0;
static uint64_t g_boot_tsc = 0;

#pragma endregion

#pragma region PIT Implementation

/**
 * pit_set_oneshot - Sets PIT channel 0 to one-shot mode (Mode 0)
 */
void pit_set_oneshot(uint16_t ticks) {
    // Channel 0, Access Mode lobyte/hibyte, Mode 0 (Interrupt on Terminal Count), Binary
    outb(0x43, 0x30); 
    outb(0x40, (uint8_t)(ticks & 0xFF));
    outb(0x40, (uint8_t)((ticks >> 8) & 0xFF));
}

/**
 * pit_prepare_sleep - Helper for short delays during early boot
 */
void pit_prepare_sleep(uint32_t ms) {
    uint32_t total_ticks = (PIT_FREQUENCY / 1000) * ms;
    
    // The PIT counter is 16-bit, max ~54ms per wrap.
    // For longer sleeps, we'd need a loop.
    if (total_ticks > 0xFFFF) total_ticks = 0xFFFF;

    pit_set_oneshot((uint16_t)total_ticks);
}

/**
 * pit_wait - Spins until PIT Channel 0 reaches 0
 */
static void pit_wait(void) {
    // In Mode 0, the OUT pin goes high when count reaches 0.
    
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

/**
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
    if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)phys_addr, &virt_addr) != VMM_OK) {
        LOGF("[TIMER] Failed to map HPET registers.\n");
        return;
    }

    g_hpet = (hpet_regs_t*)virt_addr;
    g_hpet_period = g_hpet->capabilities_high;

    // Enable HPET (Set bit 0 of General Config)
    // Also clear bit 1 (Legacy Replacement) to use it cleanly
    g_hpet->configuration |= 0x01;
    g_hpet->configuration &= ~0x02;

    LOGF("[TIMER] HPET initialized. Period: %u fs (%u MHz)\n", 
         g_hpet_period, (uint32_t)(FEMTOSECONDS_PER_SECOND / g_hpet_period / 1000000));
}

bool hpet_is_available(void) {
    return g_hpet != NULL;
}

uint64_t hpet_read_counter(void) {
    if (!g_hpet) return 0;
    return g_hpet->main_counter;
}

#pragma endregion

#pragma region Calibration Logic

/**
 * timer_calibrate_all - Calibrates LAPIC and TSC against HPET or PIT
 */
static void timer_calibrate_all(void) {
    LOGF("[TIMER] Calibrating high-precision timers...\n");

    const uint32_t CALIBRATE_MS = 10;
    uint64_t lapic_start, lapic_end;
    uint64_t tsc_start, tsc_end;

    // Prepare LAPIC timer for calibration (Divisor 16)
    lapic_write(LAPIC_TDCR, 0x03); // Divisor 16
    lapic_write(LAPIC_TICR, 0xFFFFFFFF); // Max count

    if (hpet_is_available()) {
        uint64_t hpet_target = (CALIBRATE_MS * 1000000000000ULL) / g_hpet_period;
        uint64_t hpet_start = hpet_read_counter();
        
        lapic_start = lapic_read(LAPIC_TCCR);
        tsc_start = tsc_read();

        while (hpet_read_counter() - hpet_start < hpet_target) {
            __asm__ volatile("pause");
        }

        lapic_end = lapic_read(LAPIC_TCCR);
        tsc_end = tsc_read();
    } else {
        // Fallback to PIT
        pit_set_oneshot(0xFFFF); 
        uint32_t pit_target = (PIT_FREQUENCY / 1000) * CALIBRATE_MS;
        
        outb(0x43, 0x00);
        uint8_t low = inb(0x40);
        uint8_t high = inb(0x40);
        uint16_t start_val = (high << 8) | low;

        lapic_start = lapic_read(LAPIC_TCCR);
        tsc_start = tsc_read();

        while (1) {
            outb(0x43, 0x00);
            low = inb(0x40);
            high = inb(0x40);
            uint16_t cur_val = (high << 8) | low;
            if (start_val - cur_val >= pit_target) break;
        }

        lapic_end = lapic_read(LAPIC_TCCR);
        tsc_end = tsc_read();
    }

    uint64_t lapic_ticks_per_ms = (lapic_start - lapic_end) / CALIBRATE_MS;
    g_tsc_ticks_per_ms = (tsc_end - tsc_start) / CALIBRATE_MS;

    lapic_timer_set_calibration(lapic_ticks_per_ms);

    LOGF("[TIMER] LAPIC: %lu ticks/ms, TSC: %lu ticks/ms\n", 
         lapic_ticks_per_ms, g_tsc_ticks_per_ms);
}

#pragma endregion

#pragma region Public Control API

void timer_init(void) {
    g_boot_tsc = tsc_read();
    
    hpet_init();
    timer_calibrate_all();

    // Check for TSC Deadline support
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1 << 24)) {
        LOGF("[TIMER] TSC-Deadline mode supported.\n");
    }
}

void sleep_ms(uint64_t ms) {
    if (g_tsc_ticks_per_ms > 0) {
        uint64_t target = tsc_read() + (ms * g_tsc_ticks_per_ms);
        while (tsc_read() < target) __asm__ volatile("pause");
    } else if (hpet_is_available()) {
        uint64_t target = hpet_read_counter() + (ms * 1000000000000ULL / g_hpet_period);
        while (hpet_read_counter() < target) __asm__ volatile("pause");
    } else {
        for (uint64_t i = 0; i < ms; i++) {
            pit_prepare_sleep(1);
            pit_wait();
        }
    }
}

void sleep_us(uint64_t us) {
    if (g_tsc_ticks_per_ms > 0) {
        uint64_t target = tsc_read() + (us * g_tsc_ticks_per_ms / 1000);
        while (tsc_read() < target) __asm__ volatile("pause");
    } else if (hpet_is_available()) {
        uint64_t target = hpet_read_counter() + (us * 1000000000ULL / g_hpet_period);
        while (hpet_read_counter() < target) __asm__ volatile("pause");
    } else {
        uint32_t ticks = (PIT_FREQUENCY * us) / 1000000;
        if (ticks == 0) ticks = 1;
        pit_set_oneshot((uint16_t)ticks);
        pit_wait();
    }
}

uint64_t get_uptime_ms(void) {
    if (g_tsc_ticks_per_ms == 0) return 0;
    return (tsc_read() - g_boot_tsc) / g_tsc_ticks_per_ms;
}

uint64_t get_uptime_ns(void) {
    if (g_tsc_ticks_per_ms == 0) return 0;
    return ((tsc_read() - g_boot_tsc) * 1000000) / g_tsc_ticks_per_ms;
}

#pragma endregion
