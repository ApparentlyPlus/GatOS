.intel_syntax noprefix

.global syscall_entry
.extern syscall_dispatcher

.section .text
.code64

.align 16
syscall_entry:
    // On entry: CPL=0, RSP=user_RSP, RCX=user_RIP, R11=user_RFLAGS
    // Interrupts are disabled via FMASK (IF, DF cleared)
    swapgs

    // Save user RSP and switch to kernel RSP from cpu_local
    mov gs:[8], rsp
    mov rsp, gs:[0]

    // Build an iretq frame at the top of the kernel stack.

    // SS is set EXPLICITLY here so we never rely on SYSRETQ's RPL forcing,
    // which behaves inconsistently across Intel microarchitectures

    push 0x1b
    push gs:[8]
    push r11     // RFLAGS = user RFLAGS (saved by SYSCALL into R11)
    push 0x23    
    push rcx     // RIP = user return address (saved by SYSCALL into RCX)

    // Save general-purpose registers
    push rax
    push rbx
    push rcx          // also save as GPR (= user RIP)
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11          // also save as GPR (= user RFLAGS)
    push r12
    push r13
    push r14
    push r15

    // Call the C dispatcher
    // rdi = syscall number (rax), rsi = pointer to saved register array (rsp)
    mov rdi, rax
    mov rsi, rsp
    sub rsp, 8        // Align RSP to 16 bytes per SysV ABI
    call syscall_dispatcher
    add rsp, 8

    // Restore general-purpose registers
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

    // Return to userspace via iretq using the explicit frame we built at entry.
    // The frame has SS=0x1b (USER_DS with RPL=3), so the user thread always
    // resumes with a valid segment selector regardless of CPU behaviour.
    swapgs
    iretq
