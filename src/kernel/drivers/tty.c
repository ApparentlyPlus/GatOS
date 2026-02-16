/*
 * tty.c - Dynamic TTY Management Implementation
 *
 * This module handles the creation, destruction, and switching of 
 * virtual terminals. It manages a doubly-linked list of TTY instances
 * and coordinates input flow through the line discipline.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/tty.h>
#include <kernel/memory/heap.h>
#include <kernel/sys/panic.h>
#include <libc/string.h>

// TTY Manager State
static tty_t* g_tty_list = NULL;
static spinlock_t g_tty_list_lock = {0};
static bool g_tty_list_lock_initialized = false;

tty_t* g_active_tty = NULL;

/*
 * tty_init - Internal helper to initialize a TTY structure.
 */
static void tty_init(tty_t* tty, console_t* console) {
    memset(tty->buffer, 0, TTY_BUFFER_SIZE);
    tty->head = 0;
    tty->tail = 0;
    tty->console = console;
    tty->next = NULL;
    tty->prev = NULL;
    
    spinlock_init(&tty->lock, "tty_lock");
    ldisc_init(&tty->ldisc);
}

/*
 * ensure_lock_init - Atomically ensures the global list lock is ready.
 */
static void ensure_lock_init(void) {
    if (!g_tty_list_lock_initialized) {
        spinlock_init(&g_tty_list_lock, "tty_list_lock");
        g_tty_list_lock_initialized = true;
    }
}

/*
 * tty_create - Allocates and registers a new dynamic TTY.
 */
tty_t* tty_create(void) {
    if (heap_kernel_get() == NULL) {
        panic("Attempted to create TTY before heap was ready!");
    }

    ensure_lock_init();

    // Allocate TTY structure
    tty_t* tty = (tty_t*)kmalloc(sizeof(tty_t));
    if (!tty) return NULL;

    // Allocate associated Console instance
    console_t* console = (console_t*)kmalloc(sizeof(console_t));
    if (!console) {
        kfree(tty);
        return NULL;
    }

    con_init(console);
    tty_init(tty, console);

    // Add to global linked list
    bool flags = spinlock_acquire(&g_tty_list_lock);
    if (g_tty_list == NULL) {
        g_tty_list = tty;
        tty->next = tty; // Circular doubly linked list
        tty->prev = tty;
    } else {
        tty_t* tail = g_tty_list->prev;
        tty->next = g_tty_list;
        tty->prev = tail;
        tail->next = tty;
        g_tty_list->prev = tty;
    }
    spinlock_release(&g_tty_list_lock, flags);

    return tty;
}

/*
 * tty_destroy - Frees a TTY and its associated console.
 */
void tty_destroy(tty_t* tty) {
    if (!tty) return;
    ensure_lock_init();

    bool flags = spinlock_acquire(&g_tty_list_lock);
    
    // Remove from linked list
    if (tty->next == tty) {
        g_tty_list = NULL;
    } else {
        tty->prev->next = tty->next;
        tty->next->prev = tty->prev;
        if (g_tty_list == tty) g_tty_list = tty->next;
    }

    if (g_active_tty == tty) {
        // Hide cursor before switching away
        if (tty->console) con_set_cursor_enabled(tty->console, false);
        
        g_active_tty = g_tty_list;
        if (g_active_tty) {
            con_refresh(g_active_tty->console);
            con_set_cursor_enabled(g_active_tty->console, true);
        }
    }

    spinlock_release(&g_tty_list_lock, flags);

    // Free resources
    if (tty->console) {
        if (tty->console->buffer) kfree(tty->console->buffer);
        kfree(tty->console);
    }
    kfree(tty);
}

/*
 * tty_switch - Switches focus to a specific TTY.
 */
void tty_switch(tty_t* tty) {
    if (!tty || g_active_tty == tty) return;

    // Hide cursor on the outgoing terminal
    if (g_active_tty && g_active_tty->console) {
        con_set_cursor_enabled(g_active_tty->console, false);
    }

    g_active_tty = tty;

    // Show cursor on the incoming terminal
    if (tty->console) {
        con_refresh(tty->console);
        con_set_cursor_enabled(tty->console, true);
    }
}

/*
 * tty_cycle - Cycles focus to the next available TTY.
 */
void tty_cycle(void) {
    ensure_lock_init();
    bool flags = spinlock_acquire(&g_tty_list_lock);
    if (g_active_tty && g_active_tty->next) {
        tty_switch(g_active_tty->next);
    }
    spinlock_release(&g_tty_list_lock, flags);
}

/*
 * tty_wait_for_input - Busy-waits (hlt) for data in the circular buffer.
 */
static void tty_wait_for_input(tty_t* tty) {
    while (tty->head == tty->tail) {
        __asm__ volatile("hlt");
    }
}

/*
 * tty_input - Entry point for character input.
 */
void tty_input(tty_t* tty, char c) {
    if (!tty) return;
    ldisc_input(tty, c);
}

/*
 * tty_push_char_raw - Internal logic to commit a char to the read buffer.
 */
void tty_push_char_raw(tty_t* tty, char c) {
    if (!tty) return;
    bool flags = spinlock_acquire(&tty->lock);

    uint32_t next_idx = (tty->head + 1) % TTY_BUFFER_SIZE;
    if (next_idx != tty->tail) {
        tty->buffer[tty->head] = c;
        tty->head = next_idx;
    }

    spinlock_release(&tty->lock, flags);
}

/*
 * tty_read_char - Pops one character from the TTY buffer.
 */
char tty_read_char(tty_t* tty) {
    if (!tty) return 0;
    tty_wait_for_input(tty);
    bool flags = spinlock_acquire(&tty->lock);
    char c = tty->buffer[tty->tail];
    tty->tail = (tty->tail + 1) % TTY_BUFFER_SIZE;
    spinlock_release(&tty->lock, flags);
    return c;
}

/*
 * tty_read - High-level buffered read.
 */
size_t tty_read(tty_t* tty, char* buf, size_t count) {
    if (!tty) return 0;
    size_t i = 0;
    while (i < count) {
        buf[i++] = tty_read_char(tty);
        if (buf[i-1] == '\n') break;
    }
    return i;
}

/*
 * tty_write - High-level console write.
 */
void tty_write(tty_t* tty, const char* buf, size_t count) {
    if (!tty || !tty->console) return;
    for (size_t i = 0; i < count; i++) {
        con_putc(tty->console, buf[i]);
    }
}
