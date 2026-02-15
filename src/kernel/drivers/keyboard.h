/*
 * keyboard.h - Keyboard Driver
 *
 * This driver handles scancode translation (Set 1), modifier tracking,
 * toggle state (Caps/Num/Scroll lock), and LED synchronization.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// GatOS Keycodes
typedef enum {
    KEY_UNKNOWN = 0,
    KEY_ESC,
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE,
    KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_ENTER,
    KEY_LEFT_CTRL, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_QUOTE, KEY_BACKTICK,
    KEY_LEFT_SHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_RIGHT_SHIFT,
    KEY_KPMULT, KEY_LEFT_ALT, KEY_SPACE, KEY_CAPSLOCK,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPMINUS,
    KEY_KP4, KEY_KP5, KEY_KP6, KEY_KPPLUS,
    KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0, KEY_KPDOT,
    KEY_F11 = 0x57, KEY_F12 = 0x58,
    
    // Extended keys
    KEY_KPENTER = 0x9C,
    KEY_RIGHT_CTRL = 0x9D,
    KEY_KPSLASH = 0xB5,
    KEY_RIGHT_ALT = 0xB8,
    KEY_HOME = 0xC7, KEY_UP = 0xC8, KEY_PAGEUP = 0xC9,
    KEY_LEFT = 0xCB, KEY_RIGHT = 0xCD,
    KEY_END = 0xCF, KEY_DOWN = 0xD0, KEY_PAGEDOWN = 0xD1,
    KEY_INSERT = 0xD2, KEY_DELETE = 0xD3,
    KEY_LEFT_GUI = 0xDB, KEY_RIGHT_GUI = 0xDC, KEY_APPS = 0xDD,
} keycode_t;

// Modifier Flags
#define MOD_LSHIFT      (1 << 0)
#define MOD_RSHIFT      (1 << 1)
#define MOD_LCTRL       (1 << 2)
#define MOD_RCTRL       (1 << 3)
#define MOD_LALT        (1 << 4)
#define MOD_RALT        (1 << 5)
#define MOD_LGUI        (1 << 6)
#define MOD_RGUI        (1 << 7)

#define MOD_SHIFT       (MOD_LSHIFT | MOD_RSHIFT)
#define MOD_CTRL        (MOD_LCTRL | MOD_RCTRL)
#define MOD_ALT         (MOD_LALT | MOD_RALT)
#define MOD_GUI         (MOD_LGUI | MOD_RGUI)

// Lock Flags
#define LOCK_CAPS       (1 << 0)
#define LOCK_NUM        (1 << 1)
#define LOCK_SCROLL     (1 << 2)

// Key Event
typedef struct {
    keycode_t keycode;
    bool pressed;
    uint8_t modifiers;
    uint8_t locks;
} key_event_t;

// Public API
void keyboard_init(void);
bool keyboard_get_event(key_event_t* out_event);
void keyboard_handler(void); // To be called from ISR

// Layout translation (US QWERTY default)
char keyboard_keycode_to_ascii(key_event_t event);
