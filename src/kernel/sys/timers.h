/*
 * timers.h - Kernel Timer Subsystem
 *
 * This header defines the public interface for the GatOS timer subsystem.
 * It provides abstractions for various hardware timers including the PIT,
 * HPET, Local APIC timer, and the TSC.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- Timer Constants ---

#define PIT_FREQUENCY 1193182

#define FEMTOSECONDS_PER_SECOND 1000000000000000ULL
#define FEMTOSECONDS_PER_NANO   1000000ULL

// --- Timer Structures ---

typedef struct {
    uint32_t capabilities_low;
    uint32_t capabilities_high; // Bits 63:32 = Period in femtoseconds
    uint32_t reserved0;
    uint32_t reserved1;
    uint64_t configuration;
    uint64_t reserved2;
    uint64_t interrupt_status;
    uint64_t reserved3[25];     // Padding to reach 0xF0
    uint64_t main_counter;
    uint64_t reserved4;
} __attribute__((packed)) hpet_regs_t;

// --- Public API ---

/**
 * timer_init - Detects and initializes available system timers.
 * Calibrates the Local APIC and TSC against the HPET (preferred) or PIT.
 */
void timer_init(void);

/**
 * sleep_ms - Polled sleep for a specified number of milliseconds.
 * Uses the highest precision available timer.
 */
void sleep_ms(uint64_t ms);

/**
 * sleep_us - Polled sleep for a specified number of microseconds.
 */
void sleep_us(uint64_t us);

/**
 * get_uptime_ms - Returns system uptime in milliseconds since timer initialization.
 */
uint64_t get_uptime_ms(void);

/**
 * get_uptime_ns - Returns system uptime in nanoseconds.
 */
uint64_t get_uptime_ns(void);

// --- Local APIC Timer API ---

/**
 * lapic_timer_calibrate - Calibrates the Local APIC timer against a reference.
 */
void lapic_timer_calibrate(void);

/**
 * lapic_timer_oneshot - Arms the LAPIC timer in one-shot mode.
 * @param us Duration in microseconds.
 * @param vector Interrupt vector to trigger.
 */
void lapic_timer_oneshot(uint32_t us, uint8_t vector);

/**
 * lapic_timer_periodic - Arms the LAPIC timer in periodic mode.
 * @param us Period in microseconds.
 * @param vector Interrupt vector to trigger.
 */
void lapic_timer_periodic(uint32_t us, uint8_t vector);

/**
 * lapic_timer_stop - Stops the Local APIC timer.
 */
void lapic_timer_stop(void);

// --- TSC Timer API ---

/**
 * tsc_calibrate - Calibrates the TSC frequency.
 */
void tsc_calibrate(void);

/**
 * tsc_read - Reads the current value of the Time Stamp Counter.
 */
uint64_t tsc_read(void);

/**
 * tsc_deadline_arm - Arms the TSC deadline timer.
 * @param target_tsc Absolute TSC tick count for the deadline.
 */
void tsc_deadline_arm(uint64_t target_tsc);

// --- HPET API ---

/**
 * hpet_is_available - Returns true if a HPET was detected and initialized.
 */
bool hpet_is_available(void);

/**
 * hpet_read_counter - Reads the HPET main counter.
 */
uint64_t hpet_read_counter(void);

// --- PIT API ---

/**
 * pit_set_oneshot - Sets the PIT channel 0 to one-shot mode.
 * @param ticks Initial count.
 */
void pit_set_oneshot(uint16_t ticks);

/**
 * pit_prepare_sleep - Prepares the PIT for a polled delay.
 * @param ms Duration in milliseconds.
 */
void pit_prepare_sleep(uint32_t ms);
