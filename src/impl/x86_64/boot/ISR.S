/*
 * interrupts.S - x86_64 Interrupt setup
 *
 * Implementation based on: https://github.com/dreamportdev/Osdev-Notes/blob/master/02_Architecture/05_InterruptHandling.md
 *
 * I'm citing the relevant idea here:
 * 
 * Since an interrupt handler uses the same general purpose registers as the code that was interrupted, 
 * we'll need to save and then restore the values of those registers, otherwise we may crash the interrupted program.
 * There are a number of ways we could go about something like this, we're going to use some assembly (not too much!) 
 * as it gives us the fine control over the cpu we need. There are other ways, like the infamous __attribute__((interrupt)), 
 * but these have their own issues and limitations. This small bit of assembly code will allow us to add other things as we go.
 * 
 * There are a number of places where the state of the general purpose registers could be stored, we're going to use the stack 
 * as it's extremely simple to implement. In protected mode there are the pusha/popa instructions for this, but they're not present 
 * in long mode, so we have to do this ourselves.
 * 
 * There is also one other thing: when an interrupt is served the cpu will store some things on the stack, 
 * so that when the handler is done we can return to the previous code. The cpu pushes the following on to the stack (in this order):
 * 
 * ss: The previous stack selector.
 * rsp: The previous stack-top.
 * rflags: The previous value of the flags register, before the cpu modified any flags for serving the interrupt.
 * cs: The previous code selector.
 * rip: The previous instruction pointer.
 * 
 * Optionally, for some vectors the cpu will push a 64-bit error code (see the table below for specifics). 
 * This structure is known as an iret frame, because to return from an interrupt we use the iret instruction, 
 * which pops those five values from the stack.
 */

.intel_syntax noprefix

.extern interrupt_dispatcher

.section .text
.code64

# -----------------------------------------------------------------------------
# generic_interrupt_handler:
# This is where all interrupts and exceptions eventually land.
# It saves all general purpose registers, calls the C interrupt dispatcher,
# and then restores all registers and returns from the interrupt.
# -----------------------------------------------------------------------------
.align 16
generic_interrupt_handler:

    // First, save all general purpose registers to the stack
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
    
    // Call our C interrupt dispatcher
    // with the current stack pointer as argument
    mov rdi, rsp
    call interrupt_dispatcher

    // After the dispatcher returns, we need to restore the stack pointer
    // from the output of the dispatcher (in rax)
    mov rsp, rax
    
    // Restore all general purpose registers
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
    
    // A thing to notice is that we added 16 bytes to the stack before the iret. 
    // This is because there will be an error code (real or dummy) and the vector number that we need to remove, 
    // so that the iret frame is at the top of the stack. If we don't do this, iret will use the wrong data and likely 
    // trigger a general protection fault.

    add rsp, 16
    
    // Return from the interrupt
    iretq


/* 
 * We now have to create specific interrupt stubs for each vector,
 * because we need to push the vector number and possibly a dummy error code
 * onto the stack before calling the generic interrupt stub. 
 */

.macro GENERATE_INTERRUPT_HANDLER num
    .align 16
    .global interrupt_handler_\num
    interrupt_handler_\num:
    
    // If the vector has error code (from the known ones), 
    // push just the vector number alone, otherwise push a dummy error code (0) first.

    .if \num == 8 || \num == 10 || \num == 11 || \num == 12 || \num == 13 || \num == 14 || \num == 17
        push \num
    .else
        push 0
        push \num
    .endif
    
    jmp generic_interrupt_handler
.endm

.altmacro

// Generate all 256 handlers
.set i, 0
.rept 256
    GENERATE_INTERRUPT_HANDLER %i
    .set i, i+1
.endr