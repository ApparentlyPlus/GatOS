/*
 * panic.c - Kernel panic implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/drivers/console.h>
#include <kernel/drivers/stdio.h>
#include <kernel/drivers/tty.h>
#include <kernel/memory/heap.h>
#include <kernel/debug.h>
#include <kernel/sys/panic.h>
#include <stdarg.h>

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

void panic_c(const char* message, cpu_context_t* context)
{
    disable_interrupts();
    
    // Check if memory managers and TTY are up. If not, use concise serial output.
    if (heap_kernel_get() == NULL || g_active_tty == NULL) {
        LOGF("\n******************* KERNEL PANIC *******************\n");
        LOGF("REASON: %s\n", message);
        if (context) {
            LOGF("EXCEPTION: %lu, ERROR: 0x%lx, RIP: 0x%lx\n", 
                 context->vector_number, context->error_code, context->iret_rip);
        }
        LOGF("****************************************************\n");
    } else {
        // Setup the screen
        console_set_cursor_enabled(false);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_RED);
        console_clear(CONSOLE_COLOR_RED);
        
        uint16_t screen_width = console_get_width();
        int i; // Loop counter (C89 safe)

        // Define the header messages
        const char* header_msg = "Oh no! Your GatOS ventured into undefined behavior and never returned :(";
        const char* sep_msg = "---";
        const char* footer_msg = "SYSTEM HALTED";
        
        // lengths
        int header_len = 72;
        int sep_len = 3;
        int footer_len = 13;

        // Print Header
        int pad_header = (screen_width - header_len) / 2;
        if (pad_header < 0) pad_header = 0;

        printf("\n");
        for (i = 0; i < pad_header; i++) printf(" ");
        printf("%s\n", header_msg);

        // Print Separator
        int pad_sep = (screen_width - sep_len) / 2;
        if (pad_sep < 0) pad_sep = 0;

        printf("\n");
        for (i = 0; i < pad_sep; i++) printf(" ");
        printf("%s\n", sep_msg);
        
        // Print Body
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
            printf("\n[-] No CPU context available\n");
        }
        
        int pad_footer = (screen_width - footer_len) / 2;
        if (pad_footer < 0) pad_footer = 0;

        printf("\n");
        for (i = 0; i < pad_footer; i++) printf(" ");
        printf("%s", footer_msg);

        LOGF("\n******************* KERNEL PANIC *******************\n");
        LOGF("REASON: %s\n", message);
        if (context) {
            LOGF("EXCEPTION: %lu, ERROR: 0x%lx, RIP: 0x%lx\n", 
                 context->vector_number, context->error_code, context->iret_rip);
        }
        LOGF("****************************************************\n");
    }
    
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