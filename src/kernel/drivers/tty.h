/*
 * tty.h - Teletypewriter (TTY) Abstraction Layer
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

struct thread; // forward declaration for wait queue

#define TTY_BUFFER_SIZE 4096
#define TTY_HELD_SIZE 512

// Who owns an output device right now. A framebuffer TTY owns its console alone, but
// serial TTYs all write to COM1 and so share one of these
typedef struct tty_output {
    spinlock_t lock;
    struct thread* owner;
    uint32_t depth;
    struct thread* wait;       // threads blocked waiting to write
    char held[TTY_HELD_SIZE];
    uint32_t held_len;
} tty_output_t;

typedef struct tty {
    char buffer[TTY_BUFFER_SIZE];
    volatile uint32_t head;      // Write index (volatile: polled outside the lock)
    volatile uint32_t tail;      // Read index (volatile: polled outside the lock)

    spinlock_t lock;
    struct thread* wait_head;  // threads blocked waiting for input

    tty_output_t own_out;
    tty_output_t* out;

    // Line Discipline
    ldisc_t ldisc;

    // Hardware Association
    console_t* console;

    // Linked List for dynamic management
    struct tty* next;
    struct tty* prev;

    bool hidden; // if true, excluded from Alt+Tab cycle

} tty_t;

// Public API

tty_t* tty_create(void);
void tty_destroy(tty_t* tty);
void tty_input(tty_t* tty, char c);
void tty_push_char_raw(tty_t* tty, char c);
char tty_read_char(tty_t* tty);
size_t tty_read(tty_t* tty, char* buf, size_t count);
size_t tty_pending(tty_t* tty);
void tty_set_mode(tty_t* tty, bool raw, bool echo);
void tty_write(tty_t* tty, const char* buf, size_t count);
bool tty_lock_output(tty_t* tty);
void tty_unlock_output(tty_t* tty, bool held);
void tty_header_init(tty_t* tty, size_t rows);
void tty_header_write(tty_t* tty, size_t row, const char* text, uint8_t fg, uint8_t bg);
void tty_switch(tty_t* tty);
void tty_cycle(void);

// Global active TTY
extern tty_t* volatile active_tty;
extern tty_t* kernel_tty;
