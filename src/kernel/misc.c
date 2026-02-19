/*
 * misc.c - Miscellaneous kernel utilities implementation
 *
 * Contains helper functions for kernel boot process including
 * banner display, position verification, and integer formatting.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/console.h>
#include <kernel/drivers/stdio.h>
#include <stdint.h>

/*
 * print_banner - Prints the GatOS kernel banner and metadata centered
 */
void print_banner(char* KERNEL_VERSION) {
    uint16_t screen_width = console_get_width();
    int i, j, pad;
    size_t len;
    
    const int CONTENT_WIDTH = 59; 

    // Print Logo
    console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
    
    const char* logo_lines[] = {
        "   █████████             █████       ███████     █████████ ",
        "  ███░░░░░███           ░░███      ███░░░░░███  ███░░░░░███",
        " ███     ░░░   ██████   ███████   ███     ░░███░███    ░░░ ",
        "░███          ░░░░░███ ░░░███░   ░███      ░███░░█████████ ",
        "░███    █████  ███████   ░███    ░███      ░███ ░░░░░░░░███",
        "░░███  ░░███  ███░░███   ░███ ███░░███     ███  ███    ░███",
        " ░░█████████ ░░████████  ░░█████  ░░░███████░  ░░█████████ ",
        "  ░░░░░░░░░   ░░░░░░░░    ░░░░░     ░░░░░░░     ░░░░░░░░░  "
    };

    printf("\n");

    pad = (screen_width - CONTENT_WIDTH) / 2;
    if (pad < 0) pad = 0;

    for (i = 0; i < 8; i++) {
        for (j = 0; j < pad; j++) printf(" ");
        printf("%s\n", logo_lines[i]);
    }

    // Print Version
    console_set_color(CONSOLE_COLOR_MAGENTA, CONSOLE_COLOR_BLACK);

    len = 23 + strlen(KERNEL_VERSION); 
    pad = (screen_width - len) / 2;
    if (pad < 0) pad = 0;

    printf("\n");
    for (j = 0; j < pad; j++) printf(" ");
    printf("G a t O S   K e r n e l  %s\n\n", KERNEL_VERSION);
    
    // Print Metadata
    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
    
    const char* metadata[] = {
        "Created by: u/ApparentlyPlus",
        "Name inspired by: SkylOS, a project by u/BillyZeim"
    };

    for (i = 0; i < 2; i++) {
        len = strlen(metadata[i]);
        pad = (screen_width - len) / 2;
        if (pad < 0) pad = 0;
        
        for (j = 0; j < pad; j++) printf(" ");
        printf("%s\n", metadata[i]);
    }

    printf("\n");

    // Print Separator
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    
    // Print underscores across the full width of the screen
    for (j = 0; j < screen_width; j++) printf("_");
    
    printf("\n\n");
}

/*
 * print_test_banner - Prints the GatOS kernel test build banner
 */
void print_test_banner(char* KERNEL_VERSION){
    console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
    
    printf(
        " @@@@@@@@   @@@@@@   @@@@@@@   @@@@@@    @@@@@@   \n"
        "@@@@@@@@@  @@@@@@@@  @@@@@@@  @@@@@@@@  @@@@@@@   \n"
        "!@@        @@!  @@@    @@!    @@!  @@@  !@@       \n"
        "!@!        !@!  @!@    !@!    !@!  @!@  !@!       \n"
        "!@! @!@!@  @!@!@!@!    @!!    @!@  !@!  !!@@!!    \n"
        "!!! !!@!!  !!!@!!!!    !!!    !@!  !!!   !!@!!!   \n"
        ":!!   !!:  !!:  !!!    !!:    !!:  !!!       !:!  \n"
        ":!:   !::  :!:  !:!    :!:    :!:  !:!      !:!   \n"
        " ::: ::::  ::   :::     ::    ::::: ::  :::: ::   \n"
        " :: :: :    :   : :     :      : :  :   :: : :    \n\n"
    );
    
    console_set_color(CONSOLE_COLOR_MAGENTA, CONSOLE_COLOR_BLACK);
    printf("Welcome to the GatOS Kernel %s Test Build!\n\n", KERNEL_VERSION);
}

/*
 * int_to_str - Converts integer to string representation
 */
int int_to_str(int num, char *str) {
    int i = 0, sign = num;
    if (num < 0) num = -num;  // Make num positive
    
    do {  // Generate digits in reverse order
        str[i++] = num % 10 + '0';
    } while ((num /= 10) > 0);
    
    if (sign < 0) str[i++] = '-';  // Add sign if needed
    str[i] = '\0';  // Null-terminate
    
    // Reverse the string
    for (int j = 0, k = i - 1; j < k; j++, k--) {
        char c = str[j];
        str[j] = str[k];
        str[k] = c;
    }

	return i;
}

/*
 * get_rip - Retrieves current instruction pointer value
 */
uintptr_t get_rip() {
    uintptr_t rip;
    asm volatile ("lea (%%rip), %0" : "=r" (rip));
    return rip;
}

/*
 * check_kernel_position - Verifies kernel is running in higher-half memory
 */
void check_kernel_position() {
    uintptr_t rip = get_rip();
   
    if (rip >= 0xFFFFFFFF80000000) {
        printf("[KERNEL] Running in higher-half kernel space\n");
    } else {
        printf("[KERNEL] Running in lower memory\n");
    }
}