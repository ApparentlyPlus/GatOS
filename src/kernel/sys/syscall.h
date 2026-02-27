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
#define SYS_MMAP 3
#define SYS_MUNMAP 4
#define SYS_SET_FS_BASE 5

void syscall_init(void);
