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
void check_kernel_position();
uintptr_t get_rip();

static char* KERNEL_VERSION = "v1.5.0";

/*
 * kernel_main - Main entry point for the GatOS kernel
 * @mb_info: Pointer to the Multiboot2 information structure provided by the bootloader
 */
void kernel_main(void* mb_info) {
	print_clear();
	print_banner();

	multiboot2_parser_t* p;
	multiboot2_parser_init(p, mb_info);
	mb2_dump(p);
	
	check_kernel_position();
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

uintptr_t get_rip() {
    uintptr_t rip;
    asm volatile ("lea (%%rip), %0" : "=r" (rip));
    return rip;
}

void check_kernel_position() {
    uintptr_t rip = get_rip();
   
    if (rip >= 0xFFFFFFFF80000000) {
        print_str("[KERNEL] Running in higher-half kernel space\n");
    } else {
        print_str("[KERNEL] Running in lower memory\n");
    }
}