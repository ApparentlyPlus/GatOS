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
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/layout.h>

.intel_syntax noprefix

.extern interrupt_dispatcher
.extern sched_stack_top

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
    // Check if we came from userspace (Ring 3)
    // Current stack:
    // [rsp + 0]  - vector number
    // [rsp + 8]  - error code
    // [rsp + 16] - iret rip
    // [rsp + 24] - iret cs
    // ...
    
    test qword ptr [rsp + 24], 3
    jz .no_swapgs
    swapgs
.no_swapgs:

    // Save all general purpose registers
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
    
    // Pass a pointer to the saved-context frame to the dispatcher.
    mov rdi, rsp

    // Switch to the per CPU scheduler stack before calling into C
    
    // If sched_stack_top is still 0 (early boot, before sched_init), we stay
    // on the current stack
    mov rax, [sched_stack_top + rip]
    test rax, rax
    jz .no_sched_stack
    
    // Check if we are already on the scheduler stack (nested interrupts)
    // If rsp is within [stack_top - KERNEL_STACK_SIZE, stack_top), skip the switch
    mov rbx, rax

    // rbx = stack bottom
    sub rbx, KERNEL_STACK_SIZE 
    cmp rsp, rbx

    // rsp below stack bottom, not on sched stack
    jb .do_switch 
    cmp rsp, rax

    // rsp at or above top, not on sched stack
    jae .do_switch 

    // inside bounds, already on sched stack
    jmp .no_sched_stack  

.do_switch:
    mov rsp, rax
.no_sched_stack:

    // Ensure 16b stack alignment at call boundary (SysV ABI)
    sub rsp, 8
    call interrupt_dispatcher

    // Restore the stack pointer
    mov rsp, rax
    
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

    // Swap GS back if returning to userspace
    // Current stack: [rsp] vector, [rsp+8] err, [rsp+16] rip, [rsp+24] cs
    test qword ptr [rsp + 24], 3
    jz .no_swapgs_back
    swapgs
.no_swapgs_back:
    // Cleanup vector and error code
    add rsp, 16
    
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
