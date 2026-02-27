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
#include <arch/x86_64/cpu/cpu.h>
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
    // Create Idle Task
    // The idle process shares the kernel's TTY as it does not perform I/O
    g_idle_process = process_create("idle_proc", g_active_tty);
    if (!g_idle_process) panic("Failed to create idle process!");

    g_idle_thread = thread_create(g_idle_process, "idle", idle_thread_entry, NULL, false);
    if (!g_idle_thread) panic("Failed to create idle thread!");

    // Bootstrap Current Execution
    // Use the existing kernel TTY for the main process
    process_t* kernel_proc = process_create("kernel_main_proc", g_active_tty);
    if (!kernel_proc) panic("Failed to create kernel main process!");
    
    // Wrap current context into a thread so it can be preempted and resumed
    g_current_thread = thread_create_bootstrap(kernel_proc, "kernel_main");
    if (!g_current_thread) panic("Failed to bootstrap kernel main thread!");

    // Allocate a dedicated kernel stack for kernel_main so it has its own safe space 
    // when switched back from userspace threads. This also ensures the idle thread can have its own stack.
    g_current_thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!g_current_thread->kernel_stack) panic("Failed to allocate kernel stack for kernel_main!");
    memset(g_current_thread->kernel_stack, 0, KERNEL_STACK_SIZE);

    g_scheduler_enabled = true;
    LOGF("[SCHED] Scheduler initialized and enabled.\n");
}

/*
 * sched_active - Returns whether the scheduler is initialized and enabled
 */
bool sched_active(void) {
    return g_scheduler_enabled;
}

/*
 * sched_add - Adds a thread to the scheduler's ready queue
 */
void sched_add(thread_t* thread) {
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
 * sched_schedule - Picks the next thread to run and performs context switch
 */
cpu_context_t* sched_schedule(cpu_context_t* current_context) {
    if (!g_scheduler_enabled) return current_context;

    uint64_t now = get_uptime_ms();

    // Save current context
    if (g_current_thread) {
        g_current_thread->context = current_context;
        
        if (g_current_thread->state == THREAD_STATE_RUNNING) {
            g_current_thread->state = THREAD_STATE_READY;
            if (g_current_thread != g_idle_thread) {
                sched_add(g_current_thread);
            }
        }
    }

    // Capture the process before we potentially destroy it
    process_t* old_process = g_current_thread ? g_current_thread->process : NULL;

    // Wake up sleeping threads and cleanup dead threads
    process_t* proc = process_get_all();
    while (proc) {
        process_t* next_proc = proc->next; // Save next in case proc is destroyed
        
        thread_t** prev_thread = &proc->threads;
        thread_t* thread = proc->threads;
        
        while (thread) {
            if (thread->state == THREAD_STATE_SLEEPING && now >= thread->sleep_until) {
                thread->state = THREAD_STATE_READY;
                sched_add(thread);
            } else if (thread->state == THREAD_STATE_DEAD) {
                // Remove from process thread list
                *prev_thread = thread->next;
                thread_t* to_free = thread;
                thread = thread->next;
                
                // If we are destroying the thread we just came from, clear the global pointer
                if (to_free == g_current_thread) {
                    g_current_thread = NULL;
                }
                
                // If this was the current thread, it's safe to destroy now 
                // because its context is already saved and we are about to switch.
                thread_destroy(to_free);
                continue;
            }
            
            prev_thread = &thread->next;
            thread = thread->next;
        }

        // Reap processes with no threads
        // We protect PID 1 (Idle) and PID 2 (Kernel Main)
        if (proc->threads == NULL && proc->pid > 2) {
            // If we are reaping the process we just came from, clear the old_process pointer
            if (proc == old_process) {
                old_process = NULL;
            }

            // Inform the user via the process's terminal
            if (proc->tty) {
                char term_msg[128];
                int len = snprintf_(term_msg, sizeof(term_msg), 
                                   "\n[Process %s (PID %u) has terminated]\n", 
                                   proc->name, proc->pid);
                tty_write(proc->tty, term_msg, (size_t)len);
            }
            process_destroy(proc);
        }

        proc = next_proc;
    }

    // Pick next thread from ready queue
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

    // Context Switch
    // Only switch VMM if address space actually changed
    if (old_process != next_thread->process) {
        if (next_thread->process && next_thread->process->vmm) {
            vmm_switch(next_thread->process->vmm);
        } else {
            // Fallback to kernel VMM if next thread has no process or no VMM
            vmm_switch(NULL);
        }
    }

    g_current_thread = next_thread;
    g_current_thread->state = THREAD_STATE_RUNNING;

    // Update Hardware State
    if (g_current_thread->kernel_stack) {
        uint64_t stack_top = (uint64_t)g_current_thread->kernel_stack + KERNEL_STACK_SIZE;
        tss_set_rsp0(stack_top);
        
        // Update local CPU structure for syscall entries
        g_cpu_local.kernel_stack = stack_top;
    }

    return g_current_thread->context;
}

/*
 * sched_yield - Voluntarily gives up the remaining time slice
 */
void sched_yield(void) {
    if (!g_scheduler_enabled) return;
    __asm__ volatile("int $32");
}

/*
 * sched_current - Returns the currently running thread
 */
thread_t* sched_current(void) {
    return g_current_thread;
}

/*
 * sched_sleep - Puts the current thread to sleep for X ms
 */
void sched_sleep(uint64_t ms) {
    if (!g_current_thread || !g_scheduler_enabled) return;

    g_current_thread->state = THREAD_STATE_SLEEPING;
    g_current_thread->sleep_until = get_uptime_ms() + ms;
    
    sched_yield();
}

/*
 * sched_exit - Terminates the current thread
 */
void sched_exit(void) {
    if (!g_current_thread) return;

    g_current_thread->state = THREAD_STATE_DEAD;
    LOGF("[SCHED] Thread '%s' (TID: %u) exited.\n", g_current_thread->name, g_current_thread->tid);
    
    sched_yield();
    while(1);
}
