/*
 * test_tty.c - TTY Abstraction Test Suite
 */

#include <kernel/drivers/tty.h>
#include <kernel/drivers/console.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

static int g_tests_total = 0;
static int g_tests_passed = 0;

// Mock Console State
static console_char_t g_mock_buffer[80 * 25];
static console_t g_mock_console;

static void setup_mock_console(void) {
    memset(g_mock_buffer, 0, sizeof(g_mock_buffer));
    g_mock_console.buffer = g_mock_buffer;
    g_mock_console.width = 80;
    g_mock_console.height = 25;
    g_mock_console.cursor_x = 0;
    g_mock_console.cursor_y = 0;
    g_mock_console.fg_color = CONSOLE_COLOR_WHITE;
    g_mock_console.bg_color = CONSOLE_COLOR_BLACK;
    g_mock_console.utf8_bytes_needed = 0;
    g_mock_console.utf8_codepoint = 0;
    g_mock_console.reentrancy_count = 0;
    
    spinlock_init(&g_mock_console.lock, "mock_console");
    
    for (size_t i = 0; i < 80 * 25; i++) {
        g_mock_buffer[i].codepoint = ' ';
        g_mock_buffer[i].fg = CONSOLE_COLOR_WHITE;
        g_mock_buffer[i].bg = CONSOLE_COLOR_BLACK;
    }
}

static bool check_last_char(char expected) {
    size_t x = g_mock_console.cursor_x;
    size_t y = g_mock_console.cursor_y;
    if (x == 0 && y == 0) return false; 
    if (x == 0) { x = g_mock_console.width - 1; y--; } else { x--; }
    size_t idx = y * g_mock_console.width + x;
    return g_mock_console.buffer[idx].codepoint == (uint32_t)expected;
}

#pragma region TTY Test Cases

static bool test_tty_initialization(void) {
    setup_mock_console();
    tty_t tty;
    tty_init(&tty, &g_mock_console);

    TEST_ASSERT(tty.head == 0);
    TEST_ASSERT(tty.tail == 0);
    TEST_ASSERT(tty.ldisc.pos == 0);
    TEST_ASSERT(tty.ldisc.echo == true);
    TEST_ASSERT(tty.console == &g_mock_console);
    
    return true;
}

static bool test_tty_basic_push_pop(void) {
    tty_t tty;
    tty_init(&tty, NULL); 
    tty.ldisc.echo = false;

    // We no longer have raw mode in ldisc, so we push \n to commit
    tty_input(&tty, 'G');
    tty_input(&tty, 'a');
    tty_input(&tty, 't');
    tty_input(&tty, '\n');

    TEST_ASSERT(tty_read_char(&tty) == 'G');
    TEST_ASSERT(tty_read_char(&tty) == 'a');
    TEST_ASSERT(tty_read_char(&tty) == 't');
    TEST_ASSERT(tty_read_char(&tty) == '\n');
    TEST_ASSERT(tty.head == tty.tail);

    return true;
}

static bool test_tty_echo_functionality(void) {
    setup_mock_console();
    tty_t tty;
    tty_init(&tty, &g_mock_console);
    // Canonical mode echoes immediately in our implementation

    tty_input(&tty, 'Z');
    TEST_ASSERT(check_last_char('Z'));

    tty.ldisc.echo = false;
    tty_input(&tty, 'X');
    TEST_ASSERT(check_last_char('Z')); // Should still be Z

    return true;
}

static bool test_tty_canonical_buffering(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.ldisc.echo = false;

    tty_input(&tty, 'O');
    tty_input(&tty, 'K');
    
    TEST_ASSERT(tty.head == 0); // Not committed yet
    TEST_ASSERT(tty.ldisc.pos == 2);

    tty_input(&tty, '\n');
    TEST_ASSERT(tty.head == 3); // OK\n
    TEST_ASSERT(tty.ldisc.pos == 0);

    char buf[4];
    size_t n = tty_read(&tty, buf, 4);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(memcmp(buf, "OK\n", 3) == 0);

    return true;
}

static bool test_tty_backspace_logic(void) {
    setup_mock_console();
    tty_t tty;
    tty_init(&tty, &g_mock_console);
    tty.ldisc.echo = true;

    tty_input(&tty, 'A');
    tty_input(&tty, 'B');
    tty_input(&tty, 'C');
    TEST_ASSERT(tty.ldisc.pos == 3);
    TEST_ASSERT(check_last_char('C'));

    tty_input(&tty, '\b');
    TEST_ASSERT(tty.ldisc.pos == 2);
    
    size_t idx = g_mock_console.cursor_y * g_mock_console.width + g_mock_console.cursor_x;
    TEST_ASSERT(g_mock_console.buffer[idx].codepoint == ' ');

    tty_input(&tty, '\n');
    char buf[4];
    size_t n = tty_read(&tty, buf, 4);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(memcmp(buf, "AB\n", 3) == 0);

    return true;
}

static bool test_tty_backspace_boundaries(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.ldisc.echo = false;

    tty_input(&tty, '\b');
    TEST_ASSERT(tty.ldisc.pos == 0);

    tty_input(&tty, 'A');
    tty_input(&tty, '\n');
    TEST_ASSERT(tty.head == 2);
    
    tty_input(&tty, 'B');
    tty_input(&tty, '\b'); 
    TEST_ASSERT(tty.ldisc.pos == 0);
    
    tty_input(&tty, '\b'); 
    TEST_ASSERT(tty.ldisc.pos == 0);

    return true;
}

static bool test_tty_newline_normalization(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    
    tty_input(&tty, 'X');
    tty_input(&tty, '\r'); 
    
    TEST_ASSERT(tty.buffer[1] == '\n');
    TEST_ASSERT(tty.head == 2);

    return true;
}

static bool test_tty_buffer_wrap_around(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.ldisc.echo = false;

    tty.head = TTY_BUFFER_SIZE - 1;
    tty.tail = TTY_BUFFER_SIZE - 1;

    // Use push_char_raw to test wrap around of the circular buffer itself
    tty_push_char_raw(&tty, '1'); 
    tty_push_char_raw(&tty, '2'); 
    
    TEST_ASSERT(tty.head == 1);
    TEST_ASSERT(tty_read_char(&tty) == '1');
    TEST_ASSERT(tty_read_char(&tty) == '2');
    TEST_ASSERT(tty.tail == 1);

    return true;
}

static bool test_tty_overflow_discard(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.ldisc.echo = false;

    for (int i = 0; i < TTY_BUFFER_SIZE - 1; i++) {
        tty_push_char_raw(&tty, '.');
    }
    
    uint32_t head_before = tty.head;
    tty_push_char_raw(&tty, '!'); 
    TEST_ASSERT(tty.head == head_before);

    return true;
}

#pragma endregion

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    if (func()) { g_tests_passed++; LOGF("[PASS]\n"); } else { LOGF("[FAIL]\n"); }
}

void test_tty(void) {
    g_tests_total = 0;
    g_tests_passed = 0;
    LOGF("\n--- BEGIN TTY SUBSYSTEM TEST ---\n");
    run_test("TTY Initialization", test_tty_initialization);
    run_test("Basic Push/Pop (Canonical)", test_tty_basic_push_pop);
    run_test("Echo Functionality", test_tty_echo_functionality);
    run_test("Canonical Line Buffering", test_tty_canonical_buffering);
    run_test("Backspace Core Logic", test_tty_backspace_logic);
    run_test("Backspace Boundaries", test_tty_backspace_boundaries);
    run_test("Newline Normalization", test_tty_newline_normalization);
    run_test("Buffer Wrap-around", test_tty_buffer_wrap_around);
    run_test("Overflow Discard", test_tty_overflow_discard);
    LOGF("--- END TTY SUBSYSTEM TEST ---\n");
    LOGF("TTY Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <kernel/drivers/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[-] Some TTY tests failed (%d/%d). Check debug log.\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        printf("[+] All TTY tests passed successfully! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
