/*
 * cpu.h - CPU Feature and Topology Information
 *
 * Provides detailed CPU information for GatOS, including vendor, brand,
 * feature detection, and core count. Designed for x86/x86_64 processors.
 * This is still primitive and will be expanded in future releases.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Feature flags
typedef enum {
    CF_PAE       = (1 << 0),
    CF_NX        = (1 << 1),
    CF_SSE       = (1 << 2),
    CF_SSE2      = (1 << 3),
    CF_SSE3      = (1 << 4),
    CF_SSSE3     = (1 << 5),
    CF_SSE4_1    = (1 << 6),
    CF_SSE4_2    = (1 << 7),
    CF_AVX       = (1 << 8),
    CF_AVX2      = (1 << 9),
    CF_VMX       = (1 << 10),
    CF_SVM       = (1 << 11),
    CF_64BIT     = (1 << 12),
    CF_SMEP      = (1 << 13),
    CF_SMAP      = (1 << 14),
} cpu_feature_t;

// CPU Information Structure
typedef struct {
    char vendor[13];
    char brand[49];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t core_count;
    uint64_t features;
} cpu_info_t;

// CPU-local data structure for GS base
typedef struct {
    uint64_t kernel_stack; // offset 0
    uint64_t user_stack;   // offset 8
} __attribute__((packed)) cpu_local_t;

extern cpu_local_t cpu_local;

// Public API
void cpu_init(void);
const cpu_info_t* cpu_get_info(void);
bool cpu_has_feature(cpu_feature_t feature);
bool cpu_enable_feature(cpu_feature_t feature);
bool cpu_is_feature_enabled(cpu_feature_t feature);

// TSC
uint64_t tsc_read(void);
void tsc_deadline_arm(uint64_t target_tsc);

// Register access functions
void cpuid(uint32_t eax, uint32_t ecx, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);
uint64_t read_msr(uint32_t msr);
void write_msr(uint32_t msr, uint64_t value);
uint64_t read_cr0(void);
void write_cr0(uint64_t val);
uint64_t read_cr4(void);
void write_cr4(uint64_t val);
uint64_t read_xcr0(void);
void write_xcr0(uint64_t value);

/*
 * smap_allow - Allows access to supervisor mode pages
 */
static inline void smap_allow(void) {
    if (cpu_is_feature_enabled(CF_SMAP))
        __asm__ volatile("stac" ::: "memory");
}

/*
 * smap_deny - Denies access to supervisor mode pages
 */
static inline void smap_deny(void) {
    if (cpu_is_feature_enabled(CF_SMAP))
        __asm__ volatile("clac" ::: "memory");
}

/*
 * set_cr0_ts - Sets the Task Switched bit in CR0
 */
static inline void set_cr0_ts(void) {
    __asm__ volatile("mov %%cr0, %%rax; or $8, %%rax; mov %%rax, %%cr0" ::: "rax", "memory");
}