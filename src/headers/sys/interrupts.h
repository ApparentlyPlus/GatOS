/*
 * interrupts.h - Header file for CPU interrupt management
 *
 * This file declares functions to enable and disable CPU interrupts,
 * as well as to check the current interrupt status.
 * 
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    uint16_t address_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t address_mid;
    uint32_t address_high;
    uint32_t reserved;
} __attribute__((packed)) interrupt_descriptor;

typedef struct
{
    // General purpose registers (pushed in generic_interrupt_handler)
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    // Interrupt information (pushed by our interrupt handlers)
    uint64_t vector_number;
    uint64_t error_code;

    // CPU auto-pushed state (IRET frame)
    uint64_t iret_rip;
    uint64_t iret_cs;
    uint64_t iret_flags;
    uint64_t iret_rsp;
    uint64_t iret_ss;

} cpu_context_t;

#define IDT_SIZE                256
#define INTERRUPT_GATE          0xE // This is 0b1110
#define INTERRUPT_TRAP_GATE     0xF // This is 0b1111

// The Descriptor Privilege Level determines the highest cpu ring that can trigger this interrupt via software.
#define DPL_RING_0              0x0
#define DPL_RING_1              0x1
#define DPL_RING_2              0x2
#define DPL_RING_3              0x3

// Interrupt Vector Numbers
#define INT_DIVIDE_ERROR         0   // #DE - Divide By Zero Error
#define INT_DEBUG                1   // #DB - Debug
#define INT_NMI                  2   // #NMI - Non-Maskable Interrupt
#define INT_BREAKPOINT           3   // #BP - Breakpoint
#define INT_OVERFLOW             4   // #OF - Overflow
#define INT_BOUND_RANGE          5   // #BR - Bound Range Exceeded
#define INT_INVALID_OPCODE       6   // #UD - Invalid Opcode
#define INT_DEVICE_NOT_AVAILABLE 7   // #NM - Device not available
#define INT_DOUBLE_FAULT         8   // #DF - Double Fault
#define INT_COPROCESSOR_SEGMENT  9   // Unused (was x87 Segment Overrun)
#define INT_INVALID_TSS         10   // #TS - Invalid TSS
#define INT_SEGMENT_NOT_PRESENT 11   // #NP - Segment Not Present
#define INT_STACK_SEGMENT_FAULT 12   // #SS - Stack-Segment Fault
#define INT_GENERAL_PROTECTION  13   // #GP - General Protection
#define INT_PAGE_FAULT          14   // #PF - Page Fault
#define INT_RESERVED_15         15   // Currently Unused
#define INT_X87_FPU_ERROR       16   // #MF - x87 FPU error
#define INT_ALIGNMENT_CHECK     17   // #AC - Alignment Check
#define INT_MACHINE_CHECK       18   // #MC - Machine Check
#define INT_SIMD_ERROR          19   // #XF - SIMD (SSE/AVX) error

// Useful for loops and bounds checking
#define INT_FIRST_EXCEPTION      0
#define INT_LAST_EXCEPTION       31
#define INT_FIRST_INTERRUPT      32
#define INT_LAST_INTERRUPT       255

// PIC Constants

#define PIC_MASTER_CMD    0x20
#define PIC_MASTER_DATA   0x21
#define PIC_SLAVE_CMD     0xA0
#define PIC_SLAVE_DATA    0xA1

#define ICW1_INIT         0x11   // Start initialization sequence
#define ICW4_8086         0x01   // 8086/88 (MCS-80/85) mode

#define ICW2_MASTER       0x20   // Master PIC vector offset (IRQs 0–7 → IDT 0x20–0x27)
#define ICW2_SLAVE        0x28   // Slave PIC vector offset (IRQs 8–15 → IDT 0x28–0x2F)

#define ICW3_MASTER       0x04   // Tell Master PIC that there is a slave at IRQ2 (bitmask)
#define ICW3_SLAVE        0x02   // Tell Slave PIC its cascade identity (connected to IRQ2)


static interrupt_descriptor idt[IDT_SIZE] = {0};

extern uint32_t gdt64_code_segment;

void idt_init(void);
void enable_interrupts(void);
void disable_interrupts(void);