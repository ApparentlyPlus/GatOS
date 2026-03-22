/*
 * input.c - Input Hub Implementation
 * 
 * This file implements the system input hub that handles keyboard events 
 * and routes them to the appropriate TTY.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/input.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/dashboard.h>
#include <kernel/debug.h>

/*
 * input_init - Initializes the system input hub
 */
void input_init(void) {
    LOGF("[INPUT] Hub initialized.\n");
}

/*
 * input_handle_key - Entry point for keyboard events. Handles system 
 * hotkeys and routes input to the active TTY.
 */
void input_handle_key(key_event_t event) {
    if (!event.pressed) return;

    if ((event.modifiers & MOD_CTRL) &&
        (event.modifiers & MOD_SHIFT) &&
        event.keycode == KEY_ESC) {
        dash_toggle();
        return;
    }

    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_TAB) {
        tty_cycle();
        return;
    }

    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_F4) {
        if (active_tty && active_tty != kernel_tty && !is_dash_tty()) {
            tty_destroy(active_tty);
        }
        return;
    }

    if (active_tty) {
        char c = keyboard_keycode_to_ascii(event);
        if (c) {
            tty_input(active_tty, c);
        } else if (event.keycode == KEY_BACKSPACE) {
            tty_input(active_tty, '\b');
        }
    }
}
