/*
 * main.c - Entry point for the GatOS 64-bit kernel
 *
 * This file defines the `kernel_main` function, which is the first function
 * called once the kernel takes control after boot. The entire staged init
 * sequence lives in kernel_bootstrap (kernel/bootstrap.c); this file hosts
 * only what runs after it: the userspace launch and the interactive loop.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/bootstrap.h>
#include <kernel/sys/power.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <klibc/stdio.h>

// Forward declaration of userspace app launcher
extern void uapps(void);

static char* KERNEL_VERSION = "v2.1.9";

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

	multiboot_parser_t multiboot = {0};
	if (!kernel_bootstrap(mb_info, &multiboot, true, KERNEL_VERSION)) return;

	// Enqueue userspace apps
	uapps();
	QEMU_LOG("Created userspace processes and threads", TOTAL_DBG);

	QEMU_LOG("Reached kernel end", TOTAL_DBG);

	// Simulate the kernel thread
	kprintf("[KERNEL] Kernel initialization complete, entering interactive test loop...\n");

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

	#endif
}