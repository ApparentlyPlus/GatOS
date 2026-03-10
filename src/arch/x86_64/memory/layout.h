/*
 * layout.h - Memory layout definitions
 *
 * Centralized memory addresses and sizes for both kernel and userspace.
 *
 * Author: ApparentlyPlus
 */

#pragma once

// Kernel space addresses
#define KERNEL_VIRTUAL_BASE  0xFFFFFFFF80000000
#define PHYSMAP_VIRTUAL_BASE 0xFFFF800000000000

// Userspace addresses
#define USER_CODE_VIRT_ADDR  0x400000

// Stacks
#define KERNEL_STACK_SIZE    16384 // 16 KiB
#define USER_STACK_SIZE      65536 // 64 KiB