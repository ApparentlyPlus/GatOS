#include "print.h"
#include <stdint.h>

/*
 * print_banner - Prints the GatOS kernel banner and metadata to the screen
 */
void print_banner(char* KERNEL_VERSION){
	print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
	print(
"  ____       _    ___   ____\n"
" / ___| __ _| |_ / _ \\ / ___|\n"
"| |  _ / _` | __| | | |\\___ \\\n"
"| |_| | (_| | | | |_| | ___) |\n"
" \\____|\\__,_|\\_\\ \\___/ |____/\n");

	print_set_color(PRINT_COLOR_MAGENTA, PRINT_COLOR_BLACK);
	print("\nG a t O S   K e r n e l  ");
	print(KERNEL_VERSION);
	print("\n\n");
	
	print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
	print("This is a 64-bit long mode kernel!\n");
	print("Currently in VGA text mode, for testing.\n");
	print("Created by: u/ApparentlyPlus\n");
	print("Name inspired by: SkylOS, a project by u/BillyZeim\n\n");

	print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
	print("---------------------------------------------------\n\n");
}

uintptr_t get_rip() {
    uintptr_t rip;
    asm volatile ("lea (%%rip), %0" : "=r" (rip));
    return rip;
}

void check_kernel_position() {
    uintptr_t rip = get_rip();
   
    if (rip >= 0xFFFFFFFF80000000) {
        print("[KERNEL] Running in higher-half kernel space\n");
    } else {
        print("[KERNEL] Running in lower memory\n");
    }
}