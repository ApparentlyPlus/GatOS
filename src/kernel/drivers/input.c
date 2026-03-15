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

    // Handle System Hotkeys

    // CTRL+SHIFT+ESC: Toggle kernel dashboard
    if ((event.modifiers & MOD_CTRL) &&
        (event.modifiers & MOD_SHIFT) &&
        event.keycode == KEY_ESC) {
        dash_toggle();
        return;
    }

    // Alt + Tab: Cycle through virtual terminals
    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_TAB) {
        tty_cycle();
        return;
    }

    // Alt + F4: Close the current virtual terminal (if not protected)
    if ((event.modifiers & MOD_ALT) && event.keycode == KEY_F4) {
        if (g_active_tty && g_active_tty != g_kernel_tty) {
            tty_destroy(g_active_tty);
        }
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
