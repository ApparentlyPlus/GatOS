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
#include <arch/x86_64/cpu/msr.h>
#include <kernel/memory/heap.h>
#include <kernel/debug.h>
#include <klibc/string.h>

static thread_t* g_current_thread = NULL;
static thread_t* g_ready_queue_head = NULL;
static thread_t* g_ready_queue_tail = NULL;

static thread_t* g_sleep_queue_head = NULL;
static thread_t* g_dead_queue_head = NULL;

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
 * sched_init - Initializes the scheduler and creates the idle thread
 */
void sched_init(void) {
    // Create Idle Task
    // The idle process shares the kernel's TTY as it does not perform I/O
    g_idle_process = process_create("idle_proc", g_active_tty);
    if (!g_idle_process) panic("Failed to create idle process!");

    g_idle_thread = thread_create(g_idle_process, "idle", idle_thread_entry, NULL, false, 0);
    if (!g_idle_thread) panic("Failed to create idle thread!");

    // Bootstrap Current Execution
    // Use the existing kernel TTY for the main process
    process_t* kernel_proc = process_create("kernel_main_proc", g_active_tty);
    if (!kernel_proc) panic("Failed to create kernel main process!");
    
    // Wrap current context into a thread so it can be preempted and resumed
    g_current_thread = thread_create_bootstrap(kernel_proc, "kernel_main");
    if (!g_current_thread) panic("Failed to bootstrap kernel main thread!");

    // Use the boot stack for kernel_main to keep the current execution stack valid.
    // The boot stack is 32 KiB; we treat the top 16 KiB as the kernel stack.
    extern char KERNEL_STACK_TOP;
    g_current_thread->kernel_stack = (void *)((uintptr_t)&KERNEL_STACK_TOP - KERNEL_STACK_SIZE);

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
 * sched_add_sleep - Adds a thread to the scheduler's sleep queue, sorted by wake time
 */
static void sched_add_sleep(thread_t* thread) {
    if (!thread) return;
    
    thread->sched_next = NULL;
    
    if (!g_sleep_queue_head) {
        g_sleep_queue_head = thread;
        return;
    }
    
    if (thread->sleep_until < g_sleep_queue_head->sleep_until) {
        thread->sched_next = g_sleep_queue_head;
        g_sleep_queue_head = thread;
        return;
    }
    
    thread_t* current = g_sleep_queue_head;
    while (current->sched_next && current->sched_next->sleep_until <= thread->sleep_until) {
        current = current->sched_next;
    }
    
    thread->sched_next = current->sched_next;
    current->sched_next = thread;
}

/*
 * sched_add_dead - Adds a thread to the dead queue to be reaped
 */
static void sched_add_dead(thread_t* thread) {
    if (!thread) return;
    
    thread->sched_next = g_dead_queue_head;
    g_dead_queue_head = thread;
}

/*
 * sched_schedule - Picks the next thread to run and performs context switch
 */
cpu_context_t* sched_schedule(cpu_context_t* current_context) {
    if (!g_scheduler_enabled) return current_context;

    uint64_t now = get_uptime_ms();

    // Save current context and handle enqueueing based on new state
    if (g_current_thread) {
        g_current_thread->context = current_context;
        g_current_thread->fs_base = read_msr(MSR_FS_BASE);
        
        // Save FPU state
        __asm__ volatile ("fxsave %0" : "=m"(g_current_thread->fpu_state));

        if (g_current_thread->state == THREAD_STATE_RUNNING) {
            g_current_thread->state = THREAD_STATE_READY;
            if (g_current_thread != g_idle_thread) {
                sched_add(g_current_thread);
            }
        } else if (g_current_thread->state == THREAD_STATE_SLEEPING) {
            sched_add_sleep(g_current_thread);
        } else if (g_current_thread->state == THREAD_STATE_DEAD) {
            sched_add_dead(g_current_thread);
        }
    }

    // Wake up sleeping threads
    while (g_sleep_queue_head && now >= g_sleep_queue_head->sleep_until) {
        thread_t* thread = g_sleep_queue_head;
        g_sleep_queue_head = thread->sched_next;
        sched_add(thread);
    }

    // Capture the old process before we potentially destroy it
    process_t* old_process = g_current_thread ? g_current_thread->process : NULL;

    // Process dead threads (reaping)
    while (g_dead_queue_head) {
        thread_t* thread = g_dead_queue_head;
        g_dead_queue_head = thread->sched_next;
        
        process_t* proc = thread->process;
        
        // Remove thread from process thread list
        if (proc) {
            if (proc->threads == thread) {
                proc->threads = thread->next;
            } else {
                thread_t* curr = proc->threads;
                while (curr && curr->next != thread) {
                    curr = curr->next;
                }
                if (curr) {
                    curr->next = thread->next;
                }
            }
        }
        
        if (thread == g_current_thread) {
            g_current_thread = NULL;
        }
        
        thread_destroy(thread);
        
        // Reap process if it has no more threads
        // We protect PID 1 (Idle) and PID 2 (Kernel Main)
        if (proc && proc->threads == NULL && proc->pid > 2) {
            if (proc == old_process) {
                old_process = NULL;
            }
            if (proc->tty) {
                char term_msg[128];
                int len = snprintf_(term_msg, sizeof(term_msg), 
                                   "\n[Process %s (PID %u) has terminated]\n", 
                                   proc->name, proc->pid);
                tty_write(proc->tty, term_msg, (size_t)len);
            }
            process_destroy(proc);
        }
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
    
    write_msr(MSR_FS_BASE, g_current_thread->fs_base);
    
    // Restore FPU state
    __asm__ volatile ("fxrstor %0" :: "m"(g_current_thread->fpu_state));

    cpu_context_t *next_ctx = g_current_thread->context;
    if (next_ctx) {
        uint16_t cs = (uint16_t)next_ctx->iret_cs;
        uint16_t ss = (uint16_t)next_ctx->iret_ss;
        bool is_user = (cs & 3) == 3;
        if (!is_user && (cs != KERNEL_CS || (ss != 0 && ss != KERNEL_DS)))
            panicf_c(next_ctx, "sched: corrupt kernel ctx for '%s' (cs=0x%x ss=0x%x)",
                     g_current_thread->name, cs, ss);
        if (is_user && (cs != USER_CS || ss != USER_DS))
            panicf_c(next_ctx, "sched: corrupt user ctx for '%s' (cs=0x%x ss=0x%x)",
                     g_current_thread->name, cs, ss);
    }
    return next_ctx;
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

    process_header_update(g_current_thread->process);

    sched_yield();
    while(1);
}
