# -----------------------------------------------------------------------------
# Early x86 boot and long mode setup
# This file contains the 32-bit entry point for the kernel, performs
# essential CPU and multiboot checks, sets up paging structures, enables
# long mode (64-bit mode), and jumps to the 64-bit kernel start.
#
# Includes error handling that outputs an error code on screen and halts.
# Added page table dumping functionality for debugging.
# -----------------------------------------------------------------------------
#include <arch/x86_64/memory/paging.h>

.intel_syntax noprefix

.global start
.global PML4
.global gdt64_code_segment
.global gdt64_virt_pointer

.extern long_mode_start
.extern KERNEL_STACK_TOP

.section .text
.code32

start:
	# Set stack pointer to top of the stack defined by linker symbol
	mov esp, offset KERNEL_V2P(KERNEL_STACK_TOP)

	# I am transferring the multiboot information
	# directly to edi which will then be zero extended 
	# in the 64 bit transition, and then passed to the kernel
	# as an arg. Usually I'd just keep the value in ebx, but
	# for some reason when it becomes rbx, the value is lost.
	mov edi, ebx

	# Verify multiboot magic number
	call check_multiboot

	# Verify CPUID instruction support
	call check_cpuid

	# Verify CPU supports long mode (64-bit)
	call check_long_mode

	# Verify and enable SSE support
	call check_SSE

	# Set up paging structures for long mode
	call setup_page_tables

	# Enable paging and enter long mode
	call enable_paging

	# Load the 64-bit GDT descriptor
	lgdt [KERNEL_V2P(gdt64_pointer)]

	# Far jump to 64-bit kernel entry point via code segment selector
	jmp gdt64_code_segment:KERNEL_V2P(long_mode_start)

	# Halt if for some reason jump returns
	hlt

# -----------------------------------------------------------------------------
# check_multiboot:
# Validates the multiboot magic number passed in eax
# Jumps to error if invalid
# -----------------------------------------------------------------------------
check_multiboot:
	cmp eax, 0x36d76289
	jne .no_multiboot
	ret
.no_multiboot:
	mov al, 'M'    # Error code 'M'
	jmp error

# -----------------------------------------------------------------------------
# check_SSE:
# Checks SSE support exists in the system for floating point instructions
# Jumps to error if invalid
# -----------------------------------------------------------------------------
check_SSE:
    mov eax, 1
    cpuid
    test edx, 1<<25
    jz .no_SSE
	
    # Enable SSE
    mov eax, cr0
    and eax, 0xFFFBFFFF       # clear CR0.EM (bit 2)
    or  eax, 0x00000002       # set   CR0.MP (bit 1)
    mov cr0, eax
    mov eax, cr4
    or  eax, (1<<9) | (1<<10) # set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10)
    mov cr4, eax
    ret
.no_SSE:
	mov al, 'S'    # Error code 'S'
    jmp error


# -----------------------------------------------------------------------------
# check_cpuid:
# Checks if the CPU supports the CPUID instruction by toggling ID flag in EFLAGS
# Jumps to error if unsupported
# -----------------------------------------------------------------------------
check_cpuid:
	pushfd
	pop eax
	mov ecx, eax
	xor eax, 1 << 21
	push eax
	popfd
	pushfd
	pop eax
	push ecx
	popfd
	cmp eax, ecx
	je .no_cpuid
	ret
.no_cpuid:
	mov al, 'C'    # Error code 'C'
	jmp error

# -----------------------------------------------------------------------------
# check_long_mode:
# Checks if CPU supports long mode by querying extended CPUID features
# Jumps to error if not supported
# -----------------------------------------------------------------------------
check_long_mode:
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001
	jb .no_long_mode

	mov eax, 0x80000001
	cpuid
	test edx, 1 << 29
	jz .no_long_mode
	
	ret
.no_long_mode:
	mov al, 'L'    # Error code 'L'
	jmp error

# -----------------------------------------------------------------------------
# setup_page_tables:
# Initializes the paging structures for long mode to map the first 1GB
# of physical memory using 4KiB pages.
# -----------------------------------------------------------------------------
setup_page_tables:
	# The following code does 2 things:
	# 1.) Identity maps the first 1GB worth of 4kb pages (kvirt = kphys)
	# 2.) Maps the same 1GB to kvirt higher half with base address 0xFFFFFFFF80000000

    # PML4[0] entry points to the base address of our PDPT table.
    # This maps the lower half (identity mapping)
    mov eax, offset KERNEL_V2P(PDPT) 
    or eax, 0b11
    mov dword ptr [KERNEL_V2P(PML4)], eax
    
    # PML4[511] entry also points to the same PDPT table (for higher half)
    mov dword ptr [KERNEL_V2P(PML4) + 511 * 8], eax

    # PDPT[0] entry points to the base address of our PD table.
    # Flags: Present (P=1), Read/Write (R/W=1)
    mov eax, offset KERNEL_V2P(PD)
    or eax, 0b11
    mov dword ptr [KERNEL_V2P(PDPT)], eax

	# PDPT[510] should also point to PD (for higher half)
	mov dword ptr [KERNEL_V2P(PDPT) + 510 * 8], eax

    mov ecx, 0
.PD_loop:
    mov eax, ecx
    shl eax, 12
    add eax, offset KERNEL_V2P(PT)
    or eax, 0b11
    mov ebx, ecx
    shl ebx, 3
    mov dword ptr [KERNEL_V2P(PD) + ebx], eax
    inc ecx
    cmp ecx, 512
    jne .PD_loop

    mov ecx, 0
.PT_loop:
    mov eax, ecx
    shl eax, 12
    or eax, 0b11
    mov ebx, ecx
    shl ebx, 3
    mov dword ptr [KERNEL_V2P(PT) + ebx], eax
    inc ecx
    cmp ecx, 512 * 512
    jne .PT_loop

    ret

# -----------------------------------------------------------------------------
# enable_paging:
# Enables paging by setting CR3 to PML4 address, enabling PAE,
# enabling long mode in IA32_EFER MSR, and enabling paging in CR0
# -----------------------------------------------------------------------------
enable_paging:
    # Set CR3 to address of PML4
	mov eax, offset KERNEL_V2P(PML4)
	mov cr3, eax

    # Enable PAE (Physical Address Extension)
	mov eax, cr4
	or eax, 1 << 5 # enable PAE
	mov cr4, eax

    # Enable Long Mode via IA32_EFER MSR
	mov ecx, 0xC0000080 # IA32_EFER MSR
	rdmsr
	or eax, 1 << 8 # enable long mode
	wrmsr

    # Enable paging in CR0
	mov eax, cr0
	or eax, 1 << 31 # enable paging
	mov cr0, eax

	ret

# -----------------------------------------------------------------------------
# Error:
# Displays error message "ERR: X" where X is error code in AL,
# then halts the CPU
# -----------------------------------------------------------------------------
error:
	mov dword ptr [0xb8000], 0x4f524f45 # "ERRO"
	mov dword ptr [0xb8004], 0x4f3a4f52 # "R:OR"
	mov dword ptr [0xb8008], 0x4f204f20 # " O O "
	mov byte ptr [0xb800a], al         # error code character
	hlt

# -----------------------------------------------------------------------------
# BSS section: reserve 4 KiB aligned paging tables
# -----------------------------------------------------------------------------
.section .bss
.align 4096
PML4:
	.skip 4096
PDPT:
	.skip 4096
PD:
	.skip 4096
PT:
	.skip 4096 * 512

# -----------------------------------------------------------------------------
# Read-only data section containing 64-bit GDT entries and pointer descriptor
# -----------------------------------------------------------------------------
.section .rodata
gdt64:
	.quad 0 # null descriptor
gdt64_code_segment = . - gdt64
	.quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) # 64-bit code segment descriptor
gdt64_pointer:
	.word . - gdt64 - 1  # limit (size of GDT - 1)
	.long KERNEL_V2P(gdt64)     # base address of GDT (32-bit)
gdt64_virt_pointer:
	.word . - gdt64 - 1  # limit (size of GDT - 1)
	.quad gdt64          # base address of GDT (64-bit)