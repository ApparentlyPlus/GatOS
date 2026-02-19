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