/*
 * serial.c - Serial port driver
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/serial.h>

uint16_t get_port_base(serial_port_t port) {
    switch(port) {
        case SERIAL_COM1: return COM1_PORT;
        case SERIAL_COM2: return COM2_PORT;
        case SERIAL_COM3: return COM3_PORT;
        case SERIAL_COM4: return COM4_PORT;
        default: return COM1_PORT;
    }
}

/*
 * serial_init_port - Initializes specific serial port at 38400 baud
 */
void serial_init_port(serial_port_t port) {
    uint16_t port_base = get_port_base(port);

    outb(port_base + 1, 0x00);
    outb(port_base + 3, 0x80);
    outb(port_base + 0, 0x03);
    outb(port_base + 1, 0x00);
    outb(port_base + 3, 0x03);
    outb(port_base + 2, 0xC7);
    outb(port_base + 4, 0x0B);
}

/*
 * serial_is_ready_port - Checks if transmit buffer is empty for specific port
 */
int serial_is_ready_port(serial_port_t port) {
    uint16_t port_base = get_port_base(port);
    return inb(port_base + 5) & 0x20;
}

/*
 * serial_write_char_port - Outputs single character to specific serial port
 */
void serial_write_char_port(serial_port_t port, char c) {
    uint16_t port_base = get_port_base(port);
    while (!serial_is_ready_port(port));
    outb(port_base, (uint8_t)c);
}

/*
 * serial_write_port - Outputs null-terminated string to specific serial port
 */
void serial_write_port(serial_port_t port, const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_write_char_port(port, '\r');
        }
        serial_write_char_port(port, *str++);
    }
}

/*
 * serial_write_len_port - Outputs fixed-length string to specific serial port
 */
void serial_write_len_port(serial_port_t port, const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            serial_write_char_port(port, '\r');
        }
        serial_write_char_port(port, str[i]);
    }
}

void serial_write(const char* str) { serial_write_port(SERIAL_COM1, str); }
