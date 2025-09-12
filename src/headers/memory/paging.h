/*
 * paging.h - Page table management definitions
 *
 * Defines constants and macros for x86_64 paging structures,
 * including virtual-to-physical address translation utilities.
 *
 * Author: u/ApparentlyPlus
 */

#define HH_BASE 0xFFFFFFFF80000000

#ifdef __ASSEMBLER__
#define V2P(a) ((a) - HH_BASE)
#define P2V(a) ((a) + HH_BASE)

#else
#include <stdint.h>
#define V2P(a) ((uintptr_t)(a) & ~HH_BASE)
#define P2V(a) ((uintptr_t)(a) | HH_BASE)
#endif

#define PRESENT   0x1
#define WRITABLE  0x2
#define USER      0x4
#define ADDR_MASK 0x000FFFFFFFFFF000UL
#define PAGE_SIZE 0x1000UL
#define PAGE_ENTRIES 512
#define PAGE_MASK 0xFFFFF000

#define PREALLOC_PML4s 1
#define PREALLOC_PDPTs 1
#define PREALLOC_PDs 1
#define PREALLOC_PTs 512


#ifndef __ASSEMBLER__

#include "multiboot2.h"
#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y)) //this is hacky and must be removed eventually

uintptr_t align_up(uintptr_t val, uintptr_t align);
uint64_t get_kstart();
uint64_t get_kend();
uint64_t get_canonical_kend();
uint64_t get_canonical_kstart();
uint64_t reserve_required_tablespace(multiboot_parser_t* multiboot);


void flush_tlb(void);
void dbg_dump_pmt(void);
void unmap_identity();
void cleanup_page_tables(uintptr_t start, uintptr_t end);

extern uintptr_t KPHYS_END;
extern uintptr_t KPHYS_START;
#endif
