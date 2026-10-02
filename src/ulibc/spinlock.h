/*
 * spinlock.h - Userspace spinlock primitives
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} ulock_t;

static inline void ulock_acquire(ulock_t* l) {
    while (__sync_lock_test_and_set(&l->locked, 1))
        ;
}

static inline void ulock_release(ulock_t* l) {
    __sync_lock_release(&l->locked);
}
