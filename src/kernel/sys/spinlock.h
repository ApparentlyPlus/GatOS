/*
 * spinlock.h - Spinlock Primitives
 *
 * Provides mutual exclusion for kernel data structures. Handles interrupt
 * safety by disabling interrupts on the local core during lock acquisition
 * and restoring the previous state upon release.
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    volatile int locked;    // Atomic lock variable (0 = free, 1 = taken)
    uint32_t cpu_id;        // ID of the CPU holding the lock (for debugging/SMP)
    const char* name;       // Name of the lock (for debugging)
} spinlock_t;


void spinlock_init(spinlock_t* lock, const char* name);
bool spinlock_acquire(spinlock_t* lock);
bool spinlock_try_acquire(spinlock_t* lock, bool* was_enabled);
void spinlock_release(spinlock_t* lock, bool interrupts_enabled);
bool spinlock_is_locked(spinlock_t* lock);
