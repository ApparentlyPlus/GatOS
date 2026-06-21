/*
 * debug.h - Userspace debug-only serial channel
 *
 * Mirrors the kernel's own debug logging (LOGF, on COM2) for userspace:
 * writes straight to COM3 via SYS_DEBUG_WRITE, bypassing the TTY entirely
 * so it's observable regardless of TTY/console state. Meant for Gata
 * `debug` statements running in the user realm - see kernel/sys/syscall.c
 * (SYS_DEBUG_WRITE) and kernel/drivers/serial.h (SERIAL_COM3).
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

void u_debug_write(const char* buf, unsigned long len);
void u_debug_log(const char* fmt, ...);
