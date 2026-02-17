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

#include <kernel/drivers/console.h>
#include <kernel/drivers/stdio.h>
#include <kernel/drivers/serial.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/acpi.h>
#include <kernel/sys/apic.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/input.h>
#include <kernel/debug.h>
#include <kernel/misc.h>
#include <tests/tests.h>
#include <libc/string.h>

#define TOTAL_DBG 11

static uint8_t multiboot_buffer[8 * 1024];

/*
 * kernel_test - Main entry point for the GatOS kernel test build
 */
void kernel_test(void* mb_info, char* KERNEL_VERSION) {

	// Serial Initialization
	serial_init_port(COM1_PORT);
	serial_init_port(COM2_PORT);

    LOGF("[!] This is a GatOS Kernel Test Build for version %s\n", KERNEL_VERSION);

    // Initialize core subsystems
	idt_init();
	enable_interrupts();
	cpu_init();

	// Initialize multiboot parser
	multiboot_parser_t multiboot = {0};
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));
	if (!multiboot.initialized) {
        LOGF("[KERNEL] Failed to initialize multiboot2 parser!\n");
    	return;
    }

	// Memory management setup
	reserve_required_tablespace(&multiboot);
	cleanup_kernel_page_tables(0x0, get_kend(false));
	unmap_identity();
	build_physmap();

	// Run tests for each subsystem

	pmm_status_t pmm_status = pmm_init(get_kend(false) + PAGE_SIZE, PHYSMAP_V2P(get_physmap_end()), PAGE_SIZE);
	if(pmm_status != PMM_OK){
        LOGF("[PMM] Failed to initialize physical memory manager, error code: %d\n", pmm_status);
		return;
	}
    
    QEMU_LOG("PMM Initialized (Tests deferred)", TOTAL_DBG);

	slab_status_t slab_status = slab_init();
	if(slab_status != SLAB_OK){
        LOGF("[Slab] Failed to initialize slab allocator, error code: %d\n", slab_status);
		return;
	}
    QEMU_LOG("Slab Initialized (Tests deferred)", TOTAL_DBG);

	vmm_status_t vmm_status = vmm_kernel_init(get_kend(true) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
	if(vmm_status != VMM_OK){
        LOGF("[VMM] Failed to initialize virtual memory manager, error code: %d\n", vmm_status);
		return;
	}
    QEMU_LOG("VMM Initialized (Tests deferred)", TOTAL_DBG);

    // Initialize kernel heap so we can use kmalloc for console instances
	heap_status_t heap_status = heap_kernel_init();
	if(heap_status != HEAP_OK){
        LOGF("[HEAP] Failed to initialize kernel heap, error code: %d\n", heap_status);
		return;
	}

    // Now that VMM and Heap are ready, we can map the framebuffer and setup console instances
    console_init(&multiboot);

    static console_t test_console;
    con_init(&test_console);

    // Initialize Kernel TTY and a test TTY
    tty_t* k_tty = tty_create();
    if (!k_tty) panic("Failed to create kernel TTY!");

	// Default to Kernel TTY
	g_active_tty = k_tty;

    input_init();

    print_test_banner(KERNEL_VERSION);
    
    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
    printf("[+] Kernel initialization succeded! (Console Online)\n\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

    // NOW run the tests in order
    
    printf("Running Kernel Physical Memory Manager tests...\n");
    test_pmm();
    QEMU_LOG("PMM Test Suite Completed", TOTAL_DBG);

    printf("Running Kernel Slab Allocator tests...\n");
    test_slab();
    QEMU_LOG("Slab Test Suite Completed", TOTAL_DBG);

    printf("Running Kernel Virtual Memory Manager tests...\n");
    test_vmm();
    QEMU_LOG("VMM Test Suite Completed", TOTAL_DBG);
	
	acpi_init(&multiboot);
    apic_init();
    timer_init();

    printf("Running Kernel Heap tests...\n");
    test_heap();
    QEMU_LOG("Heap Test Suite Completed", TOTAL_DBG);

    printf("Running Kernel Timer tests...\n");
    test_timers();
    QEMU_LOG("Timer Test Suite Completed", TOTAL_DBG);

    printf("Running Spinlock Primitive tests...\n");
    test_spinlock();
    QEMU_LOG("Spinlock Test Suite Completed", TOTAL_DBG);

    printf("Running TTY Abstraction tests...\n");
    test_tty();
    QEMU_LOG("TTY Test Suite Completed", TOTAL_DBG);

    // Finish up
    printf("\nAll kernel tests completed. Halting system.");
    QEMU_LOG("All Kernel Test Suites Completed", TOTAL_DBG);
}

#endif
