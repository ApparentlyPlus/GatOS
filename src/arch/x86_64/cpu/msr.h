/*
 * msr.h - Model Specific Register definitions for x86_64
 *
 * Author: ApparentlyPlus
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

// EFER bits
#define EFER_SCE         (1 << 0)
#define EFER_LME         (1 << 8)
#define EFER_LMA         (1 << 10)
#define EFER_NXE         (1 << 11)
