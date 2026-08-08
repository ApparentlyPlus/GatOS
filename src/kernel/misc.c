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

#define UTF8_COLS(s, out) do {                                  \
        const char* _p = (s);                                   \
        (out) = 0;                                              \
        while (*_p) { if ((*_p & 0xC0) != 0x80) (out)++; _p++; } \
    } while (0)

/*
 * print_banner - Prints the GatOS kernel banner (optionally the logo too) and metadata centered
 */
void print_banner(char* KERNEL_VERSION)
{
    static const char* ll[] = {
        "      🬭🬵🬹🬹████████🬹🬹🬭🬏       🬭🬭🬭           🬞   ",
        "   🬞🬹█████████████████🬺🬱     █████🬹🬱🬭🬭🬭🬭🬭🬵🬻█🬱  ",
        "  🬵██████████████████████🬏   ▐███████████████🬱 ",
        " 🬻██████🬎🬂🬂🬂🬂🬊🬬███████████🬏  ▐████████████████🬓",
        "🬷█████🬝🬀       🬊██████████🬺  ▐█████████████████",
        "██████🬀         🬨██████████🬏 🬉█████████████████",
        "██████🬏         ▐██████████🬲  🬬███████🬎🬂  🬊███🬄",
        "🬨█████🬺🬏        🬁███████████🬱  🬊🬎🬎🬎🬎🬂       🬂🬀 ",
        " 🬬██████🬹🬭🬭🬭     🬊███████████🬺🬱🬭🬭              ",
        "  🬊█████████████🬱 🬊██████████████████🬹         ",
        "   🬁🬎████████████   🬊🬬████████████████🬄        ",
        "      🬂🬊🬎🬎█████🬎🬂     🬁🬂🬎🬎🬬█████████🬎🬆         "
    };

    static const char* ww[] = {
        "   █████████             █████       ███████     █████████ ",
        "  ███░░░░░███           ░░███      ███░░░░░███  ███░░░░░███",
        " ███     ░░░   ██████   ███████   ███     ░░███░███    ░░░ ",
        "░███          ░░░░░███ ░░░███░   ░███      ░███░░█████████ ",
        "░███    █████  ███████   ░███    ░███      ░███ ░░░░░░░░███",
        "░░███  ░░███  ███░░███   ░███ ███░░███     ███  ███    ░███",
        " ░░█████████ ░░████████  ░░█████  ░░░███████░  ░░█████████ ",
        "  ░░░░░░░░░   ░░░░░░░░    ░░░░░     ░░░░░░░     ░░░░░░░░░  "
    };

    static const char* metadata[] = {
        "Created by: u/ApparentlyPlus",
        "Name inspired by: SkylOS, a project by u/BillyZeim"
    };

    static const char* version_prefix = "G a t O S   K e r n e l  ";

    const int gap = 3;

    uint16_t sw = console_get_width();
    int lr = (int)(sizeof(ll) / sizeof(ll[0]));
    int wr = (int)(sizeof(ww) / sizeof(ww[0]));
    int mr = (int)(sizeof(metadata) / sizeof(metadata[0]));
    int lc = 0, wc = 0;
    int rows, lt, wt, bc;
    int i, j, pad, w;

    for (i = 0; i < lr; i++) {
        UTF8_COLS(ll[i], w);
        if (w > lc) lc = w;
    }
    for (i = 0; i < wr; i++) {
        UTF8_COLS(ww[i], w);
        if (w > wc) wc = w;
    }

    bc = lc + gap + wc;

    rows = (lr > wr) ? lr : wr;
    lt = (rows - lr) / 2;
    wt = (rows - wr) / 2;

    kprintf("\n");

    if ((int)sw >= bc) {
        pad = ((int)sw - bc) / 2;
        if (pad < 0) pad = 0;

        for (i = 0; i < rows; i++) {
            for (j = 0; j < pad; j++) kprintf(" ");

            console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
            if (i >= lt && i < lt + lr) {
                kprintf("%s", ll[i - lt]);
                UTF8_COLS(ll[i - lt], w);
            } else {
                w = 0;
            }

            if (i >= wt && i < wt + wr) {
                for (j = w; j < lc + gap; j++) kprintf(" ");
                console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
                kprintf("%s", ww[i - wt]);
            }
            kprintf("\n");
        }
    } else {
        console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
        pad = ((int)sw - wc) / 2;
        if (pad < 0) pad = 0;
        for (i = 0; i < wr; i++) {
            for (j = 0; j < pad; j++) kprintf(" ");
            kprintf("%s\n", ww[i]);
        }
    }

    console_set_color(CONSOLE_COLOR_MAGENTA, CONSOLE_COLOR_BLACK);
    UTF8_COLS(version_prefix, w);
    UTF8_COLS(KERNEL_VERSION, j);
    pad = ((int)sw - (w + j)) / 2;
    if (pad < 0) pad = 0;

    kprintf("\n");
    for (j = 0; j < pad; j++) kprintf(" ");
    kprintf("%s%s\n\n", version_prefix, KERNEL_VERSION);

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
    for (i = 0; i < mr; i++) {
        UTF8_COLS(metadata[i], w);
        pad = ((int)sw - w) / 2;
        if (pad < 0) pad = 0;
        for (j = 0; j < pad; j++) kprintf(" ");
        kprintf("%s\n", metadata[i]);
    }

    kprintf("\n");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    for (j = 0; j < (int)sw; j++) kprintf("_");

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
