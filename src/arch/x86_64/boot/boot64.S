# boot64.S - 64-bit kernel entry point
#
# This file is the target of the long mode far jump. It moves execution and the stack
# to the higher half, reloads the GDT from there, and calls the kernel main function.
#
# Author: u/ApparentlyPlus

#include <arch/x86_64/memory/paging.h>

.intel_syntax noprefix

.global long_mode_start

# We request the linker to point us to these symbols for the gdt virtual pointer
# and the kernel main function
.extern kernel_main
.extern gdt64_virt_pointer

.section .text
.code64
long_mode_start:

    # The multiboot pointer came over in edi. The upper half of rdi is
    # undefined after the mode switch, and writing edi zeroes it.
    mov edi, edi

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

upper_memory:
    # We can move the execution from the lower half identity map to
    # the upper half where the kernel is linked to, because we have set up
    # the page tables to map the upper half virtual addresses to the same physical addresses
    # adjusted by a constant offset.
    mov rax, KERNEL_VIRTUAL_BASE
    add rsp, rax

    # Reload the GDT from its virtual address
    # because we will later unmap the lower half where the GDT
    # is currently mapped to, and we need the GDT to be accessible at all times.
    lgdt [gdt64_virt_pointer]

    # RDI contains the multiboot information
    # which is passed from the bootloader.
    # It is handled by the kernel as an argument
    movabs rax, offset kernel_main
    call rax

    # Halt should be a loop due to interrupts
.halt:
    hlt
    jmp .halt
