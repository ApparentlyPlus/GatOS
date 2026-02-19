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
#include <kernel/debug.h>

/*
 * input_init - Initializes the system input hub.
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

    // Handle System Hotkeys (Alt + Tab for TTY cycling)
    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_TAB) {
        tty_cycle();
        return;
    }

    // Route to Active TTY
    if (g_active_tty) {
        char c = keyboard_keycode_to_ascii(event);
        if (c) {
            tty_input(g_active_tty, c);
        } else if (event.keycode == KEY_BACKSPACE) {
            tty_input(g_active_tty, '\b');
        }
    }
}
