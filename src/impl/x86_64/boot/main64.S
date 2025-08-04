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
    mov rax, 0xFFFFFFFF80000000
    add rsp, rax

    movabs rax, offset gdt64_vpointer
    lgdt [rax]
    mov rax, 0x0
    mov ss, rax
    mov ds, rax
    mov es, rax

    pushq 0x8
    movabs rax, offset .transition_to_c
    push rax
    retfq

.transition_to_c:
    //mov rax, 0
    //movabs [PML4], rax /* Remove the lower mapping after mapping video! */
    //mov rax, cr3
    //mov cr3, rax  # Flush TLB
    
    # RDI contains the multiboot information
    # which is passed from the bootloader.
    # It is handled by the kernel as an argument.
    movabs rax, offset kernel_main
    call rax

    hlt

error:
	mov dword ptr [0xb8000], 0x4f524f45 # "ERRO"
	mov dword ptr [0xb8004], 0x4f3a4f52 # "R:OR"
	mov dword ptr [0xb8008], 0x4f204f20 # " O O "
	mov byte ptr [0xb800a], al         # error code character
	hlt