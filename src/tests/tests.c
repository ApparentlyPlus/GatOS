/*
 * test.c - Entry point for the GatOS 64-bit kernel test build
 *
 * This file defines the `kernel_test` function, which is the first function
 * called once the kernel takes control after boot, assuming a test build.
 *
 * Author: u/ApparentlyPlus
 */

#ifdef TEST_BUILD

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/multiboot2.h>
#include <arch/x86_64/cpu/cpu.h>

#include <kernel/drivers/vga_console.h>
#include <kernel/drivers/vga_stdio.h>
#include <kernel/drivers/serial.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <kernel/misc.h>
#include <tests/tests.h>
#include <libc/string.h>

#define TOTAL_DBG 6

static uint8_t multiboot_buffer[8 * 1024];

/*
 * kernel_test - Main entry point for the GatOS kernel test build
 */
void kernel_test(void* mb_info, char* KERNEL_VERSION) {

	// Clear the console and print a welcome message

	console_clear();
	print_test_banner(KERNEL_VERSION);

	// Serial Initialization
	serial_init_port(COM1_PORT);
	serial_init_port(COM2_PORT);

    LOGF("[!] This is a GatOS Kernel Test Build for version %s\n", KERNEL_VERSION);

    // Initialize core subsystems
	idt_init();
	enable_interrupts();
	cpu_init();

	multiboot_parser_t multiboot = {0};
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));
	if (!multiboot.initialized) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
       	printf("[KERNEL] Failed to initialize multiboot2 parser!\n");
    	return;
    }

	reserve_required_tablespace(&multiboot);
	cleanup_kernel_page_tables(0x0, get_kend(false));
	unmap_identity();
	build_physmap();
	acpi_init(&multiboot);

    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
    printf("[+] Kernel initialization succeded!\n\n");
    QEMU_LOG("Kernel initialization succeeded!", TOTAL_DBG);
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

	pmm_status_t pmm_status = pmm_init(get_kend(false) + PAGE_SIZE, PHYSMAP_V2P(get_physmap_end()), PAGE_SIZE);
	if(pmm_status != PMM_OK){
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[PMM] Failed to initialize physical memory manager, error code: %d\n", pmm_status);
		return;
	}

    printf("Running Kernel Physical Memory Manager tests...\n");
    test_pmm();
    QEMU_LOG("PMM Test Suite Completed", TOTAL_DBG);

	slab_status_t slab_status = slab_init();
	if(slab_status != SLAB_OK){
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
		printf("[Slab] Failed to initialize slab allocator, error code: %d\n", slab_status);
		return;
	}

    printf("Running Kernel Slab Allocator tests...\n");
    test_slab();
    QEMU_LOG("Slab Test Suite Completed", TOTAL_DBG);

	vmm_status_t vmm_status = vmm_kernel_init(get_kend(true) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
	if(vmm_status != VMM_OK){
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
		printf("[VMM] Failed to initialize virtual memory manager, error code: %d\n", vmm_status);
		return;
	}

    printf("Running Kernel Virtual Memory Manager tests...\n");
    test_vmm();
    QEMU_LOG("VMM Test Suite Completed", TOTAL_DBG);

	heap_status_t heap_status = heap_kernel_init();
	if(heap_status != HEAP_OK){
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
		printf("[HEAP] Failed to initialize kernel heap, error code: %d\n", heap_status);
		return;
	}

    printf("Running Kernel Heap tests...\n");
    test_heap();
    QEMU_LOG("Heap Test Suite Completed", TOTAL_DBG);

    // Finish up
    printf("\nAll kernel tests completed. Halting system.");
    QEMU_LOG("All Kernel Test Suites Completed", TOTAL_DBG);
}

#endif