/*
 * apic.h - Local and I/O APIC configuration
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <kernel/sys/acpi.h>

// Legacy PIC Definitions

#define PIC_COMMAND_MASTER  0x20
#define PIC_DATA_MASTER     0x21
#define PIC_COMMAND_SLAVE   0xA0
#define PIC_DATA_SLAVE      0xA1

#define ICW_1               0x11
#define ICW_2_M             0x20
#define ICW_2_S             0x28
#define ICW_3_M             0x04
#define ICW_3_S             0x02
#define ICW_4               0x01

// MSR Definitions
#define MSR_IA32_APIC_BASE      0x1B
#define MSR_APIC_BASE_BSP       (1 << 8)
#define MSR_APIC_BASE_X2        (1 << 10)
#define MSR_APIC_BASE_ENABLE    (1 << 11)

// Local APIC Register Offsets
#define LAPIC_ID                0x0020  // Local APIC ID
#define LAPIC_VER               0x0030  // Local APIC Version
#define LAPIC_TPR               0x0080  // Task Priority Register
#define LAPIC_PPR               0x00A0  // Processor Priority Register
#define LAPIC_EOI               0x00B0  // End of Interrupt
#define LAPIC_LDR               0x00D0  // Logical Destination Register
#define LAPIC_DFR               0x00E0  // Destination Format Register
#define LAPIC_SPURIOUS          0x00F0  // Spurious Interrupt Vector
#define LAPIC_ISR               0x0100  // In-Service Register (0x100-0x170)
#define LAPIC_TMR               0x0180  // Trigger Mode Register (0x180-0x1F0)
#define LAPIC_IRR               0x0200  // Interrupt Request Register (0x200-0x270)
#define LAPIC_ESR               0x0280  // Error Status Register
#define LAPIC_ICR_LOW           0x0300  // Interrupt Command Register (Lower 32)
#define LAPIC_ICR_HIGH          0x0310  // Interrupt Command Register (Upper 32)
#define LAPIC_LVT_TIMER         0x0320  // Timer LVT
#define LAPIC_LVT_THERMAL       0x0330  // Thermal LVT
#define LAPIC_LVT_PERF          0x0340  // Performance Counter LVT
#define LAPIC_LVT_LINT0         0x0350  // LINT0
#define LAPIC_LVT_LINT1         0x0360  // LINT1
#define LAPIC_LVT_ERROR         0x0370  // Error LVT
#define LAPIC_TICR              0x0380  // Timer Initial Count Register
#define LAPIC_TCCR              0x0390  // Timer Current Count Register
#define LAPIC_TDCR              0x03E0  // Timer Divide Configuration Register

// Constants
#define LAPIC_SPURIOUS_IV       0xFF    // Vector 255
#define LAPIC_SW_ENABLE         (1 << 8)

// LVT Masks
#define LVT_MASK                (1 << 16)
#define LVT_TIMER_PERIODIC      (1 << 17)

// I/O APIC Register Selectors
#define IOAPIC_REGSEL           0x00
#define IOAPIC_IOWIN            0x10

// I/O APIC Internal Registers
#define IOAPIC_ID               0x00
#define IOAPIC_VER              0x01
#define IOAPIC_ARB              0x02
#define IOAPIC_REDTBL           0x10

// MADT Structures
typedef struct {
    ACPISDTHeader header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed)) MADTHeader;

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) MADTRecordHeader;

// MADT Record Types
#define MADT_TYPE_LAPIC             0
#define MADT_TYPE_IOAPIC            1
#define MADT_TYPE_ISO               2
#define MADT_TYPE_NMI               4
#define MADT_TYPE_LAPIC_OVERRIDE    5

typedef struct {
    MADTRecordHeader header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) MADT_LAPIC;

typedef struct {
    MADTRecordHeader header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) MADT_IOAPIC;

typedef struct {
    MADTRecordHeader header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) MADT_ISO;

typedef struct {
    MADTRecordHeader header;
    uint8_t acpi_processor_id; // 0xFF means all processors
    uint16_t flags;
    uint8_t lint; // LINT0 or LINT1
} __attribute__((packed)) MADT_NMI;

// Public API
void apic_init(void); // High-level init (calls both lapic and ioapic init)

// Legacy
void disable_pic(void);

// Local APIC
void lapic_init(void);
void lapic_eoi(void);
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_send_ipi(uint32_t dest_id, uint8_t vector);
uint32_t lapic_get_id(void);

// I/O APIC
void ioapic_init(void);
void ioapic_set_entry(uint8_t index, uint64_t data);
void ioapic_redirect(uint8_t irq, uint8_t vector, uint32_t dest_core, uint16_t flags);
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);

// Timer
void lapic_timer_init(uint32_t ms, uint8_t vector);
void lapic_timer_stop(void);