/*
 * scheduler.c - Round-Robin Scheduler implementation
 *
 * This file implements the core scheduling logic, including thread switching,
 * idle task management, and sleep/wakeup mechanisms.
 *
 * Author: ApparentlyPlus
 */

#include <kernel/sys/scheduler.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/process.h>
#include <kernel/sys/panic.h>
#include <arch/x86_64/cpu/gdt.h>
#include <kernel/memory/heap.h>
#include <kernel/debug.h>
#include <libc/string.h>

static thread_t* g_current_thread = NULL;
static thread_t* g_ready_queue_head = NULL;
static thread_t* g_ready_queue_tail = NULL;

static thread_t* g_idle_thread = NULL;
static process_t* g_idle_process = NULL;

static bool g_scheduler_enabled = false;

/*
 * idle_thread_entry - The main function for the idle thread
 */
static void idle_thread_entry(void* arg) {
    (void)arg;
    while (1) {
        __asm__ volatile("hlt");
    }
}

/*
 * scheduler_init - Initializes the scheduler and creates the idle thread
 */
void scheduler_init(void) {
    // 1. Create Idle Task
    // The idle process shares the kernel's TTY as it does not perform I/O
    g_idle_process = process_create("idle_proc", g_active_tty);
    if (!g_idle_process) panic("Failed to create idle process!");

    g_idle_thread = thread_create(g_idle_process, "idle", idle_thread_entry, NULL, false);
    if (!g_idle_thread) panic("Failed to create idle thread!");

    // 2. Bootstrap Current Execution
    // Use the existing kernel TTY for the main process
    process_t* kernel_proc = process_create("kernel_main_proc", g_active_tty);
    if (!kernel_proc) panic("Failed to create kernel main process!");
    
    // Wrap current context into a thread so it can be preempted and resumed
    g_current_thread = thread_create_bootstrap(kernel_proc, "kernel_main");
    if (!g_current_thread) panic("Failed to bootstrap kernel main thread!");

    g_scheduler_enabled = true;
    LOGF("[SCHED] Scheduler initialized and enabled.\n");
}

/*
 * scheduler_is_active - Returns whether the scheduler is initialized and enabled
 */
bool scheduler_is_active(void) {
    return g_scheduler_enabled;
}

/*
 * scheduler_add_thread - Adds a thread to the scheduler's ready queue
 */
void scheduler_add_thread(thread_t* thread) {
    if (!thread) return;

    thread->state = THREAD_STATE_READY;
    thread->sched_next = NULL;

    if (!g_ready_queue_head) {
        g_ready_queue_head = thread;
        g_ready_queue_tail = thread;
    } else {
        g_ready_queue_tail->sched_next = thread;
        g_ready_queue_tail = thread;
    }
}

/*
 * scheduler_schedule - Picks the next thread to run and performs context switch
 */
cpu_context_t* scheduler_schedule(cpu_context_t* current_context) {
    if (!g_scheduler_enabled) return current_context;

    uint64_t now = get_uptime_ms();

    // 1. Save current context
    if (g_current_thread) {
        g_current_thread->context = current_context;
        
        if (g_current_thread->state == THREAD_STATE_RUNNING) {
            g_current_thread->state = THREAD_STATE_READY;
            if (g_current_thread != g_idle_thread) {
                scheduler_add_thread(g_current_thread);
            }
        }
    }

    // 2. Wake up sleeping threads and cleanup dead threads
    process_t* proc = process_get_all();
    while (proc) {
        thread_t** prev = &proc->threads;
        thread_t* thread = proc->threads;
        while (thread) {
            if (thread->state == THREAD_STATE_SLEEPING && now >= thread->sleep_until) {
                thread->state = THREAD_STATE_READY;
                scheduler_add_thread(thread);
            } else if (thread->state == THREAD_STATE_DEAD) {
                // Production robustness: Clean up dead threads
                // (Except the current one if it just exited, though it won't be RUNNING)
                if (thread != g_current_thread) {
                    *prev = thread->next;
                    thread_t* to_free = thread;
                    thread = thread->next;
                    thread_destroy(to_free);
                    continue;
                }
            }
            prev = &thread->next;
            thread = thread->next;
        }
        proc = proc->next;
    }

    // 3. Pick next thread from ready queue
    thread_t* next_thread = g_ready_queue_head;
    if (next_thread) {
        g_ready_queue_head = next_thread->sched_next;
        if (!g_ready_queue_head) {
            g_ready_queue_tail = NULL;
        }
        next_thread->sched_next = NULL;
    } else {
        // No ready threads, run idle thread
        next_thread = g_idle_thread;
    }

    // 4. Optimization: Only switch VMM if address space actually changed
    if (!g_current_thread || g_current_thread->process != next_thread->process) {
        if (next_thread->process && next_thread->process->vmm) {
            vmm_switch(next_thread->process->vmm);
        }
    }

    g_current_thread = next_thread;
    g_current_thread->state = THREAD_STATE_RUNNING;

    // 5. Update Hardware State
    if (g_current_thread->kernel_stack) {
        tss_set_rsp0((uint64_t)g_current_thread->kernel_stack + KERNEL_STACK_SIZE);
    }

    return g_current_thread->context;
}

/*
 * scheduler_yield - Voluntarily gives up the remaining time slice
 */
void scheduler_yield(void) {
    if (!g_scheduler_enabled) return;
    __asm__ volatile("int $32");
}

/*
 * scheduler_get_current_thread - Returns the currently running thread
 */
thread_t* scheduler_get_current_thread(void) {
    return g_current_thread;
}

/*
 * scheduler_thread_sleep - Puts the current thread to sleep for X ms
 */
void scheduler_thread_sleep(uint64_t ms) {
    if (!g_current_thread || !g_scheduler_enabled) return;

    g_current_thread->state = THREAD_STATE_SLEEPING;
    g_current_thread->sleep_until = get_uptime_ms() + ms;
    
    scheduler_yield();
}

/*
 * scheduler_thread_exit - Terminates the current thread
 */
void scheduler_thread_exit(void) {
    if (!g_current_thread) return;

    g_current_thread->state = THREAD_STATE_DEAD;
    LOGF("[SCHED] Thread '%s' (TID: %u) exited.\n", g_current_thread->name, g_current_thread->tid);
    
    scheduler_yield();
    while(1);
}
