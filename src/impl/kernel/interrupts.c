/*
 * interrupts.c - Functions for managing CPU interrupts
 * 
 * This file contains functions to enable and disable CPU interrupts,
 * as well as to check the current interrupt status.
 *
 * Author: u/ApparentlyPlus
 */

#include <debug.h>
#include <sys/interrupts.h>
#include <vga_stdio.h>

/*
 * enable_interrupts - Enable CPU interrupts.
 */
void enable_interrupts()
{
    __asm__ volatile("sti");
}

/*
 * disable_interrupts - Disable CPU interrupts.
 */
void disable_interrupts()
{
    __asm__ volatile("cli");
}

/*
 * disable_pics - Disable the legacy PICs (Programmable Interrupt Controllers)
 */
void disable_pics()
{
    // Mask all interrupts on both PICs
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xFF), "Nd"((uint16_t)0xA1));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0xFF), "Nd"((uint16_t)0x21));
}

/*
 * set_idt_entry - Set an entry in the IDT.
 */
void set_idt_entry(uint8_t vector, void* handler, uint8_t dpl)
{
    uint64_t handler_addr = (uint64_t)handler;

    interrupt_descriptor* entry = &idt[vector];
    entry->address_low = handler_addr & 0xFFFF;
    entry->address_mid = (handler_addr >> 16) & 0xFFFF;
    entry->address_high = handler_addr >> 32;
    entry->selector = (uint16_t)(uintptr_t)&gdt64_code_segment;

    //interrupt gate + present + DPL
    entry->flags = INTERRUPT_GATE | ((dpl & 0b11) << 5) |(1 << 7);

    //ist disabled for now, will revisit when implementing userspace
    entry->ist = 0;

    entry->reserved = 0;
}

/*
 * load_idt - Load the IDT from the given address.
 */
void load_idt(void* idt_addr)
{
    typedef struct 
    {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;

    // It doesn't matter that the idtr struct will be destroyed after this function, 
    // because the CPU copies the data out of it immediately. However, the actual IDT
    // must remain valid in memory.

    idtr idt_reg;
    idt_reg.limit = 0xFFF;
    idt_reg.base = (uint64_t)idt_addr;
    __asm__ volatile("lidt %0" :: "m"(idt_reg));
}

/*
 * idt_init - Initialize the IDT. Loads specific handlers for each interrupt, which in turn call a generic handler.
 */
void idt_init(void)
{
    extern char interrupt_handler_0[]; // Defined in interrupts.S

    disable_pics();

    for (size_t i = 0; i < IDT_SIZE; i++)
    {
        // We can pad 16 bytes per handler, just like we've aligned them in assembly
        set_idt_entry(i, (void*)((uint64_t)interrupt_handler_0 + (i * 16)), DPL_RING_0);
    }

    load_idt((void*)idt);

}

/*
 * interrupt_dispatcher - Dispatches the interrupt to the appropriate handler based on the vector number.\
 * For now it hust handles exceptions by printing a message to the console.
 */
cpu_context_t* interrupt_dispatcher(cpu_context_t* context)
{
    switch(context->vector_number){
        case INT_DIVIDE_ERROR:
            printf("[EXCEPTION] Divide by zero error!\n");
            break;
        case INT_DEBUG:
            printf("[EXCEPTION] Debug exception!\n");
            break;
        case INT_NMI:
            printf("[EXCEPTION] Non-maskable interrupt!\n");
            break;
        case INT_BREAKPOINT:
            printf("[EXCEPTION] Breakpoint exception!\n");
            break;
        case INT_OVERFLOW:  
            printf("[EXCEPTION] Overflow exception!\n");
            break;
        case INT_BOUND_RANGE:
            printf("[EXCEPTION] Bound range exceeded exception!\n");
            break;
        case INT_INVALID_OPCODE:
            printf("[EXCEPTION] Invalid opcode exception!\n");
            break;
        case INT_DEVICE_NOT_AVAILABLE:
            printf("[EXCEPTION] Device not available exception!\n");
            break;
        case INT_DOUBLE_FAULT:
            printf("[EXCEPTION] Double fault exception!\n");
            break;
        case INT_COPROCESSOR_SEGMENT:
            printf("[EXCEPTION] Coprocessor segment overrun exception!\n");
            break;
        case INT_INVALID_TSS:
            printf("[EXCEPTION] Invalid TSS exception!\n");
            break;
        case INT_SEGMENT_NOT_PRESENT:
            printf("[EXCEPTION] Segment not present exception!\n");
            break;
        case INT_STACK_SEGMENT_FAULT:
            printf("[EXCEPTION] Stack segment fault exception!\n");
            break;
        case INT_GENERAL_PROTECTION:
            printf("[EXCEPTION] General protection fault exception!\n");
            break;
        case INT_PAGE_FAULT:
            printf("[EXCEPTION] Page fault exception!\n");
            break;
        case INT_X87_FPU_ERROR:
            printf("[EXCEPTION] x87 FPU error exception!\n");
            break;
        case INT_ALIGNMENT_CHECK:
            printf("[EXCEPTION] Alignment check exception!\n");
            break;
        case INT_MACHINE_CHECK:
            printf("[EXCEPTION] Machine check exception!\n");
            break;
        case INT_SIMD_ERROR:
            printf("[EXCEPTION] SIMD error exception!\n");
            break;
        default:
            printf("[EXCEPTION] Unknown exception! Vector number: %lu\n", context->vector_number);
            break;  
    }

    return context;
}
