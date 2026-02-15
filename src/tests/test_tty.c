/*
 * test_tty.c - TTY Abstraction Test Suite
 *
 * Verifies line discipline (canonical mode), echoing, backspace handling,
 * and circular buffer management.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/tty.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

static int g_tests_total = 0;
static int g_tests_passed = 0;

static char g_last_echo = 0;
static int g_echo_count = 0;

/*
 * mock_write_cb - Captures TTY output for validation
 */
static void mock_write_cb(char c) {
    g_last_echo = c;
    g_echo_count++;
}

#pragma region TTY Test Cases

/*
 * test_tty_initialization - Verifies TTY starts in a clean state
 */
static bool test_tty_initialization(void) {
    tty_t tty;
    tty_init(&tty, mock_write_cb);

    TEST_ASSERT(tty.head == 0);
    TEST_ASSERT(tty.tail == 0);
    TEST_ASSERT(tty.canon_pos == 0);
    TEST_ASSERT(tty.echo == true);
    TEST_ASSERT(tty.canon == true);
    TEST_ASSERT(tty.write_callback == mock_write_cb);
    
    return true;
}

/*
 * test_tty_basic_push_pop - Verifies simple char I/O without line discipline
 */
static bool test_tty_basic_push_pop(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.canon = false;
    tty.echo = false;

    tty_push_char(&tty, 'G');
    tty_push_char(&tty, 'a');
    tty_push_char(&tty, 't');

    TEST_ASSERT(tty_read_char(&tty) == 'G');
    TEST_ASSERT(tty_read_char(&tty) == 'a');
    TEST_ASSERT(tty_read_char(&tty) == 't');
    TEST_ASSERT(tty.head == tty.tail);

    return true;
}

/*
 * test_tty_echo_functionality - Ensures input is echoed back to hardware
 */
static bool test_tty_echo_functionality(void) {
    tty_t tty;
    tty_init(&tty, mock_write_cb);
    tty.canon = false;
    g_echo_count = 0;

    tty_push_char(&tty, 'Z');
    TEST_ASSERT(g_last_echo == 'Z');
    TEST_ASSERT(g_echo_count == 1);

    tty.echo = false;
    tty_push_char(&tty, 'X');
    TEST_ASSERT(g_last_echo == 'Z'); // Should not have changed
    TEST_ASSERT(g_echo_count == 1);

    return true;
}

/*
 * test_tty_canonical_buffering - Verifies that reads wait for a full line
 */
static bool test_tty_canonical_buffering(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.echo = false;
    // canon is true by default

    tty_push_char(&tty, 'O');
    tty_push_char(&tty, 'K');
    
    // In canonical mode, head moves but tail shouldn't be "readable" by logic
    // unless a newline is pushed. Note: tty_read_char blocks, so we check internals.
    TEST_ASSERT(tty.head == 2);
    TEST_ASSERT(tty.canon_pos == 0);

    tty_push_char(&tty, '\n');
    TEST_ASSERT(tty.canon_pos == 3);

    char buf[4];
    size_t n = tty_read(&tty, buf, 4);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(memcmp(buf, "OK\n", 3) == 0);

    return true;
}

/*
 * test_tty_backspace_logic - Tests character deletion in canonical mode
 */
static bool test_tty_backspace_logic(void) {
    tty_t tty;
    tty_init(&tty, mock_write_cb);
    tty.echo = true;
    g_echo_count = 0;

    // Type "ABC", then backspace
    tty_push_char(&tty, 'A');
    tty_push_char(&tty, 'B');
    tty_push_char(&tty, 'C');
    TEST_ASSERT(tty.head == 3);

    // Backspace should move head back and echo BS-SPACE-BS
    int count_before = g_echo_count;
    tty_push_char(&tty, '\b');
    TEST_ASSERT(tty.head == 2);
    TEST_ASSERT(g_echo_count == count_before + 3); // \b, space, \b

    tty_push_char(&tty, '\n');
    char buf[4];
    size_t n = tty_read(&tty, buf, 4);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(memcmp(buf, "AB\n", 3) == 0);

    return true;
}

/*
 * test_tty_backspace_boundaries - Ensures backspace doesn't underflow or cross line boundaries
 */
static bool test_tty_backspace_boundaries(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.echo = false;

    // 1. Backspace on empty buffer
    tty_push_char(&tty, '\b');
    TEST_ASSERT(tty.head == 0);

    // 2. Backspace after a committed line
    tty_push_char(&tty, 'A');
    tty_push_char(&tty, '\n');
    TEST_ASSERT(tty.canon_pos == 2);
    
    tty_push_char(&tty, 'B');
    tty_push_char(&tty, '\b'); // Should delete 'B'
    TEST_ASSERT(tty.head == 2);
    
    tty_push_char(&tty, '\b'); // Should NOT delete '\n' or 'A'
    TEST_ASSERT(tty.head == 2);

    return true;
}

/*
 * test_tty_newline_normalization - Verifies \r is converted to \n
 */
static bool test_tty_newline_normalization(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    
    tty_push_char(&tty, 'X');
    tty_push_char(&tty, '\r'); // Normalization check
    
    TEST_ASSERT(tty.buffer[1] == '\n');
    TEST_ASSERT(tty.canon_pos == 2);

    return true;
}

/*
 * test_tty_buffer_wrap_around - Tests circularity of the 4KB buffer
 */
static bool test_tty_buffer_wrap_around(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.canon = false;
    tty.echo = false;

    // Shift head and tail near the end
    tty.head = TTY_BUFFER_SIZE - 1;
    tty.tail = TTY_BUFFER_SIZE - 1;

    tty_push_char(&tty, '1'); // At index SIZE-1
    tty_push_char(&tty, '2'); // Wraps to index 0
    
    TEST_ASSERT(tty.head == 1);
    TEST_ASSERT(tty_read_char(&tty) == '1');
    TEST_ASSERT(tty_read_char(&tty) == '2');
    TEST_ASSERT(tty.tail == 1);

    return true;
}

/*
 * test_tty_overflow_discard - Ensures TTY doesn't crash when full
 */
static bool test_tty_overflow_discard(void) {
    tty_t tty;
    tty_init(&tty, NULL);
    tty.canon = false;
    tty.echo = false;

    // Fill buffer completely (one slot remains empty in circular buffers)
    for (int i = 0; i < TTY_BUFFER_SIZE - 1; i++) {
        tty_push_char(&tty, '.');
    }
    
    uint32_t head_before = tty.head;
    tty_push_char(&tty, '!'); // Should be discarded
    TEST_ASSERT(tty.head == head_before);

    return true;
}

#pragma endregion

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
    if (func()) {
        g_tests_passed++;
        LOGF("[PASS]\n");
    } else {
        LOGF("[FAIL]\n");
    }
}

void test_tty(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN TTY SUBSYSTEM TEST ---\n");

    run_test("TTY Initialization", test_tty_initialization);
    run_test("Basic Push/Pop (Raw)", test_tty_basic_push_pop);
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

#pragma endregion
