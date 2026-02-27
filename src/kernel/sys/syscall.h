/*
 * syscall.h - Syscall interface definitions
 *
 * Defines the syscall numbers and initialization function.
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>

#define SYS_EXIT 1
#define SYS_WRITE 2

void syscall_init(void);
