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

/*
 * x86_64 Canonical Address Space Layout (48-bit addressing):
 * 
 * 0x0000000000000000 - 0x00007FFFFFFFFFFF : Lower half (user space)
 * 0x0000800000000000 - 0xFFFF7FFFFFFFFFFF : Non-canonical (causes #GP)
 * 0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF : Higher half (kernel space)
 * 
 * Kernel Virtual Memory Map:
 * 0xFFFF800000000000 - PHYSMAP_VIRTUAL_BASE : Physical memory map (physmap)
 * 0xFFFFFFFF80000000 - KERNEL_VIRTUAL_BASE  : Kernel code/data
 * 
 * The physmap allows direct access to all physical RAM via:
 *   virtual_addr = physical_addr + PHYSMAP_VIRTUAL_BASE
 */


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

#define PAGE_PRESENT        (1ULL << 0)
#define PAGE_WRITABLE       (1ULL << 1)
#define PAGE_USER           (1ULL << 2)
#define PAGE_PWT            (1ULL << 3)  // Page Write Through
#define PAGE_PCD            (1ULL << 4)  // Page Cache Disable
#define PAGE_ACCESSED       (1ULL << 5)
#define PAGE_DIRTY          (1ULL << 6)
#define PAGE_GLOBAL         (1ULL << 8)
#define PAGE_NO_EXECUTE     (1ULL << 63)

#define PAGE_SIZE           0x1000UL
#define PAGE_ENTRIES        512
#define FRAME_MASK          0xFFFFF000
#define ADDR_MASK           0x000FFFFFFFFFF000UL

#define PREALLOC_PML4s  1
#define PREALLOC_PDPTs  1
#define PREALLOC_PDs    1
#define PREALLOC_PTs    512


#define PT_ENTRY_MASK    0x1FF
#define PT_ENTRY_ADDR(entry) ((entry) & ADDR_MASK)

#define PML4_INDEX(addr) (((uintptr_t)(addr) >> 39) & PT_ENTRY_MASK)
#define PDPT_INDEX(addr) (((uintptr_t)(addr) >> 30) & PT_ENTRY_MASK)
#define PD_INDEX(addr)   (((uintptr_t)(addr) >> 21) & PT_ENTRY_MASK)
#define PT_INDEX(addr)   (((uintptr_t)(addr) >> 12) & PT_ENTRY_MASK)

#ifndef __ASSEMBLER__

#include <multiboot2.h>
#include <stdbool.h>

#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y)) // this is hacky and must be removed eventually

uintptr_t align_up(uintptr_t val, uintptr_t align);
uintptr_t align_down(uintptr_t val, uintptr_t align);

uint64_t get_kstart(bool virtual);
uint64_t get_kend(bool virtual);
uint64_t get_linker_kend(bool virtual);
uint64_t get_linker_kstart(bool virtual);
uint64_t get_physmap_start(void);
uint64_t get_physmap_end(void);

uint64_t reserve_required_tablespace(multiboot_parser_t* multiboot);

uint64_t* getPML4();

void flush_tlb(void);
void PMT_switch(uint64_t pml4);
void dbg_dump_pmt(void);

void unmap_identity();
void cleanup_kernel_page_tables(uintptr_t start, uintptr_t end);
void build_physmap();

typedef struct{
    uint64_t total_RAM;
    uint64_t total_pages;
    uintptr_t tables_base;
    uint64_t total_PTs;
    uint64_t total_PDs;
    uint64_t total_PDPTs;
    uint64_t total_PML4s;
} physmapInfo;

extern uintptr_t KPHYS_END;
extern uintptr_t KPHYS_START;

static uint64_t KSTART = (uint64_t)&KPHYS_START;
static uint64_t KEND = (uint64_t)&KPHYS_END;

static physmapInfo physmapStruct = {0};
#endif
