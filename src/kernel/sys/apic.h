/*
 * apic.h - Local APIC configuration
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// MSR Definitions
#define MSR_IA32_APIC_BASE      0x1B
#define MSR_APIC_BASE_BSP       (1 << 8)
#define MSR_APIC_BASE_ENABLE    (1 << 11)

// Local APIC Register Offsets
#define LAPIC_ID                0x0020  // Local APIC ID
#define LAPIC_EOI               0x00B0  // End of Interrupt
#define LAPIC_SPURIOUS          0x00F0  // Spurious Interrupt Vector
#define LAPIC_ICR_LOW           0x0300  // Interrupt Command Register (Lower 32)
#define LAPIC_ICR_HIGH          0x0310  // Interrupt Command Register (Upper 32)
#define LAPIC_LVT_TIMER         0x0320  // Timer LVT
#define LAPIC_TICR              0x0380  // Timer Initial Count Register
#define LAPIC_TCCR              0x0390  // Timer Current Count Register
#define LAPIC_TDCR              0x03E0  // Timer Divide Configuration Register

// Constants
#define LAPIC_SPURIOUS_IV       0xFF    // Vector 255
#define LAPIC_SW_ENABLE         (1 << 8)

// Public API
void lapic_init(void);
void lapic_eoi(void);
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_send_ipi(uint32_t dest_id, uint8_t vector);