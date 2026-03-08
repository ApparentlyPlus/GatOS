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

    .text : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.text .text.*) 
    }

    .user_text : AT(ADDR(.user_text) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        USER_TEXT_START = .;
        *(.user_text .user_text.*)
        USER_TEXT_END = .;
    }

    .user_rodata : AT(ADDR(.user_rodata) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        USER_RODATA_START = .;
        *(.user_rodata .user_rodata.*)
        USER_RODATA_END = .;
    }

    .user_data : AT(ADDR(.user_data) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        USER_DATA_START = .;
        *(.user_data .user_data.*)
        USER_DATA_END = .;
    }

    .user_bss : AT(ADDR(.user_bss) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        USER_BSS_START = .;
        *(.user_bss .user_bss.*)
        USER_BSS_END = .;
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

    . = . + 0x1000;
    
    KPHYS_END = . - KERNEL_VIRTUAL_BASE;
    KVIRT_END = .;
}