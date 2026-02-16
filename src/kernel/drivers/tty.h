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

/*
 * tty_create - Dynamically allocates and initializes a new TTY and Console.
 */
tty_t* tty_create(void);

/*
 * tty_destroy - Removes a TTY from the system and frees its resources.
 */
void tty_destroy(tty_t* tty);

/*
 * tty_input - Routes hardware input into the TTY's line discipline.
 */
void tty_input(tty_t* tty, char c);

/*
 * tty_push_char_raw - Internal helper to push characters to the read buffer.
 */
void tty_push_char_raw(tty_t* tty, char c);

/*
 * tty_read_char - Blocks until a character is available and returns it.
 */
char tty_read_char(tty_t* tty);

/*
 * tty_read - Reads up to count bytes into buf (Canonical mode).
 */
size_t tty_read(tty_t* tty, char* buf, size_t count);

/*
 * tty_write - Writes a buffer of characters to the TTY's console.
 */
void tty_write(tty_t* tty, const char* buf, size_t count);

/*
 * tty_switch - Sets the specified TTY as the active foreground terminal.
 */
void tty_switch(tty_t* tty);

/*
 * tty_cycle - Cycles the active focus to the next TTY in the linked list.
 */
void tty_cycle(void);

// Global active TTY
extern tty_t* g_active_tty;
