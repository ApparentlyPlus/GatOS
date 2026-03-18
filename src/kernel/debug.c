/*
 * debug.c - Implementation of debugging utilities for GatOS kernel
 *
 * Implements all debugging related functions declared in debug.h
 * 
 * Author: u/ApparentlyPlus
 */

#include <klibc/stdio.h>
#include <kernel/drivers/serial.h>
#include <kernel/misc.h>
#include <klibc/string.h>
#include <stddef.h>
#include <stdarg.h>

static int dbg_counter = 0;

/*
 * QEMU_LOG - Debug function to klog messages to qemu serial with counter
 */
void QEMU_LOG(const char* msg, int total) {
    char buf[256];
    ksnprintf(buf, sizeof(buf), "[%d/%d] %s\n", ++dbg_counter, total, msg);
    serial_write(buf);
}

/*
 * QEMU_GENERIC_LOG - Debug function to klog messages to qemu serial without counter
 */
void QEMU_GENERIC_LOG(const char* msg) {
    char buf[128];
    char* ptr = buf;

    size_t msg_len = kstrlen(msg);
    kmemcpy(ptr, msg, msg_len);
    ptr += msg_len;

    *ptr++ = '\n';
    *ptr = '\0';

    serial_write(buf);
}

/*
 * LOGF - Debug function to klog messages internally with format specifiers
 */
void LOGF(const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    
    va_start(args, fmt);
    kvsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // Output to COM2 instead of COM1 for internal logging
    serial_write_port(SERIAL_COM2, buffer);
}
