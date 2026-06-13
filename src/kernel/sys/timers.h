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

// Timer Constants

#define PIT_FREQUENCY 1193182

#define FEMTOSECONDS_PER_SECOND 1000000000000000ULL
#define FEMTOSECONDS_PER_NANO   1000000ULL

// Timer Structures

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

#define SCHED_QUANTUM_MS 10

// Timer API

void timer_init(void);
void sleep_ms(uint64_t ms);
void sleep_us(uint64_t us);
uint64_t get_uptime_ms(void);
uint64_t get_uptime_ns(void);
void timer_arm_next(bool going_idle);

// Local APIC Timer API

void lapic_timer_oneshot(uint32_t us, uint8_t vector);
void lapic_timer_periodic(uint32_t us, uint8_t vector);
void lapic_timer_stop(void);

// TSC Timer API

uint64_t tsc_read(void);
void tsc_deadline_arm(uint64_t target_tsc);

// HPET API

bool hpet_is_available(void);
uint64_t hpet_read_counter(void);

// PIT API 

void pit_set_oneshot(uint16_t ticks);
void pit_prepare_sleep(uint32_t ms);
