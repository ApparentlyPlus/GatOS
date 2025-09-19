/*
 * main.c - Entry point for the GatOS 64-bit kernel
 *
 * This file defines the `kernel_main` function, which is the first function
 * called once the kernel takes control after boot.
 *
 * Author: u/ApparentlyPlus
 */

#include <memory/paging.h>
#include <libc/string.h>
#include <multiboot2.h>
#include <print.h>
#include <misc.h>
#include <serial.h>
#include <debug.h>

#define TOTAL_DBG 7

static char* KERNEL_VERSION = "v1.5.5";
static uint8_t multiboot_buffer[8 * 1024]; // 8KB should be more than enough

/*
 * kernel_main - Main entry point for the GatOS kernel
 */
void kernel_main(void* mb_info) {
	DEBUG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

	print_clear();
	print_banner(KERNEL_VERSION);

	multiboot_parser_t multiboot = {0};
	
	// Initialize multiboot parser (copies everything to higher half)
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	if (!multiboot.initialized) {
       	print("[KERNEL] Failed to initialize multiboot2 parser!\n");
    	return;
    }

	DEBUG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

	// Extend the kernel region to include space for the page tables to map all physical memory
	reserve_required_tablespace(&multiboot);
	print("[MEM] Kernel region extended to include page tables.\n");
	DEBUG("Reserved the required space for page tables in the kernel region", TOTAL_DBG);

	// Unmap anything besides [0, KPHYS_END] and [HH_BASE, HH_BASE + KPHYS_END]
	cleanup_kernel_page_tables(0x0, get_kend(false));
	print("[MEM] Cleaned up page tables, unmapped everything besides the kernel range.\n");
	DEBUG("Unmapped all memory besides the kernel range", TOTAL_DBG);
	
	// Unmap [0, KPHYS_END], we only have [HH_BASE, HH_BASE + KPHYS_END] mapped
	unmap_identity();
	print("[MEM] Unmapped identity mapping, only higher half remains.\n");
	DEBUG("Unmapped identity mapping, only higher half remains", TOTAL_DBG);

	// Build the physmap (mapping of all physical RAM into virtual space)
	build_physmap();
	print("[MEM] Built physmap, all physical memory is now accessible.\n");
	DEBUG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);

	// Final sanity check
	check_kernel_position();
	DEBUG("Reached kernel end", TOTAL_DBG);
}