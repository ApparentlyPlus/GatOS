# header.S - Multiboot2 header section
# 
# This section defines the Multiboot2 header. It also requests a 
# framebuffer from the bootloader, and defines the end tag to indicate 
# the end of the header.
# 
# Author: u/ApparentlyPlus

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

    # framebuffer tag
    .align 8
    .short 5
    .short 0
    .long 20
    .long 0
    .long 0
    .long 0
	.align 8
	
	# end tag indicating the end of the Multiboot header
	.short 0
	.short 0
	.long 8

header_end: