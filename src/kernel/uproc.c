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
#include <stdbool.h>

/*
 * SieveOfEratosthenes - Implements the Sieve of Eratosthenes algorithm to find all prime numbers up to n
 */
void SieveOfEratosthenes(int n)
{
    // Allocate memory for prime array and initialize all
    // elements as true
    bool* prime = malloc((n + 1) * sizeof(bool));
    for (int i = 0; i <= n; i++)
        prime[i] = true;

    // 0 and 1 are not prime numbers
    prime[0] = prime[1] = false;

    // For each number from 2 to sqrt(n)
    for (int p = 2; p <= sqrt(n); p++) {
        // If p is prime
        if (prime[p]) {
            // Mark all multiples of p as non-prime
            for (int i = p * p; i <= n; i += p)
                prime[i] = false;
        }
    }

    // Print all prime numbers up to n
    printf("Prime numbers up to %d:\n", n);
    for (int p = 2; p <= n; p++) {
        if (prime[p])
            printf("%d ", p);
    }
    printf("\n");

    // Free allocated memory
    free(prime);
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
    printf("Nice to meet you, %s!\n", name);
    printf("Press ALT+F4 to terminate this TTY session.\n");
}

/*
 * demo2_threadA - Demonstrates the Sieve of Eratosthenes algorithm
 */
void demo2_threadA(void* arg) {
    (void)arg;
    SieveOfEratosthenes(1000);
}   