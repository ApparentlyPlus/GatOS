/*
 * bootstrap.c - Staged kernel initialization
 *
 * Implements kernel_bootstrap, the capability-gated init sequence shared by
 * the template's kernel_main and appa's emitted boot preamble.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
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
#include <klibc/stdio.h>

static uint8_t multiboot_buffer[8 * 1024];

/*
 * kernel_bootstrap - Bring the kernel from multiboot handoff to fully
 * initialized with interrupts enabled. Returns false on a fatal early
 * failure (before panic is usable). verbose gates the kprintf banner.
 */
bool kernel_bootstrap(void* mb_info, multiboot_parser_t* mb, bool verbose) {
	serial_init_port(SERIAL_COM1);
	serial_init_port(SERIAL_COM2);
#ifdef GATA_CAP_THREADS
	serial_init_port(SERIAL_COM3);
#endif

	// IDT must be initialized before pretty much anything else,
	// since we rely on interrupts for APIC, timers, input, and basically everything else
	idt_init();

	// Multiboot comes next since we need to parse the memory map and other info before we can safely initialize memory management
	multiboot_init(mb, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	if (!mb->initialized) {
		return false;
	}

	// Early paging and physmap
	reserve_required_tablespace(mb);

	cleanup_kpt(0x0, get_kend(false));

	// With this I cast memory management
	build_physmap();

	// We need panic to work right about now
	// if we panic before this, something went catastrophically wrong
	console_init(mb);

	// Initialize PMM before VMM since VMM needs to allocate memory for page tables
	pmm_status_t pmm_status = pmm_init(0x0, PHYSMAP_V2P(get_physmap_end()), PAGE_SIZE);
	if(pmm_status != PMM_OK) {
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

	// The crash console can only size its scroll shadow now that the PMM is
	// up; console_init ran before this, so until here a panic could render
	// but not scroll.
	con_crash_shadow_init();

#ifdef GATA_CAP_MEM
	// Initialize slab allocator before VMM since VMM needs to allocate memory for its structures
	slab_status_t slab_status = slab_init();
	if(slab_status != SLAB_OK) {
		return false;
	}

	// Initialize VMM and switch to it
	vmm_status_t vmm_status = vmm_kernel_init(get_kend(true) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
	if(vmm_status != VMM_OK) {
		return false;
	}
#endif // GATA_CAP_MEM

	// With the VMM online (if built), we can use virtual addresses for everything from now on.
	// GDT/CPU init don't actually need the heap - they get their stacks straight from the PMM.
	gdt_init();
	cpu_init();

#ifdef GATA_CAP_MEM
	// kmalloc after heap init is available
	heap_status_t heap_status = heap_kernel_init();

	if(heap_status != HEAP_OK) {
		return false;
	}
#endif // GATA_CAP_MEM

#ifdef GATA_NEEDS_INTERRUPT_SUBSYS
	// ACPI and APIC come after memory management since they require dynamic memory for tables and structures
	// and they need to be initialized before we can safely enable interrupts
	acpi_init(mb);
	if (verbose) {
		kprintf("[ACPI] Revision %u detected (%s supported), manufacturer: %.6s\n",
		       acpi_get_rsdp()->Revision,
		       acpi_is_xsdt_supported() ? "XSDT" : "RSDT",
		       acpi_get_rsdp()->OEMID);
	}
	
	apic_init();
	if (verbose) kprintf("[APIC] Local APIC and I/O APIC initialized successfully\n");

	// Timers before scheduler
	timer_init();
	power_rapl_init(); // RAPL needs uptime (TSC calibrated by timer_init)
#endif // GATA_NEEDS_INTERRUPT_SUBSYS

#ifdef GATA_CAP_THREADS
	// Syscalls before userspace
	syscall_init();

	// TTYs and output finally online
	tty_t* k_tty = tty_create();
	if (!k_tty) panic("Failed to create kernel TTY!");

	// Default to Kernel TTY
	active_tty = k_tty;
	kernel_tty = k_tty; // Protect this from ALT+F4
#endif // GATA_CAP_THREADS

	// Input drivers and subsystems (the static ring-buffer path when there's
	// no scheduler/TTY is harmless to init either way - it's a no-op if
	// GATA_CAP_INPUT is also off)
	input_init();

	if (verbose) {
		kprintf("[KERNEL] CPU initialization complete (x86_64, long mode).\n");
#ifdef GATA_NEEDS_INTERRUPT_SUBSYS
		kprintf("[KERNEL] ACPI revision %u detected (%s supported).\n",
	           acpi_get_rsdp()->Revision,
	           acpi_is_xsdt_supported() ? "XSDT" : "RSDT");
		kprintf("[KERNEL] Advanced Programmable Interrupt Controller (APIC) routed.\n");
#endif
		kprintf("[KERNEL] Physical Memory Manager (PMM) configured (RAM mapped via Physmap).\n");
#ifdef GATA_CAP_MEM
		kprintf("[KERNEL] Virtual Memory Manager (VMM) active (Higher Half).\n");
		kprintf("[KERNEL] Heap and Slab allocators initialized.\n");
#endif
#ifdef GATA_CAP_THREADS
		kprintf("[KERNEL] Syscall Interface (MSRs) enabled.\n");
#endif
#ifdef GATA_OUTPUT_SERIAL
		kprintf("[KERNEL] Output routed to COM1 (no framebuffer/console built).\n");
#else
		kprintf("[KERNEL] Framebuffer resolution %dx%dx%d initialized.\n",
	           multiboot_get_framebuffer(mb)->width,
	           multiboot_get_framebuffer(mb)->height,
	           multiboot_get_framebuffer(mb)->bpp);
#ifdef GATA_CAP_THREADS
		kprintf("[KERNEL] Dynamic TTY subsystem online.\n");
		kprintf("[KERNEL] Use ALT+Tab to cycle between available consoles.\n");
#else
		kprintf("[KERNEL] Static framebuffer console online (no scheduler/TTY built).\n");
#endif
#endif // GATA_OUTPUT_SERIAL
	}

#ifdef GATA_CAP_INPUT
	// Keyboard and routing
	keyboard_init();
	irq_register(INT_FIRST_INTERRUPT + 1, (irq_handler_t)keyboard_handler);
	ioapic_redirect(1, INT_FIRST_INTERRUPT + 1, lapic_get_id(), 0);
	ioapic_unmask(1); // we allow the keyboard IRQ to be handled after this point, since the handler is registered and ready to go
	if (verbose) kprintf("[KBD] Keyboard IRQ 1 routed and unmasked.\n");

#if defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)
	// PCI and USB for external keyboards
	pci_init();
	if (!xhci_init()) {
		if (verbose) kprintf("[XHCI] No USB keyboard detected; PS/2 remains active.\n");
	}
#endif // GATA_KBD_EXTERNAL || GATA_KBD_HOTPLUG
#endif // GATA_CAP_INPUT

#ifdef GATA_CAP_THREADS
	// Enable multitasking and userspace
	process_init();
	sched_init();
#if defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)
	xhci_hotplug_init();
#endif

	// Dashboard and final touches
	dash_init();
	if (verbose) kprintf("[KERNEL] Dashboard ready (CTRL+SHIFT+ESC)\n");
#endif // GATA_CAP_THREADS

	// Let the good times roll
	intr_on();

	return true;
}
