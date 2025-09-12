ENTRY(start)

HH_KERNEL_OFFSET = 0xFFFFFFFF80000000;
KPHYS_START = 0x10000;

SECTIONS
{
	. = KPHYS_START + HH_KERNEL_OFFSET;
	KVIRT_START = .;

	.boot : AT(ADDR(.boot) - HH_KERNEL_OFFSET) ALIGN(4K)
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
		. = . + 0x8000; /* 32 KiB should be fine for now, will manage in C later */
		__stack_top = .;
	}

	. = . + 0x1000; /* Skip a 4KB block so we can safely write there */
	
	KPHYS_END = . - HH_KERNEL_OFFSET;
	KVIRT_END = .;
}