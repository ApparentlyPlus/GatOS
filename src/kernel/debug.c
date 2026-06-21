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


/*
 * QEMU_LOG - Debug function to klog messages to qemu serial
 */
void QEMU_LOG(const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    
    va_start(args, fmt);
    kvsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // Output to COM1
    serial_write_port(SERIAL_COM1, buffer);
    serial_write_port(SERIAL_COM1, "\n");
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
