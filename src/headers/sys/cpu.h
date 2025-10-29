/*
 * cpu.h - CPU Feature and Topology Information
 *
 * Provides detailed CPU information for GatOS, including vendor, brand,
 * feature detection, and core count. Designed for x86/x86_64 processors.
 * This is still primitive and will be expanded in future releases.
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Feature flags
typedef enum {
    CPU_FEAT_PAE       = (1 << 0),
    CPU_FEAT_NX        = (1 << 1),
    CPU_FEAT_SSE       = (1 << 2),
    CPU_FEAT_SSE2      = (1 << 3),
    CPU_FEAT_SSE3      = (1 << 4),
    CPU_FEAT_SSSE3     = (1 << 5),
    CPU_FEAT_SSE4_1    = (1 << 6),
    CPU_FEAT_SSE4_2    = (1 << 7),
    CPU_FEAT_AVX       = (1 << 8),
    CPU_FEAT_AVX2      = (1 << 9),
    CPU_FEAT_VMX       = (1 << 10),
    CPU_FEAT_SVM       = (1 << 11),
    CPU_FEAT_64BIT     = (1 << 12),
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
} CPUInfo;

// Public API
void cpu_init(void);
const CPUInfo* cpu_get_info(void);
bool cpu_has_feature(cpu_feature_t feature);
bool cpu_enable_feature(cpu_feature_t feature);
bool cpu_is_feature_enabled(cpu_feature_t feature);