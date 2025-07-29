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
#include "serial.h"
#include "multiboot2.h"
#include "memory/paging.h"
#include "libc/string.h"

static char* KERNEL_VERSION = "v1.5.0";
static uint8_t multiboot_buffer[8 * 1024]; // 8KB should be more than enough

/*
 * kernel_main - Main entry point for the GatOS kernel
 * @mb_info: Pointer to the Multiboot2 information structure provided by the bootloader
 */
void kernel_main(void* mb_info) {
	print_clear();
	print_banner(KERNEL_VERSION);

	multiboot_parser_t multiboot = {0};
	
	// Initialize multiboot parser (copies everything to higher half)
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	// Unmap anything besides [0, KPHYS_END] and [HH_BASE, HH_BASE + KPHYS_END]
	cleanup_page_tables();
	
	// Unmap [0, KPHYS_END], we only have [HH_BASE, HH_BASE + KPHYS_END] mapped
	unmap_identity();

    if (!multiboot.initialized) {
       	print("[KERNEL] Failed to initialize multiboot2 parser!\n");
    	return;
    }

	check_kernel_position();
}