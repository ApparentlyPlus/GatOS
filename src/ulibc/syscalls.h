/*
 * syscalls.h - Userspace system call interface
 *
 * This file provides inline assembly wrappers for invoking
 * kernel system calls from Ring 3.
 * 
 * Author: u/ApparentlyPlus
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
#define SYS_READ 8

#define userspace __attribute__((section(".user_text")))

userspace static inline uint64_t sc0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline uint64_t sc1(uint64_t num, uint64_t arg1) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline uint64_t sc2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline uint64_t sc3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

userspace static inline void syscall_exit(void) {
    sc0(SYS_EXIT);
    while (1);
}

userspace static inline void syscall_write(const char* buf, size_t len) {
    sc2(SYS_WRITE, (uint64_t)buf, (uint64_t)len);
}

userspace static inline void* syscall_mmap(void* addr, size_t length, size_t flags) {
    return (void*)sc3(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)flags);
}

userspace static inline void syscall_munmap(void* addr) {
    sc1(SYS_MUNMAP, (uint64_t)addr);
}

userspace static inline void syscall_set_fs_base(uint64_t base) {
    sc1(SYS_SET_FS_BASE, base);
}

userspace static inline void syscall_yield(void) {
    sc0(SYS_YIELD);
}

userspace static inline void syscall_sleep(uint64_t ms) {
    sc1(SYS_SLEEP_MS, ms);
}

userspace static inline int64_t syscall_read(char* buf, size_t len) {
    return (int64_t)sc2(SYS_READ, (uint64_t)buf, (uint64_t)len);
}
