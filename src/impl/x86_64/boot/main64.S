# -----------------------------------------------------------------------------
# 64-bit kernel entry point
# This label is the target of the long mode far jump.
# Receives multiboot information pointer in RDI.
# Sets up segment registers and calls the kernel main function.
# Halts if multiboot info is missing.
# -----------------------------------------------------------------------------

.intel_syntax noprefix

.global long_mode_start
.extern kernel_main

.section .text
.code64
long_mode_start:

    # If for some reason the multiboot information
    # is not passed, we will just halt the system.
    cmp rdi, 0
    je .no_multiboot_info

    # NULLIFY all data segment registers
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    # RDI contains the multiboot information
    # which is passed from the bootloader.
    # It is handled by the kernel as an argument.
	call kernel_main
    hlt

.no_multiboot_info:
    hlt