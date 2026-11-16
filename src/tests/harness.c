/*
 * harness.c - Shared test runner output
 *
 * Every suite prints the same shape to the debug log:
 *
 *   Physical memory manager
 *     alloc: bad arguments                     ok
 *     free: double free caught
 *         [PMM ERROR] Double free at 0x2000
 *                                              ok
 *     22/22 passed
 *
 * Whatever a test logs on its way (subsystem errors, failed assertions) shows up indented
 * under its name, and the verdict lines up with the others either way.
 *
 * Author: Claude Code
 */

#include <tests/tests.h>
#include <kernel/debug.h>
#include <klibc/string.h>

#ifdef TEST_BUILD
#include <kernel/drivers/console.h>
#include <klibc/stdio.h>
#endif

#define NAME_WIDTH 40

// Length of the name on the open test line, the verdict pads from there
static int name_len = 0;

/*
 * suite_begin - Starts a suite: its name, then everything below it indented
 */
void suite_begin(suite_t* s, const char* name) {
    s->name = name;
    s->run = 0;
    s->pass = 0;
    LOGF("%s\n", name);
    log_indent(1);
}

/*
 * suite_end - The tally, in the debug log and on screen
 */
void suite_end(suite_t* s) {
    LOGF("%d/%d passed\n\n", s->pass, s->run);
    log_indent(-1);

    #ifdef TEST_BUILD
    bool ok = s->pass == s->run;
    console_set_color(ok ? CONSOLE_COLOR_GREEN : CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
    kprintf("[%c] %s: %d/%d passed\n", ok ? '+' : '-', s->name, s->pass, s->run);
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    #endif
}

/*
 * test_begin - Prints the test name and leaves the line open for its verdict
 */
void test_begin(const char* name) {
    name_len = (int)kstrlen(name);
    LOGH("%s", name);
    log_indent(2);
}

/*
 * verdict - Finishes the open line, or if the test logged something, puts it on its own line
 */
static void verdict(const char* text, const char* why) {
    log_indent(-2);
    int pad = log_resume() ? NAME_WIDTH - name_len : NAME_WIDTH;
    LOGF("%*s %s%s%s%s\n", pad > 0 ? pad : 0, "", text, why ? " (" : "", why ? why : "", why ? ")" : "");
}

void test_end(suite_t* s, bool pass) {
    s->run++;
    if (pass) s->pass++;
    verdict(pass ? "ok" : "FAIL", NULL);
}

void test_skip(suite_t* s, const char* why) {
    s->run++;
    verdict("skipped", why);
}
