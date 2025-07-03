ENTRY(start)

SECTIONS
{
	. = 1M;

	.boot :
	{
		KEEP(*(.multiboot_header))
	}

	.text : ALIGN(4K)
	{
		*(.text)
	}

	.rodata : ALIGN(4K)
	{
		*(.rodata*)
	}

	.data : ALIGN(4K)
	{
		*(.data)
	}

	.bss : ALIGN(4K)
	{
		*(.bss*)
	}

	.stack : ALIGN(4K)
	{
		__stack_bottom = .;
		. = . + 0x8000; /* 32 KiB should be fine for now, will manage in C later */
		__stack_top = .;
	}
}