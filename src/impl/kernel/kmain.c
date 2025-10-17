/*
 * main.c - Entry point for the GatOS 64-bit kernel
 *
 * This file defines the `kernel_main` function, which is the first function
 * called once the kernel takes control after boot.
 *
 * Author: u/ApparentlyPlus
 */

#include <sys/interrupts.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <memory/heap.h>
#include <memory/slab.h>
#include <memory/paging.h>
#include <sys/ACPI.h>
#include <libc/string.h>
#include <multiboot2.h>
#include <vga_console.h>
#include <vga_stdio.h>
#include <misc.h>
#include <serial.h>
#include <debug.h>

#define TOTAL_DBG 11

static char* KERNEL_VERSION = "v1.6.6-alpha";
static uint8_t multiboot_buffer[8 * 1024]; // 8KB should be more than enough

/*
 * kernel_main - Main entry point for the GatOS kernel
 */
void kernel_main(void* mb_info) {

	// Clear the console and print the banner

	console_clear();
	print_banner(KERNEL_VERSION);

	// Initialize serial (COM1) for QEMU output
	serial_init_port(COM1_PORT);

	// Initialize serial (COM2) for internal logging
	serial_init_port(COM2_PORT);

	QEMU_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

	// Set up the IDT

	idt_init();
	printf("[IDT] The IDT was set-up successfully.\n");
	QEMU_LOG("Initialized the IDT", TOTAL_DBG);

	// Enable interrupts

	enable_interrupts();
	printf("[IDT] Enabled interrupts.\n");
	QEMU_LOG("Enabled interrupts using asm(\"sti\")", TOTAL_DBG);
	
	// Initialize multiboot parser (copies everything to higher half)

	multiboot_parser_t multiboot = {0};

    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	if (!multiboot.initialized) {
       	printf("[KERNEL] Failed to initialize multiboot2 parser!\n");
    	return;
    }

	QEMU_LOG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

	// Extend the kernel region to include space for the page tables to map all physical memory

	reserve_required_tablespace(&multiboot);
	QEMU_LOG("Reserved the required space for page tables in the kernel region", TOTAL_DBG);

	// Unmap anything besides [0, KPHYS_END] and [HH_BASE, HH_BASE + KPHYS_END]

	cleanup_kernel_page_tables(0x0, get_kend(false));
	QEMU_LOG("Unmapped all memory besides the kernel range", TOTAL_DBG);
	
	// Unmap [0, KPHYS_END], we only have [HH_BASE, HH_BASE + KPHYS_END] mapped

	unmap_identity();
	QEMU_LOG("Unmapped identity mapping, only higher half remains", TOTAL_DBG);

	// Build the physmap (mapping of all physical RAM into virtual space)

	build_physmap();
	printf("[MEM] Built physmap, all physical memory is now accessible\n");
	QEMU_LOG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);

	// Initialize ACPI

	acpi_init(&multiboot);
	printf("[ACPI] Revision %u detected (%s supported), manufacturer: %.6s\n",
       acpi_get_rsdp()->Revision,
       acpi_is_xsdt_supported() ? "XSDT" : "RSDT",
       acpi_get_rsdp()->OEMID);

	QEMU_LOG("Initialized ACPI subsystem", TOTAL_DBG);

	// Initialize physical memory manager

	pmm_status_t pmm_status = pmm_init(get_kend(false) + PAGE_SIZE, PHYSMAP_V2P(get_physmap_end()), PAGE_SIZE);
	if(pmm_status == PMM_OK){
		printf("[PMM] Physical memory manager range: 0x%llx - 0x%llx (%d MiB)\n", 
			get_kend(false) + PAGE_SIZE, 
			PHYSMAP_V2P(get_physmap_end()), 
			pmm_managed_size() / (1024 * 1024));

	 	QEMU_LOG("Initialized physical memory manager", TOTAL_DBG);
	}
	else{
		printf("[PMM] Failed to initialize physical memory manager, error code: %d\n", pmm_status);
		return;
	}

	// Final sanity check
	QEMU_LOG("Reached kernel end", TOTAL_DBG);
}