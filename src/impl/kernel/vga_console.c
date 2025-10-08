/*
 * vga_console.c - VGA text mode printing utilities for the console
 *
 * Provides functions to print characters and strings
 * directly to the VGA text buffer at memory address 0xB8000.
 * Supports colored text output and simple scrolling when printing new lines.
 * 
 * Author: u/ApparentlyPlus
 */

#include <vga_console.h>
#include <memory/paging.h>

// VGA text mode constants
const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*)KERNEL_P2V(0xb8000);
size_t col = 0;
size_t row = 0;
uint8_t color = CONSOLE_COLOR_WHITE | CONSOLE_COLOR_BLACK << 4;

/*
 * clear_row - Clears specified VGA text row
 */
void clear_row(size_t row) {
    struct Char empty = {
        .character = ' ',
        .color = color,
    };

    for (size_t col = 0; col < NUM_COLS; col++) {
        buffer[col + NUM_COLS * row] = empty;
    }
}

/*
 * console_clear - Clears entire VGA text buffer
 */
void console_clear() {
    for (size_t i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }

    row = 0;
    col = 0;
}

/*
 * print_newline - Advances cursor to next line (with scrolling)
 */
void print_newline() {
    col = 0;

    if (row < NUM_ROWS - 1) {
        row++;
        return;
    }

    for (size_t r = 1; r < NUM_ROWS; r++) {
        for (size_t c = 0; c < NUM_COLS; c++) {
            struct Char character = buffer[c + NUM_COLS * r];
            buffer[c + NUM_COLS * (r - 1)] = character;
        }
    }

    clear_row(NUM_ROWS - 1);
}

/*
 * console_print_char - Outputs single character to screen
 */
void console_print_char(char character) {
    if (character == '\n') {
        print_newline();
        return;
    }

    if (col >= NUM_COLS) {
        print_newline();
    }

    buffer[col + NUM_COLS * row] = (struct Char) {
        .character = (uint8_t) character,
        .color = color,
    };

    col++;
}

/*
 * console_set_color - Sets foreground/background text colors
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    color = foreground + (background << 4);
}
