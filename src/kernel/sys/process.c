/*
 * process.c - Process and Thread management implementation
 *
 * This file implements the creation, destruction, and metadata management
 * for threads and processes. It coordinates with the VMM for address space
 * isolation and the heap for control block allocation.
 *
 * Author: ApparentlyPlus
 */

#include <kernel/sys/process.h>
#include <kernel/sys/scheduler.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/debug.h>
#include <libc/string.h>

static pid_t g_next_pid = 1;
static tid_t g_next_tid = 1;

static process_t* g_processes = NULL;

/*
 * thread_entry_wrapper - Wrapper function that calls the thread's entry point
 * and then gracefully exits the thread if the entry point returns.
 */
static void thread_entry_wrapper(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }
    
    // If the thread returns, terminate it properly
    scheduler_thread_exit();
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
 * process_create - Creates a new process with its own address space and TTY
 */
process_t* process_create(const char* name, tty_t* existing_tty) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;

    memset(proc, 0, sizeof(process_t));
    proc->pid = g_next_pid++;
    strncpy(proc->name, name, MAX_PROCESS_NAME - 1);

    // Create a unique address space for the process
    proc->vmm = vmm_create(0x1000, 0x00007FFFFFFFF000);
    if (!proc->vmm) {
        kfree(proc);
        return NULL;
    }

    // Initialize userspace heap tracking
    proc->user_heap_base = (void*)0x40000000;
    proc->user_heap_brk = proc->user_heap_base;
    proc->user_heap_end = proc->user_heap_base;

    // Use existing TTY or create a new one
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

    LOGF("[PROC] Created process '%s' (PID: %u)\n", proc->name, proc->pid);
    return proc;
}

/*
 * thread_create - Creates a new thread within a process
 */
thread_t* thread_create(process_t* process, const char* name, void (*entry)(void*), void* arg, bool is_user) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    memset(thread, 0, sizeof(thread_t));
    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_READY;
    strncpy(thread->name, name, MAX_THREAD_NAME - 1);

    // Allocate kernel stack
    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        kfree(thread);
        return NULL;
    }
    memset(thread->kernel_stack, 0, KERNEL_STACK_SIZE);

    // Set up the initial CPU context on the kernel stack
    uintptr_t stack_top = (uintptr_t)thread->kernel_stack + KERNEL_STACK_SIZE;
    thread->context = (cpu_context_t*)(stack_top - sizeof(cpu_context_t));
    memset(thread->context, 0, sizeof(cpu_context_t));

    // Initialize the IRET frame to point to the wrapper
    thread->context->iret_rip = (uint64_t)thread_entry_wrapper;
    thread->context->iret_flags = 0x202; // IF (Interrupt Flag) enabled
    
    if (is_user) {
        thread->context->iret_cs = USER_CS;
        thread->context->iret_ss = USER_DS;
        
        // Allocate userspace stack
        vmm_status_t status = vmm_alloc(process->vmm, USER_STACK_SIZE, VM_FLAG_WRITE | VM_FLAG_USER, NULL, &thread->user_stack);
        if (status != VMM_OK) {
            kfree(thread->kernel_stack);
            kfree(thread);
            return NULL;
        }
        thread->context->iret_rsp = (uint64_t)thread->user_stack + USER_STACK_SIZE;
    } else {
        thread->context->iret_cs = KERNEL_CS;
        thread->context->iret_ss = KERNEL_DS;
        thread->context->iret_rsp = stack_top - sizeof(cpu_context_t);
    }

    // Parameters for thread_entry_wrapper(entry, arg)
    thread->context->rdi = (uint64_t)entry;
    thread->context->rsi = (uint64_t)arg;

    // Add to process thread list
    thread->next = process->threads;
    process->threads = thread;

    LOGF("[PROC] Created thread '%s' (TID: %u) in PID %u\n", thread->name, thread->tid, process->pid);
    return thread;
}

/*
 * thread_create_bootstrap - Internal helper to wrap the current execution context into a thread
 */
thread_t* thread_create_bootstrap(process_t* process, const char* name) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    memset(thread, 0, sizeof(thread_t));
    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_RUNNING; // It's already running!
    strncpy(thread->name, name, MAX_THREAD_NAME - 1);

    // Bootstrap thread uses the stack already in use (no new allocation)
    thread->kernel_stack = NULL; 
    thread->context = NULL; // Will be set on the first interrupt

    // Add to process thread list
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

    // If it has a user stack, unmap/free it
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
 * process_destroy - Cleans up a process and all its threads
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
