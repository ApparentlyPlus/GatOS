/*
 * paging.c - Page table management implementation
 *
 * Handles higher-half memory mapping, identity mapping removal,
 * and page table cleanup for kernel memory space.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/multiboot2.h>
#include <kernel/drivers/serial.h>
#include <kernel/sys/panic.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>

/* 
 * This is a (self proclaimed) genius hack to statically reserve a single 2MB page for framebuffer purposes in the physmap,
 * which is used by the console driver to output everything without relying on the memory systems to be online.
 * 
 * Covers up to 512 * 2MB = 1GB, enough for any display at any resolution
 * Only 4 KB in BSS, nuts
 */
static uint64_t fb_pd[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

uint64_t KSTART = (uint64_t)&KPHYS_START;
uint64_t KEND = (uint64_t)&KPHYS_END;

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
    return PHYSMAP_VIRTUAL_BASE + physmap.total_RAM;
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
 * PML4_switch - Switch to a page table
 */
void PML4_switch(uint64_t pml4) {
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
    LOGF("[PAGING] Removed identity mapping\n");
}

/*
 * cleanup_kpt - Removes unused page table entries, keeps only the given range in higher half
 */
void cleanup_kpt(uintptr_t start, uintptr_t end) {
    uint64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + PAGE_ENTRIES * PREALLOC_PML4s;
    uint64_t* PD = PDPT + PAGE_ENTRIES * PREALLOC_PDPTs;
    uint64_t* PT = PD + PAGE_ENTRIES * PREALLOC_PDs;

    uintptr_t kernel_size = end - start;
    if (kernel_size > (1UL << 30)) return; // > 1 GiB not allowed
    if ((start & (PAGE_SIZE - 1)) != 0 || (end & (PAGE_SIZE - 1)) != 0) return; // alignment check

    // Compute virtual addresses (higher half only)
    uintptr_t virt_start = start + KERNEL_VIRTUAL_BASE;
    uintptr_t virt_end = end + KERNEL_VIRTUAL_BASE;

    // Get page table indices for higher half mapping only
    size_t hh_pml4 = PML4_INDEX(virt_start);
    size_t hh_pdpt = PDPT_INDEX(virt_start);
    size_t hh_pd_start = PD_INDEX(virt_start);
    size_t hh_pd_end = PD_INDEX(virt_end - 1);

    uintptr_t start_page = start >> 12;
    uintptr_t end_page = (end - 1) >> 12;
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

    LOGF("[PAGING] Cleaned up kernel page tabeles (only 0x%lx - 0x%lx remains)\n", virt_start, virt_end);
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

    // Table space sized for RAM only
    uint64_t total_pages = total_RAM / PAGE_SIZE;
    uint64_t total_PTs = CEIL_DIV(total_pages, PAGE_ENTRIES);
    uint64_t total_PDs = CEIL_DIV(total_PTs, PAGE_ENTRIES);
    uint64_t total_PDPTs = CEIL_DIV(total_PDs, PAGE_ENTRIES);
    uint64_t total_PML4s = CEIL_DIV(total_PDPTs, PAGE_ENTRIES);

    uint64_t table_bytes = (total_PTs + total_PDs + total_PDPTs + total_PML4s) * 4 * MEASUREMENT_UNIT_KB;
    table_bytes = align_up(table_bytes, PAGE_SIZE);

    PANIC_ASSERT(KEND + table_bytes < (1UL << 30) && KEND + table_bytes < total_RAM);

    // Store fb info for build_physmap, don't inflate table_bytes
    uint64_t fb_phys = 0, fb_size = 0;
    multiboot_framebuffer_t* fb = multiboot_get_framebuffer(multiboot);
    if (fb) { fb_phys = fb->addr; fb_size = (uint64_t)fb->height * fb->pitch; }

    physmap.total_RAM = total_RAM;
    physmap.fb_phys = fb_phys;
    physmap.fb_size = fb_size;
    physmap.total_pages = total_pages;
    physmap.total_PTs = total_PTs;
    physmap.total_PDs = total_PDs;
    physmap.total_PDPTs = total_PDPTs;
    physmap.total_PML4s = total_PML4s;
    physmap.tables_base = (uintptr_t)get_kend(true);

    KEND += table_bytes;

    LOGF("[PAGING] Reserved physmap tablespace (%d MiB)\n", table_bytes / MEASUREMENT_UNIT_MB);
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
    if (physmap.total_RAM == 0) {
        LOGF("[ERROR] No physmap has been built.\n");
        return;
    }

    uintptr_t pt_base = physmap.tables_base;
    uintptr_t pd_base = pt_base + physmap.total_PTs * PAGE_SIZE;
    uintptr_t pdpt_base = pd_base + physmap.total_PDs * PAGE_SIZE;
    uintptr_t pml4_base = pdpt_base + physmap.total_PDPTs * PAGE_SIZE;

    typedef uint64_t pte_t;
    typedef pte_t page_table_t[PAGE_ENTRIES];
    page_table_t* PTs = (page_table_t*)pt_base;
    page_table_t* PDs = (page_table_t*)pd_base;
    page_table_t* PDPTs = (page_table_t*)pdpt_base;
    page_table_t* PML4 = (page_table_t*)pml4_base;

    // Zero all tables
    kmemset((void*)physmap.tables_base, 0,
        (physmap.total_PTs + physmap.total_PDs +
         physmap.total_PDPTs + physmap.total_PML4s) * PAGE_SIZE);

    // Map RAM (writeback)
    uint64_t pa = 0;
    while (pa < physmap.total_RAM) {
        uint64_t pti = (pa >> 12) / PAGE_ENTRIES;
        uint64_t pte = (pa >> 12) % PAGE_ENTRIES;
        PTs[pti][pte] = pa | (PAGE_PRESENT | PAGE_WRITABLE);
        pa += PAGE_SIZE;
    }

    // Map framebuffer MMIO via static BSS PD using 2MB huge pages
    // Each PD entry covers 2MB, fb_pd alone (4 KB) handles any resolution.
    if (physmap.fb_phys && physmap.fb_phys >= physmap.total_RAM) {
        uint64_t fb_end = physmap.fb_phys + physmap.fb_size;
        uint64_t pdpt_s = (physmap.fb_phys >> 30) & 0x1FF;
        uint64_t pdpt_e = ((fb_end - 1) >> 30) & 0x1FF;
        if (pdpt_s == pdpt_e) {
            kmemset(fb_pd, 0, sizeof(fb_pd));
            uint64_t base2m = physmap.fb_phys & ~(uint64_t)(0x1FFFFF);
            uint64_t end2m = (fb_end + 0x1FFFFF) & ~(uint64_t)(0x1FFFFF);
            for (uint64_t pa2m = base2m; pa2m < end2m; pa2m += 0x200000)
                fb_pd[(pa2m >> 21) & 0x1FF] =
                    pa2m | (PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE | PAGE_PWT | PAGE_PCD);
            PDPTs[0][pdpt_s] = KERNEL_V2P(fb_pd) | (PAGE_PRESENT | PAGE_WRITABLE);
        }
    } else if (physmap.fb_phys && physmap.fb_phys < physmap.total_RAM) {
        // if fb within RAM, fix cache attrs in existing 4KB PTs
        uint64_t fb_end = physmap.fb_phys + physmap.fb_size;
        for (pa = physmap.fb_phys; pa < fb_end && pa < physmap.total_RAM; pa += PAGE_SIZE) {
            uint64_t pti = (pa >> 12) / PAGE_ENTRIES;
            uint64_t pte = (pa >> 12) % PAGE_ENTRIES;
            PTs[pti][pte] = pa | (PAGE_PRESENT | PAGE_WRITABLE | PAGE_PWT | PAGE_PCD);
        }
    }

    // Wire PDs to PTs
    uint64_t used_pt = 0;
    for (uint64_t i = 0; i < physmap.total_PDs; i++)
        for (int e = 0; e < PAGE_ENTRIES && used_pt < physmap.total_PTs; e++)
            PDs[i][e] = KERNEL_V2P(&PTs[used_pt++]) | (PAGE_PRESENT | PAGE_WRITABLE);

    // Wire PDPTs to PDs
    uint64_t used_pd = 0;
    for (uint64_t i = 0; i < physmap.total_PDPTs; i++)
        for (int e = 0; e < PAGE_ENTRIES && used_pd < physmap.total_PDs; e++)
            PDPTs[i][e] = KERNEL_V2P(&PDs[used_pd++]) | (PAGE_PRESENT | PAGE_WRITABLE);

    // Build new PML4
    kmemset(PML4, 0, PAGE_SIZE);
    uint64_t* old_pml4 = getPML4();
    size_t kernel_index = PML4_INDEX(KERNEL_VIRTUAL_BASE);
    size_t physmap_index = PML4_INDEX(PHYSMAP_VIRTUAL_BASE);
    PANIC_ASSERT(kernel_index != physmap_index);
    PML4[0][kernel_index] = old_pml4[kernel_index];
    PML4[0][physmap_index] = KERNEL_V2P(&PDPTs[0]) | (PAGE_PRESENT | PAGE_WRITABLE);

    PML4_switch(KERNEL_V2P(pml4_base));
    flush_tlb();

    LOGF("[PAGING] Physmap: RAM 0x%lx\n", physmap.total_RAM);
}

/*
 * QEMU_DUMP_PML4 - Walks the live page table and dumps its structure to serial
 * Note: Largely unused now that the kernel is stable, but can be useful for debugging early boot paging issues in QEMU
 */
void QEMU_DUMP_PML4(void) {
    uint64_t* PML4 = getPML4();
    serial_write("Page Tables:\n");

    for (int pml4_i = 0; pml4_i < PAGE_ENTRIES; pml4_i++) {
        uint64_t pml4e = PML4[pml4_i];
        if (!(pml4e & PAGE_PRESENT)) continue;

        serial_write("PML4[");
        serial_write_hex16(pml4_i);
        serial_write("]: ");
        serial_write_hex32((uint32_t)pml4e);
        serial_write(" -> PDPT\n");

        uint64_t* pdpt = (uint64_t*)(KERNEL_P2V(pml4e & FRAME_MASK));

        for (int pdpt_i = 0; pdpt_i < PAGE_ENTRIES; pdpt_i++) {
            uint64_t pdpte = pdpt[pdpt_i];
            if (!(pdpte & PAGE_PRESENT)) continue;

            serial_write("  PDPT[");
            serial_write_hex16(pdpt_i);
            serial_write("]: ");
            serial_write_hex32((uint32_t)pdpte);
            serial_write(" -> PD\n");

            uint64_t* pd = (uint64_t*)(KERNEL_P2V(pdpte & FRAME_MASK));

            for (int pd_i = 0; pd_i < PAGE_ENTRIES; pd_i++) {
                uint64_t pde = pd[pd_i];
                if (!(pde & PAGE_PRESENT)) continue;

                serial_write("    PD[");
                serial_write_hex16(pd_i);
                serial_write("]: ");
                serial_write_hex32((uint32_t)pde);
                serial_write(" -> PT\n");

                uint64_t* pt = (uint64_t*)(KERNEL_P2V(pde & FRAME_MASK));

                for (int pt_i = 0; pt_i < PAGE_ENTRIES; pt_i++) {
                    uint64_t pte = pt[pt_i];
                    if (!(pte & PAGE_PRESENT)) continue;

                    serial_write("      PT[");
                    serial_write_hex16(pt_i);
                    serial_write("]: ");
                    serial_write_hex32((uint32_t)pte);
                    serial_write(" -> PHYS\n");
                }
            }
        }
    }
}