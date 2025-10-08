/*
 * panic.c - Kernel panic implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <sys/panic.h>
#include <sys/interrupts.h>
#include <stdarg.h>
#include <vga_console.h>
#include <vga_stdio.h>

/*
 * halt_system - Halts the CPU indefinitely.
 */
void halt_system(void)
{
    while (1) {
        __asm__ volatile("hlt");
    }
}

/*
 * print_exception_name - Prints a human-readable name for the given exception vector.
 */
void print_exception_name(uint64_t vector)
{
    char* names[] = {
        "Divide By Zero",           // 0
        "Debug",                    // 1
        "Non-Maskable Interrupt",   // 2
        "Breakpoint",               // 3
        "Overflow",                 // 4
        "Bound Range Exceeded",     // 5
        "Invalid Opcode",           // 6
        "Device Not Available",     // 7
        "Double Fault",             // 8
        "Coprocessor Segment",      // 9
        "Invalid TSS",              // 10
        "Segment Not Present",      // 11
        "Stack-Segment Fault",      // 12
        "General Protection",       // 13
        "Page Fault",               // 14
        "Reserved",                 // 15
        "x87 FPU Error",            // 16
        "Alignment Check",          // 17
        "Machine Check",            // 18
        "SIMD Exception",           // 19
    };
    
    if (vector < sizeof(names)/sizeof(names[0])) {
        printf("%s (#%lu)", names[vector], vector);
    } else if (vector < 32) {
        printf("Reserved Exception (#%lu)", vector);
    } else {
        printf("Interrupt (#%lu)", vector);
    }
}

/*
 * panic_c - Handles a kernel panic with an optional CPU context.
 */
void panic_c(const char* message, cpu_context_t* context)
{
    disable_interrupts();
    
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_RED);
    console_clear();
    
    printf("\n    Oh no! Your GatOS ventured into undefined behavior and never returned :(    \n");
    printf("\n                                      ---                                       \n");
    printf("\n");
    printf("[+] Reason: %s\n", message);
    
    if (context != NULL) {
        printf("[+] Exception: ");
        print_exception_name(context->vector_number);
        printf("\n");
        printf("[+] Error Code: 0x%04lx\n", context->error_code);
        
        // For page faults
        if (context->vector_number == INT_PAGE_FAULT) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            printf("\n");
            printf("Page Fault Details:\n");
            printf("  Faulting Address (CR2): 0x%016lx\n", cr2);
            printf("  Access Type: %s\n", (context->error_code & 0x02) ? "Write" : "Read");
            printf("  Mode: %s\n", (context->error_code & 0x04) ? "User" : "Supervisor");
            printf("  Cause: %s\n", (context->error_code & 0x01) ? 
                   "Protection violation" : "Page not present");
            if (context->error_code & 0x08)
                printf("  Reserved bit set in page table entry\n");
            if (context->error_code & 0x10)
                printf("  Caused by instruction fetch\n");
        }
        
        // IP
        printf("\n");
        printf("Instruction Pointer:\n");
        printf("  RIP: 0x%016lx\n", context->iret_rip);
        printf("  CS:  0x%04lx\n", context->iret_cs);
        
        // CPU flags
        printf("\n");
        printf("CPU Flags (RFLAGS): 0x%016lx\n", context->iret_flags);
        printf("  Flags: ");
        if (context->iret_flags & (1 << 0)) printf("CF ");
        if (context->iret_flags & (1 << 2)) printf("PF ");
        if (context->iret_flags & (1 << 4)) printf("AF ");
        if (context->iret_flags & (1 << 6)) printf("ZF ");
        if (context->iret_flags & (1 << 7)) printf("SF ");
        if (context->iret_flags & (1 << 8)) printf("TF ");
        if (context->iret_flags & (1 << 9)) printf("IF ");
        if (context->iret_flags & (1 << 10)) printf("DF ");
        if (context->iret_flags & (1 << 11)) printf("OF ");
        printf("\n");
        
    }
    else{
        printf("[-] No CPU context available, that's usually bad...\n");
    }
    
    printf("\n                                 SYSTEM HALTED                                  ");
    
    halt_system();
}

/*
 * panic - Simple panic function without context.
 */
void panic(const char* message)
{
    panic_c(message, NULL);
}

/*
 * panicf - Formatted panic function without context.
 */
void panicf(const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    panic_c(buffer, NULL);
}