/*
 * tty.c - Dynamic TTY Management Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/serial.h>
#include <kernel/drivers/ldisc.h>
#include <kernel/memory/heap.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/panic.h>
#include <klibc/string.h>

// Dead weight without a scheduler, see GATA_CAP_THREADS in caps.h.
#ifdef GATA_CAP_THREADS

// TTY Manager State
static tty_t* tty_list = NULL;
static spinlock_t tty_lock = {0};
static bool tty_lock_ok = false;

tty_t* volatile active_tty = NULL;
tty_t* kernel_tty = NULL;

#ifdef GATA_OUTPUT_SERIAL
static tty_output_t com1_out = { .lock = { 0, 0xFFFFFFFF, "com1_out_lock" } };
#endif

static void tty_init(tty_t* tty, console_t* console) {
    kmemset(tty->buffer, 0, TTY_BUFFER_SIZE);
    tty->head = 0;
    tty->tail = 0;
    tty->console = console;
    tty->next = NULL;
    tty->prev = NULL;
    tty->wait_head = NULL;

    tty->own_out.owner = NULL;
    tty->own_out.depth = 0;
    tty->own_out.wait = NULL;
    tty->own_out.held_len = 0;
    spinlock_init(&tty->own_out.lock, "tty_out_lock");
#ifdef GATA_OUTPUT_SERIAL
    tty->out = &com1_out;
#else
    tty->out = &tty->own_out;
#endif

    spinlock_init(&tty->lock, "tty_lock");
    ldisc_init(&tty->ldisc);
}

/*
 * ensure_lock - Init the global list lock on first use
 */
static void ensure_lock(void) {
    if (!tty_lock_ok) {
        spinlock_init(&tty_lock, "tty_list_lock");
        tty_lock_ok = true;
    }
}

/*
 * wake_all - Hand every thread on a wait list back to the scheduler. Call with interrupts off
 */
static void wake_all(thread_t** head) {
    thread_t* t = *head;
    *head = NULL;
    while (t) {
        thread_t* next = t->rnext;
        t->rnext = NULL;
        sched_add(t);
        t = next;
    }
}

tty_t* tty_create(void) {
    if (heap_kernel_get() == NULL) {
        panic("Attempted to create TTY before heap was ready!");
    }

    ensure_lock();

    tty_t* tty = (tty_t*)kmalloc(sizeof(tty_t));
    if (!tty) return NULL;

#ifdef GATA_CAP_FRAMEBUFFER
    console_t* console = (console_t*)kmalloc(sizeof(console_t));
    if (!console) {
        kfree(tty);
        return NULL;
    }

    if (!con_init(console)) {
        kfree(console);
        kfree(tty);
        return NULL;
    }
    tty_init(tty, console);
#else
    tty_init(tty, NULL);
#endif

    bool flags = spinlock_acquire(&tty_lock);
    if (tty_list == NULL) {
        tty_list = tty;
        tty->next = tty;
        tty->prev = tty;
    } else {
        tty_t* tail = tty_list->prev;
        tty->next = tty_list;
        tty->prev = tail;
        tail->next = tty;
        tty_list->prev = tty;
    }
    spinlock_release(&tty_lock, flags);

    return tty;
}

static void out_release(tty_t* tty);

void tty_destroy(tty_t* tty) {
    if (!tty) return;
    
    procs_kill_tty(tty);

    // flush any threads still parked in the wait queues into the dead queue
    // they're already T_DEAD from procs_kill_tty, so sched_add routes them straight to reaping
    bool iflags = intr_save();
    wake_all(&tty->wait_head);

    // An owner just killed mid-write never lets go. Its TTY dies with it, but a shared COM1 doesn't
    tty_output_t* o = tty->out;
    bool oflags = spinlock_acquire(&o->lock);
    if (o->owner && o->owner->state == T_DEAD) out_release(tty);
    else wake_all(&o->wait);
    spinlock_release(&o->lock, oflags);
    intr_restore(iflags);

    ensure_lock();
    bool flags = spinlock_acquire(&tty_lock);

    if (tty->next == tty) {
        tty_list = NULL;
    } else {
        tty->prev->next = tty->next;
        tty->next->prev = tty->prev;
        if (tty_list == tty) tty_list = tty->next;
    }

    if (active_tty == tty) {
        active_tty = tty_list;
#ifdef GATA_CAP_FRAMEBUFFER
        if (active_tty && active_tty->console) {
            con_refresh(active_tty->console);
        }
#endif
    }

    spinlock_release(&tty_lock, flags);

    if (tty->console) {
        if (tty->console->buffer) kfree(tty->console->buffer);
        if (tty->console->dirty) kfree(tty->console->dirty);
        kfree(tty->console);
    }
    kfree(tty);
}

/*
 * tty_switch - Switches focus to a specific TTY
 */
void tty_switch(tty_t* tty) {
    if (!tty || active_tty == tty) return;

    active_tty = tty;

#ifdef GATA_CAP_FRAMEBUFFER
    if (tty->console) {
        con_refresh(tty->console);
    }
#endif
}

/*
 * tty_cycle - Cycles focus to the next available non-hidden TTY
 */
void tty_cycle(void) {
    ensure_lock();
    bool flags = spinlock_acquire(&tty_lock);
    if (active_tty) {
        tty_t* next = active_tty->next;
        while (next != active_tty && next->hidden)
            next = next->next;
        if (next != active_tty && !next->hidden)
            tty_switch(next);
    }
    spinlock_release(&tty_lock, flags);
}

/*
 * tty_wake - Wake threads blocked on input. Call with tty->lock held (interrupts already off)
 */
static void tty_wake(tty_t* tty) {
    wake_all(&tty->wait_head);
}

/*
 * tty_block - Sleep until data arrives. Re-checks the buffer with interrupts off to close the race between the caller's empty check and the sleep
 */
static void tty_block(tty_t* tty) {
    if (!sched_active()) return;
    thread_t* t = sched_current();
    if (!t) return;

    bool iflag = intr_save();
    if (tty->head != tty->tail) {
        // Data arrived between caller's check and here, no need to block.
        intr_restore(iflag);
        return;
    }
    t->state = T_BLOCKED;
    t->rnext = tty->wait_head;
    tty->wait_head = t;
    intr_restore(iflag);
    sched_yield();
}

/*
 * tty_input - Entry point for character input
 */
void tty_input(tty_t* tty, char c) {
    if (!tty) return;
    ldisc_input(tty, c);
}

/*
 * tty_push_char_raw - Internal logic to commit a char to the read buffer
 */
void tty_push_char_raw(tty_t* tty, char c) {
    if (!tty) return;
    bool flags = spinlock_acquire(&tty->lock);

    uint32_t next_idx = (tty->head + 1) % TTY_BUFFER_SIZE;
    if (next_idx != tty->tail) {
        tty->buffer[tty->head] = c;
        tty->head = next_idx;
        tty_wake(tty);
    }

    spinlock_release(&tty->lock, flags);
}

/*
 * tty_read_char - Pops one character from the TTY buffer, blocking if empty.
 */
char tty_read_char(tty_t* tty) {
    if (!tty) return 0;
    while (tty->head == tty->tail)
        tty_block(tty);
    bool flags = spinlock_acquire(&tty->lock);
    char c = tty->buffer[tty->tail];
    tty->tail = (tty->tail + 1) % TTY_BUFFER_SIZE;
    spinlock_release(&tty->lock, flags);
    return c;
}

/*
 * tty_read - Blocks until there's data, then drains what it can under one lock. Stops early on newline,
 * or as soon as anything was read in raw mode
 */
size_t tty_read(tty_t* tty, char* buf, size_t count) {
    if (!tty || !buf || count == 0) return 0;
    size_t i = 0;
    while (i < count) {
        while (tty->head == tty->tail)
            tty_block(tty);

        bool flags = spinlock_acquire(&tty->lock);
        while (i < count && tty->head != tty->tail) {
            char c = tty->buffer[tty->tail];
            tty->tail = (tty->tail + 1) % TTY_BUFFER_SIZE;
            buf[i++] = c;
            if (c == '\n') {
                spinlock_release(&tty->lock, flags);
                return i;
            }
        }
        spinlock_release(&tty->lock, flags);
        if (tty->ldisc.raw) break;
    }
    return i;
}

/*
 * tty_pending - Bytes a read could take right now without blocking
 */
size_t tty_pending(tty_t* tty) {
    if (!tty) return 0;
    return (tty->head - tty->tail + TTY_BUFFER_SIZE) % TTY_BUFFER_SIZE;
}

/*
 * tty_set_mode - Switch the line discipline between canonical and raw, and set echo
 */
void tty_set_mode(tty_t* tty, bool raw, bool echo) {
    if (!tty) return;
    bool flags = spinlock_acquire(&tty->lock);
    tty->ldisc.raw = raw;
    tty->ldisc.echo = echo;
    tty->ldisc.pos = 0; // a half typed line means nothing in the other mode
    spinlock_release(&tty->lock, flags);
}

/*
 * tty_emit - Put text on the TTY's console, or COM1 when output is serial
 */
static void tty_emit(tty_t* tty, const char* buf, size_t count) {
#ifdef GATA_CAP_FRAMEBUFFER
    if (tty->console) {
        con_write_batch(tty->console, buf, count);
        return;
    }
#endif
    serial_write_len_port(SERIAL_COM1, buf, count);
}

/*
 * tty_can_wait - Whether the caller is a scheduled thread that may block. Interrupt handlers and early boot can't
 */
static bool tty_can_wait(void) {
    return sched_active() && sched_current() && !sched_in_interrupt();
}

/*
 * tty_lock_output - Own the output until tty_unlock_output, so everything written in between comes out together
 */
bool tty_lock_output(tty_t* tty) {
    if (!tty || !tty_can_wait()) return false;
    thread_t* me = sched_current();

    tty_output_t* o = tty->out;
    while (true) {
        bool flags = spinlock_acquire(&o->lock);
        if (!o->owner || o->owner == me) {
            o->owner = me;
            o->depth++;
            spinlock_release(&o->lock, flags);
            return true;
        }
        me->state = T_BLOCKED;
        me->rnext = o->wait;
        o->wait = me;
        spinlock_release(&o->lock, flags);
        sched_yield();
    }
}

/*
 * tty_unlock_output - Let go of the output
 */
void tty_unlock_output(tty_t* tty, bool held) {
    if (!tty || !held) return;
    bool flags = spinlock_acquire(&tty->out->lock);
    if (--tty->out->depth == 0) out_release(tty);
    spinlock_release(&tty->out->lock, flags);
}

/*
 * out_release - Write out what interrupt context held back, drop the owner and wake the waiting writers.
 * Call with the output's lock held
 */
static void out_release(tty_t* tty) {
    tty_output_t* o = tty->out;
    if (o->held_len) tty_emit(tty, o->held, o->held_len);
    o->held_len = 0;
    o->depth = 0;
    o->owner = NULL;
    wake_all(&o->wait);
}

/*
 * tty_write - High-level console write. One call comes out in one piece
 */
void tty_write(tty_t* tty, const char* buf, size_t count) {
    if (!tty) return;

    if (tty_lock_output(tty)) {
        tty_emit(tty, buf, count);
        tty_unlock_output(tty, true);
        return;
    }

    // Can't wait
    tty_output_t* o = tty->out;
    bool flags = spinlock_acquire(&o->lock);
    if (o->owner) {
        size_t room = TTY_HELD_SIZE - o->held_len;
        if (count > room) count = room;
        kmemcpy(o->held + o->held_len, buf, count);
        o->held_len += (uint32_t)count;
    } else {
        tty_emit(tty, buf, count);
    }
    spinlock_release(&o->lock, flags);
}

/*
 * tty_header_init - Reserve the top N rows as a sticky header that never scrolls
 */
void tty_header_init(tty_t* tty, size_t rows) {
#ifdef GATA_CAP_FRAMEBUFFER
    if (!tty || !tty->console) return;
    con_header_init(tty->console, rows);
#else
    (void)tty; (void)rows;
#endif
}

/*
 * tty_header_write - Writes centred text into sticky header row and redraws it immediately
 */
void tty_header_write(tty_t* tty, size_t row, const char* text, uint8_t fg, uint8_t bg) {
#ifdef GATA_CAP_FRAMEBUFFER
    if (!tty || !tty->console) return;
    con_header_write(tty->console, row, text, fg, bg);
#else
    (void)tty; (void)row; (void)text; (void)fg; (void)bg;
#endif
}

static void tty_echo(tty_t* tty, char c) {
#ifdef GATA_CAP_FRAMEBUFFER
    if (tty->console) { con_putc(tty->console, c); return; }
#else
    (void)tty;
#endif
    serial_write_char_port(SERIAL_COM1, c);
}

/*
 * ldisc_init - Resets the line discipline state
 */
void ldisc_init(ldisc_t* ld) {
    kmemset(ld->line_buffer, 0, LDISC_LINE_MAX);
    ld->pos = 0;
    ld->echo = true;
    ld->raw = false;
}

/*
 * ldisc_input - Processes a character through the line discipline
 */
void ldisc_input(tty_t* tty, char c) {
    ldisc_t* ld = &tty->ldisc;

    // No editing in raw mode, the reader gets the byte as is
    if (ld->raw) {
        if (ld->echo) tty_echo(tty, c);
        tty_push_char_raw(tty, c);
        return;
    }

    if (c == '\b') {
        if (ld->pos > 0) {
            ld->pos--;
            if (ld->echo) tty_echo(tty, '\b');
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        if (ld->echo) tty_echo(tty, '\n');
        for (uint32_t i = 0; i < ld->pos; i++) tty_push_char_raw(tty, ld->line_buffer[i]);
        tty_push_char_raw(tty, '\n');
        ld->pos = 0;
        return;
    }

    if (ld->pos < LDISC_LINE_MAX - 1) {
        ld->line_buffer[ld->pos++] = c;
        if (ld->echo) tty_echo(tty, c);
    }
}

#endif // GATA_CAP_THREADS
