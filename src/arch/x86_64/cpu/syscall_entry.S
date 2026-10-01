/*
 * syscall_entry.S - x86_64 System Call Entry Point
 *
 * Builds the same cpu_context_t as ISR.S, so the scheduler and fault handlers see one
 * layout whether a thread entered via SYSCALL or an IRQ.
 *
 * Two exits. Fast: sysretq, when the saved user RIP (RCX) is canonical. Slow: iretq, when
 * it isn't, because sysretq with a non canonical RCX faults at CPL=0 with the user's GS base
 * still active (SWAPGS already ran), which is a privilege escalation. iretq faults at CPL=3.
 *
 * Author: u/ApparentlyPlus
 */

.intel_syntax noprefix

.global syscall_entry
.extern syscall_dispatcher

.section .text
.code64

.align 16
syscall_entry:
    // On entry CPL=0, RSP=user_RSP, RCX=user_RIP, R11=user_RFLAGS
    // Interrupts are disabled via FMASK (IF, DF cleared)
    swapgs

    // Stash user RSP in the per CPU scratch slot, switch to kernel stack
    mov gs:[8], rsp
    mov rsp, gs:[0]

    // And now, build a complete cpu_context_t on the per thread kstack

    // iret frame
    push 0x1b               // iret_ss    = USER_DS (RPL=3)
    push qword ptr gs:[8]   // iret_rsp   = user RSP (saved on kstack, survives preemption)
    push r11                // iret_flags = user RFLAGS
    push 0x23               // iret_cs    = USER_CS (RPL=3)
    push rcx                // iret_rip   = user RIP

    // Interrupt metadata
    push 0                  // error_code
    push 0x80               // vector_number = 0x80 (I guess this is a sentinel?)

    // General purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    // For syscall_dispatcher, pass in the ctx
    mov rdi, rsp

    // SysV ABI needs rsp ≡ 8 (mod 16) before calling, so adjust
    sub rsp, 8
    call syscall_dispatcher
    add rsp, 8

    // Restore all regs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    // Skip vector number and error code
    add rsp, 16

    // Now we select which exit path we take
    // Stack points at the iret frame
    // RCX = user RIP, R11 = user RFLAGS

    // Guard against the SYSRETQ non canonical RCX thingie
    mov r10, rcx
    shl r10, 16
    sar r10, 16
    cmp r10, rcx
    jne .slow_exit

    // Fast path through sysretq
    // Load user RSP from the iret frame (offset +24 = iret_rsp)
    mov rsp, [rsp + 24]
    swapgs
    sysretq

.slow_exit:
    // Slow path through iretq
    swapgs
    iretq
