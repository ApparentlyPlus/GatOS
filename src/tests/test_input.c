/*
 * test_input.c - PS/2 Controller and Keyboard Validation Suite
 *
 * Author: Claude Code
 */

#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/i8042.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <stdbool.h>
#include <stdint.h>

static suite_t suite;

#pragma region Helpers

/* Feeds a byte sequence and returns how many full events came out, the last one in *last */
static int feed(const uint8_t* bytes, int n, key_event_t* last) {
    int events = 0;
    key_event_t ev;
    for (int i = 0; i < n; i++) {
        if (keyboard_feed(bytes[i], &ev)) {
            events++;
            if (last) *last = ev;
        }
    }
    return events;
}

#pragma endregion

#pragma region Scancodes

static bool t_make_break(void) {
    key_event_t ev;
    const uint8_t a_make[] = { 0x1E }, a_break[] = { 0x9E };
    TEST_ASSERT(feed(a_make, 1, &ev) == 1);
    TEST_ASSERT(ev.keycode == KEY_A && ev.pressed);
    TEST_ASSERT(feed(a_break, 1, &ev) == 1);
    TEST_ASSERT(ev.keycode == KEY_A && !ev.pressed);
    return true;
}

static bool t_extended(void) {
    key_event_t ev;
    const uint8_t up[] = { 0xE0, 0x48 }, up_rel[] = { 0xE0, 0xC8 };
    TEST_ASSERT(feed(up, 2, &ev) == 1);
    TEST_ASSERT(ev.keycode == KEY_UP && ev.pressed);
    TEST_ASSERT(feed(up_rel, 2, &ev) == 1);
    TEST_ASSERT(ev.keycode == KEY_UP && !ev.pressed);
    return true;
}

static bool t_pause(void) {
    // Pause is E1 1D 45 E1 9D C5. Read as plain bytes that's Ctrl down + NumLock down
    key_event_t ev;
    const uint8_t pause[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    TEST_ASSERT(feed(pause, 6, &ev) == 0);

    // The next key must come through clean, no Ctrl held and NumLock untouched
    const uint8_t a[] = { 0x1E, 0x9E };
    TEST_ASSERT(feed(a, 1, &ev) == 1);
    TEST_ASSERT(ev.keycode == KEY_A);
    TEST_ASSERT(!(ev.modifiers & MOD_CTRL));
    TEST_ASSERT(!(ev.locks & LOCK_NUM));
    feed(a + 1, 1, NULL);
    return true;
}

static bool t_shift_caps(void) {
    key_event_t ev;
    const uint8_t caps[] = { 0x3A, 0xBA }, a[] = { 0x1E, 0x9E }, shift[] = { 0x2A }, unshift[] = { 0xAA };

    feed(caps, 2, &ev);
    TEST_ASSERT(ev.locks & LOCK_CAPS);
    feed(a, 1, &ev);
    TEST_ASSERT(keycode_to_ascii(ev) == 'A');

    // Shift on top of caps cancels out for letters
    feed(shift, 1, NULL);
    feed(a, 1, &ev);
    TEST_ASSERT(ev.modifiers & MOD_LSHIFT);
    TEST_ASSERT(keycode_to_ascii(ev) == 'a');
    feed(unshift, 1, NULL);
    feed(a + 1, 1, NULL);

    // Put caps back the way we found it
    feed(caps, 2, &ev);
    TEST_ASSERT(!(ev.locks & LOCK_CAPS));
    return true;
}

static bool t_caps_repeat(void) {
    // Holding Caps Lock repeats its make code. Only the first one may toggle
    key_event_t ev;
    const uint8_t held[] = { 0x3A, 0x3A, 0x3A, 0x3A, 0xBA };
    TEST_ASSERT(feed(held, 5, &ev) == 5);
    TEST_ASSERT(ev.locks & LOCK_CAPS);

    // A fresh press toggles it back off
    const uint8_t again[] = { 0x3A, 0xBA };
    feed(again, 2, &ev);
    TEST_ASSERT(!(ev.locks & LOCK_CAPS));
    return true;
}

static bool t_led_order(void) {
    // The keyboard wants bit 0 = Scroll, bit 1 = Num, bit 2 = Caps
    TEST_ASSERT(keyboard_led_byte(LOCK_SCROLL) == 1);
    TEST_ASSERT(keyboard_led_byte(LOCK_NUM) == 2);
    TEST_ASSERT(keyboard_led_byte(LOCK_CAPS) == 4);
    TEST_ASSERT(keyboard_led_byte(LOCK_CAPS | LOCK_NUM | LOCK_SCROLL) == 7);
    return true;
}

#pragma endregion

#pragma region Controller

static bool t_ps2_config(void) {
    TEST_ASSERT(i8042_init());

    i8042_write_command(PS2_CMD_READ_CONFIG);
    uint8_t cfg = i8042_read_data();
    if (!(cfg & PS2_CFG_PORT2_CLOCK) || (cfg & PS2_CFG_PORT2_INT)) LOGF("(cfg=0x%x) ", cfg);

    TEST_ASSERT(cfg & PS2_CFG_PORT1_INT);
    TEST_ASSERT(cfg & PS2_CFG_PORT1_TRANS);
    TEST_ASSERT(!(cfg & PS2_CFG_PORT1_CLOCK));
    TEST_ASSERT(cfg & PS2_CFG_PORT2_CLOCK);   // set = port 2 clock disabled
    TEST_ASSERT(!(cfg & PS2_CFG_PORT2_INT));
    return true;
}

#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    test_begin(name);
    test_end(&suite, fn());
}

void test_input(void) {
    suite_begin(&suite, "Input");

    run_test("scancode: make and break",      t_make_break);
    run_test("scancode: E0 extended key",     t_extended);
    run_test("scancode: Pause is swallowed",  t_pause);
    run_test("ascii: shift and caps",         t_shift_caps);
    run_test("locks: auto-repeat toggles once",t_caps_repeat);
    run_test("leds: PS/2 bit order",          t_led_order);
    run_test("i8042: config keeps port 2 off",t_ps2_config);

    suite_end(&suite);
}

#pragma endregion
