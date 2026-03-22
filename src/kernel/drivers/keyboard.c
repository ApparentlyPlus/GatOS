/*
 * keyboard.c - Keyboard Driver Implementation
 *
 * Features:
 * - Scancode Set 1 State Machine (Handles 0xE0 prefixes)
 * - Thread-safe circular event buffer
 * - Modifier tracking (Shift, Ctrl, Alt, Gui)
 * - Toggle state management (Caps, Num, Scroll lock)
 * - LED synchronization with i8042
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/i8042.h>
#include <kernel/drivers/input.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <arch/x86_64/cpu/io.h>

#define EVENT_BUFFER_SIZE 256

#pragma region Internal State

static struct {
    key_event_t buffer[EVENT_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
} key_events;

static uint8_t modifiers = 0;
static uint8_t locks = 0;
static bool extended = false;

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
 * update_leds - Sends command to PS/2 device to update physical LEDs
 */
static void update_leds(void) {
    i8042_write_data(0xED);
    i8042_wait_read();
    if (i8042_read_data() == 0xFA) {
        i8042_write_data(locks & 0x07);
    }
}

/*
 * push_event - Adds a key event to the circular buffer (Thread-safe)
 */
static void push_event(keycode_t key, bool pressed) {
    bool flags = spinlock_acquire(&key_events.lock);

    uint32_t next = (key_events.head + 1) % EVENT_BUFFER_SIZE;
    if (next != key_events.tail) {
        key_events.buffer[key_events.head].keycode = key;
        key_events.buffer[key_events.head].pressed = pressed;
        key_events.buffer[key_events.head].modifiers = modifiers;
        key_events.buffer[key_events.head].locks = locks;
        key_events.head = next;
    }

    spinlock_release(&key_events.lock, flags);
}

#pragma endregion

#pragma region Public API

/*
 * keyboard_init - Initializes the event buffer and controller
 */
void keyboard_init(void) {
    kmemset(&key_events, 0, sizeof(key_events));
    spinlock_init(&key_events.lock, "keyboard_events");
    
    if (i8042_init()) {
        update_leds();
        
        while (inb(0x64) & 0x01) {
            inb(0x60);
        }
        
        LOGF("[KBD] Keyboard driver initialized.\n");
    }
}

/*
 * keyboard_get_event - Pops an event from the buffer if available
 */
bool keyboard_get_event(key_event_t* out_event) {
    bool flags = spinlock_acquire(&key_events.lock);

    if (key_events.head == key_events.tail) {
        spinlock_release(&key_events.lock, flags);
        return false;
    }

    *out_event = key_events.buffer[key_events.tail];
    key_events.tail = (key_events.tail + 1) % EVENT_BUFFER_SIZE;

    spinlock_release(&key_events.lock, flags);
    return true;
}

/*
 * keyboard_handler - Main IRQ handler logic (State machine)
 */
cpu_context_t* keyboard_handler(cpu_context_t* ctx) {
    if (!(inb(0x64) & 0x01)) return ctx;

    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended = true;
        return ctx;
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

    if (key == KEY_UNKNOWN) {
        return ctx;
    }

    switch (key) {
        case KEY_LEFT_SHIFT:  pressed ? (modifiers |= MOD_LSHIFT) : (modifiers &= ~MOD_LSHIFT); break;
        case KEY_RIGHT_SHIFT: pressed ? (modifiers |= MOD_RSHIFT) : (modifiers &= ~MOD_RSHIFT); break;
        case KEY_LEFT_CTRL:   pressed ? (modifiers |= MOD_LCTRL)  : (modifiers &= ~MOD_LCTRL);  break;
        case KEY_RIGHT_CTRL:  pressed ? (modifiers |= MOD_RCTRL)  : (modifiers &= ~MOD_RCTRL);  break;
        case KEY_LEFT_ALT:    pressed ? (modifiers |= MOD_LALT)   : (modifiers &= ~MOD_LALT);   break;
        case KEY_RIGHT_ALT:   pressed ? (modifiers |= MOD_RALT)   : (modifiers &= ~MOD_RALT);   break;
        
        case KEY_CAPSLOCK:   if (pressed) { locks ^= LOCK_CAPS;   update_leds(); } break;
        case KEY_NUMLOCK:    if (pressed) { locks ^= LOCK_NUM;    update_leds(); } break;
        case KEY_SCROLLLOCK: if (pressed) { locks ^= LOCK_SCROLL; update_leds(); } break;
        
        default: break;
    }

    input_handle_key((key_event_t){ .keycode = key, .pressed = pressed, .modifiers = modifiers, .locks = locks });
    push_event(key, pressed);

    return ctx;
}

#pragma endregion
