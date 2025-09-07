/*
 * paging.c - Page table management implementation
 *
 * Handles higher-half memory mapping, identity mapping removal,
 * and page table cleanup for kernel memory space.
 *
 * Author: u/ApparentlyPlus
 */

#include "memory/paging.h"
#include "print.h"
#include "serial.h"

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
    return (uint64_t*)P2V(cr3);
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
void cleanup_page_tables(uintptr_t start, uintptr_t end) {
    uint64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + 512 * PREALLOC_PML4s;
    uint64_t* PD = PDPT + 512 * PREALLOC_PDPTs;
    uint64_t* PT = PD + 512 * PREALLOC_PDs;

    uintptr_t kernel_size = end - start;
    if (kernel_size > (1UL << 30)) return; // > 1 GiB not allowed
    if ((start & 0xFFF) != 0 || (end & 0xFFF) != 0) return; // alignment check

    // Compute virtual addresses
    uintptr_t virt_start = start + HH_BASE;
    uintptr_t virt_end   = end   + HH_BASE;

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
    PML4[id_pml4] = V2P(PDPT) | (PRESENT | WRITABLE);
    PML4[hh_pml4] = V2P(PDPT) | (PRESENT | WRITABLE);

    // Zero out all PDPT entries except the two we're using
    for (size_t i = 0; i < 512; i++) {
        if (i != id_pdpt && i != hh_pdpt) {
            PDPT[i] = 0;
        }
    }
    // Set our two PDPT entries
    PDPT[id_pdpt] = V2P(PD) | (PRESENT | WRITABLE);
    PDPT[hh_pdpt] = V2P(PD) | (PRESENT | WRITABLE);

    // Zero out all PD entries except the ones we're using
    for (size_t i = 0; i < 512; i++) {
        if (!((i >= id_pd_start && i <= id_pd_end) || (i >= hh_pd_start && i <= hh_pd_end))) {
            PD[i] = 0;
        }
    }
    // Set our PD entries
    for (size_t pd_index = id_pd_start; pd_index < total_pds; ++pd_index) {
        PD[pd_index] = V2P(PT + (pd_index << 9)) | (PRESENT | WRITABLE);
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
        uint64_t* pdpt = (uint64_t*)(P2V(pml4e & PAGE_MASK));

        for (int pdpt_i = 0; pdpt_i < PAGE_ENTRIES; pdpt_i++) {
            uint64_t pdpte = pdpt[pdpt_i];
            if (!(pdpte & PRESENT)) continue;

            serial_write("  PDPT[");
            serial_write_hex16(pdpt_i);
            serial_write("]: ");
            serial_write_hex32((uint32_t)pdpte); // Print only lower 32 bits
            serial_write(" -> PD\n");

            // Convert physical address to virtual
            uint64_t* pd = (uint64_t*)(P2V(pdpte & PAGE_MASK));

            for (int pd_i = 0; pd_i < PAGE_ENTRIES; pd_i++) {
                uint64_t pde = pd[pd_i];
                if (!(pde & PRESENT)) continue;

                serial_write("    PD[");
                serial_write_hex16(pd_i);
                serial_write("]: ");
                serial_write_hex32((uint32_t)pde); // Print only lower 32 bits
                serial_write(" -> PT\n");

                // Convert physical address to virtual
                uint64_t* pt = (uint64_t*)(P2V(pde & PAGE_MASK));

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
