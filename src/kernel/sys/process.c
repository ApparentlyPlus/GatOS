/*
 * process.c - Process and Thread management implementation
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
 * userspace_start - Entry point for every Ring 3 thread, calls the real entry then exits via SYS_EXIT
 */
userspace void userspace_start(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }

    // call SYS_EXIT
    __asm__ volatile (
        "mov $1, %rax \n"
        "syscall \n"
    );

    while(1); // Should never reach here, but just in case
}

/*
 * thread_wrap - Calls the thread's entry point and exits the thread if it returns
 */
static void thread_wrap(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }

    sched_exit();
}

void process_init(void) {
    next_pid = 1;
    next_tid = 1;
    proc_list = NULL;
    LOGF("[PROC] Process subsystem initialized.\n");
}

/*
 * proc_hdr_update - Rewrites the sticky info bar for a process
 */
void proc_hdr_update(process_t* proc) {
    if (!proc || !proc->tty) return;

    size_t total = 0;
    size_t alive = 0;
    thread_t* t = proc->threads;
    while (t) {
        total++;
        if (t->state != T_DEAD) alive++;
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

    proc->pid = next_pid++;
    kstrncpy(proc->name, name, MAX_PROCESS_NAME - 1);

    proc->vmm = vmm_create(USER_CODE_VIRT_ADDR, 0x00007FFFFFFFF000);
    if (!proc->vmm) {
        kfree(proc);
        return NULL;
    }

    // Map the executable's code, rodata, data and bss into the new process. Identity mapped for
    // now to keep loading simple, arbitrary load addresses would be an easy change

    uintptr_t t_phys = (uintptr_t)&USER_TEXT_LOAD_ADDR;
    size_t tsz = align_up((uintptr_t)&USER_TEXT_END - (uintptr_t)&USER_TEXT_START, PAGE_SIZE);
    if (tsz > 0) {
        void* out = NULL;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)USER_CODE_VIRT_ADDR, tsz, VM_FLAG_USER | VM_FLAG_EXEC | VM_FLAG_FOREIGN, (void*)t_phys, &out);
        if (st != VMM_OK) goto map_fail;
    }

    uintptr_t ro_phys = (uintptr_t)&USER_RODATA_LOAD_ADDR;
    size_t rosz = align_up((uintptr_t)&USER_RODATA_END - (uintptr_t)&USER_RODATA_START, PAGE_SIZE);
    if (rosz > 0) {
        void* out = NULL;
        uintptr_t ro_virt = (uintptr_t)&USER_RODATA_START;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)ro_virt, rosz, VM_FLAG_USER | VM_FLAG_FOREIGN, (void*)ro_phys, &out);
        if (st != VMM_OK) goto map_fail;
    }

    // .data and .bss are private to each process, so one can't scribble over another's globals
    // .data starts out as a copy of the image, .bss comes zeroed from vmm_alloc_at
    uintptr_t d_phys = (uintptr_t)&USER_DATA_LOAD_ADDR;
    size_t dsz = align_up((uintptr_t)&USER_DATA_END - (uintptr_t)&USER_DATA_START, PAGE_SIZE);
    if (dsz > 0) {
        void* out = NULL;
        uintptr_t d_virt = (uintptr_t)&USER_DATA_START;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)d_virt, dsz, VM_FLAG_USER | VM_FLAG_WRITE, NULL, &out);
        if (st != VMM_OK) goto map_fail;

        for (uintptr_t off = 0; off < dsz; off += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_get_physical(proc->vmm, (void*)(d_virt + off), &phys))
                kmemcpy((void*)PHYSMAP_P2V(phys), (void*)PHYSMAP_P2V(d_phys + off), PAGE_SIZE);
        }
    }

    size_t bsz = align_up((uintptr_t)&USER_BSS_END - (uintptr_t)&USER_BSS_START, PAGE_SIZE);
    if (bsz > 0) {
        void* out = NULL;
        uintptr_t b_virt = (uintptr_t)&USER_BSS_START;
        vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)b_virt, bsz, VM_FLAG_USER | VM_FLAG_WRITE, NULL, &out);
        if (st != VMM_OK) goto map_fail;
    }

    // If the caller provided a TTY, use it. Otherwise, create a new one for this process.
    if (existing_tty) {
        proc->tty = existing_tty;
    } else {
        proc->tty = tty_create();
        if (!proc->tty) goto map_fail;
        tty_header_init(proc->tty, 3);
        proc->tty->hidden = false;
        proc_hdr_update(proc);
    }

    // The scheduler unlinks dead processes from the timer interrupt, so the list only changes with interrupts off
    bool iflag = intr_save();
    proc->next = proc_list;
    proc_list = proc;
    intr_restore(iflag);

    LOGF("[PROC] Created process '%s' (PID: %u)\n", proc->name, proc->pid);
    return proc;

map_fail:
    vmm_destroy(proc->vmm);
    kfree(proc);
    return NULL;
}

/*
 * thread_new - A zeroed thread with its id, process and name filled in
 */
static thread_t* thread_new(process_t* process, const char* name, thread_state_t state) {
    thread_t* thread = (thread_t*)kmalloc(sizeof(thread_t));
    if (!thread) return NULL;

    thread->tid = next_tid++;
    thread->process = process;
    thread->state = state;
    thread->fpu = (uint8_t*)(((uintptr_t)thread->fpu_raw + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
    kstrncpy(thread->name, name, MAX_THREAD_NAME - 1);
    return thread;
}

/*
 * thread_link - Adds a finished thread to its process
 */
static void thread_link(thread_t* thread) {
    // Same as the process list, the scheduler unlinks dead threads from the timer interrupt
    bool iflag = intr_save();
    thread->next = thread->process->threads;
    thread->process->threads = thread;
    intr_restore(iflag);
    proc_hdr_update(thread->process);
}

/*
 * thread_create - Creates a new thread using the process's heap
 */
thread_t* thread_create(process_t* process, const char* name, void (*entry)(void*), void* arg, bool is_user, uintptr_t user_rsp) {
    thread_t* thread = thread_new(process, name, T_READY);
    if (!thread) return NULL;

    /*
     * Clean FPU state. The thread is already zeroed, so only the two control words with non-zero
     * reset values need writing: FCW = 0x037F (x87, all exceptions masked, 64-bit precision) and
     * MXCSR = 0x1F80 (SSE, all exceptions masked). No fninit + fxsave, that would capture the
     * caller's live XMM registers and leak kernel FPU state into the thread.
     */
    *(uint16_t *)(&thread->fpu[0]) = 0x037F;
    *(uint32_t *)(&thread->fpu[24]) = 0x1F80;

    thread->kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kstack) {
        kfree(thread);
        return NULL;
    }

    uintptr_t stack_top = (uintptr_t)thread->kstack + KERNEL_STACK_SIZE;

    /* Note here: Context is embedded in the thread struct, never on the kstack
    ISR.S uses &thread->context as a staging area to pop registers before iretq
    restores the real RSP */
    
    // So kstack can be freed at any time without corrupting persistent context state

    thread->context.iret_flags = 0x202; // IF enabled

    if (is_user) {
        thread->context.iret_cs = USER_CS;
        thread->context.iret_ss = USER_DS;

        // Use the global userspace_start as the RIP
        uintptr_t offset_start = (uintptr_t)userspace_start - (uintptr_t)&USER_TEXT_START;
        thread->context.iret_rip = USER_CODE_VIRT_ADDR + offset_start;

        // The actual entry function and its argument are passed via registers
        uintptr_t offset_entry = (uintptr_t)entry - (uintptr_t)&USER_TEXT_START;
        thread->context.rdi = USER_CODE_VIRT_ADDR + offset_entry;
        thread->context.rsi = (uint64_t)arg;

        if (user_rsp == 0) {
            // Allocate userspace stack lazily from the process VMM directly
            vmm_status_t alloc_status = vmm_alloc(process->vmm, USER_STACK_SIZE, VM_FLAG_USER | VM_FLAG_WRITE | VM_FLAG_LAZY, NULL, &thread->ustack);

            if (alloc_status != VMM_OK || !thread->ustack) {
                kfree(thread->kstack);
                kfree(thread);
                return NULL;
            }

            // Align to 16 bytes and leave 8 for the "return address": SysV wants (%rsp + 8) to be a
            // multiple of 16 at function entry, and userspace_start is entered via iretq, not a call
            user_rsp = (uint64_t)thread->ustack + USER_STACK_SIZE - 8;
        } else {
            // Userspace provided a stack, we don't track it
            thread->ustack = NULL;
        }

        thread->context.iret_rsp = user_rsp;

    } else {
        // entry runs via thread_wrap on a fresh kstack
        thread->context.iret_cs = KERNEL_CS;
        thread->context.iret_ss = KERNEL_DS;
        thread->context.iret_rip = (uint64_t)thread_wrap;
        thread->context.iret_rsp = stack_top - 8;

        thread->context.rdi = (uint64_t)entry;
        thread->context.rsi = (uint64_t)arg;
    }

    thread_link(thread);

    LOGF("[PROC] Created %s thread '%s' (TID: %u) in PID %u\n",
         is_user ? "USER" : "KERNEL", thread->name, thread->tid, process->pid);
    return thread;
}

/*
 * thread_create_bootstrap - Internal helper to wrap the current execution context into a thread
 */
thread_t* thread_create_bootstrap(process_t* process, const char* name) {
    thread_t* thread = thread_new(process, name, T_RUNNING);
    if (!thread) return NULL;

    fpu_save(thread->fpu);

    thread_link(thread);

    LOGF("[PROC] Bootstrapped current context as thread '%s' (TID: %u)\n", thread->name, thread->tid);
    return thread;
}

/*
 * thread_destroy - Cleans up a thread's resources
 */
void thread_destroy(thread_t* thread) {
    if (!thread) return;

    LOGF("[PROC] Destroying thread '%s' (TID: %u)\n", thread->name, thread->tid);

    // Free the user stack if it exists
    if (thread->ustack && thread->process && thread->process->vmm) {
        vmm_free(thread->process->vmm, thread->ustack);
    }

    // If kernel thread, use kfree
    if (thread->kstack) {
        kfree(thread->kstack);
    }

    // The thread struct itself is always allocated with kmalloc, so we use kfree for it as well
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

    bool iflag = intr_save();
    process_t** prev = &proc_list;
    while (*prev) {
        if (*prev == process) {
            *prev = process->next;
            break;
        }
        prev = &(*prev)->next;
    }
    intr_restore(iflag);

    kfree(process);
}

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
                thread->state = T_DEAD;
                thread = thread->next;
            }
            proc->tty = NULL;
        }
        proc = proc->next;
    }
}
