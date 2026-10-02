/*
 * debug.h - Userspace debug-only serial channel
 *
 * Writes straight to COM3 via SYS_DEBUG_WRITE, bypassing the TTY, so it works whatever the console
 * state. For Gata `debug` statements in the user realm (see kernel/sys/syscall.c, SERIAL_COM3 in
 * kernel/drivers/serial.h).
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

void u_debug_write(const char* buf, unsigned long len);
void u_debug_log(const char* fmt, ...);
