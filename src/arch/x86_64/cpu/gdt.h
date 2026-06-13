/*
 * gdt.h - Global Descriptor Table and TSS definitions
 *
 * This file defines the structures and constants for the x86_64 GDT
 * and Task State Segment (TSS), necessary for Ring 3 transitions.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GDT_ENTRIES 7 // Null, KCode, KData, UData, UCode, TSS (2 slots)

// Segment Selectors
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   (0x18 | 3)
#define USER_CS   (0x20 | 3)
#define TSS_SEL   0x28

// GDT Entry Access Flags
#define GDT_PRESENT      (1 << 7)
#define GDT_DPL_0        (0 << 5)
#define GDT_DPL_3        (3 << 5)
#define GDT_SYSTEM       (1 << 4)
#define GDT_EXECUTABLE   (1 << 3)
#define GDT_CONFORMING   (1 << 2)
#define GDT_READ_WRITE   (1 << 1)
#define GDT_ACCESSED     (1 << 0)

// GDT Flag Bits
#define GDT_GRAN_4K      (1 << 3)
#define GDT_LONG_MODE    (1 << 1)

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    gdt_entry_t entries[GDT_ENTRIES];
} __attribute__((packed)) gdt_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;


void gdt_init(void);
void tss_set_rsp0(uint64_t rsp);
