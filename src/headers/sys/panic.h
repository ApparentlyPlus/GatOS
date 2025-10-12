/*
 * panic.h - Kernel panic handling
 *
 * Author: u/ApparentlyPlus
 */

#ifndef PANIC_H
#define PANIC_H

#include <sys/interrupts.h>

#define PANIC_ASSERT(condition) ((condition) ? (void)0 : panicf("Assertion failed in %s, line %d\n[!] Condition: %s", __FILE__, __LINE__, #condition))


void panic_c(const char* message, cpu_context_t* context);
void panic(const char* message);
void panicf(const char* fmt, ...);
void halt_system(void);

#endif