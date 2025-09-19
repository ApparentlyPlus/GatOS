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
#include <libc/string.h>
#include <print.h>
#include <serial.h>
#include <multiboot2.h>

typedef struct{
    uint64_t total_RAM;
    uint64_t total_pages;
    uintptr_t tables_base;
    uint64_t total_PTs;
    uint64_t total_PDs;
    uint64_t total_PDPTs;
    uint64_t total_PML4s;
} systemInfo;

static uint64_t KSTART = (uint64_t)&KPHYS_START;
static uint64_t KEND = (uint64_t)&KPHYS_END;

static systemInfo systemStruct = {0};

/*
 * align_up - Aligns address to specified boundary
 */
uintptr_t align_up(uintptr_t val, uintptr_t align) {
    return (val + align - 1) & ~(align - 1);
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
 * get_canonical_kend - Gets the kernel end as defined by the linker symbol
 */
uint64_t get_canonical_kend(bool virtual){
    uint64_t canonical = (uint64_t)(uintptr_t)&KPHYS_END;
    return virtual ? KERNEL_P2V(canonical) : canonical;
}

/*
 * get_kend - Gets the kernel start as defined by the linker symbol
 */
uint64_t get_canonical_kstart(bool virtual){
    uint64_t canonical = (uint64_t)(uintptr_t)&KPHYS_START;
    return virtual ? KERNEL_P2V(canonical) : canonical;
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
 * getPML4 - Retrieves current PML4 table address (virtual)
 */
static inline uint64_t* getPML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)KERNEL_P2V(cr3);
}

/*
 * unmap_identity - Removes lower memory identity mapping
 */
void unmap_identity(){
    int64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + 512 * PREALLOC_PML4s;
    PML4[0] = 0;
    PDPT[0] = 0;
    flush_tlb();
}

/*
 * cleanup_page_tables:
 * Removes unused page table entries, keeps only the given range
 * That range is both identity and higher half mapped.
 */
void cleanup_kernel_page_tables(uintptr_t start, uintptr_t end) {
    uint64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + 512 * PREALLOC_PML4s;
    uint64_t* PD = PDPT + 512 * PREALLOC_PDPTs;
    uint64_t* PT = PD + 512 * PREALLOC_PDs;

    uintptr_t kernel_size = end - start;
    if (kernel_size > (1UL << 30)) return; // > 1 GiB not allowed
    if ((start & 0xFFF) != 0 || (end & 0xFFF) != 0) return; // alignment check

    // Compute virtual addresses
    uintptr_t virt_start = start + KERNEL_VIRTUAL_BASE;
    uintptr_t virt_end   = end   + KERNEL_VIRTUAL_BASE;

    // Get page table indices
    size_t id_pml4 = (start >> 39) & 0x1FF;
    size_t id_pdpt = (start >> 30) & 0x1FF;
    size_t id_pd_start = (start >> 21) & 0x1FF;
    size_t id_pd_end   = ((end - 1) >> 21) & 0x1FF;

    size_t hh_pml4 = (virt_start >> 39) & 0x1FF;
    size_t hh_pdpt = (virt_start >> 30) & 0x1FF;
    size_t hh_pd_start = (virt_start >> 21) & 0x1FF;
    size_t hh_pd_end   = ((virt_end - 1) >> 21) & 0x1FF;

    uintptr_t start_page = start >> 12;
    uintptr_t end_page   = (end - 1) >> 12;
    size_t total_pages = end_page - start_page + 1;
    size_t total_pds = (id_pd_end > hh_pd_end ? id_pd_end : hh_pd_end) + 1;

    // Zero out all PML4 entries except the two we're using
    for (size_t i = 0; i < 512; i++) {
        if (i != id_pml4 && i != hh_pml4) {
            PML4[i] = 0;
        }
    }
    // Set our two PML4 entries
    PML4[id_pml4] = KERNEL_V2P(PDPT) | (PRESENT | WRITABLE);
    PML4[hh_pml4] = KERNEL_V2P(PDPT) | (PRESENT | WRITABLE);

    // Zero out all PDPT entries except the two we're using
    for (size_t i = 0; i < 512; i++) {
        if (i != id_pdpt && i != hh_pdpt) {
            PDPT[i] = 0;
        }
    }
    // Set our two PDPT entries
    PDPT[id_pdpt] = KERNEL_V2P(PD) | (PRESENT | WRITABLE);
    PDPT[hh_pdpt] = KERNEL_V2P(PD) | (PRESENT | WRITABLE);

    // Zero out all PD entries except the ones we're using
    for (size_t i = 0; i < 512; i++) {
        if (!((i >= id_pd_start && i <= id_pd_end) || (i >= hh_pd_start && i <= hh_pd_end))) {
            PD[i] = 0;
        }
    }
    // Set our PD entries
    for (size_t pd_index = id_pd_start; pd_index < total_pds; ++pd_index) {
        PD[pd_index] = KERNEL_V2P(PT + (pd_index << 9)) | (PRESENT | WRITABLE);
    }

    // Zero out all PT entries except the ones we're using
    for (size_t i = 0; i < (512 * total_pds); i++) {
        if (i >= total_pages) {
            PT[i] = 0;
        }
    }
    // Set our PT entries
    for (uintptr_t i = 0; i < total_pages; ++i) {
        uintptr_t phys = (start_page + i) << 12;
        PT[i] = phys | (PRESENT | WRITABLE);
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

    systemStruct.total_RAM = total_RAM;
    systemStruct.total_pages = total_pages;
    systemStruct.total_PTs = total_PTs;
    systemStruct.total_PDs = total_PDs;
    systemStruct.total_PDPTs = total_PDPTs;
    systemStruct.total_PML4s = total_PML4s;
    systemStruct.tables_base = (uintptr_t)get_kend(true);

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

    if(systemStruct.total_RAM == 0){
        print("[ERROR] No systemStruct has been built. The required tablespace has not been reserved.");
        return;
    }

    uintptr_t pt_base    = systemStruct.tables_base;
    uintptr_t pd_base    = pt_base + systemStruct.total_PTs * PAGE_SIZE;
    uintptr_t pdpt_base  = pd_base + systemStruct.total_PDs * PAGE_SIZE;

    // One brand new PML4 at the end
    uintptr_t pml4_base  = pdpt_base + systemStruct.total_PDPTs * PAGE_SIZE;

    typedef uint64_t pte_t;
    typedef pte_t page_table_t[PAGE_ENTRIES];

    page_table_t* PTs    = (page_table_t*)pt_base;
    page_table_t* PDs    = (page_table_t*)pd_base;
    page_table_t* PDPTs  = (page_table_t*)pdpt_base;
    page_table_t* PML4   = (page_table_t*)pml4_base;

    memset((void*)systemStruct.tables_base, 0, 
        (systemStruct.total_PTs
            +systemStruct.total_PDs
            +systemStruct.total_PDPTs
            +systemStruct.total_PML4s) * PAGE_SIZE);

    // Fill PTs with physical addresses
    uintptr_t phys_addr = 0;
    for (uint64_t pt_index = 0; pt_index < systemStruct.total_PTs; pt_index++) {
        for (int e = 0; e < PAGE_ENTRIES && phys_addr < systemStruct.total_RAM; e++) {
            PTs[pt_index][e] = phys_addr | (PRESENT | WRITABLE);
            phys_addr += PAGE_SIZE;
        }
    }

    // Fill PDs pointing to PTs
    uint64_t used_pt = 0;
    for (uint64_t i = 0; i < systemStruct.total_PDs; i++) {
        for (int e = 0; e < PAGE_ENTRIES && used_pt < systemStruct.total_PTs; e++) {
            PDs[i][e] = KERNEL_V2P(&PTs[used_pt]) | (PRESENT | WRITABLE);
            used_pt++;
        }
    }

    // Fill PDPTs pointing to PDs
    uint64_t used_pd = 0;
    for (uint64_t i = 0; i < systemStruct.total_PDPTs; i++) {
        for (int e = 0; e < PAGE_ENTRIES && used_pd < systemStruct.total_PDs; e++) {
            PDPTs[i][e] = KERNEL_V2P(&PDs[used_pd]) | (PRESENT | WRITABLE);
            used_pd++;
        }
    }

    // Clear the new PML4
    memset(PML4, 0, PAGE_SIZE);

    // copy kernel pml4 entry at its exact index
    uint64_t *old_pml4 = getPML4();
    size_t kernel_index = (KERNEL_VIRTUAL_BASE >> 39) & 0x1FF;
    PML4[0][kernel_index] = old_pml4[kernel_index];

    // place physmap
    size_t physmap_index = (PHYSMAP_VIRTUAL_BASE >> 39) & 0x1FF;
    PML4[0][physmap_index] = KERNEL_V2P(&PDPTs[0]) | (PRESENT | WRITABLE);

    // activate new PML4 (load CR3 with *physical* address)
    uintptr_t pml4_phys = KERNEL_V2P(pml4_base);
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys));

    // For good measure, flush TLB
    flush_tlb();
}

/*
 * dbg_dump_pmt - Debug function to print page table structure
 */
void dbg_dump_pmt(void) {
    uint64_t* PML4 = getPML4();
    serial_write("Page Tables:\n");

    for (int pml4_i = 0; pml4_i < PAGE_ENTRIES; pml4_i++) {
        uint64_t pml4e = PML4[pml4_i];
        if (!(pml4e & PRESENT)) continue;

        serial_write("PML4[");
        serial_write_hex16(pml4_i);
        serial_write("]: ");
        serial_write_hex32((uint32_t)pml4e); // Print only lower 32 bits
        serial_write(" -> PDPT\n");

        // Convert physical address to virtual
        uint64_t* pdpt = (uint64_t*)(KERNEL_P2V(pml4e & PAGE_MASK));

        for (int pdpt_i = 0; pdpt_i < PAGE_ENTRIES; pdpt_i++) {
            uint64_t pdpte = pdpt[pdpt_i];
            if (!(pdpte & PRESENT)) continue;

            serial_write("  PDPT[");
            serial_write_hex16(pdpt_i);
            serial_write("]: ");
            serial_write_hex32((uint32_t)pdpte); // Print only lower 32 bits
            serial_write(" -> PD\n");

            // Convert physical address to virtual
            uint64_t* pd = (uint64_t*)(KERNEL_P2V(pdpte & PAGE_MASK));

            for (int pd_i = 0; pd_i < PAGE_ENTRIES; pd_i++) {
                uint64_t pde = pd[pd_i];
                if (!(pde & PRESENT)) continue;

                serial_write("    PD[");
                serial_write_hex16(pd_i);
                serial_write("]: ");
                serial_write_hex32((uint32_t)pde); // Print only lower 32 bits
                serial_write(" -> PT\n");

                // Convert physical address to virtual
                uint64_t* pt = (uint64_t*)(KERNEL_P2V(pde & PAGE_MASK));

                for (int pt_i = 0; pt_i < PAGE_ENTRIES; pt_i++) {
                    uint64_t pte = pt[pt_i];
                    if (!(pte & PRESENT)) continue;

                    serial_write("      PT[");
                    serial_write_hex16(pt_i);
                    serial_write("]: ");
                    serial_write_hex32((uint32_t)pte); // Print only lower 32 bits
                    serial_write(" -> PHYS\n");
                }
            }
        }
    }
}
