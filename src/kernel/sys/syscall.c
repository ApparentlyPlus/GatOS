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
#include <kernel/drivers/stdio.h>
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
    // to access specific registers, we can index from the top.
    
    switch (syscall_num) {
        case SYS_EXIT:
            // sys_exit: Terminate current thread
            sched_exit();
            break;
            
        case SYS_WRITE:
            // Soon
            break;
            
        default:
            LOGF("[SYSCALL] Unknown syscall: %lu\n", syscall_num);

            // Eh, we should probs terminate the thread here
            sched_exit();
            break;
    }
}
