/*
 * gdt.c - Global Descriptor Table and Task State Segment implementation
 *
 * This file implements the management of the GDT and TSS in C, assuming control
 * from the early assembly-based GDT defined in the bootloader. 
 *
 * Taking control in C is necessary for several reasons:
 * 1. Maintainability: Defining complex descriptors (like the 16-byte TSS) is 
 *    less error-prone and more readable in C structures than in assembly macros.
 * 2. Dynamic Management: It allows the kernel to easily allocate and update
 *    the Task State Segment (TSS) and per-thread kernel stacks at runtime.
 * 3. Ring 3 Support: Provides a clean interface for setting up User Code and 
 *    Data segments required for userspace transitions.
 *
 * Author: ApparentlyPlus
 */

#include <arch/x86_64/cpu/gdt.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <libc/string.h>

static gdt_t g_gdt;
static tss_t g_tss;

/*
 * gdt_set_entry - Populates a single GDT entry with provided parameters
 */
static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    g_gdt.entries[index].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt.entries[index].base_low = (uint16_t)(base & 0xFFFF);
    g_gdt.entries[index].base_mid = (uint8_t)((base >> 16) & 0xFF);
    g_gdt.entries[index].access = access;
    g_gdt.entries[index].flags_limit_high = (uint8_t)(((limit >> 16) & 0x0F) | (flags << 4));
    g_gdt.entries[index].base_high = (uint8_t)((base >> 24) & 0xFF);
}

/*
 * gdt_set_tss - Configures the special 16-byte TSS system descriptor in the GDT
 */
static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
    uint64_t tss_base = base;
    gdt_set_entry(index, (uint32_t)tss_base, limit, 0x89, 0x00);
    
    // The second half of the 16-byte TSS descriptor
    gdt_entry_t* extra = &g_gdt.entries[index + 1];
    uint32_t* extra_raw = (uint32_t*)extra;
    extra_raw[0] = (uint32_t)(tss_base >> 32);
    extra_raw[1] = 0;
}

/*
 * gdt_init - Entry point for initializing the GDT and TSS infrastructure
 */
void gdt_init(void) {
    memset(&g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    // 0x00: Null Descriptor
    gdt_set_entry(0, 0, 0, 0, 0);

    // 0x08: Kernel Code (Ring 0)
    gdt_set_entry(1, 0, 0xFFFFFFFF, GDT_PRESENT | GDT_DPL_0 | GDT_SYSTEM | GDT_EXECUTABLE | GDT_READ_WRITE, GDT_LONG_MODE);

    // 0x10: Kernel Data (Ring 0)
    gdt_set_entry(2, 0, 0xFFFFFFFF, GDT_PRESENT | GDT_DPL_0 | GDT_SYSTEM | GDT_READ_WRITE, 0);

    // 0x18: User Data (Ring 3)
    gdt_set_entry(3, 0, 0xFFFFFFFF, GDT_PRESENT | GDT_DPL_3 | GDT_SYSTEM | GDT_READ_WRITE, 0);

    // 0x20: User Code (Ring 3)
    gdt_set_entry(4, 0, 0xFFFFFFFF, GDT_PRESENT | GDT_DPL_3 | GDT_SYSTEM | GDT_EXECUTABLE | GDT_READ_WRITE, GDT_LONG_MODE);

    // 0x28: TSS (takes 2 slots)
    uint64_t tss_base = (uintptr_t)&g_tss;
    gdt_set_tss(5, tss_base, sizeof(tss_t) - 1);

    // Initialize TSS
    g_tss.iopb_offset = sizeof(tss_t);

    // Allocate a kernel stack for the primary CPU
    uint64_t kstack_phys;
    if (pmm_alloc(16384, &kstack_phys) == PMM_OK) {
        g_tss.rsp0 = PHYSMAP_P2V(kstack_phys + 16384);
        LOGF("[GDT] Primary kernel stack allocated at 0x%lx (virt)\n", g_tss.rsp0);
    } else {
        LOGF("[GDT] ERROR: Failed to allocate kernel stack for TSS!\n");
    }

    // Allocate IST stack for Double Faults (IST 1)
    uint64_t df_stack_phys;
    if (pmm_alloc(16384, &df_stack_phys) == PMM_OK) {
        g_tss.ist[0] = PHYSMAP_P2V(df_stack_phys + 16384);
        LOGF("[GDT] Double Fault IST stack allocated at 0x%lx\n", g_tss.ist[0]);
    }

    // Allocate IST stack for Page Faults (IST 2)
    uint64_t pf_stack_phys;
    if (pmm_alloc(16384, &pf_stack_phys) == PMM_OK) {
        g_tss.ist[1] = PHYSMAP_P2V(pf_stack_phys + 16384);
        LOGF("[GDT] Page Fault IST stack allocated at 0x%lx\n", g_tss.ist[1]);
    }

    gdt_ptr_t ptr;
    ptr.limit = sizeof(g_gdt) - 1;
    ptr.base = (uintptr_t)&g_gdt;

    // Reload GDT and all segment registers using inline assembly
    __asm__ volatile (
        "lgdt %0\n\t"
        "mov %1, %%ds\n\t"
        "mov %1, %%es\n\t"
        "mov %1, %%fs\n\t"
        "mov %1, %%gs\n\t"
        "mov %1, %%ss\n\t"
        "push %2\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "retfq\n\t"
        "1:\n\t"
        :
        : "m"(ptr), "r"((uint64_t)KERNEL_DS), "i"(KERNEL_CS)
        : "rax", "memory"
    );
    
    // Load Task Register
    __asm__ volatile("ltr %%ax" :: "a"((uint16_t)TSS_SEL));

    LOGF("[GDT] Global Descriptor Table and TSS initialized successfully.\n");
}

/*
 * tss_set_rsp0 - Updates the kernel stack pointer in the global TSS structure
 */
void tss_set_rsp0(uint64_t rsp) {
    g_tss.rsp0 = rsp;
}

/*
 * tss_set_ist - Updates a specific IST stack pointer in the global TSS structure
 */
void tss_set_ist(int index, uint64_t rsp) {
    if (index >= 0 && index < 7) {
        g_tss.ist[index] = rsp;
    }
}
