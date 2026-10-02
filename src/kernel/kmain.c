/*
 * kmain.c - Entry point for the GatOS 64-bit kernel
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <kernel/bootstrap.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/sys/power.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <klibc/stdio.h>

// run.py scrapes this for the ISO name
static char* KERNEL_VERSION = "v2.0.0";

void kernel_main(void* mb_info) {

	multiboot_parser_t multiboot = {0};
	if (!kernel_bootstrap(mb_info, &multiboot, true)) return;

	QEMU_LOG("Reached kernel end");

#ifdef GATA_CAP_INPUT
	// Simulate the kernel thread
	kprintf("[KERNEL] GatOS %s initialization complete, entering interactive test loop...\n", KERNEL_VERSION);

	while (1) {
	    char tt[128] = {0};

	    kprintf("\nType anything you want (shutdown or reboot to exit): ");

	    // Use scanset to read until newline
	    if (kscanf(" %127[^\n]", tt) > 0) {
	        if (kstrcmp(tt, "shutdown") == 0) {
				kprintf("Shutting down...\n");
	            power_off();
	        } else if (kstrcmp(tt, "reboot") == 0) {
				kprintf("Rebooting...\n");
	            reboot();
	        }
	        kprintf("You typed: %s\n", tt);
	    }
	}
#else
	// No input built, nothing to read, so idle instead of hanging on a keypress that can never come.
	kprintf("[KERNEL] Kernel initialization complete, idling (no input built).\n");
	while (1) cpu_idle();
#endif // GATA_CAP_INPUT
}
