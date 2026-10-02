/*
 * input.c - Input Hub Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <kernel/drivers/input.h>
#ifdef GATA_CAP_THREADS
#include <kernel/drivers/tty.h>
#include <kernel/drivers/dashboard.h>
#else
#include <kernel/drivers/console.h>
#include <kernel/drivers/serial.h>
#include <kernel/sys/spinlock.h>
#endif
#include <kernel/debug.h>

#ifndef GATA_CAP_THREADS

static inline void input_echo(char c) {
#ifdef GATA_OUTPUT_SERIAL
    serial_write_char_port(SERIAL_COM1, c);
#else
    con_crash_putc(c);
#endif
}

#define INPUT_RING_SIZE 256
static struct {
    char buffer[INPUT_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
} input_ring;

#define INPUT_LINE_MAX 1024
static struct {
    char line[INPUT_LINE_MAX];
    uint32_t pos;
} input_ld;

static void nothread_ldisc_input(char c) {
    if (c == '\b') {
        if (input_ld.pos > 0) {
            input_ld.pos--;
            input_echo('\b');
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        input_echo('\n');
        bool flags = spinlock_acquire(&input_ring.lock);
        for (uint32_t i = 0; i < input_ld.pos; i++) {
            uint32_t next = (input_ring.head + 1) % INPUT_RING_SIZE;
            if (next != input_ring.tail) {
                input_ring.buffer[input_ring.head] = input_ld.line[i];
                input_ring.head = next;
            }
        }
        uint32_t next = (input_ring.head + 1) % INPUT_RING_SIZE;
        if (next != input_ring.tail) {
            input_ring.buffer[input_ring.head] = '\n';
            input_ring.head = next;
        }
        spinlock_release(&input_ring.lock, flags);
        input_ld.pos = 0;
        return;
    }

    if (input_ld.pos < INPUT_LINE_MAX - 1) {
        input_ld.line[input_ld.pos++] = c;
        input_echo(c);
    }
}
#endif

void input_init(void) {
#ifndef GATA_CAP_THREADS
    spinlock_init(&input_ring.lock, "input_ring");
#endif
    LOGF("[INPUT] Hub initialized.\n");
}

#ifdef GATA_CAP_THREADS

/*
 * input_handle_key - Without a scheduler/TTY there's no dashboard, Alt+Tab or per-process routing, so the key
 * goes through the no-threads line discipline: echo, line-buffer, commit to the ring on Enter
 */
void input_handle_key(key_event_t event) {
    // Only handle key press events, ignore releases for now
    if (!event.pressed) return;

    // Dashy dashy daaaaash toggle Ctrl + Shift + Esc
    if ((event.modifiers & MOD_CTRL) &&
        (event.modifiers & MOD_SHIFT) &&
        event.keycode == KEY_ESC) {
        dash_toggle();
        return;
    }

    // Alt tab my beloved
    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_TAB) {
        tty_cycle();
        return;
    }

    // ALT+F4 to close the current tty (if not dashboard or kernel tty)
    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_F4) {
        if (active_tty && active_tty != kernel_tty && !dash_active()) {
            tty_destroy(active_tty);
        }
        return;
    }

    // If we have an active TTY, route the key event to it
    if (active_tty) {
        char c = keycode_to_ascii(event);
        if (c) {
            tty_input(active_tty, c);
        } else if (event.keycode == KEY_BACKSPACE) {
            tty_input(active_tty, '\b');
        }
    }
}

#else

/*
 * input_handle_key - Without a scheduler/TTY there's no dashboard, Alt+Tab or per-process routing, so the key
 * goes through the no-threads line discipline: echo, line-buffer, commit to the ring on Enter
 */
void input_handle_key(key_event_t event) {
    if (!event.pressed) return;

    char c = keycode_to_ascii(event);
    if (!c && event.keycode == KEY_BACKSPACE) c = '\b';
    if (!c) return;

    nothread_ldisc_input(c);
}

/*
 * input_getchar - Pop one char from the ring, -1 if empty. Non-blocking, _getchar polls
 */
int input_getchar(void) {
    bool flags = spinlock_acquire(&input_ring.lock);
    if (input_ring.head == input_ring.tail) {
        spinlock_release(&input_ring.lock, flags);
        return -1;
    }
    char c = input_ring.buffer[input_ring.tail];
    input_ring.tail = (input_ring.tail + 1) % INPUT_RING_SIZE;
    spinlock_release(&input_ring.lock, flags);
    return (unsigned char)c;
}

#endif
