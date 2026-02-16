/*
 * tty.c - TTY Abstraction Implementation
 * 
 * This file implements a basic TTY abstraction layer that manages input buffering,
 * line discipline, and console output. It serves as the interface between hardware input and the console display.
 * The line discipline currently operates in canonical mode, buffering input until a newline is received.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/tty.h>
#include <kernel/memory/heap.h>
#include <kernel/sys/panic.h>
#include <libc/string.h>

tty_t* g_active_tty = NULL;

/*
 * tty_init - Initializes a TTY instance with the associated console
 */
void tty_init(tty_t* tty, console_t* console) {
    if (heap_kernel_get() == NULL) {
        panic("Attempted to initialize TTY before heap was ready!");
    }

    memset(tty->buffer, 0, TTY_BUFFER_SIZE);
    tty->head = 0;
    tty->tail = 0;
    tty->console = console;
    
    spinlock_init(&tty->lock, "tty_lock");
    ldisc_init(&tty->ldisc);
}

/*
 * tty_switch - Switches the active TTY context
 */
void tty_switch(tty_t* tty) {
    if (!tty || g_active_tty == tty) return;

    g_active_tty = tty;
    if (tty->console) {
        con_refresh(tty->console);
    }
}

/*
 * tty_wait_for_input - Waits for input to be available in the TTY buffer
 * This will be changed in the future 
 */
static void tty_wait_for_input(tty_t* tty) {
    while (tty->head == tty->tail) {
        __asm__ volatile("hlt");
    }
}

/*
 * tty_input - Entry point for input characters (goes through line discipline)
 */
void tty_input(tty_t* tty, char c) {
    ldisc_input(tty, c);
}

/*
 * tty_push_char_raw - Pushes a character directly to the TTY buffer (bypassing line discipline)
 */
void tty_push_char_raw(tty_t* tty, char c) {
    bool flags = spinlock_acquire(&tty->lock);

    uint32_t next_idx = (tty->head + 1) % TTY_BUFFER_SIZE;
    if (next_idx != tty->tail) {
        tty->buffer[tty->head] = c;
        tty->head = next_idx;
    }

    spinlock_release(&tty->lock, flags);
}

/*
 * tty_read_char - Reads a single character from the TTY buffer
 */
char tty_read_char(tty_t* tty) {
    tty_wait_for_input(tty);
    bool flags = spinlock_acquire(&tty->lock);
    char c = tty->buffer[tty->tail];
    tty->tail = (tty->tail + 1) % TTY_BUFFER_SIZE;
    spinlock_release(&tty->lock, flags);
    return c;
}

/*
 * tty_read - Reads a buffer of characters from the TTY buffer
 */
size_t tty_read(tty_t* tty, char* buf, size_t count) {
    size_t i = 0;
    while (i < count) {
        buf[i++] = tty_read_char(tty);
        // Canonical stopping condition (now always active)
        if (buf[i-1] == '\n') break;
    }
    return i;
}

/*
 * tty_write - Writes a buffer of characters to the TTY's associated console
 */
void tty_write(tty_t* tty, const char* buf, size_t count) {
    if (!tty || !tty->console) return;
    for (size_t i = 0; i < count; i++) {
        con_putc(tty->console, buf[i]);
    }
}
