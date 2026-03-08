/*
 * process.c - Process and Thread management implementation
 *
 * This file implements the creation, destruction, and metadata management
 * for threads and processes. It coordinates with the heap manager for 
 * both kernel and userspace allocations.
 *
 * Author: ApparentlyPlus
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

static pid_t g_next_pid = 1;
static tid_t g_next_tid = 1;

static process_t* g_processes = NULL;

/*
 * userspace_start - Global entry point for all Ring 3 threads.
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
 * thread_entry_wrapper - Wrapper function that calls the thread's entry point
 * and then gracefully exits the thread if the entry point returns.
 */
static void thread_entry_wrapper(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }
    
    // If the thread returns, terminate it properly
    sched_exit();
}

/*
 * process_init - Initializes the process management subsystem
 */
void process_init(void) {
    g_next_pid = 1;
    g_next_tid = 1;
    g_processes = NULL;
    LOGF("[PROC] Process subsystem initialized.\n");
}

/*
 * process_create - Creates a new process with its own address space and heap
 */
process_t* process_create(const char* name, tty_t* existing_tty) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;

    kmemset(proc, 0, sizeof(process_t));
    proc->pid = g_next_pid++;
    kstrncpy(proc->name, name, MAX_PROCESS_NAME - 1);

    // 1. Create a unique address space for the process
    proc->vmm = vmm_create(0x1000, 0x00007FFFFFFFF000);
    if (!proc->vmm) {
        kfree(proc);
        return NULL;
    }

    // Map the kernel's .user_text into the process's VMM at USER_CODE_VIRT_ADDR
    // Every process sees the same code, but it only exists once in physical RAM.
    uintptr_t user_text_phys = (uintptr_t)KERNEL_V2P(&USER_TEXT_START);
    size_t user_text_size = (uintptr_t)&USER_TEXT_END - (uintptr_t)&USER_TEXT_START;
    user_text_size = align_up(user_text_size, PAGE_SIZE);

    vmm_status_t map_status = vmm_map_range(proc->vmm, user_text_phys, 
                                            (void*)USER_CODE_VIRT_ADDR, 
                                            user_text_size, 
                                            VM_FLAG_USER | VM_FLAG_EXEC);
    if (map_status != VMM_OK) {
        vmm_destroy(proc->vmm);
        kfree(proc);
        return NULL;
    }

    // Setup TTY
    if (existing_tty) {
        proc->tty = existing_tty;
    } else {
        proc->tty = tty_create();
        if (!proc->tty) {
            vmm_destroy(proc->vmm);
            kfree(proc);
            return NULL;
        }
    }

    // Add to global process list
    proc->next = g_processes;
    g_processes = proc;

    LOGF("[PROC] Created process '%s' (PID: %u) with shared code mapping\n", proc->name, proc->pid);
    return proc;
}

/*
 * thread_create - Creates a new thread using the process's heap
 */
thread_t* thread_create(process_t* process, const char* name, void (*entry)(void*), void* arg, bool is_user, uintptr_t user_rsp) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    kmemset(thread, 0, sizeof(thread_t));
    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_READY;
    kstrncpy(thread->name, name, MAX_THREAD_NAME - 1);

    // Allocate kernel stack from kernel heap
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
            
            user_rsp = (uint64_t)thread->user_stack + USER_STACK_SIZE;
        } else {
            // Userspace provided a stack, we don't track it
            thread->user_stack = NULL;
        }

        thread->context->iret_rsp = user_rsp;

    } else {
        thread->context->iret_cs = KERNEL_CS;
        thread->context->iret_ss = KERNEL_DS;
        thread->context->iret_rip = (uint64_t)thread_entry_wrapper;
        thread->context->iret_rsp = stack_top - sizeof(cpu_context_t);
        
        thread->context->rdi = (uint64_t)entry;
        thread->context->rsi = (uint64_t)arg;
    }

    thread->next = process->threads;
    process->threads = thread;

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
    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_RUNNING; 
    kstrncpy(thread->name, name, MAX_THREAD_NAME - 1);

    thread->kernel_stack = NULL; 
    thread->context = NULL; 

    thread->next = process->threads;
    process->threads = thread;

    LOGF("[PROC] Bootstrapped current context as thread '%s' (TID: %u)\n", thread->name, thread->tid);
    return thread;
}

/*
 * thread_destroy - Cleans up a thread's resources
 */
void thread_destroy(thread_t* thread) {
    if (!thread) return;

    LOGF("[PROC] Destroying thread '%s' (TID: %u)\n", thread->name, thread->tid);

    // If it has a user stack, free it from the process VMM
    if (thread->user_stack && thread->process && thread->process->vmm) {
        vmm_free(thread->process->vmm, thread->user_stack);
    }

    // Free kernel stack
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

    // Destroy all threads
    thread_t* thread = process->threads;
    while (thread) {
        thread_t* next = thread->next;
        thread_destroy(thread);
        thread = next;
    }

    if (process->vmm) {
        vmm_destroy(process->vmm);
    }

    // Remove from global list
    process_t** prev = &g_processes;
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
    return g_processes;
}

/*
 * process_terminate_by_tty - Marks all threads of processes using the given TTY as DEAD
 */
void process_terminate_by_tty(tty_t* tty) {
    if (!tty) return;

    process_t* proc = g_processes;
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
