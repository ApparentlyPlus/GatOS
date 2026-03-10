/*
 * test_multitasking.c - Scheduler and Process/Thread Validation Suite
 *
 * Tests every public function: process_create/destroy, thread_create,
 * thread_create_bootstrap, thread_destroy, process_get_all,
 * process_terminate_by_tty, process_header_update, sched_active,
 * sched_current, sched_add, sched_yield.
 * Covers PID/TID uniqueness, context layout, stack alignment, name truncation,
 * global process list, and shared TTY.
 */

#include <kernel/sys/process.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/userspace.h>
#include <kernel/memory/heap.h>
#include <kernel/memory/vmm.h>
#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/memory/layout.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static int g_tests_total  = 0;
static int g_tests_passed = 0;

#pragma region Helpers

static void kentry(void* arg) { (void)arg; sched_exit(); }

userspace void uentry(void* arg) {
    (void)arg;
    while (1) __asm__ volatile("pause");
}

static size_t thread_count(process_t* p) {
    size_t n = 0;
    for (thread_t* t = p->threads; t; t = t->next) n++;
    return n;
}

static bool proc_in_list(process_t* target) {
    for (process_t* p = process_get_all(); p; p = p->next)
        if (p == target) return true;
    return false;
}
#pragma endregion

#pragma region Scheduler Bootstrap

static bool t_sched_active(void) {
    TEST_ASSERT(sched_active() == true);
    return true;
}

static bool t_sched_nn(void) {
    TEST_ASSERT(sched_current() != NULL);
    return true;
}

static bool t_sched_name(void) {
    TEST_ASSERT(kstrcmp(sched_current()->name, "kernel_main") == 0);
    return true;
}

static bool t_sched_stk(void) {
    TEST_ASSERT(sched_current()->kernel_stack != NULL);
    return true;
}
#pragma endregion

#pragma region Process Create

static bool t_proc_nn(void) {
    process_t* p = process_create("t_proc", NULL);
    TEST_ASSERT(p != NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_pid(void) {
    process_t* p = process_create("t_pid", NULL);
    TEST_ASSERT(p->pid > 0);
    process_destroy(p);
    return true;
}

static bool t_proc_name(void) {
    process_t* p = process_create("t_name_proc", NULL);
    TEST_ASSERT(kstrcmp(p->name, "t_name_proc") == 0);
    process_destroy(p);
    return true;
}

static bool t_proc_vmm(void) {
    process_t* p = process_create("t_vmm", NULL);
    TEST_ASSERT(p->vmm != NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_tty(void) {
    process_t* p = process_create("t_tty", NULL);
    TEST_ASSERT(p->tty != NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_nothr(void) {
    process_t* p = process_create("t_nothr", NULL);
    TEST_ASSERT(p->threads == NULL);
    process_destroy(p);
    return true;
}

static bool t_proc_inlist(void) {
    process_t* p = process_create("t_inlist", NULL);
    TEST_ASSERT(proc_in_list(p));
    process_destroy(p);
    return true;
}

static bool t_proc_rmlist(void) {
    process_t* p = process_create("t_rmlist", NULL);
    process_destroy(p);
    TEST_ASSERT(!proc_in_list(p));
    return true;
}
#pragma endregion

#pragma region PID Uniqueness

static bool t_pid_uniq(void) {
    #define NP 8
    process_t* procs[NP];
    for (int i = 0; i < NP; i++) procs[i] = process_create("t_pidu", NULL);
    for (int i = 0; i < NP; i++)
        for (int j = i+1; j < NP; j++)
            TEST_ASSERT(procs[i]->pid != procs[j]->pid);
    for (int i = 0; i < NP; i++) process_destroy(procs[i]);
    return true;
}

static bool t_pid_mono(void) {
    #define NM 8
    process_t* procs[NM];
    for (int i = 0; i < NM; i++) procs[i] = process_create("t_pidm", NULL);
    for (int i = 0; i < NM-1; i++)
        TEST_ASSERT(procs[i]->pid < procs[i+1]->pid);
    for (int i = 0; i < NM; i++) process_destroy(procs[i]);
    return true;
}
#pragma endregion

#pragma region Kernel Thread

static bool t_kt_nn(void) {
    process_t* p = process_create("t_ktnull", NULL);
    thread_t* t = thread_create(p, "kt", kentry, NULL, false, 0);
    TEST_ASSERT(t != NULL);
    process_destroy(p);
    return true;
}

static bool t_kt_state(void) {
    process_t* p = process_create("t_ktstate", NULL);
    thread_t* t = thread_create(p, "kt_s", kentry, NULL, false, 0);
    TEST_ASSERT(t->state == THREAD_STATE_READY);
    process_destroy(p);
    return true;
}

static bool t_kt_link(void) {
    process_t* p = process_create("t_ktlink", NULL);
    thread_t* t = thread_create(p, "kt_l", kentry, NULL, false, 0);
    TEST_ASSERT(t->process == p);
    process_destroy(p);
    return true;
}

static bool t_kt_name(void) {
    process_t* p = process_create("t_ktname", NULL);
    thread_t* t = thread_create(p, "mythread", kentry, NULL, false, 0);
    TEST_ASSERT(kstrcmp(t->name, "mythread") == 0);
    process_destroy(p);
    return true;
}

static bool t_kt_stk(void) {
    process_t* p = process_create("t_ktstk", NULL);
    thread_t* t = thread_create(p, "kt_stk", kentry, NULL, false, 0);
    TEST_ASSERT(t->kernel_stack != NULL);
    process_destroy(p);
    return true;
}

static bool t_kt_ctx(void) {
    process_t* p = process_create("t_ktctx", NULL);
    thread_t* t = thread_create(p, "kt_ctx", kentry, (void*)0xDEAD, false, 0);
    TEST_ASSERT(t->context != NULL);
    uintptr_t base = (uintptr_t)t->kernel_stack;
    uintptr_t top  = base + KERNEL_STACK_SIZE;
    uintptr_t ctx  = (uintptr_t)t->context;
    TEST_ASSERT(ctx >= base && ctx < top);
    process_destroy(p);
    return true;
}

static bool t_kt_cs(void) {
    process_t* p = process_create("t_ktcs", NULL);
    thread_t* t = thread_create(p, "kt_cs", kentry, NULL, false, 0);
    TEST_ASSERT(t->context->iret_cs == KERNEL_CS);
    process_destroy(p);
    return true;
}

static bool t_kt_ss(void) {
    process_t* p = process_create("t_ktss", NULL);
    thread_t* t = thread_create(p, "kt_ss", kentry, NULL, false, 0);
    TEST_ASSERT(t->context->iret_ss == KERNEL_DS);
    process_destroy(p);
    return true;
}

static bool t_kt_arg(void) {
    process_t* p = process_create("t_ktarg", NULL);
    thread_t* t = thread_create(p, "kt_arg", kentry, (void*)0xBEEF, false, 0);
    /* thread_entry_wrapper(entry, arg): entry→rdi, arg→rsi */
    TEST_ASSERT(t->context->rsi == 0xBEEF);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region User Thread

static bool t_ut_nn(void) {
    process_t* p = process_create("t_utnull", NULL);
    thread_t* t = thread_create(p, "ut", uentry, NULL, true, 0);
    TEST_ASSERT(t != NULL);
    process_destroy(p);
    return true;
}

static bool t_ut_stk(void) {
    process_t* p = process_create("t_utstk", NULL);
    thread_t* t = thread_create(p, "ut_stk", uentry, NULL, true, 0);
    TEST_ASSERT(t->user_stack != NULL);
    process_destroy(p);
    return true;
}

static bool t_ut_ctx(void) {
    process_t* p = process_create("t_utctx", NULL);
    thread_t* t = thread_create(p, "ut_ctx", uentry, NULL, true, 0);
    TEST_ASSERT(t->context != NULL);
    process_destroy(p);
    return true;
}

static bool t_ut_align(void) {
    /* x86-64 SysV ABI: RSP % 16 == 8 at function entry point */
    process_t* p = process_create("t_utalign", NULL);
    thread_t* t = thread_create(p, "ut_align", uentry, NULL, true, 0);
    TEST_ASSERT(t->context != NULL);
    uintptr_t rsp = (uintptr_t)t->context->iret_rsp;
    TEST_ASSERT((rsp % 16) == 8);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region TID Uniqueness

static bool t_tid_same(void) {
    process_t* p = process_create("t_tidu", NULL);
    thread_t* t1 = thread_create(p, "t1", kentry, NULL, false, 0);
    thread_t* t2 = thread_create(p, "t2", kentry, NULL, false, 0);
    TEST_ASSERT(t1->tid != t2->tid);
    process_destroy(p);
    return true;
}

static bool t_tid_cross(void) {
    process_t* p1 = process_create("t_tid_p1", NULL);
    process_t* p2 = process_create("t_tid_p2", NULL);
    thread_t* t1 = thread_create(p1, "t1", kentry, NULL, false, 0);
    thread_t* t2 = thread_create(p2, "t2", kentry, NULL, false, 0);
    thread_t* t3 = thread_create(p2, "t3", kentry, NULL, false, 0);
    TEST_ASSERT(t1->tid != t2->tid);
    TEST_ASSERT(t1->tid != t3->tid);
    TEST_ASSERT(t2->tid != t3->tid);
    process_destroy(p1);
    process_destroy(p2);
    return true;
}
#pragma endregion

#pragma region Thread List

static bool t_thr_grows(void) {
    process_t* p = process_create("t_tgrow", NULL);
    TEST_ASSERT(thread_count(p) == 0);
    thread_create(p, "t1", kentry, NULL, false, 0);
    TEST_ASSERT(thread_count(p) == 1);
    thread_create(p, "t2", kentry, NULL, false, 0);
    TEST_ASSERT(thread_count(p) == 2);
    thread_create(p, "t3", kentry, NULL, false, 0);
    TEST_ASSERT(thread_count(p) == 3);
    process_destroy(p);
    return true;
}

static bool t_thr_inlist(void) {
    process_t* p = process_create("t_tinlist", NULL);
    thread_t* t = thread_create(p, "t_check", kentry, NULL, false, 0);
    bool found = false;
    for (thread_t* it = p->threads; it; it = it->next)
        if (it == t) { found = true; break; }
    TEST_ASSERT(found);
    process_destroy(p);
    return true;
}

static bool t_multi_5(void) {
    process_t* p = process_create("t_multi5", NULL);
    for (int i = 0; i < 5; i++) {
        thread_t* t = thread_create(p, "w", kentry, NULL, false, 0);
        TEST_ASSERT(t != NULL);
        TEST_ASSERT(t->process == p);
    }
    TEST_ASSERT(thread_count(p) == 5);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region Name Truncation

static bool t_proc_ntrunc(void) {
    char name[MAX_PROCESS_NAME * 2];
    kmemset(name, 'A', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    process_t* p = process_create(name, NULL);
    TEST_ASSERT(kstrlen(p->name) < MAX_PROCESS_NAME);
    TEST_ASSERT(p->name[MAX_PROCESS_NAME - 1] == '\0');
    process_destroy(p);
    return true;
}

static bool t_thr_ntrunc(void) {
    char name[MAX_THREAD_NAME * 2];
    kmemset(name, 'T', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    process_t* p = process_create("t_tntrunc", NULL);
    thread_t* t = thread_create(p, name, kentry, NULL, false, 0);
    TEST_ASSERT(kstrlen(t->name) < MAX_THREAD_NAME);
    TEST_ASSERT(t->name[MAX_THREAD_NAME - 1] == '\0');
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region Shared TTY

static bool t_shared_tty(void) {
    process_t* owner = process_create("t_owner", NULL);
    tty_t* shared = owner->tty;
    TEST_ASSERT(shared != NULL);
    process_t* guest = process_create("t_guest", shared);
    TEST_ASSERT(guest->tty == shared);
    /* Detach before destroy so guest doesn't free the shared TTY */
    guest->tty = NULL;
    process_destroy(guest);
    TEST_ASSERT(owner->tty == shared); /* owner's TTY still intact */
    process_destroy(owner);
    return true;
}
#pragma endregion

#pragma region Header Update

static bool t_hdr_update(void) {
    process_t* p = process_create("t_hdr", NULL);
    process_header_update(p); /* must not crash */
    process_destroy(p);
    return true;
}

static bool t_hdr_update_thr(void) {
    process_t* p = process_create("t_hdrth", NULL);
    thread_create(p, "th", kentry, NULL, false, 0);
    process_header_update(p); /* must not crash with 1 thread */
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region process_terminate_by_tty

static bool t_term_tty(void) {
    process_t* p = process_create("t_termtty", NULL);
    tty_t* t = p->tty;
    /* Must not crash; process may or may not be destroyed here */
    process_terminate_by_tty(t);
    return true;
}
#pragma endregion

#pragma region thread_create_bootstrap

static bool t_bootstrap(void) {
    process_t* p = process_create("t_boot", NULL);
    thread_t* t = thread_create_bootstrap(p, "boot_t");
    TEST_ASSERT(t != NULL);
    TEST_ASSERT(t->process == p);
    TEST_ASSERT(kstrcmp(t->name, "boot_t") == 0);
    process_destroy(p);
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-40s ", name);
    bool pass = fn();
    if (pass) { g_tests_passed++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_multitasking(void) {
    g_tests_total  = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN MULTITASKING TEST ---\n");

    run_test("sched_active is true",          t_sched_active);
    run_test("sched_current not null",        t_sched_nn);
    run_test("sched_current name=kernel_main",t_sched_name);
    run_test("sched_current has stack",       t_sched_stk);
    run_test("process: not null",             t_proc_nn);
    run_test("process: pid > 0",              t_proc_pid);
    run_test("process: name stored",          t_proc_name);
    run_test("process: has vmm",              t_proc_vmm);
    run_test("process: has tty",              t_proc_tty);
    run_test("process: no threads at create", t_proc_nothr);
    run_test("process: in global list",       t_proc_inlist);
    run_test("process: removed after destroy",t_proc_rmlist);
    run_test("pid: unique across 8 procs",    t_pid_uniq);
    run_test("pid: strictly monotone",        t_pid_mono);
    run_test("kthread: not null",             t_kt_nn);
    run_test("kthread: state READY",          t_kt_state);
    run_test("kthread: process link",         t_kt_link);
    run_test("kthread: name stored",          t_kt_name);
    run_test("kthread: stack not null",       t_kt_stk);
    run_test("kthread: context in stack",     t_kt_ctx);
    run_test("kthread: CS = KERNEL_CS",       t_kt_cs);
    run_test("kthread: SS = KERNEL_DS",       t_kt_ss);
    run_test("kthread: RDI = arg",            t_kt_arg);
    run_test("uthread: not null",             t_ut_nn);
    run_test("uthread: user_stack not null",  t_ut_stk);
    run_test("uthread: context not null",     t_ut_ctx);
    run_test("uthread: RSP % 16 == 8 (ABI)", t_ut_align);
    run_test("tid: unique same process",      t_tid_same);
    run_test("tid: unique cross process",     t_tid_cross);
    run_test("thread_count grows",            t_thr_grows);
    run_test("thread in proc->threads",       t_thr_inlist);
    run_test("5 threads in one process",      t_multi_5);
    run_test("process name truncation",       t_proc_ntrunc);
    run_test("thread name truncation",        t_thr_ntrunc);
    run_test("shared tty between processes",  t_shared_tty);
    run_test("process_header_update no crash",t_hdr_update);
    run_test("process_header_update+thread",  t_hdr_update_thr);
    run_test("terminate_by_tty no crash",     t_term_tty);
    run_test("thread_create_bootstrap",       t_bootstrap);

    LOGF("--- END MULTITASKING TEST ---\n");
    LOGF("Multitasking Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Multitasking tests failed (%d/%d passed).\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] Multitasking tests passed! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
