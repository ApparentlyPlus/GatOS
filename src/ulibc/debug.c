/*
 * debug.c - Userspace debug-only serial channel implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <ulibc/debug.h>
#include <ulibc/syscalls.h>
#include <ulibc/stdio.h>
#include <stdarg.h>
#include <stddef.h>

userspace void u_debug_write(const char* buf, unsigned long len) {
    if (!buf || !len) return;
    syscall_debug_write(buf, (size_t)len);
}

userspace void u_debug_log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = uvsnprintf_(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) syscall_debug_write(buf, (size_t)n);
}
