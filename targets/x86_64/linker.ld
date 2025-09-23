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
		*(.text)
	}

	.rodata : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
	{
		*(.rodata*)
	}

	.data : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
	{
		*(.data)
	}

	.bss : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
	{
		*(.bss*)
	}

	.stack : AT(ADDR(.stack) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
	{
		KERNEL_STACK_TOP = .;
		. = . + 0x8000; /* 32 KiB should be fine for now, will manage in C later */
		KERNEL_STACK_TOP = .;
	}

	. = . + 0x1000; /* Skip a 4KB block so we can safely write there */
	
	KPHYS_END = . - KERNEL_VIRTUAL_BASE;
	KVIRT_END = .;
}