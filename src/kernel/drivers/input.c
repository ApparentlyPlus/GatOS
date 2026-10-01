/*
 * input.c - Input Hub Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/input.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/dashboard.h>
#include <kernel/debug.h>

void input_init(void) {
    LOGF("[INPUT] Hub initialized.\n");
}

/*
 * input_handle_key - Keyboard event entry point, handles hotkeys and routes the rest to the active TTY
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
