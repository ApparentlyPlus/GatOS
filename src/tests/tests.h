/*
 * tests.h - Header file for kernel functionality tests
 *
 * Author: Claude Code
 */

#pragma once

#include <kernel/debug.h>
#include <stdbool.h>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        LOGF("assertion failed: %s (line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

#define TEST_ASSERT_STATUS(s, e) do { \
    int got_ = (int)(s); \
    if (got_ != (int)(e)) { \
        LOGF("got status %d, expected %d (line %d)\n", got_, (int)(e), __LINE__); \
        return false; \
    } \
} while(0)

// One test suite's tally, see harness.c
typedef struct {
    const char* name;
    int run;
    int pass;
} suite_t;

void suite_begin(suite_t* s, const char* name);
void suite_end(suite_t* s);
void test_begin(const char* name);
void test_end(suite_t* s, bool pass);
void test_skip(suite_t* s, const char* why);

// For test builds, we should run kernel_test from kmain.c:

#ifdef TEST_BUILD
void kernel_test(void* mb_info, char* KERNEL_VERSION);
#endif

// Function declarations for individual test suites

void test_boot();
void test_pmm();
void test_vmm();
void test_slab();
void test_heap();
void test_timers();
void test_spinlock();
void test_tty();
void test_input();
void test_multitasking();