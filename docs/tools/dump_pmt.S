#include "paging.h"
.intel_syntax noprefix

.extern PML4
.global dbg_dump_pmt

.section .text
.code32

# -----------------------------------------------------------------------------
# Serial port output functions
# -----------------------------------------------------------------------------
# Write character in AL to serial port
serial_write_char:
	push dx
	mov dx, 0x3f8  # COM1 port
	out dx, al
	pop dx
	ret

# Write string pointed to by ESI to serial port (null-terminated)
serial_write_string:
	push eax
	push esi
.loop:
	lodsb
	test al, al
	jz .done
	call serial_write_char
	jmp .loop
.done:
	pop esi
	pop eax
	ret

# Write newline to serial port
serial_write_newline:
	push eax
	mov al, 13  # CR
	call serial_write_char
	mov al, 10  # LF
	call serial_write_char
	pop eax
	ret

# Write hex number in EAX to serial port (8 hex digits)
serial_write_hex32:
	push eax
	push ebx
	push ecx
	push edx
	
	mov ecx, 8  # 8 hex digits
	mov ebx, eax
.hex_loop:
	rol ebx, 4
	mov eax, ebx
	and eax, 0xf
	cmp eax, 9
	jle .digit
	add eax, 7  # A-F
.digit:
	add eax, 48  # '0'
	call serial_write_char
	dec ecx
	jnz .hex_loop
	
	pop edx
	pop ecx
	pop ebx
	pop eax
	ret

# Write 16-bit number in AX to serial port (4 hex digits)
serial_write_hex16:
	push eax
	push ebx
	push ecx
	push edx
	
	mov ecx, 4  # 4 hex digits
	mov ebx, eax
.hex16_loop:
	rol bx, 4
	mov eax, ebx
	and eax, 0xf
	cmp eax, 9
	jle .digit16
	add eax, 7  # A-F
.digit16:
	add eax, 48  # '0'
	call serial_write_char
	dec ecx
	jnz .hex16_loop
	
	pop edx
	pop ecx
	pop ebx
	pop eax
	ret

# -----------------------------------------------------------------------------
# Dump all present entries in PML4, PDPT, PD, PT
# Dumps physical address and flags
# -----------------------------------------------------------------------------
dbg_dump_pmt:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp

    # Save current stack frame
    mov ebp, esp

    # Print header
    mov al, 'P'
    call serial_write_char
    mov al, 'a'
    call serial_write_char
    mov al, 'g'
    call serial_write_char
    mov al, 'e'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'T'
    call serial_write_char
    mov al, 'a'
    call serial_write_char
    mov al, 'b'
    call serial_write_char
    mov al, 'l'
    call serial_write_char
    mov al, 'e'
    call serial_write_char
    mov al, 's'
    call serial_write_char
    mov al, ':'
    call serial_write_char
    call serial_write_newline

    # Start with PML4
    mov esi, offset V2P(PML4)
    mov ecx, 0  # PML4 index

.dump_pml4_loop:
    cmp ecx, 512
    jge .dump_done
    
    mov eax, [esi + ecx*8]
    test eax, 1
    jz .next_pml4

    # Print: PML4[i]: addr -> PDPT[j] where j is the PDPT index we'll iterate through
    mov al, 'P'
    call serial_write_char
    mov al, 'M'
    call serial_write_char
    mov al, 'L'
    call serial_write_char
    mov al, '4'
    call serial_write_char
    mov al, '['
    call serial_write_char
    mov eax, ecx
    call serial_write_hex16
    mov al, ']'
    call serial_write_char
    mov al, ':'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov eax, [esi + ecx*8]
    call serial_write_hex32
    mov al, ' '
    call serial_write_char
    mov al, '-'
    call serial_write_char
    mov al, '>'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'D'
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'T'
    call serial_write_char
    call serial_write_newline

    # Now dump PDPT entries for this PML4 entry
    mov edi, [esi + ecx*8]  # Get PDPT base address
    and edi, 0xFFFFF000     # Mask off flags
    push ecx                # Save PML4 index
    mov edx, 0              # PDPT index

.dump_pdpt_loop:
    cmp edx, 512
    jge .pdpt_done
    
    mov eax, [edi + edx*8]
    test eax, 1
    jz .next_pdpt

    # Print PDPT[i]: addr -> PD[j] where j is the PD index we'll iterate through
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'D'
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'T'
    call serial_write_char
    mov al, '['
    call serial_write_char
    mov eax, edx
    call serial_write_hex16
    mov al, ']'
    call serial_write_char
    mov al, ':'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov eax, [edi + edx*8]
    call serial_write_hex32
    mov al, ' '
    call serial_write_char
    mov al, '-'
    call serial_write_char
    mov al, '>'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'D'
    call serial_write_char
    call serial_write_newline

    # Now dump PD entries for this PDPT entry
    mov ebx, [edi + edx*8]  # Get PD base address
    and ebx, 0xFFFFF000     # Mask off flags
    push edx                # Save PDPT index
    mov edx, 0              # PD index (reuse edx)

.dump_pd_loop:
    cmp edx, 512
    jge .pd_done
    
    mov eax, [ebx + edx*8]
    test eax, 1
    jz .next_pd

    # Print PD[i]: addr -> PT[j] where j is the PT index we'll iterate through
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'D'
    call serial_write_char
    mov al, '['
    call serial_write_char
    mov eax, edx
    call serial_write_hex16
    mov al, ']'
    call serial_write_char
    mov al, ':'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov eax, [ebx + edx*8]
    call serial_write_hex32
    mov al, ' '
    call serial_write_char
    mov al, '-'
    call serial_write_char
    mov al, '>'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'T'
    call serial_write_char
    call serial_write_newline

    # For brevity, let's only show first few PT entries for each PD
    # Otherwise output would be massive (512*512 = 262k entries)
    mov eax, [ebx + edx*8]  # Get PT base address
    and eax, 0xFFFFF000     # Mask off flags
    push edx                # Save PD index
    push ebx                # Save PD base
    mov ebx, eax            # PT base address
    mov edx, 0              # PT index

.dump_pt_loop:
    cmp edx, 512             # Only show first 16 PT entries per PD
    jge .pt_done
    
    mov eax, [ebx + edx*8]
    test eax, 1
    jz .next_pt

    # Print PT[i]: addr -> PHYS[j] where j is the physical page index we'll iterate through
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'T'
    call serial_write_char
    mov al, '['
    call serial_write_char
    mov eax, edx
    call serial_write_hex16
    mov al, ']'
    call serial_write_char
    mov al, ':'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov eax, [ebx + edx*8]
    call serial_write_hex32
    mov al, ' '
    call serial_write_char
    mov al, '-'
    call serial_write_char
    mov al, '>'
    call serial_write_char
    mov al, ' '
    call serial_write_char
    mov al, 'P'
    call serial_write_char
    mov al, 'H'
    call serial_write_char
    mov al, 'Y'
    call serial_write_char
    mov al, 'S'
    call serial_write_char
    call serial_write_newline

.next_pt:
    inc edx
    jmp .dump_pt_loop

.pt_done:
    pop ebx                 # Restore PD base
    pop edx                 # Restore PD index

.next_pd:
    inc edx
    jmp .dump_pd_loop

.pd_done:
    pop edx                 # Restore PDPT index

.next_pdpt:
    inc edx
    jmp .dump_pdpt_loop

.pdpt_done:
    pop ecx                 # Restore PML4 index

.next_pml4:
    inc ecx
    jmp .dump_pml4_loop

.dump_done:
    call serial_write_newline

    # Restore stack frame
    mov esp, ebp
    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret