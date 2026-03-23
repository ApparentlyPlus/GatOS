/*
 * panic.c - Kernel panic implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/drivers/console.h>
#include <kernel/drivers/serial.h>
#include <kernel/sys/panic.h>
#include <kernel/debug.h>
#include <klibc/stdio.h>
#include <klibc/string.h>
#include <stdarg.h>

/*
 * halt_system - Halts the CPU indefinitely
 */
void halt_system(void)
{
    while (1) __asm__ volatile("hlt");
}

/*
 * exc_name - Returns a human-readable name for an exception vector
 */
static const char* exc_name(uint64_t vec)
{
    static const char* names[] = {
        "Divide-by-Zero",       "Debug",               "NMI",
        "Breakpoint",           "Overflow",             "Bound Range",
        "Invalid Opcode",       "Device Not Available", "Double Fault",
        "Coprocessor Segment",  "Invalid TSS",          "Segment Not Present",
        "Stack-Segment Fault",  "General Protection",   "Page Fault",
        "Reserved",             "x87 FPU Error",        "Alignment Check",
        "Machine Check",        "SIMD Exception",
    };
    if (vec < sizeof(names) / sizeof(names[0])) return names[vec];
    if (vec < 32) return "Reserved Exception";
    return "External Interrupt";
}

/*
 * panic_log - Writes panic info unconditionally to the serial port
 */
static void panic_log(const char* msg, cpu_context_t* ctx)
{
    LOGF("\n*** KERNEL PANIC ***\n");
    LOGF("REASON: %s\n", msg);
    if (ctx)
        LOGF("%s (#%lu)  ERR=0x%lx  RIP=0x%016lx\n",
             exc_name(ctx->vector_number),
             ctx->vector_number,
             ctx->error_code,
             ctx->iret_rip);
    LOGF("********************\n");
}

/*
 * panic_c - Core panic handler: logs to serial, then renders the crash screen
 */
void panic_c(const char* message, cpu_context_t* context)
{
    int i;
    int pad;

    intr_off();
    panic_log(message, context);

    uint16_t screen_width = (uint16_t)con_crash_width();

    con_crash_clear(CONSOLE_COLOR_RED);

    #define HEADER_MSG "Oh no! Your GatOS ventured into undefined behavior and never returned :("
    #define SEP_MSG    "---"

    con_crash_puts("\n");

    pad = (screen_width - (int)kstrlen(HEADER_MSG)) / 2;
    for (i = 0; i < pad; i++) con_crash_puts(" ");
    con_crash_puts(HEADER_MSG "\n");

    con_crash_puts("\n");

    pad = (screen_width - (int)kstrlen(SEP_MSG)) / 2;
    for (i = 0; i < pad; i++) con_crash_puts(" ");
    con_crash_puts(SEP_MSG "\n");

    con_crash_printf("[+] Reason: %s\n", message);

    if (context) {
        con_crash_printf("[+] Exception: %s (#%lu)\n",
                         exc_name(context->vector_number),
                         context->vector_number);
        con_crash_printf("[+] Error Code: 0x%04lx\n", context->error_code);

        if (context->vector_number == INT_PAGE_FAULT) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            con_crash_printf("[+] CR2 (fault addr): 0x%016lx\n", cr2);
            con_crash_printf("[+] Access: %s  Mode: %s  Cause: %s\n",
                (context->error_code & 0x02) ? "write"      : "read",
                (context->error_code & 0x04) ? "user"       : "supervisor",
                (context->error_code & 0x01) ? "protection" : "not-present");
            if (context->error_code & 0x08) con_crash_puts("[+] Reserved bit set in PTE\n");
            if (context->error_code & 0x10) con_crash_puts("[+] Caused by instruction fetch\n");
        }

        con_crash_printf("\nInstruction Pointer:\n");
        con_crash_printf("  RIP: 0x%016lx\n", context->iret_rip);
        con_crash_printf("  CS:  0x%04lx\n",  context->iret_cs);
        con_crash_printf("  RSP: 0x%016lx\n", context->iret_rsp);
        con_crash_printf("  SS:  0x%04lx\n",  context->iret_ss);


        uint64_t fl = context->iret_flags;
        con_crash_printf("\nCPU Flags (RFLAGS): 0x%016lx\n", fl);
        con_crash_printf("  Flags:%s%s%s%s%s%s%s%s%s\n",
            (fl & (1 <<  0)) ? " CF" : "",
            (fl & (1 <<  2)) ? " PF" : "",
            (fl & (1 <<  4)) ? " AF" : "",
            (fl & (1 <<  6)) ? " ZF" : "",
            (fl & (1 <<  7)) ? " SF" : "",
            (fl & (1 <<  8)) ? " TF" : "",
            (fl & (1 <<  9)) ? " IF" : "",
            (fl & (1 << 10)) ? " DF" : "",
            (fl & (1 << 11)) ? " OF" : "");
    } else {
        con_crash_puts("\n(no CPU context)\n");
    }

    con_crash_puts("\n");

    #define FOOTER_MSG "SYSTEM HALTED"
    pad = (screen_width - (int)kstrlen(FOOTER_MSG)) / 2;
    for (i = 0; i < pad; i++) con_crash_puts(" ");
    con_crash_puts(FOOTER_MSG "\n");

    halt_system();
}

/*
 * panic - Simple panic with no CPU context
 */
void panic(const char* message)
{
    panic_c(message, NULL);
}

/*
 * panicf - Formatted panic with no CPU context
 */
void panicf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    panic_c(buf, NULL);
}

/*
 * panicf_c - Formatted panic preserving the CPU context for the crash screen
 */
void panicf_c(cpu_context_t* context, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    panic_c(buf, context);
}
