/*
 * bootstrap.c - Staged kernel initialization
 *
 * Implements kernel_bootstrap, the full init sequence formerly inlined in
 * kernel_main. Keeping it here leaves kmain.c as a thin caller.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/bootstrap.h>

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
#include <kernel/sys/panic.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/timers.h>
#include <kernel/sys/power.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <kernel/misc.h>
#include <klibc/stdio.h>

static uint8_t multiboot_buffer[8 * 1024];

/*
 * kernel_bootstrap - Bring the kernel from multiboot handoff to fully
 * initialized with interrupts enabled. Returns false on a fatal early
 * failure (before panic is usable). verbose gates the banner/kprintf
 * output; version is what the banner displays.
 */
bool kernel_bootstrap(void* mb_info, multiboot_parser_t* mb, bool verbose, const char* version) {

	// Init serial
	serial_init_port(COM1_PORT);
	serial_init_port(COM2_PORT);
	QEMU_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

	// IDT must be initialized before pretty much anything else,
	// since we rely on interrupts for APIC, timers, input, and basically everything else
	idt_init();
	QEMU_LOG("Initialized the IDT", TOTAL_DBG);

	// Multiboot comes next since we need to parse the memory map and other info before we can safely initialize memory management
	multiboot_init(mb, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	if (!mb->initialized) {
		QEMU_LOG("[KERNEL] Failed to initialize multiboot2 parser!", TOTAL_DBG);
		return false;
	}

	QEMU_LOG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

	// Early paging and physmap
	reserve_required_tablespace(mb);
	QEMU_LOG("Reserved the required space for page tables in the kernel region", TOTAL_DBG);

	cleanup_kpt(0x0, get_kend(false));
	QEMU_LOG("Unmapped all memory besides the higher half kernel range", TOTAL_DBG);

	// With this I cast memory management
	build_physmap();
	QEMU_LOG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);

	// We need panic to work right about now
	// if we panic before this, something went catastrophically wrong
	console_init(mb);
	QEMU_LOG("Initialized console", TOTAL_DBG);

	// Initialize PMM before VMM since VMM needs to allocate memory for page tables
	pmm_status_t pmm_status = pmm_init(0x0, PHYSMAP_V2P(get_physmap_end()), PAGE_SIZE);
	if(pmm_status != PMM_OK) {
		QEMU_LOG("[PMM] Failed to initialize physical memory manager", TOTAL_DBG);
		return false;
	}

	// Exclude kernel image from the allocator before populating freelists
	pmm_exclude_range(get_kstart(false), get_kend(false));

	// Populate freelists from firmware reported available regions
	for (size_t i = 0; i < mb->memory_map_length; i++) {
		uintptr_t region_start, region_end;
		uint32_t region_type;
		if (multiboot_get_memory_region(mb, i, &region_start, &region_end, &region_type) != 0)
			continue;
		if (region_type != MULTIBOOT_MEMORY_AVAILABLE){
			vmm_add_mmio(region_end - region_start);
			continue;
		}
		pmm_populate((uint64_t)region_start, (uint64_t)region_end);
	}
	QEMU_LOG("Initialized physical memory manager", TOTAL_DBG);

	// Initialize slab allocator before VMM since VMM needs to allocate memory for its structures
	slab_status_t slab_status = slab_init();
	if(slab_status != SLAB_OK) {
		QEMU_LOG("[Slab] Failed to initialize slab allocator", TOTAL_DBG);
		return false;
	}
	QEMU_LOG("Initialized slab allocator", TOTAL_DBG);

	// Initialize VMM and switch to it
	vmm_status_t vmm_status = vmm_kernel_init(get_kend(true) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
	if(vmm_status != VMM_OK) {
		QEMU_LOG("[VMM] Failed to initialize virtual memory manager", TOTAL_DBG);
		return false;
	}
	QEMU_LOG("Initialized kernel virtual memory manager", TOTAL_DBG);

	// With the VMM online, we can use virtual addresses for everything from now on
	gdt_init();
	cpu_init();
	QEMU_LOG("Parsed CPU information and configured GS base", TOTAL_DBG);

	// kmalloc after heap init is available
	heap_status_t heap_status = heap_kernel_init();

	if(heap_status != HEAP_OK) {
		QEMU_LOG("[HEAP] Failed to initialize kernel heap", TOTAL_DBG);
		return false;
	}
	QEMU_LOG("Initialized kernel heap", TOTAL_DBG);

	// ACPI and APIC come after memory management since they require dynamic memory for tables and structures
	// and they need to be initialized before we can safely enable interrupts
	acpi_init(mb);
	if (verbose) {
		kprintf("[ACPI] Revision %u detected (%s supported), manufacturer: %.6s\n",
		       acpi_get_rsdp()->Revision,
		       acpi_is_xsdt_supported() ? "XSDT" : "RSDT",
		       acpi_get_rsdp()->OEMID);
	}

	QEMU_LOG("Initialized ACPI subsystem", TOTAL_DBG);

	apic_init();
	QEMU_LOG("Initialized APIC subsystem", TOTAL_DBG);
	if (verbose) kprintf("[APIC] Local APIC and I/O APIC initialized successfully\n");

	// Timers before scheduler
	timer_init();
	power_rapl_init(); // RAPL needs uptime (TSC calibrated by timer_init)
	QEMU_LOG("Initialized system timers", TOTAL_DBG);

	// Syscalls before userspace
	syscall_init();
	QEMU_LOG("Initialized Syscall Interface", TOTAL_DBG);

	// TTYs and output finally online
	tty_t* k_tty = tty_create();
	if (!k_tty) panic("Failed to create kernel TTY!");

	// Default to Kernel TTY
	active_tty = k_tty;
	kernel_tty = k_tty; // Protect this from ALT+F4
	QEMU_LOG("Initialized Kernel TTY", TOTAL_DBG);

	// Input drivers and subsystems
	input_init();
	QEMU_LOG("Initialized input handling subsystem", TOTAL_DBG);

	// Banneeeeeeer!
	if (verbose) {
		print_banner((char*)version);
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
	           multiboot_get_framebuffer(mb)->width,
	           multiboot_get_framebuffer(mb)->height,
	           multiboot_get_framebuffer(mb)->bpp);
		kprintf("[KERNEL] Dynamic TTY subsystem online.\n");
		kprintf("[KERNEL] Use ALT+Tab to cycle between available consoles.\n");
	}

	// Keyboard and routing
	keyboard_init();
	irq_register(INT_FIRST_INTERRUPT + 1, (irq_handler_t)keyboard_handler);
	ioapic_redirect(1, INT_FIRST_INTERRUPT + 1, lapic_get_id(), 0);
	ioapic_unmask(1); // we allow the keyboard IRQ to be handled after this point, since the handler is registered and ready to go
	QEMU_LOG("Initialized Keyboard and routed IRQ 1", TOTAL_DBG);
	if (verbose) kprintf("[KBD] Keyboard IRQ 1 routed and unmasked.\n");

	// PCI and USB for external keyboards
	pci_init();
	if (xhci_init()) {
		QEMU_LOG("Initialized USB xHCI keyboard", TOTAL_DBG);
	} else {
		QEMU_LOG("No USB xHCI keyboard found (falling back to PS/2)", TOTAL_DBG);
		if (verbose) kprintf("[XHCI] No USB keyboard detected; PS/2 remains active.\n");
	}

	// Enable multitasking and userspace
	process_init();
	sched_init();
	xhci_hotplug_init();
	QEMU_LOG("Initialized Multitasking (Process & Scheduler)", TOTAL_DBG);

	// Dashboard and final touches
	dash_init();
	if (verbose) kprintf("[KERNEL] Dashboard ready (CTRL+SHIFT+ESC)\n");
	QEMU_LOG("Initialized kernel dashboard (CTRL+SHIFT+ESC)", TOTAL_DBG);

	// Let the good times roll
	intr_on();
	QEMU_LOG("Enabled interrupts", TOTAL_DBG);

	return true;
}
