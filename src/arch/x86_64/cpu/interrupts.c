/*
 * interrupts.c - Functions for managing CPU interrupts
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/cpu/gdt.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>

#pragma region Types and Globals

idt_entry_t idt[IDT_SIZE] = {0};
static irq_handler_t irq_handlers[IDT_SIZE] = {0};
static irq_handler_t user_fault = NULL;
extern uint64_t isr_stubs[IDT_SIZE];

#pragma endregion


#pragma region Interrupt Management API

/*
 * irq_register - Register a custom handler for a specific vector
 */
void irq_register(uint8_t vector, irq_handler_t handler)
{
    irq_handlers[vector] = handler;
}

/*
 * irq_unregister - Remove a custom handler for a specific vector
 */
void irq_unregister(uint8_t vector)
{
    irq_handlers[vector] = NULL;
}

/*
 * irq_user_fault - Register the handler for exceptions raised in ring 3 that nobody else took
 */
void irq_user_fault(irq_handler_t handler)
{
    user_fault = handler;
}

#pragma endregion

#pragma region IDT Setup

static void set_idt_entry(uint8_t vector, void* handler, uint8_t dpl, uint8_t ist_index)
{
    uint64_t handler_addr = (uint64_t)handler;

    idt_entry_t* entry = &idt[vector];
    entry->address_low = handler_addr & 0xFFFF;
    entry->address_mid = (handler_addr >> 16) & 0xFFFF;
    entry->address_high = handler_addr >> 32;
    entry->selector = KERNEL_CS;
    entry->flags = INTERRUPT_GATE | ((dpl & 0b11) << 5) | (1 << 7);
    entry->ist = ist_index & 0x7; // 0 = no IST stack switching

    entry->reserved = 0;
}

/*
 * idt_set_ist - Make a vector switch to IST stack ist (1-7) on entry, 0 turns it off
 */
void idt_set_ist(uint8_t vector, uint8_t ist)
{
    idt[vector].ist = ist & 0x7;
}

static void load_idt(void* idt_addr)
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
 * idt_init - Initialize the Interrupt Descriptor Table
 */
void idt_init(void)
{
    // imagine using the PIC in the big 2026, this is some museum stuff here
    // jokes aside, we'll use the modern APIC instead
    disable_pic();

    // No IST stacks yet, they come with the TSS (gdt_init calls idt_set_ist)
    for (size_t i = 0; i < IDT_SIZE; i++)
    {
        // Breakpoint and Debug can be triggered from Ring 3 for debugging
        // but this is untested and possibly out of scope for now
        uint8_t dpl = (i == INT_BREAKPOINT || i == INT_DEBUG) ? DPL_RING_3 : DPL_RING_0;
        set_idt_entry(i, (void*)isr_stubs[i], dpl, 0);
    }

    load_idt((void*)idt);

    LOGF("[IDT] Interrupt Descriptor Table initialized and loaded.\n");
}

#pragma endregion

#pragma region Dispatcher

/*
 * check_frame - Catch a smashed iret frame before iretq faults on it
 * This helped me debug the sysretq microarhitecture implementation nightmare
 */
static void check_frame(cpu_context_t* context, uint64_t vec)
{
    uint16_t cs = (uint16_t)context->iret_cs;
    uint16_t ss = (uint16_t)context->iret_ss;

    if ((cs & 3) == 0) {
        if (cs != KERNEL_CS)
            panicf_c(context, "Bad kernel CS in iret frame (cs=0x%04x ss=0x%04x vec=%lu)", cs, ss, vec);
        if (ss != 0 && ss != KERNEL_DS)
            panicf_c(context, "Bad kernel SS in iret frame (ss=0x%04x cs=0x%04x vec=%lu)", ss, cs, vec);
    } else {
        if (cs != USER_CS)
            panicf_c(context, "Bad user CS in iret frame (cs=0x%04x ss=0x%04x vec=%lu)", cs, ss, vec);
        if (ss != USER_DS)
            panicf_c(context, "Bad user SS in iret frame (ss=0x%04x cs=0x%04x vec=%lu)", ss, cs, vec);
    }
}

/*
 * interrupt_dispatcher - Central C handler called by assembly stubs
 */
cpu_context_t* interrupt_dispatcher(cpu_context_t* context)
{
    uint64_t vec = context->vector_number;
   
    // Spurious interrupts do not require an EOI
    if (vec == INT_SPURIOUS_INTERRUPT) {
        return context;
    }

    if (irq_handlers[vec] != NULL) {
        cpu_context_t* next = irq_handlers[vec](context);

        if (next) {
            check_frame(next, vec);

            // exceptions don't need EOI, only real hw interrupts
            if (vec >= INT_FIRST_INTERRUPT) lapic_eoi();
            return next;
        }

        // An exception handler returns NULL for faults that aren't its business
        if (vec >= INT_FIRST_INTERRUPT) panic("IRQ handler returned NULL context");
    }

    // no handler, but still gotta EOI or we'll never get another interrupt
    if (vec >= INT_FIRST_INTERRUPT) {
        LOGF("[INT] Unhandled interrupt vector: %d\n", vec);
        lapic_eoi();
        return context;
    }

    // rdmsr_safe probing an MSR that doesn't exist
    extern char msr_safe_insn[], msr_safe_fail[];
    if (vec == INT_GENERAL_PROTECTION && context->iret_rip == (uint64_t)msr_safe_insn) {
        context->iret_rip = (uint64_t)msr_safe_fail;
        return context;
    }

    // A broken program only takes itself down, not the whole system
    if ((context->iret_cs & 3) == 3 && user_fault) {
        cpu_context_t* next = user_fault(context);
        if (next) {
            check_frame(next, vec);
            return next;
        }
    }

    panic_c(exc_name(vec), context);
    return context;
}

#pragma endregion
