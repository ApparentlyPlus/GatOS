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
 * SieveOfEratosthenes - Segmented bit-packed odd-only Sieve of Eratosthenes
 */
void SieveOfEratosthenes(int n)
{
    #define SEG_BYTES (32u * 1024u)
    #define SEG_BITS (SEG_BYTES * 8u)
    #define BIT_SET(arr, i) ((arr)[(unsigned)(i) >> 3] |= (uint8_t)(1u << ((unsigned)(i) & 7u)))
    #define BIT_GET(arr, i) ((arr)[(unsigned)(i) >> 3] & (uint8_t)(1u << ((unsigned)(i) & 7u)))

    if (n < 2) return;
    printf("Prime numbers up to %d:\n", n);
    if (n >= 2) printf("2 ");
    if (n < 3) { printf("\n"); return; }
    int sqrtn = (int)sqrt((double)n);
    while ((long long)(sqrtn + 1) * (sqrtn + 1) <= (long long)n) sqrtn++;
    while ((long long)sqrtn * sqrtn > (long long)n) sqrtn--;

    unsigned small_count = (unsigned)(sqrtn >= 3 ? (sqrtn - 3) / 2 + 1 : 0);
    unsigned small_bytes  = (small_count + 7) / 8;

    uint8_t* small = (uint8_t*)calloc(small_bytes + 1, 1);
    if (!small) { printf("(out of memory)\n"); return; }

    for (unsigned pi = 0; pi < small_count; pi++) {
        if (BIT_GET(small, pi)) continue;
        int p = 2 * (int)pi + 3;
        long long pp = (long long)p * p;
        if (pp > sqrtn) break;
        for (long long m = pp; m <= sqrtn; m += 2 * p) {
            unsigned mi = (unsigned)(m - 3) / 2;
            BIT_SET(small, mi);
        }
    }
    unsigned sp_cap = small_count + 8;
    int* sp = (int*)malloc(sp_cap * sizeof(int));
    if (!sp) { free(small); printf("(out of memory)\n"); return; }
    unsigned sp_cnt = 0;
    for (unsigned pi = 0; pi < small_count; pi++) {
        if (!BIT_GET(small, pi))
            sp[sp_cnt++] = 2 * (int)pi + 3;
    }
    free(small);

    uint8_t* seg = (uint8_t*)malloc(SEG_BYTES);
    if (!seg) { free(sp); printf("(out of memory)\n"); return; }

    for (long long seg_low = 3; seg_low <= (long long)n; seg_low += 2 * SEG_BITS) {
        long long seg_high = seg_low + 2 * SEG_BITS - 2;
        if (seg_high > (long long)n) seg_high = (long long)n | 1;
        unsigned seg_cnt = (unsigned)((seg_high - seg_low) / 2) + 1;
        memset(seg, 0, (seg_cnt + 7) / 8);
        for (unsigned si = 0; si < sp_cnt; si++) {
            long long p = sp[si];
            long long start = ((seg_low + p - 1) / p) * p;
            if (start == p) start = p * p;
            if ((start & 1) == 0) start += p;

            for (long long m = start; m <= seg_high; m += 2 * p) {
                unsigned bit = (unsigned)((m - seg_low) / 2);
                BIT_SET(seg, bit);
            }
        }
        for (unsigned bi = 0; bi < seg_cnt; bi++) {
            if (!BIT_GET(seg, bi)) {
                long long prime = seg_low + 2LL * bi;
                if (prime <= (long long)n)
                    printf("%lld ", prime);
            }
        }
    }

    free(seg);
    free(sp);
    printf("\n");
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
    scanf("%s", name);
    printf("> Nice to meet you, %s!\n\n", name);
    printf("Press ALT+F4 to terminate this TTY session.\n");
}

/*
 * demo2_threadA - Demonstrates the Sieve of Eratosthenes algorithm
 */
void demo2_threadA(void* arg) {
    (void)arg;
    SieveOfEratosthenes(1000);
}   