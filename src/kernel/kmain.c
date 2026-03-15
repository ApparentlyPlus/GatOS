/*
 * main.c - Entry point for the GatOS 64-bit kernel
 *
 * This file defines the `kernel_main` function, which is the first function
 * called once the kernel takes control after boot.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/multiboot2.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/sys/apic.h>

#include <kernel/drivers/console.h>
#include <kernel/drivers/serial.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/userspace.h>
#include <kernel/drivers/input.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/xhci.h>
#include <kernel/drivers/dashboard.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/slab.h>
#include <kernel/sys/process.h>
#include <kernel/sys/syscall.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <kernel/misc.h>
#include <klibc/stdio.h>

// Forward declaration of userspace app launcher
extern void uapps(void);

#define TOTAL_DBG 26

static char* KERNEL_VERSION = "v2.0.0";

// If it is a test build, the multiboot buffer will be defined in tests.c
#ifndef TEST_BUILD
static uint8_t multiboot_buffer[8 * 1024];
#endif

/*
 * kernel_main - Main entry point for the GatOS kernel
 */
void kernel_main(void* mb_info) {

	// If this is a test build, run the test suite instead
	#ifdef TEST_BUILD
	#include <tests/tests.h>
		kernel_test(mb_info, KERNEL_VERSION);
		return;
	#else

	// Initialize serial (COM1) for QEMU output

	serial_init_port(COM1_PORT);

	// Initialize serial (COM2) for internal logging

	serial_init_port(COM2_PORT);
	QEMU_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

	// Set up the IDT

	idt_init();
	QEMU_LOG("Initialized the IDT", TOTAL_DBG);

	// Initialize multiboot parser (copies everything to higher half)

	multiboot_parser_t multiboot = {0};
	multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	if (!multiboot.initialized) {
		// kprintf won't work here yet, use serial
		QEMU_LOG("[KERNEL] Failed to initialize multiboot2 parser!", TOTAL_DBG);
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

	// Build the physmap (mapping of all physical RAM + framebuffer into virtual space)

	build_physmap();
	QEMU_LOG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);

	// console_init is heap free
	console_init(&multiboot);
	QEMU_LOG("Initialized console", TOTAL_DBG);

	// Initialize physical memory manager

	pmm_status_t pmm_status = pmm_init(get_kend(false) + PAGE_SIZE, PHYSMAP_V2P(get_physmap_end()), PAGE_SIZE);
	if(pmm_status == PMM_OK) {
		QEMU_LOG("Initialized physical memory manager", TOTAL_DBG);
	}
	else {
		QEMU_LOG("[PMM] Failed to initialize physical memory manager", TOTAL_DBG);
		return;
	}

	// Initialize slab allocator

	slab_status_t slab_status = slab_init();
	if(slab_status != SLAB_OK) {
		QEMU_LOG("[Slab] Failed to initialize slab allocator", TOTAL_DBG);
		return;
	}
	QEMU_LOG("Initialized slab allocator", TOTAL_DBG);

	// Initialize virtual memory manager

	vmm_status_t vmm_status = vmm_kernel_init(get_kend(true) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
	if(vmm_status != VMM_OK) {
		QEMU_LOG("[VMM] Failed to initialize virtual memory manager", TOTAL_DBG);
		return;
	}
	QEMU_LOG("Initialized kernel virtual memory manager", TOTAL_DBG);

	// Initialize GDT and TSS for Ring 3 support
	gdt_init();

	// Parse CPU information and setup GS base
	cpu_init();
	QEMU_LOG("Parsed CPU information and configured GS base", TOTAL_DBG);

	// Initialize kernel heap

	heap_status_t heap_status = heap_kernel_init();

	if(heap_status != HEAP_OK) {
		// Serial klog as console isn't up
		QEMU_LOG("[HEAP] Failed to initialize kernel heap", TOTAL_DBG);
		return;
	}
	QEMU_LOG("Initialized kernel heap", TOTAL_DBG);

	// Initialize ACPI

	acpi_init(&multiboot);
	kprintf("[ACPI] Revision %u detected (%s supported), manufacturer: %.6s\n",
	       acpi_get_rsdp()->Revision,
	       acpi_is_xsdt_supported() ? "XSDT" : "RSDT",
	       acpi_get_rsdp()->OEMID);

	QEMU_LOG("Initialized ACPI subsystem", TOTAL_DBG);

	// Initialize APIC subsystem

	apic_init();
	QEMU_LOG("Initialized APIC subsystem", TOTAL_DBG);
	kprintf("[APIC] Local APIC and I/O APIC initialized successfully\n");

	// Initialize timers

	timer_init();
	QEMU_LOG("Initialized system timers", TOTAL_DBG);

	// Initialize Syscall Interface
    syscall_init();
	QEMU_LOG("Initialized Syscall Interface", TOTAL_DBG);

	// Initialize Kernel TTY
    tty_t* k_tty = tty_create();
    if (!k_tty) panic("Failed to create kernel TTY!");

	// Default to Kernel TTY
	g_active_tty = k_tty;
    g_kernel_tty = k_tty; // Protect this from ALT+F4
	QEMU_LOG("Initialized Kernel TTY", TOTAL_DBG);

	// Initialize input handling subsystem

	input_init();
	QEMU_LOG("Initialized input handling subsystem", TOTAL_DBG);

	print_banner(KERNEL_VERSION);
	kprintf("[KERNEL] CPU initialization complete (x86_64, long mode).\n");
	kprintf("[KERNEL] ACPI revision %u detected (%s supported).\n", 
           acpi_get_rsdp()->Revision, 
           acpi_is_xsdt_supported() ? "XSDT" : "RSDT");
	kprintf("[KERNEL] Advanced Programmable Interrupt Controller (APIC) routed.\n");
	kprintf("[KERNEL] Physical Memory Manager (PMM) configured (RAM mapped via Physmap).\n");
	kprintf("[KERNEL] Virtual Memory Manager (VMM) active (Higher Half).\n");
	kprintf("[KERNEL] Heap and Slab allocators initialized.\n");
	kprintf("[KERNEL] Syscall Interface (MSRs) enabled.\n");
	kprintf("[KERNEL] Framebuffer resolution %dx%dx%d initialized.\n", 
           multiboot_get_framebuffer(&multiboot)->width, 
           multiboot_get_framebuffer(&multiboot)->height,
           multiboot_get_framebuffer(&multiboot)->bpp);
	kprintf("[KERNEL] Dynamic TTY subsystem online.\n");
	kprintf("[KERNEL] Use ALT+Tab to cycle between available consoles.\n");

	// Initialize Keyboard

	keyboard_init();
	irq_register(INT_FIRST_INTERRUPT + 1, (irq_handler_t)keyboard_handler);
	ioapic_redirect(1, INT_FIRST_INTERRUPT + 1, lapic_get_id(), 0);
	ioapic_unmask(1);
	QEMU_LOG("Initialized Keyboard and routed IRQ 1", TOTAL_DBG);
	kprintf("[KBD] Keyboard IRQ 1 routed and unmasked.\n");

	// Initialize PCI subsystem and USB/xHCI stack
	pci_init();
	if (xhci_init()) {
		QEMU_LOG("Initialized USB xHCI keyboard", TOTAL_DBG);
	} else {
		QEMU_LOG("No USB xHCI keyboard found (falling back to PS/2)", TOTAL_DBG);
		kprintf("[XHCI] No USB keyboard detected; PS/2 remains active.\n");
	}

	// Initialize Multitasking
    process_init();
    sched_init();
	QEMU_LOG("Initialized Multitasking (Process & Scheduler)", TOTAL_DBG);

	// Create userspace processes and threads
	uapps();
	QEMU_LOG("Created userspace processes and threads", TOTAL_DBG);

	// Initialize kernel dashboard
	dash_init();
	kprintf("[KERNEL] Dashboard ready (CTRL+SHIFT+ESC)\n");
	QEMU_LOG("Initialized kernel dashboard (CTRL+SHIFT+ESC)", TOTAL_DBG);

	// Enable interrupts

	enable_interrupts();
	QEMU_LOG("Enabled interrupts", TOTAL_DBG);

	// All subsystems initialized successfully
	QEMU_LOG("Reached kernel end", TOTAL_DBG);
	kprintf("[KERNEL] Kernel initialization complete, entering interactive test loop...\n");
	
	while (1) {
	    char tt[128] = {0};

	    kprintf("\nType anything you want: ");

	    // Use scanset to read until newline
	    if (kscanf(" %127[^\n]", tt) > 0) {
	        kprintf("You typed: %s\n", tt);
	    }
	}
	
	#endif
}