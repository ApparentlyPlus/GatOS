/*
 * syscall_entry.S - x86_64 System Call Entry Point
 *
 * The implementation below is largely a deviation from the standard syscall/sysret mechanism,
 * which is notoriously inconsistent across different Intel and AMD microarchitectures.
 *
 * I am citing the relevant idea here:
 *
 * When a user program executes the 'syscall' instruction, the CPU does not push 
 * anything to the stack automatically (unlike an interrupt). Instead, it loads 
 * a new RIP from the LSTAR MSR and saves the return address into RCX and the 
 * RFLAGS into R11. This means we are responsible for manually constructing a 
 * state that we can eventually return from.
 * 
 * Since we want a robust way to return to userspace that avoids the inconsistencies 
 * of 'sysret' across different Intel and AMD microarchitectures, we manually 
 * build an 'iret frame' on the kernel stack. This allows us to use 'iretq' to 
 * return, ensuring that segment registers like SS and CS are restored exactly 
 * as we define them instead of relying on 'sysret' to do the right thing with RPLs, 
 * which it often doesn't (loooong story here, and lots of late night coffee).
 *
 * The process follows these critical steps:
 * 
 * We use 'swapgs' to give us access to kernel-side thread-local storage 
 * (via the GS base), allowing us to find the kernel stack pointer. Then, we save the 
 * user's RSP and switch to the kernel stack.
 * 
 * We manually push SS, RSP, RFLAGS, CS, and RIP onto the stack. This mimics the hardware
 * generated stack frame created during a standard interrupt.
 *
 * Since 'syscall' destroys no registers other than RCX and R11, but our C dispatcher might, 
 * we push all general purpose registers to the stack to preserve the user's state.
 * 
 * By the time we call the 'syscall_dispatcher', the stack represents a complete 
 * snapshot of the CPU state at the moment the system call was invoked.
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
    // On entry, we have CPL=0, RSP=user_RSP, RCX=user_RIP, R11=user_RFLAGS
    // Interrupts are disabled via FMASK (IF, DF cleared)
    swapgs

    // Save user RSP and switch to kernel RSP from cpu_local
    mov gs:[8], rsp
    mov rsp, gs:[0]

    // Build an iretq frame at the top of the kernel stack

    // SS is set EXPLICITLY here so we never rely on SYSRETQ's RPL forcing,
    // which behaves inconsistently across Intel microarchitectures

    push 0x1b
    push gs:[8]

    // RFLAGS = user RFLAGS (saved by SYSCALL into R11)
    push r11  
    push 0x23

    // RIP = user return address (saved by SYSCALL into RCX)
    push rcx     

    // Save general-purpose registers
    push rax
    push rbx
    push rcx // also save as GPR (= user RIP)
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11 // also save as GPR (= user RFLAGS)
    push r12
    push r13
    push r14
    push r15

    // Call the C dispatcher
    // rdi = syscall number (rax), rsi = pointer to saved register array (rsp)
    mov rdi, rax
    mov rsi, rsp

    // Align RSP to 16 bytes per SysV ABI
    sub rsp, 8

    // Call and then clean up the stack after the call
    call syscall_dispatcher
    add rsp, 8

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

    // Return to userspace via iretq using the explicit frame we built at entry.
    // The frame has SS=0x1b (USER_DS with RPL=3), so the user thread always
    // resumes with a valid segment selector regardless of CPU behaviour.
    swapgs
    iretq
