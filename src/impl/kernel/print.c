/*
 * print.c - VGA text mode printing utilities for GatOS kernel
 *
 * Provides functions to print characters, strings, integers, and hex values
 * directly to the VGA text buffer at memory address 0xB8000.
 * Supports colored text output and simple scrolling when printing new lines.
 * 
 * Author: u/ApparentlyPluss
 */

#include "print.h"

// VGA text mode constants
const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*) 0xb8000;
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | PRINT_COLOR_BLACK << 4;

/*
 * clear_row - Clears all characters in a specific row on the screen
 * @row: The row index to clear (0-based)
 *
 * Fills the entire row with spaces using the current color attribute.
 */
void clear_row(size_t row) {
    struct Char empty = (struct Char) {
        character: ' ',
        color: color,
    };

    for (size_t col = 0; col < NUM_COLS; col++) {
        buffer[col + NUM_COLS * row] = empty;
    }
}

/*
 * print_clear - Clears the entire VGA text buffer screen
 *
 * Calls clear_row for each row to reset the display.
 */
void print_clear() {
    for (size_t i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
}

/*
 * print_newline - Moves cursor to the next line,
 * scrolling the screen up if at the bottom.
 */
void print_newline() {
    col = 0;

    if (row < NUM_ROWS - 1) {
        row++;
        return;
    }

    for (size_t row = 1; row < NUM_ROWS; row++) {
        for (size_t col = 0; col < NUM_COLS; col++) {
            struct Char character = buffer[col + NUM_COLS * row];
            buffer[col + NUM_COLS * (row - 1)] = character;
        }
    }

    clear_row(NUM_COLS - 1);
}

/*
 * print_char - Prints a single character at current cursor position
 * @character: The ASCII character to print
 *
 * Supports newline character to advance to next line.
 */
void print_char(char character) {
    if (character == '\n') {
        print_newline();
        return;
    }

    if (col > NUM_COLS) {
        print_newline();
    }

    buffer[col + NUM_COLS * row] = (struct Char) {
        character: (uint8_t) character,
        color: color,
    };

    col++;
}

/*
 * print_str - Prints a null-terminated string to the screen
 * @str: Pointer to the string to print
 */
void print_str(char* str) {
    for (size_t i = 0; 1; i++) {
        char character = (uint8_t) str[i];

        if (character == '\0') {
            return;
        }

        print_char(character);
    }
}

/*
 * print_set_color - Sets the current foreground and background color
 * @foreground: Foreground color code (0-15)
 * @background: Background color code (0-15)
 */
void print_set_color(uint8_t foreground, uint8_t background) {
    color = foreground + (background << 4);
}

/*
 * print_int - Prints a signed integer as decimal characters
 * @value: The integer value to print
 */
void print_int(int value) {
    char buffer[12]; // Enough for -2^31 and null terminator
    int i = 0;
    int is_negative = 0;

    if (value == 0) {
        print_char('0');
        return;
    }

    if (value < 0) {
        is_negative = 1;
        value = -value;
    }

    while (value > 0) {
        int digit = value % 10;
        buffer[i++] = '0' + digit;
        value /= 10;
    }

    if (is_negative) {
        buffer[i++] = '-';
    }

    // Reverse and print
    while (i--) {
        print_char(buffer[i]);
    }
}

/*
 * print_hex - Prints a 32-bit unsigned integer in hexadecimal format
 * @value: The unsigned integer to print as hex
 */
void print_hex(uint32_t value) {
    print_str("0x");

    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (value >> (i * 4)) & 0xF;
        char hex_char = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        print_char(hex_char);
    }
}
