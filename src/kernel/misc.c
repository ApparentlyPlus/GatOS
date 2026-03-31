/*
 * misc.c - Miscellaneous kernel utilities implementation
 *
 * Contains helper functions for kernel boot process including
 * banner display, position verification, and integer formatting.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/console.h>
#include <klibc/stdio.h>
#include <klibc/string.h>
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

    kprintf("\n");

    pad = (screen_width - CONTENT_WIDTH) / 2;
    if (pad < 0) pad = 0;

    for (i = 0; i < 8; i++) {
        for (j = 0; j < pad; j++) kprintf(" ");
        kprintf("%s\n", logo_lines[i]);
    }

    // Print Version
    console_set_color(CONSOLE_COLOR_MAGENTA, CONSOLE_COLOR_BLACK);

    len = 23 + kstrlen(KERNEL_VERSION); 
    pad = (screen_width - len) / 2;
    if (pad < 0) pad = 0;

    kprintf("\n");
    for (j = 0; j < pad; j++) kprintf(" ");
    kprintf("G a t O S   K e r n e l  %s\n\n", KERNEL_VERSION);
    
    // Print Metadata
    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
    
    const char* metadata[] = {
        "Created by: u/ApparentlyPlus",
        "Name inspired by: SkylOS, a project by u/BillyZeim"
    };

    for (i = 0; i < 2; i++) {
        len = kstrlen(metadata[i]);
        pad = (screen_width - len) / 2;
        if (pad < 0) pad = 0;
        
        for (j = 0; j < pad; j++) kprintf(" ");
        kprintf("%s\n", metadata[i]);
    }

    kprintf("\n");

    // Print Separator
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    
    // Print underscores across the full width of the screen
    for (j = 0; j < screen_width; j++) kprintf("_");
    
    kprintf("\n\n");
}

/*
 * print_test_banner - Prints the GatOS kernel test build banner
 */
void print_test_banner(char* KERNEL_VERSION){
    console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
    
    kprintf(
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
    kprintf("Welcome to the GatOS Kernel %s Test Build!\n\n", KERNEL_VERSION);
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
 * check_kpos - Verifies kernel is running in higher-half memory
 */
void check_kpos() {
    uintptr_t rip = get_rip();
   
    if (rip >= 0xFFFFFFFF80000000) {
        kprintf("[KERNEL] Running in higher-half kernel space\n");
    } else {
        kprintf("[KERNEL] Running in lower memory\n");
    }
}
