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
 * donut - Renders a spinning ASCII donut in the console using only syscalls
 */
void donut(void) {
    syscall_tty_ctrl(TTY_CTRL_CURSOR, 0);

    uint64_t dims = syscall_tty_ctrl(TTY_CTRL_GET_DIMS, 0);
    int screen_width  = (int)(dims & 0xFFFFFFFF);
    int screen_height = (int)(dims >> 32);

    if (screen_width  <= 0) screen_width  = 80;
    if (screen_height <= 0) screen_height = 24;

    int center_x = screen_width / 2;
    int center_y = screen_height / 2;
    int buffer_size = screen_width * screen_height;

    char* b = (char*)malloc(buffer_size);
    float* z = (float*)malloc(buffer_size * sizeof(float));
    char* out_buf = (char*)malloc(buffer_size + 3);

    if (!b || !z || !out_buf) return;

    const float SIN_DJ = sin(0.07f), COS_DJ = cos(0.07f);
    const float SIN_DI = sin(0.02f), COS_DI = cos(0.02f);
    const int J_STEPS = 90;
    const int I_STEPS = 314;

    static const char shading[12] = ".,-~:;=!*#$@";

    out_buf[0] = '\x1b';
    out_buf[1] = '[';
    out_buf[2] = 'H';

    float A = 0.0f, B = 0.0f;

    syscall_tty_ctrl(TTY_CTRL_CLEAR, 0);

    for (;;) {
        memset(b, 32, buffer_size);
        memset(z, 0,  buffer_size * sizeof(float));

        float sinA = sin(A), cosA = cos(A);
        float sinB = sin(B), cosB = cos(B);
        float cosA_cosB = cosA * cosB;
        float cosA_sinB = cosA * sinB;
        float cosA_cosB_plus_sinA = cosA_cosB + sinA;
        float sinj = 0.0f, cosj = 1.0f;

        for (int jj = 0; jj < J_STEPS; jj++) {
            float h = cosj + 2.0f;
            float h_sinA = h * sinA;
            float h_cosB = h * cosB;
            float h_sinB = h * sinB;
            float h_cosA_cosB = h * cosA_cosB;
            float h_cosA_sinB = h * cosA_sinB;
            float sinj_cosA = sinj * cosA;
            float sinj_sinA = sinj * sinA;
            float sinj_cosA_p5 = sinj_cosA + 5.0f;
            float sinj_sinA_sinB = sinj_sinA * sinB;
            float sinj_sinA_cosB = sinj_sinA * cosB;

            float n_outer = sinj_sinA_cosB - sinj_cosA;
            float cosj_ca = cosj * cosA_cosB_plus_sinA;
            float cosj_sinB = cosj * sinB;
            float sini = 0.0f, cosi = 1.0f;

            for (int ii = 0; ii < I_STEPS; ii++) {
                float D = 1.0f / (sini * h_sinA + sinj_cosA_p5);
                int x = center_x + (int)(30.0f * D * (cosi * h_cosB - sini * h_cosA_sinB + sinj_sinA_sinB));
                int y = center_y + (int)(15.0f * D * (cosi * h_sinB + sini * h_cosA_cosB - sinj_sinA_cosB));

                if ((unsigned)x < (unsigned)screen_width && (unsigned)y < (unsigned)screen_height) {
                    int o = x + screen_width * y;
                    if (D > z[o]) {
                        z[o] = D;
                        int N = (int)(8.0f * (n_outer - sini * cosj_ca - cosi * cosj_sinB));
                        if (N < 0)  N = 0;
                        if (N > 11) N = 11;
                        b[o] = shading[N];
                    }
                }

                float ns = sini * COS_DI + cosi * SIN_DI;
                float nc = cosi * COS_DI - sini * SIN_DI;
                sini = ns; cosi = nc;
            }

            float ns = sinj * COS_DJ + cosj * SIN_DJ;
            float nc = cosj * COS_DJ - sinj * SIN_DJ;
            sinj = ns; cosj = nc;
        }

        memcpy(out_buf + 3, b, buffer_size);
        syscall_write(out_buf, buffer_size + 3);

        A += 0.04f;
        B += 0.02f;
    }
}

/*
 * demo_threadA - Prints sqrt(i) for i in [1, 10], then exits
 */
void demo_threadA(void* arg) {
    (void)arg;
    for (int i = 1; i <= 10; i++) {
        printf("Hello from USERSPACE Thread A (sqrt(%d) = %lf)\n", i, sqrt(i));
        syscall_sleep(500);
    }
}

/*
 * demo_threadB - Iterates, then exercises mmap/munmap to demonstrate an intentional page fault on access after unmap
 */
void demo_threadB(void* arg) {
    (void)arg;

    for (int i = 1; i <= 10; i++) {
        printf("Greetings from USERSPACE Thread B (iteration %d)\n", i);
        syscall_sleep(1000);
    }

    printf("What's your name?\n> ");
    char name[100];
    scanf(" %99[^\n]", name);
    printf("> Nice to meet you, %s!\n\n", name);
    printf("Press ALT+F4 to terminate this TTY session.\n");
}

/*
 * donut_sim - Donut
 */
void donut_sim(void* arg) {
    (void)arg;
    donut();
}   