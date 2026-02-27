.intel_syntax noprefix

.global syscall_entry
.extern syscall_dispatcher

.section .text
.code64

.align 16
syscall_entry:
    // We are in ring 0 now, but RSP is still the user stack
    // Interrupts are disabled (via FMASK)
    // Swap GS base to access our cpu_local_t structure
    swapgs

    // Save user RSP and load kernel RSP
    mov gs:[8], rsp
    mov rsp, gs:[0]

    // Save the state needed to return, and all general purpose registers
    push gs:[8] // User RSP
    push r11 // User RFLAGS
    push rcx // User RIP (sysret destination)

    push rax
    push rbx
    push rcx // Also save rcx as a GPR
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11 // Also save r11 as a GPR
    push r12
    push r13
    push r14
    push r15

    // Call the C dispatcher
    // rdi = syscall number (rax)
    // rsi = pointer to saved registers (rsp)
    mov rdi, rax
    mov rsi, rsp
    call syscall_dispatcher

    // Restore general purpose registers
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

    // Restore sysret state
    pop rcx // RIP
    pop r11 // RFLAGS
    pop rsp // User RSP

    // Swap GS back to user GS
    swapgs

    // Return to userspace
    sysretq
