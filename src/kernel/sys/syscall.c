/*
 * syscall.c - Syscall initialization and dispatching
 *
 * Configures the MSRs for the syscall/sysret instructions and
 * dispatches syscalls from userspace.
 *
 * Author: ApparentlyPlus
 */

#include <kernel/sys/syscall.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <kernel/drivers/stdio.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>

extern void syscall_entry(void);

void syscall_init(void) {
    // Enable SCE (System Call Enable) bit in EFER
    uint64_t efer = read_msr(MSR_EFER);
    efer |= EFER_SCE; 
    write_msr(MSR_EFER, efer);

    // STAR MSR: 
    // Bits 63:48 - User CS/SS base. sysret uses STAR[63:48]+16 for CS, STAR[63:48]+8 for SS.
    // We want User CS = 0x20 (index 4), User SS = 0x18 (index 3).
    // So base should be 0x10.
    // Bits 47:32 - Kernel CS/SS base. syscall uses STAR[47:32] for CS, STAR[47:32]+8 for SS.
    // We want Kernel CS = 0x08, Kernel SS = 0x10.
    // So base should be 0x08.

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);

    // LSTAR MSR - Target RIP for syscall
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    // FMASK MSR - RFLAGS to clear on syscall. We clear IF (bit 9), DF (bit 10).
    write_msr(MSR_FMASK, 0x200 | 0x400);

    LOGF("[SYSCALL] Syscall interface initialized.\n");
}

/*
 * syscall_dispatcher - Called from assembly stub.
 */
void syscall_dispatcher(uint64_t syscall_num, uint64_t* registers) {
    // registers[0] = r15 ... registers[14] = rax
    // According to our push order:
    // rdi = registers[9]
    // rsi = registers[10]
    // rdx = registers[11]
    
    thread_t* current = sched_current();
    if (!current) return;

    switch (syscall_num) {
        case SYS_EXIT:
            // sys_exit: Terminate current thread
            sched_exit();
            break;
            
        case SYS_WRITE: {
            // sys_write(buffer, length)
            const char* buf = (const char*)registers[9];
            size_t len = (size_t)registers[10];

            if (!vmm_check_buffer(current->process->vmm, buf, len, VM_FLAG_USER)) {
                LOGF("[SYSCALL] SYS_WRITE: Invalid buffer pointer 0x%lx (len: %zu) from thread '%s' (PID %u)\n", 
                     (uintptr_t)buf, len, current->name, current->process ? current->process->pid : 0);
                sched_exit();
                break;
            }

            // We need to verify the buffer is in user space
            // For now, let's just use the process's TTY if it has one.
            if (current->process && current->process->tty) {
                tty_write(current->process->tty, buf, len);
            }
            break;
        }
            
        case SYS_MMAP: {
            void* addr = (void*)registers[9];
            size_t length = (size_t)registers[10];
            size_t vm_flags = (size_t)registers[11];
            
            void* out_addr = NULL;
            vmm_status_t status;
            
            if (addr) {
                status = vmm_alloc_at(current->process->vmm, addr, length, vm_flags | VM_FLAG_USER, NULL, &out_addr);
            } else {
                status = vmm_alloc(current->process->vmm, length, vm_flags | VM_FLAG_USER, NULL, &out_addr);
            }
            
            if (status == VMM_OK) {
                registers[14] = (uint64_t)out_addr; // rax
            } else {
                registers[14] = (uint64_t)-1;
            }
            break;
        }
        
        case SYS_MUNMAP: {
            void* addr = (void*)registers[9];
            vmm_free(current->process->vmm, addr);
            registers[14] = 0;
            break;
        }
        
        case SYS_SET_FS_BASE: {
            uint64_t base = registers[9];
            current->fs_base = base;
            write_msr(MSR_FS_BASE, base);
            registers[14] = 0;
            break;
        }
            
        default:
            LOGF("[SYSCALL] Unknown syscall: %lu from thread '%s' (PID %u)\n", 
                 syscall_num, current->name, current->process ? current->process->pid : 0);

            // Terminate offending thread
            sched_exit();
            break;
    }
}
