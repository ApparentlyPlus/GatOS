/*
 * tty.h - Teletypewriter (TTY) Abstraction Layer
 *
 * This module provides a high-level abstraction for terminal-like devices.
 * It handles line discipline (canonical mode), echoing, and provides
 * a thread-safe interface for reading and writing characters.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <kernel/sys/spinlock.h>

#define TTY_BUFFER_SIZE 4096

typedef struct tty {
    char buffer[TTY_BUFFER_SIZE];
    uint32_t head;      // Write index
    uint32_t tail;      // Read index
    uint32_t canon_pos; // Start of current line (for canonical mode)

    spinlock_t lock;

    // Flags
    bool echo;          // Echo input characters back to output
    bool canon;         // Canonical mode (line-buffered, backspace handled)

    // Callbacks
    void (*write_callback)(char c); // Hardware output (e.g., console_print_char)

} tty_t;

// Public API
void tty_init(tty_t* tty, void (*write_cb)(char));
void tty_push_char(tty_t* tty, char c);
char tty_read_char(tty_t* tty);
size_t tty_read(tty_t* tty, char* buf, size_t count);
void tty_write(tty_t* tty, const char* buf, size_t count);

// Global active TTY
extern tty_t* g_active_tty;
