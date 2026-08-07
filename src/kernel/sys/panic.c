/*
 * panic.c - Kernel panic implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/caps.h>
#include <kernel/drivers/console.h>
#include <kernel/drivers/serial.h>
#include <kernel/sys/panic.h>
#include <kernel/debug.h>
#include <klibc/stdio.h>
#include <klibc/string.h>
#include <stdarg.h>

#ifdef GATA_OUTPUT_SERIAL

#define PNC_WIDTH() ((uint16_t)80)
#define PNC_BEGIN() ((void)0)
#define PNC_PUTS(s) serial_write_port(SERIAL_COM1, (s))
#define PNC_PRINTF(...) pnc_printf(__VA_ARGS__)

static void pnc_printf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_write_port(SERIAL_COM1, buf);
}

#else

#define PNC_WIDTH() ((uint16_t)con_crash_width())
#define PNC_BEGIN() do {                                                  \
        con_crash_set_colors(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_RED);         \
        con_crash_clear(CONSOLE_COLOR_RED);                                   \
    } while (0)
#define PNC_PUTS(s) con_crash_puts(s)
#define PNC_PRINTF(...) con_crash_printf(__VA_ARGS__)

#endif

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

    // Disable interrupts to prevent further state corruption and ensure the panic log is not interleaved with other output
    intr_off();
    panic_log(message, context);

    uint16_t screen_width = PNC_WIDTH();

    PNC_BEGIN();

    #define HEADER_MSG "Oh no! Your GatOS ventured into undefined behavior and never returned :("
    #define SEP_MSG    "---"

    PNC_PUTS("\n");

    pad = (screen_width - (int)kstrlen(HEADER_MSG)) / 2;
    for (i = 0; i < pad; i++) PNC_PUTS(" ");
    PNC_PUTS(HEADER_MSG "\n");

    PNC_PUTS("\n");

    pad = (screen_width - (int)kstrlen(SEP_MSG)) / 2;
    for (i = 0; i < pad; i++) PNC_PUTS(" ");
    PNC_PUTS(SEP_MSG "\n");

    PNC_PRINTF("[+] Reason: %s\n", message);

    // If we have CPU context, print detailed register and error information
    if (context) {
        PNC_PRINTF("[+] Exception: %s (#%lu)\n",
                         exc_name(context->vector_number),
                         context->vector_number);
        PNC_PRINTF("[+] Error Code: 0x%04lx\n", context->error_code);

        if (context->vector_number == INT_PAGE_FAULT) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            PNC_PRINTF("[+] CR2 (fault addr): 0x%016lx\n", cr2);
            PNC_PRINTF("[+] Access: %s  Mode: %s  Cause: %s\n",
                (context->error_code & 0x02) ? "write"      : "read",
                (context->error_code & 0x04) ? "user"       : "supervisor",
                (context->error_code & 0x01) ? "protection" : "not-present");
            if (context->error_code & 0x08) PNC_PUTS("[+] Reserved bit set in PTE\n");
            if (context->error_code & 0x10) PNC_PUTS("[+] Caused by instruction fetch\n");
        }

        PNC_PRINTF("\nInstruction Pointer:\n");
        PNC_PRINTF("  RIP: 0x%016lx\n", context->iret_rip);
        PNC_PRINTF("  CS:  0x%04lx\n",  context->iret_cs);
        PNC_PRINTF("  RSP: 0x%016lx\n", context->iret_rsp);
        PNC_PRINTF("  SS:  0x%04lx\n",  context->iret_ss);


        uint64_t fl = context->iret_flags;
        PNC_PRINTF("\nCPU Flags (RFLAGS): 0x%016lx\n", fl);
        PNC_PRINTF("  Flags:%s%s%s%s%s%s%s%s%s\n",
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
        PNC_PUTS("\n(no CPU context)\n");
    }

    PNC_PUTS("\n");

    #define FOOTER_MSG "SYSTEM HALTED"
    pad = (screen_width - (int)kstrlen(FOOTER_MSG)) / 2;
    for (i = 0; i < pad; i++) PNC_PUTS(" ");
    PNC_PUTS(FOOTER_MSG "\n");

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
