/*
 * ISR.S - x86_64 Interrupt setup
 *
 * Based on: https://github.com/dreamportdev/Osdev-Notes/blob/master/02_Architecture/05_InterruptHandling.md
 *
 * We save/restore the GPRs ourselves (no pusha/popa in long mode) on the stack. On entry
 * the cpu has already pushed an iret frame: ss, rsp, rflags, cs, rip, plus an error code
 * for some vectors.
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

    // Force 16b alignment before call. Works for both the sched stack case
    // (rsp = page aligned sched_stack_top) and the non switched case (rsp
    // offset is a multiple of 16 from original RSP since we pushed 22 QWORDs)
    and rsp, -16
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
 * One stub per vector, each pushes the vector number (plus a dummy error code when the cpu
 * doesn't push one) before calling the generic stub.
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
