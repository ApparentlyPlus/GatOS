/*
 * umain.c - User-space main function and thread implementations
 */

#include <ulibc/syscalls.h>
#include <ulibc/stdio.h>
#include <ulibc/math.h>
#include <kernel/sys/scheduler.h>
#include <kernel/sys/process.h>
#include <kernel/sys/userspace.h>

/*
 * tA - Sample thread function A for showcase
 */
userspace_rodata const char fmtA[] = "Hello from USERSPACE Thread A (sqrt(%d) = %lf)\n";
userspace void tA(void* arg) {
    (void)arg;
    for (int i = 1; i <= 10; i++) {
		printf(fmtA, i, sqrt(i));
		sys_sleep_ms(500);
	}
}

/*
 * tB - Sample thread function B for showcase
 */
userspace_rodata const char fmtB[] = "Greetings from USERSPACE Thread B (iteration %d)\n";
userspace_rodata const char fmtB2[] = "You can press ALT+F4 to terminate this tty session and see how the kernel handles it!\n";
userspace void tB(void* arg) {
	(void)arg;
	for (int i = 1; i <= 10; i++) {
		printf(fmtB, i);
		sys_sleep_ms(1000);
	}
	printf(fmtB2);
}

/*
 * uapps - Entry point for the userspace applications
 */
void uapps(){
    // Create test threads
    // Use NULL for test_proc to create its own TTY
    process_t* test_proc = process_create("test_proc", NULL);
    sched_add(thread_create(test_proc, "thread_a", tA, NULL, true, 0));
	sched_add(thread_create(test_proc, "thread_b", tB, NULL, true, 0));
}