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
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/memory/heap.h>
#include <kernel/debug.h>
#include <klibc/avl.h>
#include <klibc/string.h>
#include <klibc/stdio.h>

static thread_t* g_current_thread = NULL;
static thread_t* g_ready_queue_head = NULL;
static thread_t* g_ready_queue_tail = NULL;

static avl_tree_t g_sleep_tree;
static thread_t*  g_dead_queue_head = NULL;

static thread_t* g_idle_thread = NULL;
static process_t* g_idle_process = NULL;

static bool g_scheduler_enabled = false;

// Lazy FPU, track which thread's state is live in the FPU hardware
static thread_t* g_fpu_owner = NULL;

/*
 * sleep_cmp - Comparison function for the sleep tree
 */
static int sleep_cmp(const avl_node_t* a, const avl_node_t* b) {
    const thread_t* ta = AVL_ENTRY(a, thread_t, sleep_node);
    const thread_t* tb = AVL_ENTRY(b, thread_t, sleep_node);
    if (ta->sleep_until < tb->sleep_until) return -1;
    if (ta->sleep_until > tb->sleep_until) return  1;
    if (ta->tid < tb->tid) return -1;
    if (ta->tid > tb->tid) return  1;
    return 0;
}

/*
 * fpu_nm_handler - Handles the FPU not available interrupt
 */
static cpu_context_t* fpu_nm_handler(cpu_context_t* ctx) {
    __asm__ volatile("clts" ::: "memory");

    if (!g_current_thread) {
        if (g_fpu_owner) {
            __asm__ volatile("fxsave %0" : "=m"(g_fpu_owner->fpu_state));
            g_fpu_owner = NULL;
        }
        return ctx;
    }

    if (g_fpu_owner != g_current_thread) {
        if (g_fpu_owner)
            __asm__ volatile("fxsave %0" : "=m"(g_fpu_owner->fpu_state));
        __asm__ volatile("fxrstor %0" :: "m"(g_current_thread->fpu_state));
        g_fpu_owner = g_current_thread;
    }
    return ctx;
}

static void sched_add_dead(thread_t* thread);
static void sched_add_sleep(thread_t* thread);

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

    avl_init(&g_sleep_tree, sleep_cmp);
    irq_register(INT_DEVICE_NOT_AVAILABLE, fpu_nm_handler);

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

    if (thread->state == THREAD_STATE_DEAD) {
        sched_add_dead(thread);
        return;
    }

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
 * sched_add_sleep - Inserts a thread into the AVL sleep tre
 */
static void sched_add_sleep(thread_t* thread) {
    if (!thread) return;
    thread->sched_next = NULL;
    avl_insert(&g_sleep_tree, &thread->sleep_node);
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
 * sched_remove_process - Removes all threads belonging to a process from all scheduler queues
 */
void sched_remove_process(process_t* proc) {
    if (!proc) return;

    // Remove from ready queue
    thread_t* prev = NULL;
    thread_t* curr = g_ready_queue_head;
    while (curr) {
        if (curr->process == proc) {
            if (prev) {
                prev->sched_next = curr->sched_next;
            } else {
                g_ready_queue_head = curr->sched_next;
            }
            if (g_ready_queue_tail == curr) {
                g_ready_queue_tail = prev;
            }
            curr = curr->sched_next;
        } else {
            prev = curr;
            curr = curr->sched_next;
        }
    }

    // Remove from sleep tree
    avl_node_t* sn = avl_min(&g_sleep_tree);
    while (sn) {
        thread_t* t = AVL_ENTRY(sn, thread_t, sleep_node);
        avl_node_t* next_sn = avl_next(sn);
        if (t->process == proc)
            avl_remove(&g_sleep_tree, sn);
        sn = next_sn;
    }

    // Remove from dead queue
    prev = NULL;
    curr = g_dead_queue_head;
    while (curr) {
        if (curr->process == proc) {
            if (prev) {
                prev->sched_next = curr->sched_next;
            } else {
                g_dead_queue_head = curr->sched_next;
            }
            curr = curr->sched_next;
        } else {
            prev = curr;
            curr = curr->sched_next;
        }
    }
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
    avl_node_t* sn = avl_min(&g_sleep_tree);
    while (sn) {
        thread_t* sleeper = AVL_ENTRY(sn, thread_t, sleep_node);
        if (sleeper->sleep_until > now) break;
        avl_node_t* next_sn = avl_next(sn);
        avl_remove(&g_sleep_tree, sn);
        sched_add(sleeper);
        sn = next_sn;
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
        if (thread == g_fpu_owner) {
            g_fpu_owner = NULL;
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
                int len = ksnprintf(term_msg, sizeof(term_msg), 
                                   "\n[Process %s (PID %u) has terminated]\n", 
                                   proc->name, proc->pid);
                tty_write(proc->tty, term_msg, (size_t)len);
            }
            process_destroy(proc);
        }
    }

    // Pick next thread from ready queue
    thread_t* next_thread = NULL;
    while (g_ready_queue_head) {
        next_thread = g_ready_queue_head;
        g_ready_queue_head = next_thread->sched_next;
        if (!g_ready_queue_head) {
            g_ready_queue_tail = NULL;
        }
        next_thread->sched_next = NULL;

        if (next_thread->state == THREAD_STATE_DEAD) {
            sched_add_dead(next_thread);
            next_thread = NULL;
        } else {
            break;
        }
    }
    
    if (!next_thread) {
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

    // fpu_nm_handler will lazily restore state on first use
    set_cr0_ts();

    // Update Hardware State
    if (g_current_thread->kernel_stack) {
        uint64_t stack_top = (uint64_t)g_current_thread->kernel_stack + KERNEL_STACK_SIZE;
        tss_set_rsp0(stack_top);

        // Update local CPU structure for syscall entries
        g_cpu_local.kernel_stack = stack_top;
    }

    write_msr(MSR_FS_BASE, g_current_thread->fs_base);

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
