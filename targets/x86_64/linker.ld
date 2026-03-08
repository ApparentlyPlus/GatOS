ENTRY(start)

KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000;
KPHYS_START = 0x10000;

SECTIONS
{
    . = KPHYS_START + KERNEL_VIRTUAL_BASE;
    KVIRT_START = .;

    .boot : AT(ADDR(.boot) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        KEEP(*(.multiboot_header))
    }

    /* Save the kernel's virtual address state */
    KERNEL_VMA_MARK = .;
    KERNEL_LMA_MARK = LOADADDR(.boot) + SIZEOF(.boot);

    /* Switch VMA to userspace */
    . = 0x400000;

    .user_text : AT(ALIGN(KERNEL_LMA_MARK, 4096)) ALIGN(4K)
    {
        USER_TEXT_LOAD_ADDR = LOADADDR(.user_text);
        USER_TEXT_START = .;
        *(.user_text .user_text.*)
        *ulibc*(.text .text.*)
        USER_TEXT_END = .;
    }

    .user_rodata : AT(ALIGN(LOADADDR(.user_text) + SIZEOF(.user_text), 4096)) ALIGN(4K)
    {
        USER_RODATA_LOAD_ADDR = LOADADDR(.user_rodata);
        USER_RODATA_START = .;
        *(.user_rodata .user_rodata.*)
        *ulibc*(.rodata .rodata.*)
        USER_RODATA_END = .;
    }

    .user_data : AT(ALIGN(LOADADDR(.user_rodata) + SIZEOF(.user_rodata), 4096)) ALIGN(4K)
    {
        USER_DATA_LOAD_ADDR = LOADADDR(.user_data);
        USER_DATA_START = .;
        *(.user_data .user_data.*)
        *ulibc*(.data .data.*)
        USER_DATA_END = .;
    }

    .user_bss : AT(ALIGN(LOADADDR(.user_data) + SIZEOF(.user_data), 4096)) ALIGN(4K)
    {
        USER_BSS_LOAD_ADDR = LOADADDR(.user_bss);
        USER_BSS_START = .;
        *(.user_bss .user_bss.*)
        *ulibc*(.bss .bss.* COMMON)
        USER_BSS_END = .;
    }

    /* Restore VMA back to kernel space */
    . = KERNEL_VMA_MARK + (LOADADDR(.user_bss) + SIZEOF(.user_bss) - KERNEL_LMA_MARK);
    . = ALIGN(4K);

    .text : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.text .text.*) 
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.bss*)
        *(COMMON)
    }

    .stack : AT(ADDR(.stack) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        KERNEL_STACK_BOTTOM = .; 
        . = . + 0x8000; /* 32 KiB */
        KERNEL_STACK_TOP = .;
    }

    . = ALIGN(4K);
    
    KPHYS_END = . - KERNEL_VIRTUAL_BASE;
    KVIRT_END = .;
}