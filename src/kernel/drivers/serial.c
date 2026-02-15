#include <kernel/drivers/serial.h>

/*
 * get_port_base - Get port base address for each serial port
 */
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
    
    outb(port_base + 1, 0x00);    // Disable interrupts
    outb(port_base + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(port_base + 0, 0x03);    // Set divisor to 3 (38400 baud)
    outb(port_base + 1, 0x00);
    outb(port_base + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(port_base + 2, 0xC7);    // Enable FIFO, clear them, 14-byte threshold
    outb(port_base + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

/*
 * serial_init_all - Initializes all available serial ports
 */
void serial_init_all(void) {
    serial_init_port(SERIAL_COM1);
    serial_init_port(SERIAL_COM2);
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
    while (!serial_is_ready_port(port)); // Wait until THR is empty
    outb(port_base, (uint8_t)c);
}

/*
 * serial_write_port - Outputs null-terminated string to specific serial port
 */
void serial_write_port(serial_port_t port, const char* str) {
    while (*str) {
        if (*str == '\n') {
            serial_write_char_port(port, '\r'); // Add carriage return
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

/*
 * serial_write_hex_digit_port - Internal helper for hex digit output to specific port
 */
void serial_write_hex_digit_port(serial_port_t port, uint8_t val) {
    val &= 0xF;
    if (val < 10)
        serial_write_char_port(port, '0' + val);
    else
        serial_write_char_port(port, 'A' + (val - 10));
}

/*
 * serial_write_hex8_port - Outputs 8-bit value in hexadecimal to specific port
 */
void serial_write_hex8_port(serial_port_t port, uint8_t value) {
    serial_write_hex_digit_port(port, value >> 4);
    serial_write_hex_digit_port(port, value & 0xF);
}

/*
 * serial_write_hex16_port - Outputs 16-bit value in hexadecimal to specific port
 */
void serial_write_hex16_port(serial_port_t port, uint16_t value) {
    for (int i = 12; i >= 0; i -= 4)
        serial_write_hex_digit_port(port, (value >> i) & 0xF);
}

/*
 * serial_write_hex32_port - Outputs 32-bit value in hexadecimal to specific port
 */
void serial_write_hex32_port(serial_port_t port, uint32_t value) {
    for (int i = 28; i >= 0; i -= 4)
        serial_write_hex_digit_port(port, (value >> i) & 0xF);
}

/*
 * serial_write_hex64_port - Outputs 64-bit value in hexadecimal to specific port
 */
void serial_write_hex64_port(serial_port_t port, uint64_t value) {
    for (int i = 60; i >= 0; i -= 4)
        serial_write_hex_digit_port(port, (value >> i) & 0xF);
}

// Default implementations (COM1 for backward compatibility)
int serial_is_ready(void) { return serial_is_ready_port(SERIAL_COM1); }
void serial_write_char(char c) { serial_write_char_port(SERIAL_COM1, c); }
void serial_write(const char* str) { serial_write_port(SERIAL_COM1, str); }
void serial_write_len(const char* str, size_t len) { serial_write_len_port(SERIAL_COM1, str, len); }
void serial_write_hex8(uint8_t value) { serial_write_hex8_port(SERIAL_COM1, value); }
void serial_write_hex16(uint16_t value) { serial_write_hex16_port(SERIAL_COM1, value); }
void serial_write_hex32(uint32_t value) { serial_write_hex32_port(SERIAL_COM1, value); }
void serial_write_hex64(uint64_t value) { serial_write_hex64_port(SERIAL_COM1, value); }
