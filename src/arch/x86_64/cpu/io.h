/*
 * io.h - Low-level I/O port primitives
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>

/*
 * outb - Writes a byte to an I/O port
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * inb - Reads a byte from an I/O port
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * outw - Writes a word to an I/O port
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * inw - Reads a word from an I/O port
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * io_wait - Small delay for I/O operations
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}
