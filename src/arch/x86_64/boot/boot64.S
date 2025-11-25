# -----------------------------------------------------------------------------
# 64-bit kernel entry point
# This label is the target of the long mode far jump.
# Receives multiboot information pointer in RDI.
# Sets up segment registers and calls the kernel main function.
# Halts if multiboot info is missing.
# -----------------------------------------------------------------------------

#include <arch/x86_64/memory/paging.h>

.intel_syntax noprefix

.global long_mode_start
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

    movabs rax, offset upper_memory
    jmp rax

.no_multiboot_info:
    hlt


upper_memory:
    mov rax, KERNEL_VIRTUAL_BASE
    add rsp, rax

    mov rax, 0x0
    mov ss, rax
    mov ds, rax
    mov es, rax

    # Reload the GDT from its virtual address
    # because we will need to access it in long mode.
    lgdt [gdt64_virt_pointer]

    pushq 0x8
    movabs rax, offset .transition_to_c
    push rax
    retfq

.transition_to_c:
    # RDI contains the multiboot information
    # which is passed from the bootloader.
    # It is handled by the kernel as an argument.
    movabs rax, offset kernel_main
    call rax

    # Halt shoud be a loop due to interrupts
    jmp .halt

.halt:
    hlt
    jmp .halt