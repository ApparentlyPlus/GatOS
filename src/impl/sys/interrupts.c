/*
 * interrupts.c - Functions for managing CPU interrupts
 *
 * Author: u/ApparentlyPlus
 */

#include <misc.h>
#include <debug.h>
#include <libc/string.h>
#include <sys/interrupts.h>
#include <vga_stdio.h>
#include <sys/panic.h>

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
void disable_pics(void)
{
    // --- Begin initialization (ICW1)
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW1_INIT), "Nd"((uint16_t)PIC_MASTER_CMD));
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW1_INIT), "Nd"((uint16_t)PIC_SLAVE_CMD));

    // --- Set vector offsets (ICW2)
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW2_MASTER), "Nd"((uint16_t)PIC_MASTER_DATA));
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW2_SLAVE),  "Nd"((uint16_t)PIC_SLAVE_DATA));

    // --- Configure cascade identity (ICW3)
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW3_MASTER), "Nd"((uint16_t)PIC_MASTER_DATA));
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW3_SLAVE),  "Nd"((uint16_t)PIC_SLAVE_DATA));

    // --- Set operation mode (ICW4)
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW4_8086), "Nd"((uint16_t)PIC_MASTER_DATA));
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)ICW4_8086), "Nd"((uint16_t)PIC_SLAVE_DATA));

    // --- Mask all interrupts (disable all IRQ lines)
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)0xFF), "Nd"((uint16_t)PIC_MASTER_DATA));
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)0xFF), "Nd"((uint16_t)PIC_SLAVE_DATA));
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
    switch (context->vector_number) {
        case INT_DIVIDE_ERROR:
            DEBUG_GENERIC_LOG("[EXCEPTION] Divide by zero error!\n");
            panic_c("Attempted to divide by zero", context);
            break;

        case INT_DEBUG:
            DEBUG_GENERIC_LOG("[EXCEPTION] Debug exception!\n");
            panic_c("Triggered a debug trap", context);
            break;

        case INT_NMI:
            DEBUG_GENERIC_LOG("[EXCEPTION] Non-maskable interrupt!\n");
            panic_c("Crazy, you got a non-maskable interrupt", context);
            break;

        case INT_BREAKPOINT:
            DEBUG_GENERIC_LOG("[EXCEPTION] Breakpoint exception!\n");
            panic_c("Breakpoint triggered", context);
            break;

        case INT_OVERFLOW:
            DEBUG_GENERIC_LOG("[EXCEPTION] Overflow exception!\n");
            panic_c("Arithmetic overflow", context);
            break;

        case INT_BOUND_RANGE:
            DEBUG_GENERIC_LOG("[EXCEPTION] Bound range exceeded exception!\n");
            panic_c("Bound range exceeded", context);
            break;

        case INT_INVALID_OPCODE:
            DEBUG_GENERIC_LOG("[EXCEPTION] Invalid opcode exception!\n");
            panic_c("Invalid instruction opcode", context);
            break;

        case INT_DEVICE_NOT_AVAILABLE:
            DEBUG_GENERIC_LOG("[EXCEPTION] Device not available exception!\n");
            panic_c("Device not available", context);
            break;

        case INT_DOUBLE_FAULT:
            DEBUG_GENERIC_LOG("[EXCEPTION] Double fault exception!\n");
            panic_c("A double fault occured. Sorry to hear that.", context);
            break;

        case INT_COPROCESSOR_SEGMENT:
            DEBUG_GENERIC_LOG("[EXCEPTION] Coprocessor segment overrun exception!\n");
            panic_c("Coprocessor segment overrun", context);
            break;

        case INT_INVALID_TSS:
            DEBUG_GENERIC_LOG("[EXCEPTION] Invalid TSS exception!\n");
            panic_c("Invalid task state segment", context);
            break;

        case INT_SEGMENT_NOT_PRESENT:
            DEBUG_GENERIC_LOG("[EXCEPTION] Segment not present exception!\n");
            panic_c("Segment not present", context);
            break;

        case INT_STACK_SEGMENT_FAULT:
            DEBUG_GENERIC_LOG("[EXCEPTION] Stack segment fault exception!\n");
            panic_c("A stack segment fault occured", context);
            break;

        case INT_GENERAL_PROTECTION:
            DEBUG_GENERIC_LOG("[EXCEPTION] General protection fault exception!\n");
            panic_c("A general protection fault occured", context);
            break;

        case INT_PAGE_FAULT:
            DEBUG_GENERIC_LOG("[EXCEPTION] Page fault exception!\n");
            panic_c("A page fault occured", context);
            break;

        case INT_X87_FPU_ERROR:
            DEBUG_GENERIC_LOG("[EXCEPTION] x87 FPU error exception!\n");
            panic_c("An x87 floating point error occured", context);
            break;

        case INT_ALIGNMENT_CHECK:
            DEBUG_GENERIC_LOG("[EXCEPTION] Alignment check exception!\n");
            panic_c("Memory alignment check failed", context);
            break;

        case INT_MACHINE_CHECK:
            DEBUG_GENERIC_LOG("[EXCEPTION] Machine check exception!\n");
            panic_c("Machine check", context);
            break;

        case INT_SIMD_ERROR:
            DEBUG_GENERIC_LOG("[EXCEPTION] SIMD error exception!\n");
            panic_c("A SIMD floating point erroroccured, check SSE", context);
            break;

        default:
            DEBUG_GENERIC_LOG("[EXCEPTION] Unknown exception!\n");
            panic_c("Unknown interrupt vector", context);
            break;
    }

    return context;
}