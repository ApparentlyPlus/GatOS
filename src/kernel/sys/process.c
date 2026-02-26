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
 * process_create - Creates a new process with its own address space and heap
 */
process_t* process_create(const char* name, tty_t* existing_tty) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;

    memset(proc, 0, sizeof(process_t));
    proc->pid = g_next_pid++;
    strncpy(proc->name, name, MAX_PROCESS_NAME - 1);

    // 1. Create a unique address space for the process
    proc->vmm = vmm_create(0x1000, 0x00007FFFFFFFF000);
    if (!proc->vmm) {
        kfree(proc);
        return NULL;
    }

    // Create a private heap for the process (manages lower-half memory)
    // We MUST switch to the new VMM so the kernel can write heap metadata to the lower half
    vmm_switch(proc->vmm);

    // Initial size 1MB, Max size 1GB. We add HEAP_FLAG_EXECUTABLE because this heap will store the program code.
    proc->user_heap = heap_create(proc->vmm, 1024 * 1024, 1024 * 1024 * 1024, HEAP_FLAG_EXECUTABLE);
    
    // Switch back immediately after heap initialization
    vmm_switch(NULL);

    if (!proc->user_heap) {
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
            heap_destroy(proc->user_heap);
            vmm_destroy(proc->vmm);
            kfree(proc);
            return NULL;
        }
    }

    // Add to global process list
    proc->next = g_processes;
    g_processes = proc;

    LOGF("[PROC] Created process '%s' (PID: %u) with private heap\n", proc->name, proc->pid);
    return proc;
}

/*
 * thread_create - Creates a new thread using the process's heap
 */
thread_t* thread_create(process_t* process, const char* name, void (*entry)(void*), void* arg, bool is_user) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    memset(thread, 0, sizeof(thread_t));
    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_READY;
    strncpy(thread->name, name, MAX_THREAD_NAME - 1);

    // Allocate kernel stack from kernel heap
    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        kfree(thread);
        return NULL;
    }
    memset(thread->kernel_stack, 0, KERNEL_STACK_SIZE);

    uintptr_t stack_top = (uintptr_t)thread->kernel_stack + KERNEL_STACK_SIZE;
    thread->context = (cpu_context_t*)(stack_top - sizeof(cpu_context_t));
    memset(thread->context, 0, sizeof(cpu_context_t));

    thread->context->iret_flags = 0x202; // IF enabled
    
    if (is_user) {
        thread->context->iret_cs = USER_CS;
        thread->context->iret_ss = USER_DS;
        
        // Critical section here
        vmm_switch(process->vmm);

        // Load userspace code into the process heap
        size_t user_text_size = (uintptr_t)&USER_TEXT_END - (uintptr_t)&USER_TEXT_START;
        if (user_text_size == 0) user_text_size = PAGE_SIZE;

        // Use the heap to allocate space for the program code
        void* code_buffer = heap_malloc(process->user_heap, user_text_size);
        if (!code_buffer) {
            vmm_switch(NULL);
            LOGF("[PROC] ERROR: Failed to allocate heap memory for code of thread '%s'\n", name);
            kfree(thread->kernel_stack);
            kfree(thread);
            return NULL;
        }

        // Copy hardcoded code into the heap buffer
        memcpy(code_buffer, &USER_TEXT_START, user_text_size);

        // Calculate the RIP relative to the heap buffer
        uintptr_t offset = (uintptr_t)entry - (uintptr_t)&USER_TEXT_START;
        thread->context->iret_rip = (uint64_t)code_buffer + offset;

        // Allocate and clear userspace stack from the process heap
        thread->user_stack = heap_malloc(process->user_heap, USER_STACK_SIZE);
        if (!thread->user_stack) {
            heap_free(process->user_heap, code_buffer);
            vmm_switch(NULL);
            kfree(thread->kernel_stack);
            kfree(thread);
            return NULL;
        }
        memset(thread->user_stack, 0, USER_STACK_SIZE);
        
        // Subtract 8 bytes to avoid pointing exactly at the heap footer
        thread->context->iret_rsp = (uint64_t)thread->user_stack + USER_STACK_SIZE - 8;

        // Back to the kernel vmm
        vmm_switch(NULL);

        thread->context->rdi = (uint64_t)arg;

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

    memset(thread, 0, sizeof(thread_t));
    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_STATE_RUNNING; 
    strncpy(thread->name, name, MAX_THREAD_NAME - 1);

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

    // If it has a user stack, free it from the process heap
    if (thread->user_stack && thread->process && thread->process->user_heap) {
        heap_free(thread->process->user_heap, thread->user_stack);
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

    // Destroy userspace heap
    if (process->user_heap) {
        heap_destroy(process->user_heap);
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
