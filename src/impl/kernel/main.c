/*
 * kernel_main.c - Entry point for the GatOS 64-bit kernel
 *
 * This file defines the `kernel_main` function, which is the first function
 * called once the kernel takes control after boot.
 *
 * Author: u/ApparentlyPlus
 */

#include "print.h"
#include "misc.h"
#include "multiboot2.h"

static char* KERNEL_VERSION = "v1.5.0";

/*
 * kernel_main - Main entry point for the GatOS kernel
 * @mb_info: Pointer to the Multiboot2 information structure provided by the bootloader
 */
void kernel_main(void* mb_info) {
	print_clear();
	print_banner(KERNEL_VERSION);

	multiboot2_parser_t* p;
	multiboot2_parser_init(p, mb_info);
	mb2_dump(p);
	
	check_kernel_position();
}