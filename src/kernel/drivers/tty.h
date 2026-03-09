/*
 * tty.h - Teletypewriter (TTY) Abstraction Layer
 *
 * This module provides a high-level abstraction for terminal-like devices.
 * It handles line discipline (canonical mode) and provides a thread-safe
 * interface for reading and writing characters. TTYs are managed dynamically
 * in a global doubly-linked list.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <kernel/sys/spinlock.h>
#include <kernel/drivers/console.h>
#include <kernel/drivers/ldisc.h>

#define TTY_BUFFER_SIZE 4096

typedef struct tty {
    char buffer[TTY_BUFFER_SIZE];
    uint32_t head;      // Write index
    uint32_t tail;      // Read index

    spinlock_t lock;

    // Line Discipline
    ldisc_t ldisc;

    // Hardware Association
    console_t* console;

    // Linked List for dynamic management
    struct tty* next;
    struct tty* prev;

} tty_t;

// Public API

tty_t* tty_create(void);
void tty_destroy(tty_t* tty);
void tty_input(tty_t* tty, char c);
void tty_push_char_raw(tty_t* tty, char c);
char tty_read_char(tty_t* tty);
size_t tty_read(tty_t* tty, char* buf, size_t count);
void tty_write(tty_t* tty, const char* buf, size_t count);
void tty_header_init(tty_t* tty, size_t rows);
void tty_header_write(tty_t* tty, size_t row, const char* text, uint8_t fg, uint8_t bg);
void tty_switch(tty_t* tty);
void tty_cycle(void);

// Global active TTY
extern tty_t* g_active_tty;
extern tty_t* g_kernel_tty;
