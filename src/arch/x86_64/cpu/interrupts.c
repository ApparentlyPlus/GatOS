/*
 * interrupts.c - Functions for managing CPU interrupts
 *
 * This file implements the Interrupt Descriptor Table (IDT) initialization,
 * interrupt dispatching, and handler registration.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/drivers/stdio.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <kernel/misc.h>
#include <libc/string.h>

#pragma region Types and Globals

// Function pointer type for interrupt handlers
typedef void (*irq_handler_t)(cpu_context_t*);

// Static array of registered interrupt handlers
static irq_handler_t g_irq_handlers[IDT_SIZE] = {0};

// External assembly handler entry point
extern char interrupt_handler_0[];

#pragma endregion


#pragma region Interrupt Management API

/*
 * register_interrupt_handler - Register a custom handler for a specific vector
 */
void register_interrupt_handler(uint8_t vector, irq_handler_t handler)
{
    g_irq_handlers[vector] = handler;
}

/*
 * unregister_interrupt_handler - Remove a custom handler for a specific vector
 */
void unregister_interrupt_handler(uint8_t vector)
{
    g_irq_handlers[vector] = NULL;
}

/*
 * enable_interrupts - Enable CPU interrupts (STI)
 */
void enable_interrupts()
{
    __asm__ volatile("sti");
}

/*
 * disable_interrupts - Disable CPU interrupts (CLI)
 */
void disable_interrupts()
{
    __asm__ volatile("cli");
}

#pragma endregion

#pragma region IDT Setup

/*
 * set_idt_entry - Populate a single IDT entry
 */
void set_idt_entry(uint8_t vector, void* handler, uint8_t dpl)
{
    uint64_t handler_addr = (uint64_t)handler;

    interrupt_descriptor* entry = &idt[vector];
    entry->address_low = handler_addr & 0xFFFF;
    entry->address_mid = (handler_addr >> 16) & 0xFFFF;
    entry->address_high = handler_addr >> 32;
    entry->selector = (uint16_t)(uintptr_t)&gdt64_code_segment;
    entry->flags = INTERRUPT_GATE | ((dpl & 0b11) << 5) | (1 << 7);

    //ist disabled for now, will revisit when implementing userspace
    entry->ist = 0;

    entry->reserved = 0;
}

/*
 * load_idt - Load the IDT pointer into the CPU (LIDT)
 */
void load_idt(void* idt_addr)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt_addr;

    __asm__ volatile("lidt %0" :: "m"(idtr));
}

/*
 * idt_init - Initialize the Interrupt Descriptor Table.
 */
void idt_init(void)
{
    // Disable the legacy 8259 PICs to prevent interference with the APIC.
    disable_pic();

    // Populate IDT entries with the assembly stubs
    // The stubs are spaced 16 bytes apart in the interrupt_handler_0 block
    for (size_t i = 0; i < IDT_SIZE; i++)
    {
        void* handler = (void*)((uint64_t)interrupt_handler_0 + (i * 16));
        set_idt_entry(i, handler, DPL_RING_0);
    }

    // Load the IDT into the CPU
    load_idt((void*)idt);

    LOGF("[IDT] Interrupt Descriptor Table initialized and loaded.\n");
}

#pragma endregion

#pragma region Dispatcher

/*
 * interrupt_dispatcher - Central C handler called by assembly stubs.
 */
cpu_context_t* interrupt_dispatcher(cpu_context_t* context)
{
    uint64_t vec = context->vector_number;
   
    // Here we got harmless spurious interrupts, just ignore them
    // They do NOT require an EOI
    if (vec == INT_SPURIOUS_INTERRUPT) {
        return context;
    }

    // Here is the general dispatcher logic
    // If a driver or kernel subsystem has registered a handler, this is the time to invoke it
    if (g_irq_handlers[vec] != NULL) {
        g_irq_handlers[vec](context);

        // If it was a hardware interrupt, we must ack it
        // Exceptions (0-31) generally do not need EOI
        if (vec >= INT_FIRST_INTERRUPT) {
            lapic_eoi();
        }
        return context;
    }

    // Handle Exceptions
    if (vec < INT_FIRST_INTERRUPT) {
        // Print extra debug info for specific faults
        if (vec == INT_PAGE_FAULT) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

            LOGF("[EXCEPTION] Page Fault at address: 0x%lx\n", cr2);
            LOGF("CR2 (Fault Address): 0x%lx\n", cr2);
            LOGF("Error Code: 0x%lx (P:%d W:%d U:%d R:%d I:%d)\n", 
                context->error_code,
                (context->error_code & 1) ? 1 : 0, // Present
                (context->error_code & 2) ? 1 : 0, // Write
                (context->error_code & 4) ? 1 : 0, // User
                (context->error_code & 8) ? 1 : 0, // Reserved Write
                (context->error_code & 16)? 1 : 0  // Instruction Fetch
            );
        }

        const char* panic_msg = "Unknown Exception";
        
        switch (vec) {
            case INT_DIVIDE_ERROR:         panic_msg = "Divide by zero"; break;
            case INT_DEBUG:                panic_msg = "Debug trap"; break;
            case INT_NMI:                  panic_msg = "Non-maskable interrupt"; break;
            case INT_BREAKPOINT:           panic_msg = "Breakpoint"; break;
            case INT_OVERFLOW:             panic_msg = "Overflow"; break;
            case INT_BOUND_RANGE:          panic_msg = "Bound range exceeded"; break;
            case INT_INVALID_OPCODE:       panic_msg = "Invalid opcode"; break;
            case INT_DEVICE_NOT_AVAILABLE: panic_msg = "Device not available (FPU)"; break;
            case INT_DOUBLE_FAULT:         panic_msg = "Double Fault (Critical)"; break;
            case INT_INVALID_TSS:          panic_msg = "Invalid TSS"; break;
            case INT_SEGMENT_NOT_PRESENT:  panic_msg = "Segment not present"; break;
            case INT_STACK_SEGMENT_FAULT:  panic_msg = "Stack segment fault"; break;
            case INT_GENERAL_PROTECTION:   panic_msg = "General protection fault"; break;
            case INT_PAGE_FAULT:           panic_msg = "Page Fault"; break;
            case INT_X87_FPU_ERROR:        panic_msg = "x87 FPU error"; break;
            case INT_ALIGNMENT_CHECK:      panic_msg = "Alignment check"; break;
            case INT_MACHINE_CHECK:        panic_msg = "Machine check"; break;
            case INT_SIMD_ERROR:           panic_msg = "SIMD exception"; break;
        }

        // Panic will dump the register state from ctx
        LOGF("[PANIC] %s (Vector %d)\n", panic_msg, vec);
        panic_c(panic_msg, context);
    }

    // Handle Unregistered Hardware Interrupts (Vectors 32+)
    // If we receive an IRQ but have no handler, we must still ack it to
    // prevent the APIC from blocking future interrupts.
    else {
        LOGF("[INT] Unhandled interrupt vector: %d\n", vec);
        lapic_eoi();
    }

    return context;
}
