/*
 * input.c - Input Hub Implementation
 * 
 * This file implements the system input hub that handles keyboard events 
 * and routes them to the appropriate TTY.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <kernel/drivers/input.h>
#ifdef GATA_CAP_THREADS
#include <kernel/drivers/tty.h>
#include <kernel/drivers/dashboard.h>
#else
#include <kernel/sys/spinlock.h>
#endif
#include <kernel/debug.h>

#ifndef GATA_CAP_THREADS
// Static ring buffer feeding input_getchar() when there's no scheduler/TTY
// to route key events through. Producer: the keyboard IRQ handler (via
// input_handle_key, below). Consumer: input_getchar(), called by _getchar()
// in klibc/stdio.c.
#define INPUT_RING_SIZE 256
static struct {
    char buffer[INPUT_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
} input_ring;
#endif

/*
 * input_init - Initializes the system input hub
 */
void input_init(void) {
#ifndef GATA_CAP_THREADS
    spinlock_init(&input_ring.lock, "input_ring");
#endif
    LOGF("[INPUT] Hub initialized.\n");
}

#ifdef GATA_CAP_THREADS
/*
 * input_handle_key - Entry point for keyboard events. Handles system
 * hotkeys and routes input to the active TTY.
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
        char c = keyboard_keycode_to_ascii(event);
        if (c) {
            tty_input(active_tty, c);
        } else if (event.keycode == KEY_BACKSPACE) {
            tty_input(active_tty, '\b');
        }
    }
}

#else

/*
 * input_handle_key - With no scheduler/TTY there's no dashboard, no Alt+Tab
 * TTY cycling, and no per-process routing to do - just turn the key into a
 * character (if it is one) and push it onto the static ring buffer.
 */
void input_handle_key(key_event_t event) {
    if (!event.pressed) return;

    char c = keyboard_keycode_to_ascii(event);
    if (!c && event.keycode == KEY_BACKSPACE) c = '\b';
    if (!c) return;

    bool flags = spinlock_acquire(&input_ring.lock);
    uint32_t next = (input_ring.head + 1) % INPUT_RING_SIZE;
    if (next != input_ring.tail) {
        input_ring.buffer[input_ring.head] = c;
        input_ring.head = next;
    }
    spinlock_release(&input_ring.lock, flags);
}

/*
 * input_getchar - Pops one character from the static ring buffer, or -1 if
 * it's empty. Non-blocking - callers (e.g. _getchar's busy-wait) are
 * expected to poll.
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
