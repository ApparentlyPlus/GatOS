; -----------------------------------------------------------------------------
; Early x86 boot and long mode setup
; This file contains the 32-bit entry point for the kernel, performs
; essential CPU and multiboot checks, sets up paging structures, enables
; long mode (64-bit mode), and jumps to the 64-bit kernel start.
;
; Includes error handling that outputs an error code on screen and halts.
; -----------------------------------------------------------------------------

global start
extern long_mode_start
extern __stack_top

section .text
bits 32

start:
	; Set stack pointer to top of the stack defined by linker symbol
	mov esp, __stack_top

	; I am transferring the multiboot information
	; directly to edi which will then be zero extended 
	; in the 64 bit transition, and then passed to the kernel
	; as an arg. Usually I'd just keep the value in ebx, but
	; for some reason when it becomes rbx, the value is lost.
	mov edi, ebx

	; Verify multiboot magic number
	call check_multiboot

	; Verify CPUID instruction support
	call check_cpuid

	; Verify CPU supports long mode (64-bit)
	call check_long_mode

	; Set up paging structures for long mode
	call setup_page_tables

	; Enable paging and enter long mode
	call enable_paging

	; Load the 64-bit GDT descriptor
	lgdt [gdt64.pointer]

	; Far jump to 64-bit kernel entry point via code segment selector
	jmp gdt64.code_segment:long_mode_start

	; Halt if for some reason jump returns
	hlt

; -----------------------------------------------------------------------------
; check_multiboot:
; Validates the multiboot magic number passed in eax
; Jumps to error if invalid
; -----------------------------------------------------------------------------
check_multiboot:
	cmp eax, 0x36d76289
	jne .no_multiboot
	ret
.no_multiboot:
	mov al, "M"    ; Error code 'M'
	jmp error

; -----------------------------------------------------------------------------
; check_cpuid:
; Checks if the CPU supports the CPUID instruction by toggling ID flag in EFLAGS
; Jumps to error if unsupported
; -----------------------------------------------------------------------------
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
	mov al, "C"    ; Error code 'C'
	jmp error

; -----------------------------------------------------------------------------
; check_long_mode:
; Checks if CPU supports long mode by querying extended CPUID features
; Jumps to error if not supported
; -----------------------------------------------------------------------------
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
	mov al, "L"    ; Error code 'L'
	jmp error

; -----------------------------------------------------------------------------
; setup_page_tables:
; Initializes the paging structures for long mode with 2MiB pages
; Sets up PML4, PDPT, and PD entries with appropriate flags
; -----------------------------------------------------------------------------
setup_page_tables:
	mov eax, page_table_l3
	or eax, 0b11 ; present, writable
	mov [page_table_l4], eax
	
	mov eax, page_table_l2
	or eax, 0b11 ; present, writable
	mov [page_table_l3], eax

	mov ecx, 0 ; counter
.loop:
	mov eax, 0x200000 ; 2MiB page size
	mul ecx
	or eax, 0b10000011 ; present, writable, huge page
	mov [page_table_l2 + ecx * 8], eax

	inc ecx
	cmp ecx, 512 ; fill all 512 entries
	jne .loop

	ret

; -----------------------------------------------------------------------------
; enable_paging:
; Enables paging by setting CR3 to PML4 address, enabling PAE,
; enabling long mode in IA32_EFER MSR, and enabling paging in CR0
; -----------------------------------------------------------------------------
enable_paging:
	mov eax, page_table_l4
	mov cr3, eax

	mov eax, cr4
	or eax, 1 << 5 ; enable PAE
	mov cr4, eax

	mov ecx, 0xC0000080 ; IA32_EFER MSR
	rdmsr
	or eax, 1 << 8 ; enable long mode
	wrmsr

	mov eax, cr0
	or eax, 1 << 31 ; enable paging
	mov cr0, eax

	ret

; -----------------------------------------------------------------------------
; error:
; Displays error message "ERR: X" where X is error code in AL,
; then halts the CPU
; -----------------------------------------------------------------------------
error:
	mov dword [0xb8000], 0x4f524f45 ; "ERRO"
	mov dword [0xb8004], 0x4f3a4f52 ; "R:OR"
	mov dword [0xb8008], 0x4f204f20 ; " O O "
	mov byte  [0xb800a], al         ; error code character
	hlt

; -----------------------------------------------------------------------------
; BSS section: reserve 4 KiB aligned paging tables
; -----------------------------------------------------------------------------
section .bss
align 4096
page_table_l4:
	resb 4096
page_table_l3:
	resb 4096
page_table_l2:
	resb 4096

; Uncomment the following to reserve stack here (instead of linker)
;stack_bottom:
;	resb 4096 * 4
;stack_top:

; -----------------------------------------------------------------------------
; Read-only data section containing 64-bit GDT entries and pointer descriptor
; -----------------------------------------------------------------------------
section .rodata
gdt64:
	dq 0 ; null descriptor
.code_segment: equ $ - gdt64
	dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) ; 64-bit code segment descriptor
.pointer:
	dw $ - gdt64 - 1 ; limit (size of GDT - 1)
	dq gdt64          ; base address of GDT
