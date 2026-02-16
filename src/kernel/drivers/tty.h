/*
 * tty.h - Teletypewriter (TTY) Abstraction Layer
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

} tty_t;

// Public API
void tty_init(tty_t* tty, console_t* console);
void tty_input(tty_t* tty, char c);         // Input from keyboard/source (goes through ldisc)
void tty_push_char_raw(tty_t* tty, char c); // Raw push to buffer (bypasses ldisc)

char tty_read_char(tty_t* tty);
size_t tty_read(tty_t* tty, char* buf, size_t count);
void tty_write(tty_t* tty, const char* buf, size_t count);

// Switch the active terminal
void tty_switch(tty_t* tty);

// Global active TTY
extern tty_t* g_active_tty;
