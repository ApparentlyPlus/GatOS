/*
 * vga_console.h - VGA text mode printing console interface
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <arch/x86_64/multiboot2.h>
#include <kernel/sys/spinlock.h>

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

typedef struct {
    uint32_t codepoint;
    uint8_t fg;
    uint8_t bg;
} console_char_t;

typedef struct {
    size_t cursor_x; // In characters
    size_t cursor_y; // In characters
    uint8_t fg_color;
    uint8_t bg_color;

    // UTF-8 State
    uint32_t utf8_codepoint;
    int utf8_bytes_needed;

    // Backbuffer
    console_char_t* buffer;
    size_t width;  // in chars
    size_t height; // in chars
    
    spinlock_t lock;
    int reentrancy_count;
} console_t;

// Global Hardware Management
void console_init(multiboot_parser_t* parser);

// Instance Management (Clean API)
void con_init(console_t* con);
void con_putc(console_t* con, char character);
void con_set_color(console_t* con, uint8_t foreground, uint8_t background);
void con_clear(console_t* con);
void con_refresh(console_t* con);

// Global Accessors (Targeting the active TTY's console)
void console_print_char(char character);
void console_set_color(uint8_t foreground, uint8_t background);
void console_clear(void);
size_t console_get_width();
size_t console_get_height();
