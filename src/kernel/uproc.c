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
    syscall_tty_ctrl(TTY_CTRL_CURSOR, 0); // Hide cursor for better aesthetics
    
    uint64_t dims = syscall_tty_ctrl(TTY_CTRL_GET_DIMS, 0);
    int screen_width = (int)(dims & 0xFFFFFFFF);
    int screen_height = (int)(dims >> 32);
    
    // Fallback just in case
    if (screen_width <= 0) screen_width = 80;
    if (screen_height <= 0) screen_height = 24;

    int center_x = screen_width / 2;
    int center_y = screen_height / 2;

    int buffer_size = screen_width * screen_height;
    char* b = (char*)malloc(buffer_size);
    float* z = (float*)malloc(buffer_size * sizeof(float));
    char* out_buf = (char*)malloc(buffer_size + 4); // +4 for \x1b[H

    if (!b || !z || !out_buf) return; // Silent fail if out of memory

    float A = 0, B = 0, i, j;

    syscall_tty_ctrl(TTY_CTRL_CLEAR, 0);

    for(;;) {
        memset(b, 32, buffer_size);
        memset(z, 0, buffer_size * sizeof(float));
        
        // Hoist frame-constant math
        float sinA = sin(A), cosA = cos(A);
        float sinB = sin(B), cosB = cos(B);

        for(j = 0; 6.28 > j; j += 0.07) {
            // Hoist outer-loop-constant math
            float sinj = sin(j), cosj = cos(j);
            float h = cosj + 2;
            
            for(i = 0; 6.28 > i; i += 0.02) {
                float sini = sin(i), cosi = cos(i);
                
                float D = 1 / (sini * h * sinA + sinj * cosA + 5);
                float t = sini * h * cosA - sinj * sinA;
                
                // Scale factors adjusted for terminal aspect ratio (approx 2:1 character height:width)
                int x = center_x + (int)(30 * D * (cosi * h * cosB - t * sinB));
                int y = center_y + (int)(15 * D * (cosi * h * sinB + t * cosB));
                
                if(y >= 0 && y < screen_height && x >= 0 && x < screen_width) {
                    int o = x + screen_width * y;
                    int N = 8 * ((sinj * sinA - sini * cosj * cosA) * cosB - sini * cosj * sinA - sinj * cosA - cosi * cosj * sinB);
                    if (D > z[o]) {
                        z[o] = D;
                        b[o] = ".,-~:;=!*#$@"[N > 0 ? N : 0];
                    }
                }
            }
        }
        
        out_buf[0] = '\x1b';
        out_buf[1] = '[';
        out_buf[2] = 'H';
        
        for(int k = 0; k < buffer_size; k++) {
            // Allow the console to naturally auto-wrap at width
            out_buf[3 + k] = b[k];
        }
        syscall_write(out_buf, buffer_size + 3);

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