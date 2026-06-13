# boot64.S - 64-bit kernel entry point
# 
# This file is the target of the long mode far jump. It sets up segment registers 
# and calls the kernel main function, and halts if multiboot info is missing.
#
# Author: u/ApparentlyPlus

#include <arch/x86_64/memory/paging.h>

.intel_syntax noprefix

.global long_mode_start

# We request the linker to point us to these symbols for the gdt virtual pointer
# and the long mode entry point
.extern kernel_main
.extern gdt64_virt_pointer

.section .text
.code64
long_mode_start:

    # If for some reason the multiboot information
    # is not passed, we will just halt the system.
    cmp rdi, 0
    je .no_multiboot_info

    # NULLIFY all data segment registers
    mov ax, 0x0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    # We do an absolute jump to the upper memory region where the kernel is linked to
    # now that the upper_memory address is mapped in the page tables. Also, our registers
    # are now 64 bit, so we can use the full virtual address of the upper memory region.
    movabs rax, offset upper_memory
    jmp rax

.no_multiboot_info:
    hlt


upper_memory:
    # We can move the execution from the lower half identity map to 
    # the upper half where the kernel is linked to, because we have set up
    # the page tables to map the upper half virtual addresses to the same physical addresses
    # adjusted by a constant offset.
    mov rax, KERNEL_VIRTUAL_BASE
    add rsp, rax

    # Clear segment registers to avoid issues with segmentation in long mode
    mov rax, 0x0
    mov ss, rax
    mov ds, rax
    mov es, rax

    # Reload the GDT from its virtual address
    # because we will later unmap the lower half where the GDT
    # is currently mapped to, and we need the GDT to be accessible at all times.
    lgdt [gdt64_virt_pointer]

    # Push the code segment selector and the long mode entry point onto the stack for the far return
    pushq 0x8

    # The long mode entry point is the kernel main function
    # which is linked to the upper half virtual address space
    # so we can do another absolute jump to it but this time we 
    # also push the address onto the stack and do a far return
    movabs rax, offset .transition_to_c
    push rax
    retfq

.transition_to_c:
    # RDI contains the multiboot information
    # which is passed from the bootloader.
    # It is handled by the kernel as an argument
    movabs rax, offset kernel_main
    call rax

    # Halt should be a loop due to interrupts
    jmp .halt

.halt:
    hlt
    jmp .halt