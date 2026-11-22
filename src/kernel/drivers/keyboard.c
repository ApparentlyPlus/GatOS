/*
 * keyboard.c - Keyboard Driver Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/i8042.h>
#include <kernel/drivers/input.h>
#include <kernel/sys/timers.h>
#include <kernel/debug.h>
#include <arch/x86_64/cpu/io.h>

#pragma region Internal State

static uint8_t modifiers = 0;
static uint8_t locks = 0;
static bool extended = false;

// Lock keys currently held down. Holding a key repeats its make code, which must not toggle the lock again
static uint8_t lock_held = 0;

// Bytes left to drop after an 0xE1 prefix
static uint8_t e1_skip = 0;

#pragma endregion

#pragma region Scancode Translation Tables

static const keycode_t scancode_set1[] = {
    KEY_UNKNOWN, KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
    KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I,
    KEY_O, KEY_P, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_ENTER, KEY_LEFT_CTRL, KEY_A, KEY_S,
    KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON,
    KEY_QUOTE, KEY_BACKTICK, KEY_LEFT_SHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V,
    KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_RIGHT_SHIFT, KEY_KPMULT,
    KEY_LEFT_ALT, KEY_SPACE, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_KP7,
    KEY_KP8, KEY_KP9, KEY_KPMINUS, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KPPLUS, KEY_KP1,
    KEY_KP2, KEY_KP3, KEY_KP0, KEY_KPDOT, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_F11,
    KEY_F12
};

#pragma endregion

#pragma region Internal Helpers

/*
 * lock_key - Toggles a lock on the first make code only, auto-repeat while held is ignored
 */
static void lock_key(uint8_t bit, bool pressed) {
    if (pressed && !(lock_held & bit)) locks ^= bit;
    lock_held = pressed ? (lock_held | bit) : (lock_held & ~bit);
}

/*
 * keyboard_led_byte - Our lock flags in the order the keyboard wants them (bit 0 Scroll, 1 Num, 2 Caps)
 */
uint8_t keyboard_led_byte(uint8_t l) {
    return ((l & LOCK_SCROLL) ? 1 : 0) | ((l & LOCK_NUM) ? 2 : 0) | ((l & LOCK_CAPS) ? 4 : 0);
}

static void update_leds(void) {
    // Gotta love this hardware interface!
    // we have to send a command to set LED state,
    // then wait for an ACK, then send the actual LED state
    i8042_write_data(0xED);
    if (i8042_read_data() == 0xFA) {
        i8042_write_data(keyboard_led_byte(locks));
        i8042_read_data(); // consume ACK for the LED state byte
    }
}

#pragma endregion

#pragma region Public API

/*
 * keyboard_init - Initializes the controller and syncs the LEDs
 */
void keyboard_init(void) {
    if (i8042_init()) {
        update_leds();

        /*
            Author's Note:

            Wait for any in flight PS/2 responses to arrive, then drain.
            Without this delay, an unread byte can hold IRQ1 high and
            prevent the IOAPIC from ever seeing an edge transition.

            Ask me how I know.
        */

        sleep_us(2000);
        while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            inb(PS2_DATA_PORT);
        }

        LOGF("[KBD] Keyboard driver initialized.\n");
    }
}

/*
 * keyboard_feed - Runs one scancode byte through the state machine, true when it finished a key event
 */
bool keyboard_feed(uint8_t scancode, key_event_t* out) {

    // This is hardcoded magic values galore
    // but it works and it's not like we have a choice given the hardware interface, so ¯\_(ツ)_/¯

    if (e1_skip) {
        e1_skip--;
        return false;
    }

    if (scancode == 0xE1) {
        e1_skip = 5;
        return false;
    }

    if (scancode == 0xE0) {
        extended = true;
        return false;
    }

    bool pressed = !(scancode & 0x80);
    uint8_t code = scancode & 0x7F;

    keycode_t key = KEY_UNKNOWN;

    if (extended) {
        key = (keycode_t)(code | 0x80);
        extended = false;
    } else if (code < sizeof(scancode_set1)/sizeof(scancode_set1[0])) {
        key = scancode_set1[code];
    }

    if (key == KEY_UNKNOWN) return false;

    switch (key) {
        case KEY_LEFT_SHIFT: pressed ? (modifiers |= MOD_LSHIFT) : (modifiers &= ~MOD_LSHIFT); break;
        case KEY_RIGHT_SHIFT: pressed ? (modifiers |= MOD_RSHIFT) : (modifiers &= ~MOD_RSHIFT); break;
        case KEY_LEFT_CTRL: pressed ? (modifiers |= MOD_LCTRL) : (modifiers &= ~MOD_LCTRL); break;
        case KEY_RIGHT_CTRL: pressed ? (modifiers |= MOD_RCTRL) : (modifiers &= ~MOD_RCTRL); break;
        case KEY_LEFT_ALT: pressed ? (modifiers |= MOD_LALT) : (modifiers &= ~MOD_LALT); break;
        case KEY_RIGHT_ALT: pressed ? (modifiers |= MOD_RALT) : (modifiers &= ~MOD_RALT); break;

        case KEY_CAPSLOCK: lock_key(LOCK_CAPS, pressed); break;
        case KEY_NUMLOCK: lock_key(LOCK_NUM, pressed); break;
        case KEY_SCROLLLOCK: lock_key(LOCK_SCROLL, pressed); break;

        default: break;
    }

    *out = (key_event_t){ .keycode = key, .pressed = pressed, .modifiers = modifiers, .locks = locks };
    return true;
}

/*
 * keyboard_handler - IRQ 1, reads the byte and hands finished events to the input hub
 */
cpu_context_t* keyboard_handler(cpu_context_t* ctx) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) return ctx;

    uint8_t scancode = inb(PS2_DATA_PORT);

    // A byte from port 2 (the mouse) shares the same buffer, it isn't ours
    if (status & PS2_STATUS_AUX_OUTPUT) return ctx;

    uint8_t before = locks;
    key_event_t ev;
    if (!keyboard_feed(scancode, &ev)) return ctx;

    if (locks != before) update_leds();
    input_handle_key(ev);

    return ctx;
}

#pragma endregion
