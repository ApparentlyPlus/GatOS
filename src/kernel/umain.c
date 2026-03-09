/*
 * umain.c - Userspace thread implementations and launch
 */

#include <ulibc/syscalls.h>
#include <ulibc/stdio.h>
#include <ulibc/math.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <kernel/sys/userspace.h>

/*
 * tA - Sample userspace thread: prints sqrt of i for i in [1,10].
 */
userspace void tA(void* arg) {
    (void)arg;
    for (int i = 1; i <= 10; i++) {
        printf(ustr("Hello from USERSPACE Thread A (sqrt(%d) = %lf)\n"), i, sqrt(i));
        sys_sleep_ms(500);
    }
}

/*
 * tB - Sample userspace thread: iterates, then exercises mmap/munmap to
 *      demonstrate intentional page-fault on access after unmap.
 */
userspace void tB(void* arg) {
    (void)arg;

    for (int i = 1; i <= 10; i++) {
        printf(ustr("Greetings from USERSPACE Thread B (iteration %d)\n"), i);
        sys_sleep_ms(1000);
    }

    void* addr = sys_mmap(NULL, 4096, 1);
    if (addr == (void*)-1) {
        printf(ustr("Thread B: Failed to map memory!\n"));
        return;
    }

    printf(ustr("Thread B: Mapped page at %p\n"), addr);

    int* ptr = (int*)addr;
    *ptr = 1337;
    printf(ustr("Thread B: Wrote value %d to %p\n"), *ptr, addr);

    sys_sleep_ms(1000);

    printf(ustr("Thread B: Unmapped page at %p. Now attempting access (expecting Page Fault)...\n"), addr);
    sys_munmap(addr);

    /* Intentional page fault — should never print the next line */
    printf(ustr("Thread B: Value after unmap: %d (this shouldn't be printed!)\n"), *ptr);

    printf(ustr("You can press ALT+F4 to terminate this tty session.\n"));
}

/*
 * uapps - Creates the demo process and threads. Called from kernel_main.
 */
void uapps(void) {
    process_t* proc = process_create("test_proc", NULL);
    sched_add(thread_create(proc, "thread_a", tA, NULL, true, 0));
    sched_add(thread_create(proc, "thread_b", tB, NULL, true, 0));
}
