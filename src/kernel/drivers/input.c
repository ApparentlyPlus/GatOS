/*
 * input.c - Input Hub Implementation
 * 
 * This file implements the system input hub that handles keyboard events and routes them to the appropriate TTY.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/input.h>
#include <kernel/drivers/tty.h>
#include <kernel/debug.h>

extern tty_t g_ttys[4];

/*
 * input_init - Initializes the system input hub.
 */
void input_init(void) {
    LOGF("[INPUT] Hub initialized.\n");
}

/*
 * input_handle_key - Entry point for keyboard events. Handles system hotkeys and routes input to the active TTY.
 */
void input_handle_key(key_event_t event) {
    if (!event.pressed) return;

    // Handle System Hotkeys (Shift + Tab for TTY cycling)
    if ((event.modifiers & MOD_SHIFT) && event.keycode == KEY_TAB) {
        if (g_active_tty) {
            // Calculate current index based on pointer arithmetic
            uint32_t current_index = 0;
            for (uint32_t i = 0; i < 4; i++) {
                if (g_active_tty == &g_ttys[i]) {
                    current_index = i;
                    break;
                }
            }
            uint32_t next_index = (current_index + 1) % 4;
            tty_switch(&g_ttys[next_index]);
        } else {
            // Default to TTY 0 if none active
            tty_switch(&g_ttys[0]);
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
