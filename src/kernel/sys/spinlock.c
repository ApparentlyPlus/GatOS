/*
 * spinlock.c - Spinlock implementation
 *
 * Implements low-level mutual exclusion using atomic test-and-set.
 * Includes interrupt-saving logic to prevent deadlocks between
 * thread context and interrupt context.
 *
 * Author: ApparentlyPlus
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
    // Disable interrupts and save state
    bool was_enabled = interrupts_save();

    // Spin until we get the lock
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        // CPU hint that we are in a spin loop
        __asm__ volatile("pause");
    }

    // Mark ownership
    lock->cpu_id = lapic_get_id();
    
    return was_enabled;
}

/*
 * spinlock_release - Release the lock and restore interrupt state
 */
void spinlock_release(spinlock_t* lock, bool interrupts_enabled) {
    // Clear ownership
    lock->cpu_id = 0xFFFFFFFF;

    // Atomic release
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);

    // Restore interrupts
    interrupts_restore(interrupts_enabled);
}

/*
 * spinlock_is_locked - Check if lock is held
 */
bool spinlock_is_locked(spinlock_t* lock) {
    return lock->locked != 0;
}
