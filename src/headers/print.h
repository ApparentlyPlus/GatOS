#pragma once

#include <stdint.h>
#include <stddef.h>

#define PRINT_COLOR_BLACK 0
#define PRINT_COLOR_BLUE 1
#define PRINT_COLOR_GREEN 2
#define PRINT_COLOR_CYAN 3
#define PRINT_COLOR_RED 4
#define PRINT_COLOR_MAGENTA 5
#define PRINT_COLOR_BROWN 6
#define PRINT_COLOR_LIGHT_GRAY 7
#define PRINT_COLOR_DARK_GRAY 8
#define PRINT_COLOR_LIGHT_BLUE 9
#define PRINT_COLOR_LIGHT_GREEN 10
#define PRINT_COLOR_LIGHT_CYAN 11
#define PRINT_COLOR_LIGHT_RED 12
#define PRINT_COLOR_PINK 13
#define PRINT_COLOR_YELLOW 14
#define PRINT_COLOR_WHITE 15

void print_char(char character);
void print(const char* string);
void print_int(int value);
void print_hex32(uint32_t value);
void print_hex64(uint64_t value);
void print_set_color(uint8_t foreground, uint8_t background);
void print_clear(void);
