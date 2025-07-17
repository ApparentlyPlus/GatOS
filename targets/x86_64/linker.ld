ENTRY(start)

HH_KERNEL_OFFSET = 0xFFFFFF8000000000;
LH_KERNEL_START = 0x10000;

SECTIONS
{
	. = LH_KERNEL_START + HH_KERNEL_OFFSET;
	HH_KERNEL_START = .;

	.boot : AT(ADDR(.boot) - HH_KERNEL_OFFSET)
	{
		KEEP(*(.multiboot_header))
	}

	.text : AT(ADDR(.text) - HH_KERNEL_OFFSET) ALIGN(4K)
	{
		*(.text)
	}

	.rodata : AT(ADDR(.rodata) - HH_KERNEL_OFFSET) ALIGN(4K)
	{
		*(.rodata*)
	}

	.data : AT(ADDR(.data) - HH_KERNEL_OFFSET) ALIGN(4K)
	{
		*(.data)
	}

	.bss : AT(ADDR(.bss) - HH_KERNEL_OFFSET) ALIGN(4K)
	{
		*(.bss*)
	}

	.stack : AT(ADDR(.stack) - HH_KERNEL_OFFSET) ALIGN(4K)
	{
		__stack_bottom = .;
		. = . + 0x8000; /* 32 KiB should be fine for now */
		__stack_top = .;
	}
}