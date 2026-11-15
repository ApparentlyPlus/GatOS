/*
 * debug.c - Implementation of debugging utilities for GatOS kernel
 *
 * Author: u/ApparentlyPlus
 */

#include <klibc/stdio.h>
#include <kernel/drivers/serial.h>
#include <kernel/sys/spinlock.h>
#include <klibc/string.h>
#include <stddef.h>
#include <stdarg.h>

static int dbg_counter = 0;

// Serializes serial log writes
static spinlock_t log_lock = {0};

/*
 * QEMU_LOG - Debug function to klog messages to qemu serial with counter
 */
void QEMU_LOG(const char* msg, int total) {
    char buf[256];
    bool flags = spinlock_acquire(&log_lock);
    ksnprintf(buf, sizeof(buf), "[%d/%d] %s\n", ++dbg_counter, total, msg);
    serial_write(buf);
    spinlock_release(&log_lock, flags);
}

/*
 * QEMU_GENERIC_LOG - Debug function to klog messages to qemu serial without counter
 */
void QEMU_GENERIC_LOG(const char* msg) {
    char buf[256];
    bool flags = spinlock_acquire(&log_lock);
    ksnprintf(buf, sizeof(buf), "%s\n", msg);
    serial_write(buf);
    spinlock_release(&log_lock, flags);
}

// LOGF indentation (two spaces per level), and whether COM2 sits at the start of a line
static int log_depth = 0;
static bool log_bol = true;

// A line LOGH left open
static bool log_open = false;

/*
 * log_put - Writes s to COM2, indenting every line it starts (log_lock held)
 */
static void log_put(const char* s) {
    if (log_open) {
        serial_write_port(SERIAL_COM2, "\n");
        log_bol = true;
        log_open = false;
    }

    for (; *s; s++) {
        if (log_bol && *s != '\n') {
            for (int i = 0; i < log_depth * 2; i++) serial_write_char_port(SERIAL_COM2, ' ');
            log_bol = false;
        }
        if (*s == '\n') {
            serial_write_char_port(SERIAL_COM2, '\r');
            log_bol = true;
        }
        serial_write_char_port(SERIAL_COM2, *s);
    }
}

/*
 * log_vput - Formats and writes under the log lock, optionally leaving the line open
 */
static void log_vput(bool hold, const char* fmt, va_list args) {
    char buffer[512];
    kvsnprintf(buffer, sizeof(buffer), fmt, args);

    bool flags = spinlock_acquire(&log_lock);
    log_put(buffer);
    log_open = hold;
    spinlock_release(&log_lock, flags);
}

/*
 * LOGF - Debug function to klog messages internally with format specifiers
 * Output goes to COM2 instead of COM1, indented by the current log depth
 */
void LOGF(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vput(false, fmt, args);
    va_end(args);
}

/*
 * LOGH - Like LOGF, but holds the line open for log_resume to finish
 */
void LOGH(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vput(true, fmt, args);
    va_end(args);
}

/*
 * log_resume - True if the line LOGH held is still open, so the next LOGF continues it
 */
bool log_resume(void) {
    bool flags = spinlock_acquire(&log_lock);
    bool open = log_open;
    log_open = false;
    spinlock_release(&log_lock, flags);
    return open;
}

/*
 * log_indent - Indents (or with a negative count, dedents) everything LOGF prints from here on
 */
void log_indent(int levels) {
    bool flags = spinlock_acquire(&log_lock);
    log_depth += levels;
    if (log_depth < 0) log_depth = 0;
    spinlock_release(&log_lock, flags);
}
