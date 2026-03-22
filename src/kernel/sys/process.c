/*
 * process.c - Process and Thread management implementation
 *
 * This file implements the creation, destruction, and metadata management
 * for threads and processes. It coordinates with the heap manager for 
 * both kernel and userspace allocations.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/sys/process.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/userspace.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <klibc/stdio.h>

static pid_t next_pid = 1;
static tid_t next_tid = 1;

static process_t* proc_list = NULL;

/*
 * userspace_start - Global entry point for all Ring 3 threads
 * It calls the entry function and then exits via SYS_EXIT.
 */
userspace void userspace_start(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }

    // SYS_EXIT
    __asm__ volatile (
        "mov $1, %rax \n"
        "syscall \n"
    );

    while(1); // Should never reach here, but just in case
}

/*
 * thread_wrap - Wrapper function that calls the thread's entry point
 * and then gracefully exits the thread if the entry point returns.
 */
static void thread_wrap(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }

    sched_exit();
}

/*
 * process_init - Initializes the process management subsystem
 */
void process_init(void) {
    next_pid = 1;
    next_tid = 1;
    proc_list = NULL;
    LOGF("[PROC] Process subsystem initialized.\n");
}

/*
 * process_header_update - Rewrites the sticky info bar for a process
 */
void process_header_update(process_t* proc) {
    if (!proc || !proc->tty) return;

    size_t total = 0;
    size_t alive = 0;
    thread_t* t = proc->threads;
    while (t) {
        total++;
        if (t->state != THREAD_STATE_DEAD) alive++;
        t = t->next;
    }

    const char* state = (total == 0 || alive > 0) ? "Running" : "Terminated";

    char hdr[128];
    ksnprintf(hdr, sizeof(hdr), "Process: %s  |  PID: %u  |  Threads: %zu  |  State: %s", proc->name, proc->pid, alive, state);

    tty_header_write(proc->tty, 1, hdr, CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
}

/*
 * process_create - Creates a new process with its own address space and heap
 */
process_t* process_create(const char* name, tty_t* existing_tty) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;

    kmemset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    kstrncpy(proc->name, name, MAX_PROCESS_NAME - 1);

    proc->vmm = vmm_create(USER_CODE_VIRT_ADDR, 0x00007FFFFFFFF000);
    if (!proc->vmm) {
        kfree(proc);
        return NULL;
    }

    uintptr_t t_phys = (uintptr_t)&USER_TEXT_LOAD_ADDR;
    size_t tsz = align_up((uintptr_t)&USER_TEXT_END - (uintptr_t)&USER_TEXT_START, PAGE_SIZE);
    if (tsz > 0) {
        void* out = NULL;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)USER_CODE_VIRT_ADDR, tsz, VM_FLAG_USER | VM_FLAG_EXEC | VM_FLAG_MMIO, (void*)t_phys, &out);
        if (st != VMM_OK) goto map_fail;
    }

    uintptr_t ro_phys = (uintptr_t)&USER_RODATA_LOAD_ADDR;
    size_t rosz = align_up((uintptr_t)&USER_RODATA_END - (uintptr_t)&USER_RODATA_START, PAGE_SIZE);
    if (rosz > 0) {
        void* out = NULL;
        uintptr_t ro_virt = (uintptr_t)&USER_RODATA_START;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)ro_virt, rosz, VM_FLAG_USER | VM_FLAG_MMIO, (void*)ro_phys, &out);
        if (st != VMM_OK) goto map_fail;
    }

    uintptr_t d_phys = (uintptr_t)&USER_DATA_LOAD_ADDR;
    size_t dsz = align_up((uintptr_t)&USER_DATA_END - (uintptr_t)&USER_DATA_START, PAGE_SIZE);
    if (dsz > 0) {
        void* out = NULL;
        uintptr_t d_virt = (uintptr_t)&USER_DATA_START;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)d_virt, dsz, VM_FLAG_USER | VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)d_phys, &out);
        if (st != VMM_OK) goto map_fail;
    }

    uintptr_t b_phys = (uintptr_t)&USER_BSS_LOAD_ADDR;
    size_t bsz = align_up((uintptr_t)&USER_BSS_END - (uintptr_t)&USER_BSS_START, PAGE_SIZE);
    if (bsz > 0) {
        void* out = NULL;
        uintptr_t b_virt = (uintptr_t)&USER_BSS_START;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)b_virt, bsz, VM_FLAG_USER | VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)b_phys, &out);
        if (st != VMM_OK) goto map_fail;
    }

    if (existing_tty) {
        proc->tty = existing_tty;
    } else {
        proc->tty = tty_create();
        if (!proc->tty) goto map_fail;
        tty_header_init(proc->tty, 3);
        proc->tty->hidden = false;
        process_header_update(proc);
    }

    proc->next = proc_list;
    proc_list = proc;

    LOGF("[PROC] Created process '%s' (PID: %u) with shared code mapping\n", proc->name, proc->pid);
    return proc;

map_fail:
    vmm_destroy(proc->vmm);
    kfree(proc);
    return NULL;
}

/*
 * thread_create - Creates a new thread using the process's heap
 */
thread_t* thread_create(process_t* process, const char* name, void (*entry)(void*), void* arg, bool is_user, uintptr_t user_rsp) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    kmemset(thread, 0, sizeof(thread_t));
    thread->tid = next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_READY;
    kstrncpy(thread->name, name, MAX_THREAD_NAME - 1);

    /*
     * Initialize FPU state to a clean architectural default.
     * The thread struct is already fully zeroed by kmemset above, so all
     * x87/MMX/XMM registers are zero. We only need to write the two control
     * words that have non-zero reset values:
     *
     *   FCW  (offset  0) = 0x037F - x87: all exceptions masked, 64-bit precision
     *   MXCSR(offset 24) = 0x1F80 - SSE: all exceptions masked
     *
     * This avoids fninit + fxsave, which would capture the calling context's
     * live XMM registers and potentially leak kernel FPU state into the thread.
     */
    *(uint16_t *)(&thread->fpu_state[0])  = 0x037F;
    *(uint32_t *)(&thread->fpu_state[24]) = 0x1F80;

    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        kfree(thread);
        return NULL;
    }
    kmemset(thread->kernel_stack, 0, KERNEL_STACK_SIZE);

    uintptr_t stack_top = (uintptr_t)thread->kernel_stack + KERNEL_STACK_SIZE;
    thread->context = (cpu_context_t*)(stack_top - sizeof(cpu_context_t));
    kmemset(thread->context, 0, sizeof(cpu_context_t));

    thread->context->iret_flags = 0x202; // IF enabled
    
    if (is_user) {
        thread->context->iret_cs = USER_CS;
        thread->context->iret_ss = USER_DS;
        
        // Use the global userspace_start as the RIP
        uintptr_t offset_start = (uintptr_t)userspace_start - (uintptr_t)&USER_TEXT_START;
        thread->context->iret_rip = USER_CODE_VIRT_ADDR + offset_start;

        // The actual entry function and its argument are passed via registers
        uintptr_t offset_entry = (uintptr_t)entry - (uintptr_t)&USER_TEXT_START;
        thread->context->rdi = USER_CODE_VIRT_ADDR + offset_entry;
        thread->context->rsi = (uint64_t)arg;

        if (user_rsp == 0) {
            // Allocate userspace stack lazily from the process VMM directly
            vmm_status_t alloc_status = vmm_alloc(process->vmm, USER_STACK_SIZE, VM_FLAG_USER | VM_FLAG_WRITE | VM_FLAG_LAZY, NULL, &thread->user_stack);

            if (alloc_status != VMM_OK || !thread->user_stack) {
                kfree(thread->kernel_stack);
                kfree(thread);
                return NULL;
            }
            
            // Align to 16 bytes and leave 8 bytes for ABI (as if called)
            // The System V ABI says: "The value (%rsp + 8) is always a multiple of 16 
            // when control is transferred to the function entry point."
            // Since userspace_start is entered via iretq, we want it to look like a call.
            user_rsp = (uint64_t)thread->user_stack + USER_STACK_SIZE - 8;
        } else {
            // Userspace provided a stack, we don't track it
            thread->user_stack = NULL;
        }

        thread->context->iret_rsp = user_rsp;

    } else {
        thread->context->iret_cs = KERNEL_CS;
        thread->context->iret_ss = KERNEL_DS;
        thread->context->iret_rip = (uint64_t)thread_wrap;
        thread->context->iret_rsp = stack_top - sizeof(cpu_context_t);
        
        thread->context->rdi = (uint64_t)entry;
        thread->context->rsi = (uint64_t)arg;
    }

    thread->next = process->threads;
    process->threads = thread;
    process_header_update(process);

    LOGF("[PROC] Created %s thread '%s' (TID: %u) in PID %u\n",
         is_user ? "USER" : "KERNEL", thread->name, thread->tid, process->pid);
    return thread;
}

/*
 * thread_create_bootstrap - Internal helper to wrap the current execution context into a thread
 */
thread_t* thread_create_bootstrap(process_t* process, const char* name) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    kmemset(thread, 0, sizeof(thread_t));
    thread->tid = next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_RUNNING; 
    kstrncpy(thread->name, name, MAX_THREAD_NAME - 1);

    __asm__ volatile (
        "fxsave %0 \n"
        : "=m"(thread->fpu_state)
    );

    thread->kernel_stack = NULL; 
    thread->context = NULL; 

    thread->next = process->threads;
    process->threads = thread;
    process_header_update(process);

    LOGF("[PROC] Bootstrapped current context as thread '%s' (TID: %u)\n", thread->name, thread->tid);
    return thread;
}

/*
 * thread_destroy - Cleans up a thread's resources
 */
void thread_destroy(thread_t* thread) {
    if (!thread) return;

    LOGF("[PROC] Destroying thread '%s' (TID: %u)\n", thread->name, thread->tid);

    if (thread->user_stack && thread->process && thread->process->vmm) {
        vmm_free(thread->process->vmm, thread->user_stack);
    }

    if (thread->kernel_stack) {
        kfree(thread->kernel_stack);
    }

    kfree(thread);
}

/*
 * process_destroy - Cleans up a process, its heap, and all its threads
 */
void process_destroy(process_t* process) {
    if (!process) return;

    LOGF("[PROC] Destroying process '%s' (PID: %u)\n", process->name, process->pid);

    sched_drop_proc(process);

    thread_t* thread = process->threads;
    while (thread) {
        thread_t* next = thread->next;
        thread_destroy(thread);
        thread = next;
    }

    if (process->vmm) {
        vmm_destroy(process->vmm);
    }

    process_t** prev = &proc_list;
    while (*prev) {
        if (*prev == process) {
            *prev = process->next;
            break;
        }
        prev = &(*prev)->next;
    }

    kfree(process);
}

/*
 * process_get_all - Returns the head of the global process list
 */
process_t* process_get_all(void) {
    return proc_list;
}

/*
 * procs_kill_tty - Marks all threads of processes using the given TTY as DEAD
 */
void procs_kill_tty(tty_t* tty) {
    if (!tty) return;

    process_t* proc = proc_list;
    while (proc) {
        if (proc->tty == tty) {
            thread_t* thread = proc->threads;
            while (thread) {
                thread->state = THREAD_STATE_DEAD;
                thread = thread->next;
            }
            proc->tty = NULL;
        }
        proc = proc->next;
    }
}
