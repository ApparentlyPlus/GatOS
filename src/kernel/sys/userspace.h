/*
 * userspace.h - Utilities for hardcoded userspace execution
 *
 * This file provides macros and definitions to facilitate the transition
 * from kernel execution to userspace (Ring 3).
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>

// Attribute to place functions in the .user_text section for Ring 3 mapping
#define userspace __attribute__((section(".user_text")))

// Linker symbols for the userspace code region
extern uint8_t USER_TEXT_START;
extern uint8_t USER_TEXT_END;

// Global entry point for userspace threads
userspace void userspace_start(void (*entry)(void*), void* arg);

// Fixed virtual address where userspace code will be mapped in the lower half
#define USER_CODE_VIRT_ADDR 0x400000
