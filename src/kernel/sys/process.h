/*
 * process.h - Process and Thread definitions
 *
 * This file defines the structures for Process Control Blocks (PCB) and
 * Thread Control Blocks (TCB), which are fundamental for multitasking.
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/memory/vmm.h>
#include <kernel/drivers/tty.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_PROCESS_NAME 64
#define MAX_THREAD_NAME  64
#define KERNEL_STACK_SIZE 16384 // 16 KiB
#define USER_STACK_SIZE   65536 // 64 KiB

typedef uint32_t pid_t;
typedef uint32_t tid_t;

typedef enum {
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_SLEEPING,
    THREAD_STATE_DEAD
} thread_state_t;

struct process;

typedef struct thread {
    tid_t tid;
    char name[MAX_THREAD_NAME];
    struct process* process;
    
    thread_state_t state;
    cpu_context_t* context; // Pointer to the saved state on the kernel stack
    void* kernel_stack;     // Base of the kernel stack
    void* user_stack;       // Virtual address of the user stack
    
    uint64_t sleep_until;   // Timestamp for waking up
    
    struct thread* next;       // Next thread in the process (linked list)
    struct thread* sched_next; // Next thread in the scheduler's ready queue
} thread_t;

typedef struct process {
    pid_t pid;
    char name[MAX_PROCESS_NAME];
    vmm_t* vmm;             // Address space
    tty_t* tty;             // Associated terminal
    
    thread_t* threads;      // Linked list of threads in this process
    
    // Userspace Heap management
    void* user_heap_base;   
    void* user_heap_brk;    // Current program break
    void* user_heap_end;    // End of allocated heap region
    
    struct process* next;   // Next process in the system
} process_t;

void process_init(void);
process_t* process_create(const char* name, tty_t* existing_tty);
thread_t* thread_create(process_t* process, const char* name, void (*entry)(void*), void* arg, bool is_user);
thread_t* thread_create_bootstrap(process_t* process, const char* name);
void thread_destroy(thread_t* thread);
void process_destroy(process_t* process);
process_t* process_get_all(void);