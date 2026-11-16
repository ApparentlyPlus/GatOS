/*
 * spinlock.c - Spinlock implementation
 *
 * Saves and disables interrupts while held, so thread and IRQ context can't deadlock.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/sys/spinlock.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/debug.h>

void spinlock_init(spinlock_t* lock, const char* name) {
    lock->locked = 0;
    lock->name = name;
}

/*
 * spinlock_acquire - Take the lock, saving interrupt state
 */
bool spinlock_acquire(spinlock_t* lock) {
    bool was_enabled = intr_save();

    // Atomically swap in a 1. Getting a 0 back means the lock was free, and now it's ours
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    return was_enabled;
}

/*
 * spinlock_try_acquire - Attempt to take the lock without spinning
 */
bool spinlock_try_acquire(spinlock_t* lock, bool* was_enabled) {
    *was_enabled = intr_save();

    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        intr_restore(*was_enabled);
        return false;
    }

    return true;
}

/*
 * spinlock_release - Release the lock and restore interrupt state
 */
void spinlock_release(spinlock_t* lock, bool interrupts_enabled) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    intr_restore(interrupts_enabled);
}

/*
 * spinlock_is_locked - Check if lock is held
 */
bool spinlock_is_locked(spinlock_t* lock) {
    return lock->locked != 0;
}
