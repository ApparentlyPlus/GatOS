/*
 * spinlock.h - Userspace spinlock primitives
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <ulibc/syscalls.h>

typedef struct {
    volatile uint32_t locked;
} ulock_t;

static inline void ulock_acquire(ulock_t* l) {
    // On one core, the holder can't let go until it runs again, so spinning would only burn
    // the rest of our time slice. Give the CPU back instead
    while (__sync_lock_test_and_set(&l->locked, 1))
        syscall_yield();
}

static inline void ulock_release(ulock_t* l) {
    __sync_lock_release(&l->locked);
}
