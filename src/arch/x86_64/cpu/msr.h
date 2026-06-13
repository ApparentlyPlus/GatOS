/*
 * msr.h - Model Specific Register definitions for x86_64
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#define MSR_EFER         0xC0000080
#define MSR_STAR         0xC0000081
#define MSR_LSTAR        0xC0000082
#define MSR_CSTAR        0xC0000083
#define MSR_FMASK        0xC0000084

#define MSR_FS_BASE      0xC0000100
#define MSR_GS_BASE      0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

#define MSR_APIC_BASE    0x0000001B

// TSC Deadline
#define MSR_IA32_TSC_DEADLINE   0x6E0

// Intel RAPL energy counters
#define MSR_RAPL_POWER_UNIT     0x606
#define MSR_PKG_ENERGY_STATUS   0x611
#define MSR_PP0_ENERGY_STATUS   0x639

// AMD RAPL energy counters
#define MSR_AMD_ENERGY_UNIT     0xC0010299
#define MSR_AMD_PKG_ENERGY      0xC001029B

// EFER bits
#define EFER_SCE         (1 << 0)
#define EFER_LME         (1 << 8)
#define EFER_LMA         (1 << 10)
#define EFER_NXE         (1 << 11)
