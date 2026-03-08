/*
 * syscalls.h - Userspace system call interface
 *
 * This file provides inline assembly wrappers for invoking
 * kernel system calls from Ring 3.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define SYS_EXIT 1
#define SYS_WRITE 2
#define SYS_MMAP 3
#define SYS_MUNMAP 4
#define SYS_SET_FS_BASE 5
#define SYS_YIELD 6
#define SYS_SLEEP_MS 7

#define userspace __attribute__((section(".user_text")))

userspace static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline uint64_t syscall1(uint64_t num, uint64_t arg1) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline uint64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline uint64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline void sys_exit(void) {
    syscall0(SYS_EXIT);
    while (1);
}

userspace static inline void sys_write(const char* buf, size_t len) {
    syscall2(SYS_WRITE, (uint64_t)buf, (uint64_t)len);
}

userspace static inline void* sys_mmap(void* addr, size_t length, size_t flags) {
    return (void*)syscall3(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)flags);
}

userspace static inline void sys_munmap(void* addr) {
    syscall1(SYS_MUNMAP, (uint64_t)addr);
}

userspace static inline void sys_set_fs_base(uint64_t base) {
    syscall1(SYS_SET_FS_BASE, base);
}

userspace static inline void sys_yield(void) {
    syscall0(SYS_YIELD);
}

userspace static inline void sys_sleep_ms(uint64_t ms) {
    syscall1(SYS_SLEEP_MS, ms);
}
