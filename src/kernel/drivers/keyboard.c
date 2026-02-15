
/*
 * keyboard.c - Keyboard Driver Implementation
 *
 * Features:
 * Scancode Set 1 State Machine (Handles 0xE0 prefixes)
 * Thread-safe circular event buffer
 * Modifier tracking (Shift, Ctrl, Alt, Gui)
 * Toggle state management (Caps, Num, Scroll lock)
 * LED synchronization with i8042
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/i8042.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/apic.h>
#include <kernel/debug.h>
#include <libc/string.h>

#define EVENT_BUFFER_SIZE 256

#pragma region Internal State

static struct {
    key_event_t buffer[EVENT_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
} g_key_events;

static uint8_t g_current_modifiers = 0;
static uint8_t g_current_locks = 0;
static bool g_extended = false;

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

static void update_leds(void) {
    i8042_write_data(0xED);
    i8042_wait_read();
    if (i8042_read_data() == 0xFA) {
        i8042_write_data(g_current_locks & 0x07);
    }
}

static void push_event(keycode_t key, bool pressed) {
    bool flags = spinlock_acquire(&g_key_events.lock);

    uint32_t next = (g_key_events.head + 1) % EVENT_BUFFER_SIZE;
    if (next != g_key_events.tail) {
        g_key_events.buffer[g_key_events.head].keycode = key;
        g_key_events.buffer[g_key_events.head].pressed = pressed;
        g_key_events.buffer[g_key_events.head].modifiers = g_current_modifiers;
        g_key_events.buffer[g_key_events.head].locks = g_current_locks;
        g_key_events.head = next;
    }

    spinlock_release(&g_key_events.lock, flags);
}

#pragma endregion

#pragma region Public API

void keyboard_init(void) {
    memset(&g_key_events, 0, sizeof(g_key_events));
    spinlock_init(&g_key_events.lock, "keyboard_events");
    
    if (i8042_init()) {
        update_leds();
        LOGF("[KBD] Keyboard driver initialized.\n");
    }
}

bool keyboard_get_event(key_event_t* out_event) {
    bool flags = spinlock_acquire(&g_key_events.lock);

    if (g_key_events.head == g_key_events.tail) {
        spinlock_release(&g_key_events.lock, flags);
        return false;
    }

    *out_event = g_key_events.buffer[g_key_events.tail];
    g_key_events.tail = (g_key_events.tail + 1) % EVENT_BUFFER_SIZE;

    spinlock_release(&g_key_events.lock, flags);
    return true;
}

void keyboard_handler(void) {
    uint8_t scancode = i8042_read_data();

    if (scancode == 0xE0) {
        g_extended = true;
        return;
    }

    bool pressed = !(scancode & 0x80);
    uint8_t code = scancode & 0x7F;

    keycode_t key = KEY_UNKNOWN;

    if (g_extended) {
        key = (keycode_t)(code | 0x80);
        g_extended = false;
    } else if (code < sizeof(scancode_set1)/sizeof(scancode_set1[0])) {
        key = scancode_set1[code];
    }

    if (key == KEY_UNKNOWN) return;

    switch (key) {
        case KEY_LEFT_SHIFT:  pressed ? (g_current_modifiers |= MOD_LSHIFT) : (g_current_modifiers &= ~MOD_LSHIFT); break;
        case KEY_RIGHT_SHIFT: pressed ? (g_current_modifiers |= MOD_RSHIFT) : (g_current_modifiers &= ~MOD_RSHIFT); break;
        case KEY_LEFT_CTRL:   pressed ? (g_current_modifiers |= MOD_LCTRL)  : (g_current_modifiers &= ~MOD_LCTRL);  break;
        case KEY_RIGHT_CTRL:  pressed ? (g_current_modifiers |= MOD_RCTRL)  : (g_current_modifiers &= ~MOD_RCTRL);  break;
        case KEY_LEFT_ALT:    pressed ? (g_current_modifiers |= MOD_LALT)   : (g_current_modifiers &= ~MOD_LALT);   break;
        case KEY_RIGHT_ALT:   pressed ? (g_current_modifiers |= MOD_RALT)   : (g_current_modifiers &= ~MOD_RALT);   break;
        
        case KEY_CAPSLOCK:   if (pressed) { g_current_locks ^= LOCK_CAPS;   update_leds(); } break;
        case KEY_NUMLOCK:    if (pressed) { g_current_locks ^= LOCK_NUM;    update_leds(); } break;
        case KEY_SCROLLLOCK: if (pressed) { g_current_locks ^= LOCK_SCROLL; update_leds(); } break;
        
        default: break;
    }

    push_event(key, pressed);
}

#pragma endregion

#pragma region Layout Translation

static const char layout_us_qwerty[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0
};

static const char layout_us_qwerty_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0
};

char keyboard_keycode_to_ascii(key_event_t event) {
    if (event.keycode > KEY_CAPSLOCK) return 0;
    
    bool shift = (event.modifiers & MOD_SHIFT) != 0;
    bool caps = (event.locks & LOCK_CAPS) != 0;
    
    bool upper = shift;
    if (event.keycode >= KEY_Q && event.keycode <= KEY_P) upper ^= caps;
    if (event.keycode >= KEY_A && event.keycode <= KEY_L) upper ^= caps;
    if (event.keycode >= KEY_Z && event.keycode <= KEY_M) upper ^= caps;

    return upper ? layout_us_qwerty_shift[event.keycode] : layout_us_qwerty[event.keycode];
}

#pragma endregion
