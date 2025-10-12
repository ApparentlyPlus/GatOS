/*
 * paging.c - Page table management implementation
 *
 * Handles higher-half memory mapping, identity mapping removal,
 * and page table cleanup for kernel memory space.
 *
 * Author: u/ApparentlyPlus
 */

#include <stdbool.h>
#include <memory/paging.h>
#include <sys/panic.h>
#include <libc/string.h>
#include <vga_stdio.h>
#include <serial.h>
#include <multiboot2.h>

/*
 * align_up - Aligns address to specified boundary
 */
uintptr_t align_up(uintptr_t val, uintptr_t align) {
    return (val + align - 1) & ~(align - 1);
}

/*
 * align_down - Aligns address down to specified boundary
 */
uintptr_t align_down(uintptr_t val, uintptr_t align) {
    return val & ~(align - 1);
}

/*
 * get_kstart - Gets the (current) kernel start
 */
uint64_t get_kstart(bool virtual){
    return virtual ? KERNEL_P2V(KSTART) : KSTART;
}

/*
 * get_kend - Gets the (current) kernel end
 */
uint64_t get_kend(bool virtual){
    return virtual ? KERNEL_P2V(KEND) : KEND;
}

/*
 * get_linker_kend - Gets the kernel end as defined by the linker symbol
 */
uint64_t get_linker_kend(bool virtual){
    uint64_t linker = (uint64_t)(uintptr_t)&KPHYS_END;
    return virtual ? KERNEL_P2V(linker) : linker;
}

/*
 * get_linker_kstart - Gets the kernel start as defined by the linker symbol
 */
uint64_t get_linker_kstart(bool virtual){
    uint64_t linker = (uint64_t)(uintptr_t)&KPHYS_START;
    return virtual ? KERNEL_P2V(linker) : linker;
}

/*
 * get_physmap_start - Gets the start address of the physmap region (virtual)
 */
uint64_t get_physmap_start(void) {
    return PHYSMAP_VIRTUAL_BASE;
}

/*
 * get_physmap_end - Gets the end address of the physmap region (virtual)
 */
uint64_t get_physmap_end(void) {
    return PHYSMAP_VIRTUAL_BASE + physmapStruct.total_RAM;
}

/*
 * flush_tlb - Invalidates TLB cache
 */
void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

/*
 * PMT_switch - Switch to a page table
 */
void PMT_switch(uint64_t pml4) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4));
}

/*
 * getPML4 - Retrieves current PML4 table address (virtual)
 */
uint64_t* getPML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)KERNEL_P2V(cr3);
}

/*
 * unmap_identity - Removes lower memory identity mapping
 */
void unmap_identity(){
    int64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + PAGE_ENTRIES * PREALLOC_PML4s;
    PML4[0] = 0;
    PDPT[0] = 0;
    flush_tlb();
}

/*
 * cleanup_page_tables:
 * Removes unused page table entries, keeps only the given range in higher half
 */
void cleanup_kernel_page_tables(uintptr_t start, uintptr_t end) {
    uint64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + PAGE_ENTRIES * PREALLOC_PML4s;
    uint64_t* PD = PDPT + PAGE_ENTRIES * PREALLOC_PDPTs;
    uint64_t* PT = PD + PAGE_ENTRIES * PREALLOC_PDs;

    uintptr_t kernel_size = end - start;
    if (kernel_size > (1UL << 30)) return; // > 1 GiB not allowed
    if ((start & (PAGE_SIZE - 1)) != 0 || (end & (PAGE_SIZE - 1)) != 0) return; // alignment check

    // Compute virtual addresses (higher half only)
    uintptr_t virt_start = start + KERNEL_VIRTUAL_BASE;
    uintptr_t virt_end   = end   + KERNEL_VIRTUAL_BASE;

    // Get page table indices for higher half mapping only
    size_t hh_pml4 = PML4_INDEX(virt_start);
    size_t hh_pdpt = PDPT_INDEX(virt_start);
    size_t hh_pd_start = PD_INDEX(virt_start);
    size_t hh_pd_end   = PD_INDEX(virt_end - 1);

    uintptr_t start_page = start >> 12;
    uintptr_t end_page   = (end - 1) >> 12;
    size_t total_pages = end_page - start_page + 1;
    size_t total_pds = hh_pd_end + 1;

    // Zero out all PML4 entries except the higher half one we're using
    for (size_t i = 0; i < PAGE_ENTRIES; i++) {
        if (i != hh_pml4) {
            PML4[i] = 0;
        }
    }
    // Set only the higher half PML4 entry
    PML4[hh_pml4] = KERNEL_V2P(PDPT) | (PAGE_PRESENT | PAGE_WRITABLE);

    // Zero out all PDPT entries except the higher half one we're using
    for (size_t i = 0; i < PAGE_ENTRIES; i++) {
        if (i != hh_pdpt) {
            PDPT[i] = 0;
        }
    }
    // Set only the higher half PDPT entry
    PDPT[hh_pdpt] = KERNEL_V2P(PD) | (PAGE_PRESENT | PAGE_WRITABLE);

    // Zero out all PD entries except the higher half ones we're using
    for (size_t i = 0; i < PAGE_ENTRIES; i++) {
        if (!(i >= hh_pd_start && i <= hh_pd_end)) {
            PD[i] = 0;
        }
    }
    // Set only the higher half PD entries
    for (size_t pd_index = hh_pd_start; pd_index <= hh_pd_end; ++pd_index) {
        PD[pd_index] = KERNEL_V2P(PT + ((pd_index - hh_pd_start) << 9)) | (PAGE_PRESENT | PAGE_WRITABLE);
    }

    // Zero out all PT entries except the ones we're using for higher half
    for (size_t i = 0; i < (PAGE_ENTRIES * (hh_pd_end - hh_pd_start + 1)); i++) {
        if (i >= total_pages) {
            PT[i] = 0;
        }
    }
    // Set only the higher half PT entries
    for (uintptr_t i = 0; i < total_pages; ++i) {
        uintptr_t phys = (start_page + i) << 12;
        PT[i] = phys | (PAGE_PRESENT | PAGE_WRITABLE);
    }

    flush_tlb();
}

/*
 * reserve_required_tablespace - This function utilizes the multiboot2
 * struct to find out the RAM size of the machine, then reserves enough
 * memory for page tables to map all of it to virtual memory.
 * Returns the size needed for the page tables, in bytes.
 */
uint64_t reserve_required_tablespace(multiboot_parser_t* multiboot) {
    uint64_t total_RAM = align_up(multiboot_get_total_RAM(multiboot, MEASUREMENT_UNIT_BYTES), PAGE_SIZE);
    uint64_t total_pages = total_RAM / PAGE_SIZE;

    uint64_t total_PTs    = CEIL_DIV(total_pages, PAGE_ENTRIES);
    uint64_t total_PDs    = CEIL_DIV(total_PTs, PAGE_ENTRIES);
    uint64_t total_PDPTs  = CEIL_DIV(total_PDs, PAGE_ENTRIES);
    uint64_t total_PML4s  = CEIL_DIV(total_PDPTs, PAGE_ENTRIES);

    uint64_t table_bytes = (total_PTs + total_PDs + total_PDPTs + total_PML4s) * 4 * MEASUREMENT_UNIT_KB;
    table_bytes = align_up(table_bytes, PAGE_SIZE); //align to 4kb

    physmapStruct.total_RAM = total_RAM;
    physmapStruct.total_pages = total_pages;
    physmapStruct.total_PTs = total_PTs;
    physmapStruct.total_PDs = total_PDs;
    physmapStruct.total_PDPTs = total_PDPTs;
    physmapStruct.total_PML4s = total_PML4s;
    physmapStruct.tables_base = (uintptr_t)get_kend(true);

    KEND += table_bytes;

    return table_bytes;
}

/*
 * build_physmap - This function creates a mapping of all physical RAM into a reserved
 * region of the virtual address space (the physmap). This allows
 * the kernel to access any physical memory through a simple offset calculation.
 *
 * The mapping is created at PHYSMAP_VIRTUAL_BASE (0xFFFF800000000000), providing
 * a window where virtual address = physical_address + PHYSMAP_VIRTUAL_BASE.
 */
void build_physmap() {

    if(physmapStruct.total_RAM == 0){
        printf("[ERROR] No physmapStruct has been built. The required tablespace has not been reserved.");
        return;
    }

    uintptr_t pt_base    = physmapStruct.tables_base;
    uintptr_t pd_base    = pt_base + physmapStruct.total_PTs * PAGE_SIZE;
    uintptr_t pdpt_base  = pd_base + physmapStruct.total_PDs * PAGE_SIZE;

    // One brand new PML4 at the end
    uintptr_t pml4_base  = pdpt_base + physmapStruct.total_PDPTs * PAGE_SIZE;

    typedef uint64_t pte_t;
    typedef pte_t page_table_t[PAGE_ENTRIES];

    page_table_t* PTs    = (page_table_t*)pt_base;
    page_table_t* PDs    = (page_table_t*)pd_base;
    page_table_t* PDPTs  = (page_table_t*)pdpt_base;
    page_table_t* PML4   = (page_table_t*)pml4_base;

    memset((void*)physmapStruct.tables_base, 0, 
        (physmapStruct.total_PTs
            +physmapStruct.total_PDs
            +physmapStruct.total_PDPTs
            +physmapStruct.total_PML4s) * PAGE_SIZE);

    // Fill PTs with physical addresses
    uintptr_t phys_addr = 0;
    for (uint64_t pt_index = 0; pt_index < physmapStruct.total_PTs; pt_index++) {
        for (int e = 0; e < PAGE_ENTRIES && phys_addr < physmapStruct.total_RAM; e++) {
            PTs[pt_index][e] = phys_addr | (PAGE_PRESENT | PAGE_WRITABLE);
            phys_addr += PAGE_SIZE;
        }
    }

    // Fill PDs pointing to PTs
    uint64_t used_pt = 0;
    for (uint64_t i = 0; i < physmapStruct.total_PDs; i++) {
        for (int e = 0; e < PAGE_ENTRIES && used_pt < physmapStruct.total_PTs; e++) {
            PDs[i][e] = KERNEL_V2P(&PTs[used_pt]) | (PAGE_PRESENT | PAGE_WRITABLE);
            used_pt++;
        }
    }

    // Fill PDPTs pointing to PDs
    uint64_t used_pd = 0;
    for (uint64_t i = 0; i < physmapStruct.total_PDPTs; i++) {
        for (int e = 0; e < PAGE_ENTRIES && used_pd < physmapStruct.total_PDs; e++) {
            PDPTs[i][e] = KERNEL_V2P(&PDs[used_pd]) | (PAGE_PRESENT | PAGE_WRITABLE);
            used_pd++;
        }
    }

    // Clear the new PML4
    memset(PML4, 0, PAGE_SIZE);

    // copy kernel pml4 entry at its exact index
    uint64_t *old_pml4 = getPML4();
    size_t kernel_index = PML4_INDEX(KERNEL_VIRTUAL_BASE);
    PML4[0][kernel_index] = old_pml4[kernel_index];

    // place physmap
    size_t physmap_index = PML4_INDEX(PHYSMAP_VIRTUAL_BASE);

    PANIC_ASSERT(kernel_index != physmap_index);
    
    PML4[0][physmap_index] = KERNEL_V2P(&PDPTs[0]) | (PAGE_PRESENT | PAGE_WRITABLE);

    // activate new PML4 (load CR3 with *physical* address)
    uintptr_t pml4_phys = KERNEL_V2P(pml4_base);
    PMT_switch(pml4_phys);

    // For good measure, flush TLB
    flush_tlb();
}