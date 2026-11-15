/*
 * paging.c - Page table management implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/multiboot2.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/sys/panic.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>

#define PAGE_1GB (1UL << 30)

typedef struct {
    uint64_t wc_start; // [wc_start, wc_end) is mapped write combining (framebuffer)
    uint64_t wc_end;
    uintptr_t tables_base;
    uint64_t total_PDs;
    uint64_t total_PDPTs;
    uint64_t total_kernel_PTs;
} physmap_t;

static physmap_t physmap = {0};

// True once pat_init has made PAT entry 1 (PWT) write combining
static bool patwc = false;

static kmem_t layout = {
    .kstart = (uint64_t)&KPHYS_START,
    .kend = (uint64_t)&KPHYS_END,
};

const kmem_t* const kmem = &layout;

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
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
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
 * unmap_identity - Drops the lower half identity map the boot tables set up
 */
void unmap_identity(void) {
    uint64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + PAGE_ENTRIES * PREALLOC_PML4s;
    PML4[0] = 0;
    PDPT[0] = 0;
    flush_tlb();
    LOGF("[PAGING] Removed identity mapping\n");
}

/*
 * kend_grow - Moves kend up by bytes (page aligned) and returns the old physical kend.
 * The new space must be usable RAM.
 */
uint64_t kend_grow(multiboot_parser_t* mb, uint64_t bytes) {
    uint64_t start = layout.kend;
    bytes = align_up(bytes, PAGE_SIZE);
    PANIC_ASSERT(multiboot_is_avail(mb, start, start + bytes));
    layout.kend += bytes;
    return start;
}

/*
 * reserve_tables - Sizes the physmap from the multiboot2 struct and reserves its page tables past kend
 */
uint64_t reserve_tables(multiboot_parser_t* multiboot) {
    uint64_t total_RAM = align_up(multiboot_total_ram(multiboot, MEASUREMENT_UNIT_BYTES), PAGE_SIZE);
    uint64_t map_end = total_RAM;

    // The framebuffer lives in the physmap too, so the console can reach it with PHYSMAP_P2V
    // WC only goes on 2MiB pages without usable RAM in them, RAM must never be write combining

    uint64_t wc_start = 0, wc_end = 0;
    multiboot_framebuffer_t* fb = multiboot_framebuffer(multiboot);

    if (fb) {
        uint64_t fb_end = fb->addr + (uint64_t)fb->height * fb->pitch;
        wc_start = align_down(fb->addr, PAGE_2MB);
        wc_end = align_up(fb_end, PAGE_2MB);
        if (multiboot_hits_avail(multiboot, wc_start, wc_start + PAGE_2MB)) wc_start += PAGE_2MB;
        if (wc_end > wc_start && multiboot_hits_avail(multiboot, wc_end - PAGE_2MB, wc_end)) wc_end -= PAGE_2MB;
        if (fb_end > map_end) map_end = fb_end;
    }
    map_end = align_up(map_end, PAGE_2MB);

    /*
     *  Author's Note:
     *  The physmap uses 2MiB pages, so it needs no PTs at all, except one. The first 2MiB
     *  get mapped with 4KiB pages, because firmware tends to give the first MiB a patchwork of
     *  memory types, and a huge page over mixed memory types is undefined behavior.
     */

    uint64_t total_PDs = CEIL_DIV(map_end, PAGE_1GB);
    uint64_t total_PDPTs = CEIL_DIV(total_PDs, PAGE_ENTRIES);
    PANIC_ASSERT(total_PDPTs < PML4_INDEX(KERNEL_VIRTUAL_BASE) - PML4_INDEX(PHYSMAP_VIRTUAL_BASE));

    // Low PT + PDs + PDPTs + PML4 + the kernel's own PDPT and PD
    uint64_t table_bytes = (1 + total_PDs + total_PDPTs + 1 + 2) * PAGE_SIZE;

    // The kernel range comes from the same pool (so the boot tables stay scratch), one PT per
    // 2MiB. The PTs are part of the range they map, so add one PT of slack for them.
    uint64_t kernel_PTs = CEIL_DIV(layout.kend + table_bytes, PAGE_2MB) + 1;
    PANIC_ASSERT(kernel_PTs <= PAGE_ENTRIES);
    table_bytes += kernel_PTs * PAGE_SIZE;

    layout.ram = total_RAM;
    layout.physmap_end = map_end;
    physmap.wc_start = wc_start;
    physmap.wc_end = wc_end;
    physmap.total_PDs = total_PDs;
    physmap.total_PDPTs = total_PDPTs;
    physmap.total_kernel_PTs = kernel_PTs;
    physmap.tables_base = KERNEL_P2V(layout.kend);

    // Tables are reached through the boot mapping, so they have to fit in its first 1GiB
    kend_grow(multiboot, table_bytes);
    PANIC_ASSERT(layout.kend < PAGE_1GB);

    LOGF("[PAGING] Reserved physmap tablespace (%lu KiB)\n", table_bytes / MEASUREMENT_UNIT_KB);
    return table_bytes;
}

/*
 * pat_init - Flip PAT entry 1 (PWT only) from write through to write combining so the framebuffer can be WC.
 * Entry 0 (WB) and 3 (UC) stay as they are
 */
static void pat_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 16))) return;

    uint64_t pat = read_msr(MSR_PAT);
    pat &= ~(0xFFULL << 8);
    pat |= (0x01ULL << 8);
    write_msr(MSR_PAT, pat);
    patwc = true;
}

/*
 * fb_cache_flags - WC (PWT selects PAT entry 1) when available, UC otherwise
 */
static inline uint64_t fb_cache_flags(void) {
    return patwc ? PAGE_PWT : (PAGE_PWT | PAGE_PCD);
}

/*
 * build_physmap - Map [0, physmap_end) at PHYSMAP_VIRTUAL_BASE (0xFFFF800000000000), so virt = phys + base
 */
void build_physmap() {
    if (layout.physmap_end == 0) {
        LOGF("[ERROR] No physmap has been built.\n");
        return;
    }

    pat_init();

    typedef uint64_t pte_t;
    typedef pte_t page_table_t[PAGE_ENTRIES];
    const uint64_t RW = PAGE_PRESENT | PAGE_WRITABLE;

    page_table_t* LOWPT = (page_table_t*)physmap.tables_base;
    page_table_t* PDs = LOWPT + 1;
    page_table_t* PDPTs = PDs + physmap.total_PDs;
    page_table_t* PML4 = PDPTs + physmap.total_PDPTs;
    page_table_t* KPDPT = PML4 + 1;
    page_table_t* KPD = KPDPT + 1;
    page_table_t* KPTs = KPD + 1;

    kmemset(LOWPT, 0, (uintptr_t)(KPTs + physmap.total_kernel_PTs) - (uintptr_t)LOWPT);

    // Physma is 2MiB pages, WC over the framebuffer, writeback everywhere else
    for (uint64_t pa = 0; pa < layout.physmap_end; pa += PAGE_2MB) {
        uint64_t cache = (pa >= physmap.wc_start && pa < physmap.wc_end) ? fb_cache_flags() : 0;
        PDs[pa / PAGE_1GB][PD_INDEX(pa)] = pa | RW | PAGE_HUGE | cache;
    }

    // ...except the first 2MiB, which get 4KiB pages
    for (uint64_t i = 0; i < PAGE_ENTRIES; i++)
        LOWPT[0][i] = (i * PAGE_SIZE) | RW;
    PDs[0][0] = KERNEL_V2P(LOWPT) | RW;

    for (uint64_t i = 0; i < physmap.total_PDs; i++)
        PDPTs[i / PAGE_ENTRIES][i % PAGE_ENTRIES] = KERNEL_V2P(&PDs[i]) | RW;

    // Map the kernel range from the pool so nothing depends on the boot tables
    for (uint64_t p = 0; p < layout.kend; p += PAGE_SIZE)
        KPTs[p / PAGE_2MB][PT_INDEX(p)] = p | RW;
    for (uint64_t i = 0; i < physmap.total_kernel_PTs; i++)
        KPD[0][i] = KERNEL_V2P(&KPTs[i]) | RW;
    KPDPT[0][PDPT_INDEX(KERNEL_VIRTUAL_BASE)] = KERNEL_V2P(KPD) | RW;

    // Build new PML4
    size_t physmap_index = PML4_INDEX(PHYSMAP_VIRTUAL_BASE);
    for (uint64_t i = 0; i < physmap.total_PDPTs; i++)
        PML4[0][physmap_index + i] = KERNEL_V2P(&PDPTs[i]) | RW;
    PML4[0][PML4_INDEX(KERNEL_VIRTUAL_BASE)] = KERNEL_V2P(KPDPT) | RW;

    PML4_switch(KERNEL_V2P(PML4));

    LOGF("[PAGING] Physmap: RAM 0x%lx, mapped up to 0x%lx\n", layout.ram, layout.physmap_end);
}

/*
 * dump_table - Logs the present entries of one table, then walks into the level below
 */
static void dump_table(uint64_t* table, int level) {
    static const char* names[] = { "PML4", "  PDPT", "    PD", "      PT" };

    for (int i = 0; i < PAGE_ENTRIES; i++) {
        uint64_t e = table[i];
        if (!(e & PAGE_PRESENT)) continue;

        bool leaf = level == 3 || (level > 0 && (e & PAGE_HUGE));
        LOGF("%s[%x]: 0x%lx%s\n", names[level], i, e, leaf ? "" : " ->");
        if (!leaf) dump_table((uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e)), level + 1);
    }
}

/*
 * QEMU_DUMP_PML4 - Walks the live page table and dumps its structure to the debug log
 * Needs the physmap. Prints every mapped page, so expect a wall of text
 */
void QEMU_DUMP_PML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    LOGF("Page Tables:\n");
    dump_table((uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(cr3)), 0);
}
