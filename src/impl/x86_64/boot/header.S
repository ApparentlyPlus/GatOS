# -----------------------------------------------------------------------------
# Multiboot2 header section
# This section defines the Multiboot2 header that informs the bootloader
# about the kernel's entry point, architecture, and other metadata.
# It must be placed in a specific section and follow the Multiboot2 spec.
# -----------------------------------------------------------------------------

.intel_syntax noprefix

.section .multiboot_header, "a"

header_start:
	# magic number identifying Multiboot2 header
	.long 0xe85250d6

	# architecture (0 = i386 protected mode)
	.long 0

	# length of this header (from header_start to header_end)
	.long header_end - header_start

	# checksum (magic + architecture + length must sum to 0)
	.long -(0xe85250d6 + 0 + (header_end - header_start))

	# end tag indicating the end of the Multiboot header
	.short 0
	.short 0
	.long 8

header_end: