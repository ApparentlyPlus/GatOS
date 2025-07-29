/*
 * print.c - VGA text mode printing utilities for GatOS kernel
 *
 * Provides functions to print characters, strings, integers, and hex values
 * directly to the VGA text buffer at memory address 0xB8000.
 * Supports colored text output and simple scrolling when printing new lines.
 * 
 * Author: u/ApparentlyPlus
 */

#include "print.h"
#include "memory/paging.h"

// VGA text mode constants
const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*)P2V(0xb8000);
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | PRINT_COLOR_BLACK << 4;

void clear_row(size_t row) {
    struct Char empty = {
        .character = ' ',
        .color = color,
    };

    for (size_t col = 0; col < NUM_COLS; col++) {
        buffer[col + NUM_COLS * row] = empty;
    }
}

void print_clear() {
    for (size_t i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
}

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

void print_char(char character) {
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

void print(const char* str) {
    for (size_t i = 0; ; i++) {
        char character = (uint8_t) str[i];
        if (character == '\0') return;
        print_char(character);
    }
}

void print_set_color(uint8_t foreground, uint8_t background) {
    color = foreground + (background << 4);
}

void print_int(int value) {
    char buffer[12];
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
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    if (is_negative) {
        buffer[i++] = '-';
    }

    while (i--) {
        print_char(buffer[i]);
    }
}

void print_hex32(uint32_t value) {
    print("0x");

    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (value >> (i * 4)) & 0xF;
        char hex_char = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        print_char(hex_char);
    }
}

void print_hex64(uint64_t value) {
    print("0x");

    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (value >> (i * 4)) & 0xF;
        char hex_char = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        print_char(hex_char);
    }
}
