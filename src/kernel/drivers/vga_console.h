/*
 * vga_console.h - VGA text mode printing console interface
 *
 * Provides color definitions and function prototypes for screen output
 * including character/string printing and screen control.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define CONSOLE_COLOR_BLACK 0
#define CONSOLE_COLOR_BLUE 1
#define CONSOLE_COLOR_GREEN 2
#define CONSOLE_COLOR_CYAN 3
#define CONSOLE_COLOR_RED 4
#define CONSOLE_COLOR_MAGENTA 5
#define CONSOLE_COLOR_BROWN 6
#define CONSOLE_COLOR_LIGHT_GRAY 7
#define CONSOLE_COLOR_DARK_GRAY 8
#define CONSOLE_COLOR_LIGHT_BLUE 9
#define CONSOLE_COLOR_LIGHT_GREEN 10
#define CONSOLE_COLOR_LIGHT_CYAN 11
#define CONSOLE_COLOR_LIGHT_RED 12
#define CONSOLE_COLOR_PINK 13
#define CONSOLE_COLOR_YELLOW 14
#define CONSOLE_COLOR_WHITE 15

void console_print_char(char character);
void console_set_color(uint8_t foreground, uint8_t background);
void console_clear(void);
