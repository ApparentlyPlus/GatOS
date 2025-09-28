/*
 * paging.h - Page table management definitions
 *
 * Defines constants and macros for x86_64 paging structures,
 * including virtual-to-physical address translation utilities.
 *
 * Author: u/ApparentlyPlus
 */

// This is the base address of all the kernel code. Kernel execution happens here.
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000

// This is the base address of the physmap (mapping of the entirety of RAM into virtual space)
#define PHYSMAP_VIRTUAL_BASE 0xFFFF800000000000

#ifdef __ASSEMBLER__

#define KERNEL_V2P(a) ((a) - KERNEL_VIRTUAL_BASE)
#define KERNEL_P2V(a) ((a) + KERNEL_VIRTUAL_BASE)

#define PHYSMAP_V2P(a) ((a) - PHYSMAP_VIRTUAL_BASE)
#define PHYSMAP_P2V(a) ((a) + PHYSMAP_VIRTUAL_BASE)

#else

#include <stdint.h>
#define KERNEL_V2P(a) ((uintptr_t)(a) & ~KERNEL_VIRTUAL_BASE)
#define KERNEL_P2V(a) ((uintptr_t)(a) | KERNEL_VIRTUAL_BASE)

#define PHYSMAP_V2P(a) ((uintptr_t)(a) & ~PHYSMAP_VIRTUAL_BASE)
#define PHYSMAP_P2V(a) ((uintptr_t)(a) | PHYSMAP_VIRTUAL_BASE)

#endif

#define PRESENT         (1ULL << 0)
#define WRITABLE        (1ULL << 1)
#define USER            (1ULL << 2)
#define NO_EXECUTE      (1ULL << 63)
#define ADDR_MASK       0x000FFFFFFFFFF000UL
#define PAGE_SIZE       0x1000UL
#define PAGE_ENTRIES    512
#define PAGE_MASK       0xFFFFF000

#define PREALLOC_PML4s  1
#define PREALLOC_PDPTs  1
#define PREALLOC_PDs    1
#define PREALLOC_PTs    512


#ifndef __ASSEMBLER__

#include <multiboot2.h>
#include <stdbool.h>

#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y)) // this is hacky and must be removed eventually

uintptr_t align_up(uintptr_t val, uintptr_t align);
uint64_t get_kstart(bool virtual);
uint64_t get_kend(bool virtual);
uint64_t get_linker_kend(bool virtual);
uint64_t get_linker_kstart(bool virtual);
uint64_t reserve_required_tablespace(multiboot_parser_t* multiboot);

uint64_t* getPML4();

void flush_tlb(void);
void dbg_dump_pmt(void);
void unmap_identity();
void cleanup_kernel_page_tables(uintptr_t start, uintptr_t end);
void build_physmap();

extern uintptr_t KPHYS_END;
extern uintptr_t KPHYS_START;
#endif
