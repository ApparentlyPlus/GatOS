/*
 * uproc.c - Userspace thread implementations
 *
 * This entire translation unit is routed into the userspace sections
 * (.user_text / .user_rodata / .user_data / .user_bss) by the linker script
 * via the *uproc* filename pattern, exactly as ulibc is. No section attributes
 * are needed here, just plain C.
 * 
 * Author: u/ApparentlyPlus
 */

#include <ulibc/syscalls.h>
#include <ulibc/stdio.h>
#include <ulibc/math.h>

/*
 * demo_threadA - Prints sqrt(i) for i in [1, 10], then exits.
 */
void demo_threadA(void* arg) {
    (void)arg;
    for (int i = 1; i <= 10; i++) {
        printf("Hello from USERSPACE Thread A (sqrt(%d) = %lf)\n", i, sqrt(i));
        sys_sleep_ms(500);
    }
}

/*
 * demo_threadB - Iterates, then exercises mmap/munmap to demonstrate an intentional page fault on access after unmap.
 */
void demo_threadB(void* arg) {
    (void)arg;

    for (int i = 1; i <= 10; i++) {
        printf("Greetings from USERSPACE Thread B (iteration %d)\n", i);
        sys_sleep_ms(1000);
    }

    void* addr = sys_mmap(NULL, 4096, 1);
    if (addr == (void*)-1) {
        printf("Thread B: Failed to map memory!\n");
        return;
    }

    printf("Thread B: Mapped page at %p\n", addr);

    int* ptr = (int*)addr;
    *ptr = 1337;
    printf("Thread B: Wrote value %d to %p\n", *ptr, addr);

    sys_sleep_ms(1000);

    printf("Thread B: Unmapped page at %p. Attempting access (expecting Page Fault)...\n", addr);
    sys_munmap(addr);

    printf("Thread B: Value after unmap: %d (this shouldn't be printed!)\n", *ptr);

    printf("Press ALT+F4 to terminate this TTY session.\n");
}
