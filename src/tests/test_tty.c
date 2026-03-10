/*
 * test_tty.c - TTY Subsystem Validation Suite
 *
 * Tests every public TTY function: create, destroy, input, push_char_raw,
 * read_char, read, write, header_init, header_write, switch, cycle.
 * Covers canonical line discipline, backspace, ring buffer wrap/overflow,
 * ldisc overflow, header state, and mass create/destroy.
 */

#include <kernel/drivers/tty.h>
#include <kernel/drivers/console.h>
#include <kernel/drivers/ldisc.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stddef.h>

static int g_tests_total  = 0;
static int g_tests_passed = 0;

#pragma region Init / Destroy

static bool t_create_nn(void) {
    tty_t* t = tty_create();
    TEST_ASSERT(t != NULL);
    tty_destroy(t);
    return true;
}

static bool t_create_fields(void) {
    tty_t* t = tty_create();
    TEST_ASSERT(t->head == 0);
    TEST_ASSERT(t->tail == 0);
    TEST_ASSERT(t->ldisc.pos == 0);
    TEST_ASSERT(t->ldisc.echo == true);
    tty_destroy(t);
    return true;
}

static bool t_create_con(void) {
    tty_t* t = tty_create();
    TEST_ASSERT(t->console != NULL);
    tty_destroy(t);
    return true;
}

static bool t_create_lkfree(void) {
    tty_t* t = tty_create();
    TEST_ASSERT(t->lock.locked == 0);
    tty_destroy(t);
    return true;
}

static bool t_destroy_16(void) {
    #define D16 16
    tty_t* ttys[D16];
    for (int i = 0; i < D16; i++) {
        ttys[i] = tty_create();
        if (!ttys[i]) { for (int j=0;j<i;j++) tty_destroy(ttys[j]); return false; }
    }
    for (int i = D16-1; i >= 0; i--) tty_destroy(ttys[i]);
    return true;
}

static bool t_destroy_32(void) {
    #define D32 32
    tty_t* ttys[D32];
    for (int i = 0; i < D32; i++) {
        ttys[i] = tty_create();
        if (!ttys[i]) { for (int j=0;j<i;j++) tty_destroy(ttys[j]); return false; }
    }
    for (int i = 0; i < D32; i++) tty_destroy(ttys[i]);
    return true;
}
#pragma endregion

#pragma region push_char_raw

static bool t_raw_head(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    TEST_ASSERT(t->head == 0);
    tty_push_char_raw(t, 'A');
    TEST_ASSERT(t->head == 1);
    tty_push_char_raw(t, 'B');
    TEST_ASSERT(t->head == 2);
    tty_destroy(t);
    return true;
}

static bool t_raw_read(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_push_char_raw(t, 'X');
    tty_push_char_raw(t, 'Y');
    tty_push_char_raw(t, 'Z');
    TEST_ASSERT(tty_read_char(t) == 'X');
    TEST_ASSERT(tty_read_char(t) == 'Y');
    TEST_ASSERT(tty_read_char(t) == 'Z');
    TEST_ASSERT(t->head == t->tail);
    tty_destroy(t);
    return true;
}

static bool t_raw_wrap(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    t->head = TTY_BUFFER_SIZE - 1;
    t->tail = TTY_BUFFER_SIZE - 1;
    tty_push_char_raw(t, '1');
    tty_push_char_raw(t, '2');
    TEST_ASSERT(t->head == 1);
    TEST_ASSERT(tty_read_char(t) == '1');
    TEST_ASSERT(tty_read_char(t) == '2');
    TEST_ASSERT(t->tail == 1);
    tty_destroy(t);
    return true;
}

static bool t_raw_ovf(void) {
    /* Push capacity+1 chars; the last must be silently dropped */
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    uint32_t cap = TTY_BUFFER_SIZE - 1;
    for (uint32_t i = 0; i < cap; i++) tty_push_char_raw(t, 'X');
    uint32_t h = t->head;
    uint32_t tl = t->tail;
    tty_push_char_raw(t, 'Z'); /* must be dropped */
    TEST_ASSERT(t->head == h);
    TEST_ASSERT(t->tail == tl);
    for (uint32_t i = 0; i < cap; i++) TEST_ASSERT(tty_read_char(t) == 'X');
    TEST_ASSERT(t->head == t->tail);
    tty_destroy(t);
    return true;
}

static bool t_raw_ovf_stbl(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    for (uint32_t i = 0; i < TTY_BUFFER_SIZE; i++) tty_push_char_raw(t, 'A');
    uint32_t h = t->head;
    /* One more must not advance head */
    tty_push_char_raw(t, 'B');
    TEST_ASSERT(t->head == h);
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region tty_input / canonical

static bool t_inp_hold(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t, 'A'); tty_input(t, 'B');
    TEST_ASSERT(t->head == 0);       /* not committed yet */
    TEST_ASSERT(t->ldisc.pos == 2);
    tty_destroy(t);
    return true;
}

static bool t_inp_nl(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t, 'O'); tty_input(t, 'K'); tty_input(t, '\n');
    TEST_ASSERT(t->head == 3);   /* OK\n committed */
    TEST_ASSERT(t->ldisc.pos == 0);
    tty_destroy(t);
    return true;
}

static bool t_inp_data(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t, 'H'); tty_input(t, 'i'); tty_input(t, '\n');
    char buf[4]; size_t n = tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(n == 3);
    TEST_ASSERT(kmemcmp(buf, "Hi\n", 3) == 0);
    tty_destroy(t);
    return true;
}

static bool t_inp_empty(void) {
    /* Just a newline → commits only '\n' */
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t, '\n');
    char buf[2]; size_t n = tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(n == 1 && buf[0] == '\n');
    tty_destroy(t);
    return true;
}

static bool t_inp_2lines(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'H'); tty_input(t,'i'); tty_input(t,'\n');
    tty_input(t,'B'); tty_input(t,'y'); tty_input(t,'e'); tty_input(t,'\n');
    char b[8];
    size_t n1 = tty_read(t, b, sizeof(b));
    TEST_ASSERT(n1 == 3 && kmemcmp(b, "Hi\n", 3) == 0);
    size_t n2 = tty_read(t, b, sizeof(b));
    TEST_ASSERT(n2 == 4 && kmemcmp(b, "Bye\n", 4) == 0);
    TEST_ASSERT(t->head == t->tail);
    tty_destroy(t);
    return true;
}

static bool t_inp_5lines(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    static const char* lines[] = {"A\n","BB\n","CCC\n","DDDD\n","EEEEE\n"};
    static const size_t lens[] = {2, 3, 4, 5, 6};
    for (int i = 0; i < 5; i++)
        for (size_t j = 0; j < lens[i]; j++) tty_input(t, lines[i][j]);
    for (int i = 0; i < 5; i++) {
        char buf[8]; size_t n = tty_read(t, buf, sizeof(buf));
        TEST_ASSERT(n == lens[i]);
        TEST_ASSERT(kmemcmp(buf, lines[i], lens[i]) == 0);
    }
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region tty_read

static bool t_read_part(void) {
    /* Request fewer bytes than committed */
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'A'); tty_input(t,'B'); tty_input(t,'C'); tty_input(t,'\n');
    char buf[2]; size_t n = tty_read(t, buf, 2);
    TEST_ASSERT(n == 2);
    TEST_ASSERT(buf[0] == 'A' && buf[1] == 'B');
    tty_destroy(t);
    return true;
}

static bool t_read_stop(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'X'); tty_input(t,'\n');
    tty_input(t,'Y'); tty_input(t,'\n'); /* second line */
    char buf[8]; size_t n = tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(n == 2); /* only first line */
    TEST_ASSERT(buf[0] == 'X' && buf[1] == '\n');
    tty_destroy(t);
    return true;
}

static bool t_read_drain(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'Z'); tty_input(t,'\n');
    char buf[4]; tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(t->head == t->tail);
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region tty_read_char

static bool t_rchar_ord(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'1'); tty_input(t,'2'); tty_input(t,'3'); tty_input(t,'\n');
    TEST_ASSERT(tty_read_char(t) == '1');
    TEST_ASSERT(tty_read_char(t) == '2');
    TEST_ASSERT(tty_read_char(t) == '3');
    TEST_ASSERT(tty_read_char(t) == '\n');
    TEST_ASSERT(t->head == t->tail);
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region Backspace

static bool t_bs_dec(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'A'); tty_input(t,'B'); tty_input(t,'C');
    TEST_ASSERT(t->ldisc.pos == 3);
    tty_input(t, '\b');
    TEST_ASSERT(t->ldisc.pos == 2);
    tty_destroy(t);
    return true;
}

static bool t_bs_zero(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t, '\b');
    TEST_ASSERT(t->ldisc.pos == 0);
    tty_input(t, '\b');
    TEST_ASSERT(t->ldisc.pos == 0);
    tty_destroy(t);
    return true;
}

static bool t_bs_all(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'A'); tty_input(t,'B'); tty_input(t,'C');
    tty_input(t,'\b'); tty_input(t,'\b'); tty_input(t,'\b');
    TEST_ASSERT(t->ldisc.pos == 0);
    tty_input(t, '\n');
    char buf[4]; size_t n = tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(n == 1 && buf[0] == '\n');
    tty_destroy(t);
    return true;
}

static bool t_bs_nocross(void) {
    /* Backspace must not erase previously committed data */
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'A'); tty_input(t,'\n'); /* committed */
    uint32_t h_after_commit = t->head;
    tty_input(t,'B'); tty_input(t,'\b'); /* erase B, but not the committed A */
    TEST_ASSERT(t->ldisc.pos == 0);
    tty_input(t,'\b'); /* extra — must be safe */
    TEST_ASSERT(t->ldisc.pos == 0);
    TEST_ASSERT(t->head == h_after_commit); /* committed data intact */
    tty_destroy(t);
    return true;
}

static bool t_bs_multi(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'X'); tty_input(t,'Y'); tty_input(t,'Z');
    tty_input(t,'\b'); tty_input(t,'\b');
    TEST_ASSERT(t->ldisc.pos == 1);
    tty_input(t,'Q'); tty_input(t,'\n');
    char buf[4]; size_t n = tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(n == 3);
    TEST_ASSERT(kmemcmp(buf, "XQ\n", 3) == 0);
    tty_destroy(t);
    return true;
}

static bool t_bs_data(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'A'); tty_input(t,'B'); tty_input(t,'C');
    tty_input(t,'\b'); /* erase C */
    tty_input(t,'\n');
    char buf[4]; size_t n = tty_read(t, buf, sizeof(buf));
    TEST_ASSERT(n == 3);
    TEST_ASSERT(kmemcmp(buf, "AB\n", 3) == 0);
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region ldisc overflow

static bool t_ldisc_ovf(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    /* Fill ldisc completely */
    for (int i = 0; i < LDISC_LINE_MAX - 1; i++) tty_input(t, 'A' + (i % 26));
    /* pos must not exceed the buffer */
    TEST_ASSERT(t->ldisc.pos <= (uint32_t)(LDISC_LINE_MAX - 1));
    /* Push extra — must not crash */
    tty_input(t, 'Z');
    TEST_ASSERT(t->ldisc.pos <= (uint32_t)(LDISC_LINE_MAX - 1));
    /* Commit — must produce valid data */
    tty_input(t, '\n');
    TEST_ASSERT(t->ldisc.pos == 0);
    TEST_ASSERT(t->head != t->tail);
    tty_destroy(t);
    return true;
}

static bool t_ldisc_rst(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_input(t,'A'); tty_input(t,'B'); tty_input(t,'\n');
    TEST_ASSERT(t->ldisc.pos == 0);
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region tty_write

static bool t_wr_noring(void) {
    /* tty_write must not modify the input ring buffer */
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    uint32_t h0 = t->head, tl0 = t->tail;
    tty_write(t, "Hello TTY", 9);
    TEST_ASSERT(t->head == h0 && t->tail == tl0);
    tty_destroy(t);
    return true;
}

static bool t_wr_zero(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    tty_write(t, "X", 0); /* zero-length must not crash */
    tty_destroy(t);
    return true;
}

static bool t_wr_single(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    uint32_t h0 = t->head, tl0 = t->tail;
    tty_write(t, "A", 1);
    TEST_ASSERT(t->head == h0 && t->tail == tl0);
    tty_destroy(t);
    return true;
}

static bool t_wr_large(void) {
    tty_t* t = tty_create();
    t->ldisc.echo = false;
    char buf[512]; kmemset(buf, 'X', sizeof(buf));
    uint32_t h0 = t->head;
    tty_write(t, buf, sizeof(buf));
    TEST_ASSERT(t->head == h0); /* ring buffer untouched */
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region Header

static bool t_hdr_rows(void) {
    tty_t* t = tty_create();
    TEST_ASSERT(t->console->header_rows == 0);
    tty_header_init(t, 3);
    TEST_ASSERT(t->console->header_rows == 3);
    tty_destroy(t);
    return true;
}

static bool t_hdr_stable(void) {
    tty_t* t = tty_create();
    tty_header_init(t, 3);
    tty_header_write(t, 1, "Test", CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
    TEST_ASSERT(t->console->header_rows == 3); /* must not corrupt header_rows */
    tty_destroy(t);
    return true;
}

static bool t_hdr_cursor(void) {
    tty_t* t = tty_create();
    tty_header_init(t, 3);
    TEST_ASSERT(t->console->cursor_y >= t->console->header_rows);
    tty_destroy(t);
    return true;
}

static bool t_hdr_all(void) {
    /* Write to every row in the header — must not crash */
    tty_t* t = tty_create();
    tty_header_init(t, 4);
    for (size_t r = 0; r < 4; r++)
        tty_header_write(t, r, "Row", CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    TEST_ASSERT(t->console->header_rows == 4);
    tty_destroy(t);
    return true;
}

static bool t_hdr_one(void) {
    tty_t* t = tty_create();
    tty_header_init(t, 1);
    TEST_ASSERT(t->console->header_rows == 1);
    TEST_ASSERT(t->console->cursor_y >= 1);
    tty_destroy(t);
    return true;
}
#pragma endregion

#pragma region tty_switch / tty_cycle

static bool t_switch(void) {
    /* Switch to a new TTY and back — must not crash */
    tty_t* orig = g_active_tty;
    tty_t* t = tty_create();
    tty_switch(t);
    tty_switch(orig);
    tty_destroy(t);
    return true;
}

static bool t_cycle(void) {
    /* tty_cycle must not crash even if only one TTY exists */
    tty_cycle();
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-40s ", name);
    if (fn()) { g_tests_passed++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_tty(void) {
    g_tests_total  = 0;
    g_tests_passed = 0;
    LOGF("\n--- BEGIN TTY SUBSYSTEM TEST ---\n");

    run_test("create: not null",              t_create_nn);
    run_test("create: fields zero",           t_create_fields);
    run_test("create: has console",           t_create_con);
    run_test("create: lock free",             t_create_lkfree);
    run_test("destroy 16 TTYs (reverse)",     t_destroy_16);
    run_test("destroy 32 TTYs (forward)",     t_destroy_32);
    run_test("raw: head advances",            t_raw_head);
    run_test("raw: read_char order",          t_raw_read);
    run_test("raw: wrap at SIZE-1",           t_raw_wrap);
    run_test("raw: overflow no overwrite",    t_raw_ovf);
    run_test("raw: overflow head stable",     t_raw_ovf_stbl);
    run_test("input: holds in ldisc",         t_inp_hold);
    run_test("input: newline commits",        t_inp_nl);
    run_test("input: data correct",           t_inp_data);
    run_test("input: empty line",             t_inp_empty);
    run_test("input: 2 lines",               t_inp_2lines);
    run_test("input: 5 lines",               t_inp_5lines);
    run_test("read: partial count",           t_read_part);
    run_test("read: stops at newline",        t_read_stop);
    run_test("read: drains buffer",           t_read_drain);
    run_test("read_char: FIFO order",         t_rchar_ord);
    run_test("backspace: decrements pos",     t_bs_dec);
    run_test("backspace: safe at zero",       t_bs_zero);
    run_test("backspace: erase all + newline",t_bs_all);
    run_test("backspace: no cross-commit",    t_bs_nocross);
    run_test("backspace: multi erase",        t_bs_multi);
    run_test("backspace: data correct",       t_bs_data);
    run_test("ldisc: overflow safe",          t_ldisc_ovf);
    run_test("ldisc: pos resets after commit",t_ldisc_rst);
    run_test("write: no ring buffer touch",   t_wr_noring);
    run_test("write: zero length safe",       t_wr_zero);
    run_test("write: single char",            t_wr_single);
    run_test("write: large buffer",           t_wr_large);
    run_test("header: init sets rows",        t_hdr_rows);
    run_test("header: write stable",          t_hdr_stable);
    run_test("header: cursor below header",   t_hdr_cursor);
    run_test("header: write all rows",        t_hdr_all);
    run_test("header: single row",            t_hdr_one);
    run_test("switch: no crash",              t_switch);
    run_test("cycle: no crash",               t_cycle);

    LOGF("--- END TTY SUBSYSTEM TEST ---\n");
    LOGF("TTY Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some TTY tests failed (%d/%d passed).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All TTY tests passed! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
