/*
 * kernel_main.c - Entry point for the GatOS 64-bit kernel
 *
 * This file defines the `kernel_main` function, which is the first function
 * called once the kernel takes control after boot.
 *
 * Author: u/ApparentlyPlus
 */

#include "print.h"
#include "multiboot2.h"

void print_banner();

static char* KERNEL_VERSION = "v1.0.0";

/*
 * kernel_main - Main entry point for the GatOS kernel
 * @mb_info: Pointer to the Multiboot2 information structure provided by the bootloader
 */
void kernel_main(void* mb_info) {

	print_clear();
	//print_banner();
	
	//print_set_color(PRINT_COLOR_LIGHT_GREEN, PRINT_COLOR_BLACK);
	//print_str("[+] 32 KiB of memory reserved for the kernel stack\n");

	multiboot2_parser_t mb2;
	multiboot2_parser_init(&mb2, mb_info);

	mb2_dump(&mb2);
	mb2_dump_memory_map(&mb2);
}

/*
 * print_banner - Prints the GatOS kernel banner and metadata to the screen
 */
void print_banner(){
	print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
	print_str(
"  ____       _    ___   ____\n"
" / ___| __ _| |_ / _ \\ / ___|\n"
"| |  _ / _` | __| | | |\\___ \\\n"
"| |_| | (_| | | | |_| | ___) |\n"
" \\____|\\__,_|\\_\\ \\___/ |____/\n");

	print_set_color(PRINT_COLOR_MAGENTA, PRINT_COLOR_BLACK);
	print_str("\nG a t O S   K e r n e l  ");
	print_str(KERNEL_VERSION);
	print_str("\n\n");
	
	print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
	print_str("This is a 64-bit long mode kernel!\n");
	print_str("Currently in VGA text mode, for testing.\n");
	print_str("Created by: u/ApparentlyPlus\n");
	print_str("Name inspired by: SkylOS, a project by u/BillyZeim\n\n");

	print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
	print_str("---------------------------------------------------\n\n");
}
