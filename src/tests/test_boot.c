/*
 * test_boot.c - Boot, paging and IDT validation suite
 *
 * Checks the state the early boot code leaves behind: the identity map is gone,
 * the physmap is laid out the way paging.c says, and the IDT points where it should.
 *
 * Author: Claude Code
 */

#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/memory/pmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <stdbool.h>
#include <stdint.h>

static suite_t suite;

extern uint64_t isr_stubs[IDT_SIZE];

#pragma region Helpers

/*
 * leaf_of - Walk the live tables for virt, returns the leaf entry and its level (2 = PD, 3 = PT)
 */
static uint64_t leaf_of(uint64_t virt, int* level) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(cr3));
    size_t idx[4] = { PML4_INDEX(virt), PDPT_INDEX(virt), PD_INDEX(virt), PT_INDEX(virt) };

    for (int l = 0; l < 4; l++) {
        uint64_t e = table[idx[l]];
        if (!(e & PAGE_PRESENT)) { *level = l; return 0; }
        if (l == 3 || (l == 2 && (e & PAGE_HUGE))) { *level = l; return e; }
        table = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    }
    return 0;
}

static uint64_t idt_target(int v) {
    return idt[v].address_low | ((uint64_t)idt[v].address_mid << 16) | ((uint64_t)idt[v].address_high << 32);
}
#pragma endregion

#pragma region Paging

static bool t_identity_gone(void) {
    int level;
    TEST_ASSERT(leaf_of(0x100000, &level) == 0);
    TEST_ASSERT(leaf_of(0x0, &level) == 0);
    return true;
}

static bool t_physmap_alias(void) {
    // The same physical byte through the kernel window and through the physmap
    volatile uint64_t* kv = (volatile uint64_t*)KERNEL_P2V(kmem->kstart);
    volatile uint64_t* pv = (volatile uint64_t*)PHYSMAP_P2V(kmem->kstart);
    TEST_ASSERT(*kv == *pv);

    static volatile uint64_t probe = 0;
    uint64_t phys = KERNEL_V2P(&probe);
    *(volatile uint64_t*)PHYSMAP_P2V(phys) = 0xC0FFEE;
    TEST_ASSERT(probe == 0xC0FFEE);
    return true;
}

static bool t_physmap_layout(void) {
    int level;

    // First 2MiB in 4KiB pages, the rest of RAM in 2MiB pages
    uint64_t low = leaf_of(PHYSMAP_P2V(0x1000), &level);
    TEST_ASSERT(level == 3 && PT_ENTRY_ADDR(low) == 0x1000);

    uint64_t ram = kmem->ram;
    uint64_t probe = align_down(ram - 1, PAGE_2MB);
    TEST_ASSERT(probe >= PAGE_2MB);
    uint64_t big = leaf_of(PHYSMAP_P2V(probe), &level);
    TEST_ASSERT(level == 2 && (big & PAGE_HUGE));
    TEST_ASSERT((big & ADDR_MASK & ~(PAGE_2MB - 1)) == probe);

    // Physmap is kernel only, writeback for RAM
    TEST_ASSERT(!(big & PAGE_USER));
    TEST_ASSERT(!(big & (PAGE_PWT | PAGE_PCD)));

    // Nothing past the end of the physmap
    TEST_ASSERT(leaf_of(PHYSMAP_P2V(kmem->physmap_end), &level) == 0);
    return true;
}

static bool t_kernel_window(void) {
    int level;
    uint64_t e = leaf_of(KERNEL_P2V(kmem->kstart), &level);
    TEST_ASSERT(level == 3 && PT_ENTRY_ADDR(e) == kmem->kstart);

    // The kernel window maps 4KiB pages, starting at KERNEL_VIRTUAL_BASE + 0
    e = leaf_of(KERNEL_VIRTUAL_BASE, &level);
    TEST_ASSERT(level == 3 && PT_ENTRY_ADDR(e) == 0);
    return true;
}
#pragma endregion

#pragma region IDT

static bool t_idt_stubs(void) {
    for (int v = 0; v < IDT_SIZE; v++) {
        TEST_ASSERT(idt_target(v) == isr_stubs[v]);
        TEST_ASSERT(v == 0 || isr_stubs[v] != isr_stubs[v - 1]);
        TEST_ASSERT(idt[v].flags & 0x80);
    }
    return true;
}

static bool t_idt_ist(void) {
    // gdt_init ran, so the TSS exists and the IST indices are on
    TEST_ASSERT(idt[INT_DOUBLE_FAULT].ist == 1);
    TEST_ASSERT(idt[INT_PAGE_FAULT].ist == 2);
    for (int v = 0; v < IDT_SIZE; v++)
        if (v != INT_DOUBLE_FAULT && v != INT_PAGE_FAULT) TEST_ASSERT(idt[v].ist == 0);
    return true;
}

static bool t_idt_dpl(void) {
    for (int v = 0; v < IDT_SIZE; v++) {
        int dpl = (idt[v].flags >> 5) & 3;
        int want = (v == INT_BREAKPOINT || v == INT_DEBUG) ? 3 : 0;
        TEST_ASSERT(dpl == want);
    }
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    test_begin(name);
    bool pass = fn();
    test_end(&suite, pass);
}

void test_boot(void) {
    suite_begin(&suite, "Boot and paging");

    run_test("identity map removed",          t_identity_gone);
    run_test("physmap aliases kernel memory", t_physmap_alias);
    run_test("physmap: 4K low, 2M rest",      t_physmap_layout);
    run_test("kernel window: 4K pages",       t_kernel_window);
    run_test("idt: every vector -> its stub", t_idt_stubs);
    run_test("idt: IST only on #DF and #PF",  t_idt_ist);
    run_test("idt: DPL 3 only on #BP and #DB",t_idt_dpl);

    suite_end(&suite);
}
#pragma endregion
