/*
 * serial.h - Serial port communication interface
 *
 * Declares functions for initializing COM1 port and outputting data,
 * including formatted hexadecimal value printing.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

void serial_init(void);
int serial_is_ready(void);
void serial_write_char(char c);
void serial_write(const char* str);
void serial_write_len(const char* str, size_t len);

static void serial_write_hex_digit(uint8_t val);
void serial_write_hex8(uint8_t value);
void serial_write_hex16(uint16_t value);
void serial_write_hex32(uint32_t value);
void serial_write_hex64(uint64_t value);