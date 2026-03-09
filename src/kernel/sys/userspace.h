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

#include <arch/x86_64/memory/layout.h>

// Attributes to place code and data in Ring 3 mappings
#define userspace      __attribute__((section(".user_text")))
#define userspace_data __attribute__((section(".user_data")))
#define userspace_bss  __attribute__((section(".user_bss")))

/*
 * ustr - Embed a string literal in .user_rodata at the call site.
 */
#define ustr(s) \
    (__extension__ ({ \
        static const char _ustr[] \
            __attribute__((section(".user_rodata"), used)) = (s); \
        _ustr; \
    }))

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
