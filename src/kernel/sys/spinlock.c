/*
 * spinlock.c - Spinlock implementation
 *
 * Implements low-level mutual exclusion using atomic test-and-set.
 * Includes interrupt-saving logic to prevent deadlocks between
 * thread context and interrupt context.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/sys/spinlock.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>

/*
 * spinlock_init - Initialize a spinlock to an unlocked state
 */
void spinlock_init(spinlock_t* lock, const char* name) {
    lock->locked = 0;
    lock->cpu_id = 0xFFFFFFFF; // No CPU holds it
    lock->name = name;
}

/*
 * spinlock_acquire - Take the lock, saving interrupt state
 */
bool spinlock_acquire(spinlock_t* lock) {
    bool was_enabled = intr_save();

    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    lock->cpu_id = lapic_get_id();
    
    return was_enabled;
}

/*
 * spinlock_try_acquire - Attempt to take the lock without spinning
 */
bool spinlock_try_acquire(spinlock_t* lock, bool* was_enabled) {
    *was_enabled = intr_save();

    if (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        intr_restore(*was_enabled);
        return false;
    }

    lock->cpu_id = lapic_get_id();
    return true;
}

/*
 * spinlock_release - Release the lock and restore interrupt state
 */
void spinlock_release(spinlock_t* lock, bool interrupts_enabled) {
    lock->cpu_id = 0xFFFFFFFF;
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
    intr_restore(interrupts_enabled);
}

/*
 * spinlock_is_locked - Check if lock is held
 */
bool spinlock_is_locked(spinlock_t* lock) {
    return lock->locked != 0;
}
