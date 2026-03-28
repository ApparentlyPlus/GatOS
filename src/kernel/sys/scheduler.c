/*
 * scheduler.c - Round-Robin Scheduler implementation
 *
 * This file implements the core scheduling logic, including thread switching,
 * idle task management, and sleep/wakeup mechanisms.
 *
 * Author: u/ApparentlyPlus
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

// Scheduler state
static thread_t* cur = NULL;
static thread_t* rq_head = NULL;
static thread_t* rq_tail = NULL;

static avl_tree_t sleep_tree;
static thread_t*  dead_head = NULL;

static thread_t* idle = NULL;
static process_t* idle_proc = NULL;

static bool sched_on = false;

// Lazy FPU, track which thread's state is live in the FPU hardware
static thread_t* fpu_owner = NULL;

/*
 * sleep_cmp - Comparison function for the sleep tree
 */
static int sleep_cmp(const avl_node_t* a, const avl_node_t* b) {
    const thread_t* ta = AVL_ENTRY(a, thread_t, sleep_node);
    const thread_t* tb = AVL_ENTRY(b, thread_t, sleep_node);
    if (ta->wake_at < tb->wake_at) return -1;
    if (ta->wake_at > tb->wake_at) return  1;
    if (ta->tid < tb->tid) return -1;
    if (ta->tid > tb->tid) return  1;
    return 0;
}

/*
 * fpu_nm_handler - Handles the FPU not available interrupt
 */
static cpu_context_t* fpu_nm_handler(cpu_context_t* ctx) {

    // In here, we lazily save/restore the FPU state only when a thread that doesn't own the FPU tries to use it
    // This avoids unnecessary FPU state saves/restores on every context switch, which can be expensive

    __asm__ volatile("clts" ::: "memory");

    if (!cur) {
        if (fpu_owner) {
            __asm__ volatile("fxsave %0" : "=m"(fpu_owner->fpu));
            fpu_owner = NULL;
        }
        return ctx;
    }

    if (fpu_owner != cur) {
        if (fpu_owner)
            __asm__ volatile("fxsave %0" : "=m"(fpu_owner->fpu));
        __asm__ volatile("fxrstor %0" :: "m"(cur->fpu));
        fpu_owner = cur;
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
    idle_proc = process_create("idle_proc", active_tty);
    if (!idle_proc) panic("Failed to create idle process!");

    idle = thread_create(idle_proc, "idle", idle_thread_entry, NULL, false, 0);
    if (!idle) panic("Failed to create idle thread!");

    process_t* kproc = process_create("kproc", active_tty);
    if (!kproc) panic("Failed to create kernel main process!");

    // Wrap current context into a thread so it can be preempted and resumed
    cur = thread_create_bootstrap(kproc, "kernel_main");
    if (!cur) panic("Failed to bootstrap kernel main thread!");

    // Use the boot stack for kernel_main to keep the current execution stack valid
    // The boot stack is 32 KiB; we treat the top 16 KiB as the kernel stack
    extern char KERNEL_STACK_TOP;
    cur->kstack = (void *)((uintptr_t)&KERNEL_STACK_TOP - KERNEL_STACK_SIZE);

    avl_init(&sleep_tree, sleep_cmp);
    irq_register(INT_DEVICE_NOT_AVAILABLE, fpu_nm_handler);

    sched_on = true;
    LOGF("[SCHED] Scheduler initialized and enabled.\n");
}

/*
 * sched_active - Returns whether the scheduler is initialized and enabled
 */
bool sched_active(void) {
    return sched_on;
}

/*
 * sched_add - Adds a thread to the scheduler's ready queue
 */
void sched_add(thread_t* thread) {
    if (!thread) return;

    if (thread->state == T_DEAD) {
        sched_add_dead(thread);
        return;
    }

    thread->state = T_READY;
    thread->rnext = NULL;

    if (!rq_head) {
        rq_head = thread;
        rq_tail = thread;
    } else {
        rq_tail->rnext = thread;
        rq_tail = thread;
    }
}

/*
 * sched_add_sleep - Inserts a thread into the AVL sleep tre
 */
static void sched_add_sleep(thread_t* thread) {
    if (!thread) return;
    thread->rnext = NULL;
    avl_insert(&sleep_tree, &thread->sleep_node);
}

/*
 * sched_add_dead - Adds a thread to the dead queue to be reaped
 */
static void sched_add_dead(thread_t* thread) {
    if (!thread) return;

    thread->rnext = dead_head;
    dead_head = thread;
}

/*
 * sched_drop_proc - Removes all threads belonging to a process from all scheduler queues
 */
void sched_drop_proc(process_t* proc) {
    if (!proc) return;

    thread_t* prev = NULL;
    thread_t* curr = rq_head;
    while (curr) {
        if (curr->process == proc) {
            if (prev) {
                prev->rnext = curr->rnext;
            } else {
                rq_head = curr->rnext;
            }
            if (rq_tail == curr) {
                rq_tail = prev;
            }
            curr = curr->rnext;
        } else {
            prev = curr;
            curr = curr->rnext;
        }
    }

    // Remove from sleep tree
    avl_node_t* sn = avl_min(&sleep_tree);
    while (sn) {
        thread_t* t = AVL_ENTRY(sn, thread_t, sleep_node);
        avl_node_t* next_sn = avl_next(sn);
        if (t->process == proc)
            avl_remove(&sleep_tree, sn);
        sn = next_sn;
    }

    // Remove from dead queue
    prev = NULL;
    curr = dead_head;
    while (curr) {
        if (curr->process == proc) {
            if (prev) {
                prev->rnext = curr->rnext;
            } else {
                dead_head = curr->rnext;
            }
            curr = curr->rnext;
        } else {
            prev = curr;
            curr = curr->rnext;
        }
    }
}

/*
 * sched_schedule - Picks the next thread to run and performs context switch
 */
cpu_context_t* sched_schedule(cpu_context_t* ctx) {
    if (!sched_on) return ctx;

    uint64_t now = get_uptime_ms();

    if (cur) {
        cur->context = ctx;
        cur->fs_base = read_msr(MSR_FS_BASE);

        if (cur->state == T_RUNNING) {
            cur->state = T_READY;
            if (cur != idle) {
                sched_add(cur);
            }
        } else if (cur->state == T_SLEEPING) {
            sched_add_sleep(cur);
        } else if (cur->state == T_DEAD) {
            sched_add_dead(cur);
        }
    }

    // Check the sleep tree for any threads that need to be woken up
    avl_node_t* sn = avl_min(&sleep_tree);
    while (sn) {
        thread_t* sleeper = AVL_ENTRY(sn, thread_t, sleep_node);
        if (sleeper->wake_at > now) break;
        avl_node_t* next_sn = avl_next(sn);
        avl_remove(&sleep_tree, sn);
        sched_add(sleeper);
        sn = next_sn;
    }

    process_t* old_proc = cur ? cur->process : NULL;

    // If there are no ready threads, run the idle thread
    while (dead_head) {
        thread_t* thread = dead_head;
        dead_head = thread->rnext;

        process_t* proc = thread->process;

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

        if (thread == cur) {
            cur = NULL;
        }
        if (thread == fpu_owner) {
            fpu_owner = NULL;
        }

        thread_destroy(thread);

        // We protect PID 1 (Idle) and PID 2 (Kernel Main)
        if (proc && proc->threads == NULL && proc->pid > 2) {
            if (proc == old_proc) {
                old_proc = NULL;
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

    thread_t* nxt = NULL;
    while (rq_head) {
        nxt = rq_head;
        rq_head = nxt->rnext;
        if (!rq_head) {
            rq_tail = NULL;
        }
        nxt->rnext = NULL;

        if (nxt->state == T_DEAD) {
            sched_add_dead(nxt);
            nxt = NULL;
        } else {
            break;
        }
    }

    if (!nxt) {
        nxt = idle;
    }

    // Only switch VMM if address space actually changed
    if (old_proc != nxt->process) {
        if (nxt->process && nxt->process->vmm) {
            vmm_switch(nxt->process->vmm);
        } else {
            // Fallback to kernel VMM if next thread has no process or no VMM
            vmm_switch(NULL);
        }
    }

    cur = nxt;
    cur->state = T_RUNNING;

    // fpu_nm_handler will lazily restore state on first use
    set_cr0_ts();

    if (cur->kstack) {
        uint64_t stack_top = (uint64_t)cur->kstack + KERNEL_STACK_SIZE;
        tss_set_rsp0(stack_top);

        // Update local CPU structure for syscall entries
        cpu_local.kernel_stack = stack_top;
    }

    write_msr(MSR_FS_BASE, cur->fs_base);

    cpu_context_t *next_ctx = cur->context;
    if (next_ctx) {
        uint16_t cs = (uint16_t)next_ctx->iret_cs;
        uint16_t ss = (uint16_t)next_ctx->iret_ss;
        bool is_user = (cs & 3) == 3;
        if (!is_user && (cs != KERNEL_CS || (ss != 0 && ss != KERNEL_DS)))
            panicf_c(next_ctx, "sched: corrupt kernel ctx for '%s' (cs=0x%x ss=0x%x)",
                     cur->name, cs, ss);
        if (is_user && (cs != USER_CS || ss != USER_DS))
            panicf_c(next_ctx, "sched: corrupt user ctx for '%s' (cs=0x%x ss=0x%x)",
                     cur->name, cs, ss);
    }
    return next_ctx;
}

/*
 * sched_yield - Voluntarily gives up the remaining time slice
 */
void sched_yield(void) {
    if (!sched_on) return;
    __asm__ volatile("int $32");
}

/*
 * sched_current - Returns the currently running thread
 */
thread_t* sched_current(void) {
    return cur;
}

/*
 * sched_sleep - Puts the current thread to sleep for X ms
 */
void sched_sleep(uint64_t ms) {
    if (!cur || !sched_on) return;

    cur->state = T_SLEEPING;
    cur->wake_at = get_uptime_ms() + ms;

    sched_yield();
}

/*
 * sched_exit - Terminates the current thread
 */
void sched_exit(void) {
    if (!cur) return;

    cur->state = T_DEAD;
    LOGF("[SCHED] Thread '%s' (TID: %u) exited.\n", cur->name, cur->tid);

    proc_hdr_update(cur->process);

    sched_yield();
    while(1);
}
