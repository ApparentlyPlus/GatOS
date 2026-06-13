/*
 * userspace.h - Utilities for hardcoded userspace execution
 *
 * This file provides macros and definitions to facilitate the transition
 * from kernel execution to userspace (Ring 3).
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>

#include <arch/x86_64/memory/layout.h>


// Per-symbol section attributes

// Use these only when a single symbol in an otherwise
// kernel-linked file must live in a userspace section (eg. userspace_start in process.c).
// For normal userspace code, put it in uproc.c and initialize it in umain.c
#define userspace        __attribute__((section(".user_text")))
#define userspace_rodata __attribute__((section(".user_rodata")))
#define userspace_data   __attribute__((section(".user_data")))
#define userspace_bss    __attribute__((section(".user_bss")))

// Linker symbols for the userspace regions
extern uint8_t USER_TEXT_START;
extern uint8_t USER_TEXT_END;
extern uint8_t USER_TEXT_LOAD_ADDR;

extern uint8_t USER_RODATA_START;
extern uint8_t USER_RODATA_END;
extern uint8_t USER_RODATA_LOAD_ADDR;

extern uint8_t USER_DATA_START;
extern uint8_t USER_DATA_END;
extern uint8_t USER_DATA_LOAD_ADDR;

extern uint8_t USER_BSS_START;
extern uint8_t USER_BSS_END;
extern uint8_t USER_BSS_LOAD_ADDR;

// Global entry point for userspace threads
userspace void userspace_start(void (*entry)(void*), void* arg);
