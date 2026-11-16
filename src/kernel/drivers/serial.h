/*
 * serial.h - Serial port driver header
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <arch/x86_64/cpu/io.h>
#include <stddef.h>
#include <stdint.h>

// Serial port bases
#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define COM3_PORT 0x3E8
#define COM4_PORT 0x2E8

// Serial port identifiers
typedef enum {
    SERIAL_COM1 = 0,
    SERIAL_COM2 = 1,
    SERIAL_COM3 = 2, 
    SERIAL_COM4 = 3
} serial_port_t;

// Initialization
void serial_init_port(serial_port_t port);

// Port-specific operations
int serial_is_ready_port(serial_port_t port);
void serial_write_char_port(serial_port_t port, char c);
void serial_write_port(serial_port_t port, const char* str);
void serial_write_len_port(serial_port_t port, const char* str, size_t len);

// COM1 shorthand
void serial_write(const char* str);
