/*
 * tests.h - Header file for kernel functionality tests. Contains common
 * macros and function declarations for test suites.
 */

#pragma once

#include <kernel/debug.h>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        LOGF("[FAIL] Assertion failed: %s (Line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

#define TEST_ASSERT_STATUS(s, e) do { \
    if ((s) != (e)) { \
        LOGF("[FAIL] Status mismatch: Got %d, Expected %d (Line %d)\n", s, e, __LINE__); \
        return false; \
    } \
} while(0)

// Function declarations for individual test suites

void test_pmm();
void test_vmm();
void test_slab();
void test_heap();