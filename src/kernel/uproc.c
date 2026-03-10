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
#include <ulibc/stdlib.h>
#include <ulibc/stdio.h>
#include <ulibc/math.h>
#include <ulibc/string.h>
#include <stdint.h>

/*
 * donut - Renders a spinning ASCII donut in the console using only syscalls.
 */
void donut(void) {
    syscall_tty_cursor(false); // Hide cursor for better aesthetics
    float A = 0, B = 0, i, j, z[1760];
    char b[1760];
    char out_buf[1764];

    syscall_write("\x1b[2J", 4);

    for(;;) {
        memset(b, 32, 1760);
        memset(z, 0, 7040);
        for(j = 0; 6.28 > j; j += 0.07) {
            for(i = 0; 6.28 > i; i += 0.02) {
                float c = sin(i), d = cos(j), e = sin(A), f = sin(j), g = cos(A), h = d + 2, D = 1 / (c * h * e + f * g + 5), l = cos(i), m = cos(B), n = sin(B), t = c * h * g - f * e;
                int x = 40 + 30 * D * (l * h * m - t * n), y = 12 + 15 * D * (l * h * n + t * m), o = x + 80 * y, N = 8 * ((f * e - c * d * g) * m - c * d * e - f * g - l * d * n);
                if(22 > y && y > 0 && x > 0 && 80 > x && D > z[o]) {
                    z[o] = D;
                    b[o] = ".,-~:;=!*#$@"[N > 0 ? N : 0];
                }
            }
        }
        
        out_buf[0] = '\x1b';
        out_buf[1] = '[';
        out_buf[2] = 'H';
        for(int k = 0; 1761 > k; k++) {
            out_buf[3 + k] = k % 80 ? b[k] : '\n';
        }
        syscall_write(out_buf, 1764);

        A += 0.04;
        B += 0.02;
    }
}

/*
 * demo_threadA - Prints sqrt(i) for i in [1, 10], then exits.
 */
void demo_threadA(void* arg) {
    (void)arg;
    for (int i = 1; i <= 10; i++) {
        printf("Hello from USERSPACE Thread A (sqrt(%d) = %lf)\n", i, sqrt(i));
        syscall_sleep(500);
    }
}

/*
 * demo_threadB - Iterates, then exercises mmap/munmap to demonstrate an intentional page fault on access after unmap.
 */
void demo_threadB(void* arg) {
    (void)arg;

    for (int i = 1; i <= 10; i++) {
        printf("Greetings from USERSPACE Thread B (iteration %d)\n", i);
        syscall_sleep(1000);
    }

    printf("What's your name?\n> ");
    char name[100];
    scanf("%[^\n]", name);
    printf("> Nice to meet you, %s!\n\n", name);
    printf("Press ALT+F4 to terminate this TTY session.\n");
}

/*
 * demo2_threadA - Demonstrates the Sieve of Eratosthenes algorithm
 */
void demo2_threadA(void* arg) {
    (void)arg;
    donut();
}   