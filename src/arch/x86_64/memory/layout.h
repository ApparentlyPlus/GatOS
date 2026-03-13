/*
 * layout.h - Memory layout definitions
 *
 * Centralized memory addresses, sizes, and primitive memory utilities
 * shared by all kernel subsystems. Included by assembler and C code.
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

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stdbool.h>

/*
 * Linker-defined kernel physical extent symbols.
 * Their addresses (not values) are the kernel start/end physical addresses.
 */
extern uintptr_t KPHYS_START;
extern uintptr_t KPHYS_END;

/*
 * Runtime kernel extent, tracked in paging.c.
 * KEND is bumped by reserve_required_tablespace to include page table space.
 * All callers must use get_kend() to read the authoritative value.
 */
extern uint64_t KSTART;
extern uint64_t KEND;

/* Alignment utilities — power-of-two align only */
static inline uintptr_t align_up(uintptr_t val, uintptr_t align) {
    return (val + align - 1) & ~(align - 1);
}

static inline uintptr_t align_down(uintptr_t val, uintptr_t align) {
    return val & ~(align - 1);
}

/* Kernel extent accessors */
static inline uint64_t get_kstart(bool virtual) {
    return virtual ? (KSTART | KERNEL_VIRTUAL_BASE) : KSTART;
}

static inline uint64_t get_kend(bool virtual) {
    return virtual ? (KEND | KERNEL_VIRTUAL_BASE) : KEND;
}

/* Linker-symbol extent (addresses of KPHYS_* symbols, never modified at runtime) */
static inline uint64_t get_linker_kstart(bool virtual) {
    uint64_t l = (uint64_t)(uintptr_t)&KPHYS_START;
    return virtual ? (l | KERNEL_VIRTUAL_BASE) : l;
}

static inline uint64_t get_linker_kend(bool virtual) {
    uint64_t l = (uint64_t)(uintptr_t)&KPHYS_END;
    return virtual ? (l | KERNEL_VIRTUAL_BASE) : l;
}

#endif /* __ASSEMBLER__ */