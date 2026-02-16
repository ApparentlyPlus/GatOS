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

#pragma region TTY Test Cases

static bool test_tty_initialization(void) {
    tty_t* tty = tty_create();
    TEST_ASSERT(tty != NULL);
    TEST_ASSERT(tty->head == 0);
    TEST_ASSERT(tty->tail == 0);
    TEST_ASSERT(tty->ldisc.pos == 0);
    TEST_ASSERT(tty->ldisc.echo == true);
    
    tty_destroy(tty);
    return true;
}

static bool test_tty_basic_push_pop(void) {
    tty_t* tty = tty_create();
    tty->ldisc.echo = false;

    tty_input(tty, 'G');
    tty_input(tty, 'a');
    tty_input(tty, 't');
    tty_input(tty, '\n');

    TEST_ASSERT(tty_read_char(tty) == 'G');
    TEST_ASSERT(tty_read_char(tty) == 'a');
    TEST_ASSERT(tty_read_char(tty) == 't');
    TEST_ASSERT(tty_read_char(tty) == '\n');
    TEST_ASSERT(tty->head == tty->tail);

    tty_destroy(tty);
    return true;
}

static bool test_tty_canonical_buffering(void) {
    tty_t* tty = tty_create();
    tty->ldisc.echo = false;

    tty_input(tty, 'O');
    tty_input(tty, 'K');
    
    TEST_ASSERT(tty->head == 0); // Not committed yet
    TEST_ASSERT(tty->ldisc.pos == 2);

    tty_input(tty, '\n');
    TEST_ASSERT(tty->head == 3); // OK\n
    TEST_ASSERT(tty->ldisc.pos == 0);

    char buf[4];
    size_t n = tty_read(tty, buf, 4);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(memcmp(buf, "OK\n", 3) == 0);

    tty_destroy(tty);
    return true;
}

static bool test_tty_backspace_logic(void) {
    tty_t* tty = tty_create();
    tty->ldisc.echo = false;

    tty_input(tty, 'A');
    tty_input(tty, 'B');
    tty_input(tty, 'C');
    TEST_ASSERT(tty->ldisc.pos == 3);

    tty_input(tty, '\b');
    TEST_ASSERT(tty->ldisc.pos == 2);

    tty_input(tty, '\n');
    char buf[4];
    size_t n = tty_read(tty, buf, 4);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(memcmp(buf, "AB\n", 3) == 0);

    tty_destroy(tty);
    return true;
}

static bool test_tty_backspace_boundaries(void) {
    tty_t* tty = tty_create();
    tty->ldisc.echo = false;

    tty_input(tty, '\b');
    TEST_ASSERT(tty->ldisc.pos == 0);

    tty_input(tty, 'A');
    tty_input(tty, '\n');
    TEST_ASSERT(tty->head == 2);
    
    tty_input(tty, 'B');
    tty_input(tty, '\b'); 
    TEST_ASSERT(tty->ldisc.pos == 0);
    
    tty_input(tty, '\b'); 
    TEST_ASSERT(tty->ldisc.pos == 0);

    tty_destroy(tty);
    return true;
}

static bool test_tty_buffer_wrap_around(void) {
    tty_t* tty = tty_create();
    tty->ldisc.echo = false;

    tty->head = TTY_BUFFER_SIZE - 1;
    tty->tail = TTY_BUFFER_SIZE - 1;

    tty_push_char_raw(tty, '1'); 
    tty_push_char_raw(tty, '2'); 
    
    TEST_ASSERT(tty->head == 1);
    TEST_ASSERT(tty_read_char(tty) == '1');
    TEST_ASSERT(tty_read_char(tty) == '2');
    TEST_ASSERT(tty->tail == 1);

    tty_destroy(tty);
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
    run_test("TTY Dynamic Creation", test_tty_initialization);
    run_test("Basic Push/Pop (Canonical)", test_tty_basic_push_pop);
    run_test("Canonical Line Buffering", test_tty_canonical_buffering);
    run_test("Backspace Core Logic", test_tty_backspace_logic);
    run_test("Backspace Boundaries", test_tty_backspace_boundaries);
    run_test("Buffer Wrap-around", test_tty_buffer_wrap_around);
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
