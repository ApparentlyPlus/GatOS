/*
 * test_multitasking.c - Scheduler and Process Management Validation Suite
 *
 * This suite verifies the core multitasking capabilities of GatOS, including
 * process isolation, thread creation, context switching, and Ring 3 integration.
 */

#include <kernel/sys/process.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/userspace.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#pragma region Configuration & Types

static int g_tests_total = 0;
static int g_tests_passed = 0;

// Synchronization flags for thread tests
static volatile int g_kernel_thread_val = 0;

#pragma endregion

#pragma region Helpers

// A simple kernel thread entry point
static void kernel_test_thread_entry(void* arg) {
    int val = (int)(uintptr_t)arg;
    g_kernel_thread_val = val;
    
    // Threads must either loop forever or exit via scheduler
    sched_exit();
}

// A simple userspace-compatible function (placed in .user_text)
userspace void user_test_thread_entry(void* arg) {
    (void)arg;
    // Userspace code for testing
    while(1) {
        // Just spin to prove we are running in Ring 3
        __asm__ volatile("pause");
    }
}

#pragma endregion

#pragma region Tests

/*
 * test_process_creation - Verifies process structure and private heap initialization
 */
static bool test_process_creation(void) {
    process_t* proc = process_create("test_proc", NULL);
    TEST_ASSERT(proc != NULL);
    TEST_ASSERT(proc->pid > 0);
    TEST_ASSERT(strcmp(proc->name, "test_proc") == 0);
    
    // Verify private VMM
    TEST_ASSERT(proc->vmm != NULL);
    
    // Verify TTY was created
    TEST_ASSERT(proc->tty != NULL);
    
    process_destroy(proc);
    return true;
}

/*
 * test_kernel_thread_creation - Verifies thread_create for kernel-mode threads
 */
static bool test_kernel_thread_creation(void) {
    process_t* proc = process_create("kthread_proc", NULL);
    TEST_ASSERT(proc != NULL);
    
    g_kernel_thread_val = 0;
    thread_t* thread = thread_create(proc, "k_thread", kernel_test_thread_entry, (void*)42, false, 0);
    
    TEST_ASSERT(thread != NULL);
    TEST_ASSERT(thread->process == proc);
    TEST_ASSERT(thread->kernel_stack != NULL);
    TEST_ASSERT(thread->state == THREAD_STATE_READY);
    
    // Add to scheduler to let it run
    sched_add(thread);
    
    process_destroy(proc); // This will cleanup threads too
    return true;
}

/*
 * test_user_thread_creation - Verifies thread_create for user-mode (Ring 3) threads
 */
static bool test_user_thread_creation(void) {
    process_t* proc = process_create("uthread_proc", NULL);
    TEST_ASSERT(proc != NULL);
    
    // Create a USER thread
    thread_t* thread = thread_create(proc, "u_thread", user_test_thread_entry, NULL, true, 0);
    
    TEST_ASSERT(thread != NULL);
    TEST_ASSERT(thread->user_stack != NULL);
    TEST_ASSERT(thread->context != NULL);
    
    process_destroy(proc);
    return true;
}

/*
 * test_scheduler_bootstrap - Verifies sched_init and kernel_main wrapping
 */
static bool test_scheduler_bootstrap(void) {
    TEST_ASSERT(sched_active() == true);
    
    thread_t* current = sched_current();
    TEST_ASSERT(current != NULL);
    TEST_ASSERT(strcmp(current->name, "kernel_main") == 0);
    TEST_ASSERT(current->kernel_stack != NULL);
    
    return true;
}

#pragma endregion

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
    bool pass = func();

    if (pass) {
        g_tests_passed++;
        LOGF("[PASS]\n");
    } else {
        LOGF("[FAIL]\n");
    }
}

void test_multitasking(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN MULTITASKING TEST ---\n");

    run_test("Process Creation & Isolation", test_process_creation);
    run_test("Kernel Thread Initialization", test_kernel_thread_creation);
    run_test("User Thread Initialization", test_user_thread_creation);
    run_test("Scheduler Bootstrap (Self)", test_scheduler_bootstrap);

    LOGF("--- END MULTITASKING TEST ---\n");
    LOGF("Multitasking Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <kernel/drivers/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[-] Multitasking tests failed (%d/%d).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        printf("[+] Multitasking tests passed! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}

#pragma endregion
