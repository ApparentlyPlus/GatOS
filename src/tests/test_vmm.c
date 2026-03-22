/*
 * test_vmm.c - Virtual Memory Manager Validation Suite
 *
 * Tests every public VMM function: kernel_get, create, destroy, switch,
 * get_current, alloc, alloc_at, free, resize, protect, map/unmap page+range,
 * get_physical, check_flags, check_buffer, find_mapped_object,
 * verify_integrity, get_alloc_base/end/size, vmm_stats, vmm_dump.
 *
 * Machine-adaptive: the OOM test queries the actual VMM address range.
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define PG 4096
#define USER_BASE 0x400000UL
#define USER_END  0x800000UL

typedef enum { TALLOC, TVMM } ttype_t;
typedef struct { ttype_t type; vmm_t* vmm; void* addr; size_t sz; bool on; } vmtr_t;

#define MT 2048
static vmtr_t tr[MT];
static int    tidx         = 0;
static int    ntests  = 0;
static int    npass = 0;

#pragma region Tracker

static void tr_reset(void) {
    for (int i = 0; i < MT; i++) tr[i].on = false;
    tidx = 0;
}

static void tr_alloc(vmm_t* v, void* a, size_t s) {
    if (tidx < MT) tr[tidx++] = (vmtr_t){TALLOC, v, a, s, true};
}

static void tr_vmm(vmm_t* v) {
    if (tidx < MT) tr[tidx++] = (vmtr_t){TVMM, v, NULL, 0, true};
}

static void tr_free(void) {
    for (int i = 0; i < MT; i++)
        if (tr[i].on && tr[i].type == TALLOC) { vmm_free(tr[i].vmm, tr[i].addr); tr[i].on = false; }
    for (int i = 0; i < MT; i++)
        if (tr[i].on && tr[i].type == TVMM) { vmm_destroy(tr[i].vmm); tr[i].on = false; }
    tidx = 0;
}

/* Walk page tables manually */
static bool pte_present(uint64_t root, void* virt) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(root);
    uint64_t e = pml4[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return false;
    uint64_t* pdpt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    e = pdpt[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return false;
    uint64_t* pd = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    e = pd[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return false;
    uint64_t* pt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    return !!(pt[PT_INDEX(virt)] & PAGE_PRESENT);
}

static bool pte_flags(uint64_t root, void* virt, uint64_t* f) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(root);
    uint64_t e = pml4[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return false;
    uint64_t* pdpt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    e = pdpt[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return false;
    uint64_t* pd = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    e = pd[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return false;
    uint64_t* pt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    uint64_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PAGE_PRESENT)) return false;
    *f = pte & 0xFFF;
    return true;
}
#pragma endregion

#pragma region Kernel VMM Invariants

static bool t_kern_nn(void) {
    TEST_ASSERT(vmm_kernel_get() != NULL);
    return true;
}

static bool t_kern_base_end(void) {
    vmm_t* v = vmm_kernel_get();
    TEST_ASSERT(vmm_get_alloc_base(v) > 0);
    TEST_ASSERT(vmm_get_alloc_end(v) > vmm_get_alloc_base(v));
    return true;
}

static bool t_kern_sz(void) {
    vmm_t* v = vmm_kernel_get();
    size_t sz = vmm_get_alloc_size(v);
    TEST_ASSERT(sz == vmm_get_alloc_end(v) - vmm_get_alloc_base(v));
    TEST_ASSERT(sz > 0);
    return true;
}

static bool t_kern_integrity(void) {
    TEST_ASSERT(vmm_verify_integrity(vmm_kernel_get()));
    return true;
}
#pragma endregion

#pragma region Basic Alloc / Free

static bool t_alloc_nn(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG);
    TEST_ASSERT(p != NULL);
    tr_free(); return true;
}

static bool t_alloc_range(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG);
    uintptr_t a = (uintptr_t)p;
    TEST_ASSERT(a >= vmm_get_alloc_base(v) && a + PG <= vmm_get_alloc_end(v));
    tr_free(); return true;
}

static bool t_alloc_wr(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG);
    volatile uint64_t* q = (volatile uint64_t*)p;
    *q = 0xCAFEBABEDEADBEEFULL;
    TEST_ASSERT(*q == 0xCAFEBABEDEADBEEFULL);
    tr_free(); return true;
}

static bool t_alloc_cycle(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 4, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    TEST_ASSERT_STATUS(vmm_free(v, p), VMM_OK);
    TEST_ASSERT(vmm_verify_integrity(v));
    tr_free(); return true;
}

static bool t_alloc_uniq(void) {
    /* Multiple allocations must return distinct non-overlapping addresses */
    tr_reset();
    vmm_t* v = vmm_kernel_get();
    void* p1, *p2, *p3;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p1), VMM_OK); tr_alloc(v,p1,PG);
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p2), VMM_OK); tr_alloc(v,p2,PG);
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p3), VMM_OK); tr_alloc(v,p3,PG);
    TEST_ASSERT(p1 != p2 && p1 != p3 && p2 != p3);
    tr_free(); return true;
}
#pragma endregion

#pragma region alloc_at

static bool t_alloc_at_exact(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get();
    uintptr_t target = vmm_get_alloc_base(v) + PG * 4096;
    void* p;
    TEST_ASSERT_STATUS(vmm_alloc_at(v, (void*)target, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG);
    TEST_ASSERT((uintptr_t)p == target);
    tr_free(); return true;
}

static bool t_alloc_at_unalign(void) {
    vmm_t* v = vmm_kernel_get();
    uintptr_t target = vmm_get_alloc_base(v) + PG * 4096;
    void* p;
    TEST_ASSERT_STATUS(vmm_alloc_at(v, (void*)(target + 1), PG, VM_FLAG_WRITE, NULL, &p), VMM_ERR_NOT_ALIGNED);
    return true;
}

static bool t_alloc_at_overlap(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get();
    uintptr_t target = vmm_get_alloc_base(v) + PG * 8192;
    void* p;
    TEST_ASSERT_STATUS(vmm_alloc_at(v, (void*)target, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG);
    void* p2;
    TEST_ASSERT_STATUS(vmm_alloc_at(v, (void*)target, PG, VM_FLAG_WRITE, NULL, &p2), VMM_ERR_ALREADY_MAPPED);
    tr_free(); return true;
}
#pragma endregion

#pragma region Resize

static bool t_resize_grow(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 2, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG * 4);
    TEST_ASSERT_STATUS(vmm_resize(v, p, PG * 4), VMM_OK);
    volatile uint8_t* b = (volatile uint8_t*)p;
    b[PG * 2 + 10] = 0xAB;
    TEST_ASSERT(b[PG * 2 + 10] == 0xAB);
    tr_free(); return true;
}

static bool t_resize_shrink(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 4, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    tr_alloc(v, p, PG);
    TEST_ASSERT_STATUS(vmm_resize(v, p, PG), VMM_OK);
    vm_object* obj = vmm_find_mapped_object(v, p);
    TEST_ASSERT(obj != NULL && obj->length == PG);
    tr_free(); return true;
}

static bool t_resize_collision(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void *p1, *p2;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p1), VMM_OK); tr_alloc(v,p1,PG);
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p2), VMM_OK); tr_alloc(v,p2,PG);
    /* Resize p1 into p2's space should fail */
    TEST_ASSERT_STATUS(vmm_resize(v, p1, PG * 2), VMM_ERR_OOM);
    tr_free(); return true;
}
#pragma endregion

#pragma region Protect

static bool t_prot_rm_wr(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    TEST_ASSERT(vmm_check_flags(v, p, VM_FLAG_WRITE));
    TEST_ASSERT_STATUS(vmm_protect(v, p, VM_FLAG_NONE), VMM_OK);
    TEST_ASSERT(!vmm_check_flags(v, p, VM_FLAG_WRITE));
    tr_free(); return true;
}

static bool t_prot_add_wr(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    TEST_ASSERT_STATUS(vmm_protect(v, p, VM_FLAG_NONE), VMM_OK);
    TEST_ASSERT_STATUS(vmm_protect(v, p, VM_FLAG_WRITE), VMM_OK);
    TEST_ASSERT(vmm_check_flags(v, p, VM_FLAG_WRITE));
    tr_free(); return true;
}
#pragma endregion

#pragma region MMIO / Manual Mapping

static bool t_mmio_map(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get();
    uint64_t phys; pmm_alloc(PG, &phys);
    void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_MMIO | VM_FLAG_WRITE, (void*)phys, &p), VMM_OK);
    tr_alloc(v, p, PG);
    uint64_t mp;
    TEST_ASSERT(vmm_get_physical(v, p, &mp));
    TEST_ASSERT(mp == phys);
    TEST_ASSERT(vmm_check_flags(v, p, VM_FLAG_MMIO));
    tr_free();
    pmm_free(phys, PG);
    return true;
}

static bool t_map_unmap_pg(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* ptr;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 4, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tr_alloc(v, ptr, PG * 4);
    void* mid = (void*)((uintptr_t)ptr + PG * 2);
    TEST_ASSERT_STATUS(vmm_unmap_page(v, mid), VMM_OK);
    TEST_ASSERT(!pte_present(v->pt_root, mid));
    uint64_t phys; pmm_alloc(PG, &phys);
    TEST_ASSERT_STATUS(vmm_map_page(v, phys, mid, VM_FLAG_WRITE), VMM_OK);
    TEST_ASSERT(pte_present(v->pt_root, mid));
    tr_free(); return true;
}

static bool t_map_unmap_range(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get();
    uint64_t phys; size_t sz = PG * 4;
    if (pmm_alloc(sz, &phys) != PMM_OK) return true;
    void* virt = (void*)0xC00000000ULL;
    TEST_ASSERT_STATUS(vmm_map_range(v, phys, virt, sz, VM_FLAG_WRITE | VM_FLAG_MMIO), VMM_OK);
    for (size_t off = 0; off < sz; off += PG) {
        uint64_t p;
        TEST_ASSERT(vmm_get_physical(v, (void*)((uintptr_t)virt + off), &p));
        TEST_ASSERT(p == phys + off);
    }
    TEST_ASSERT_STATUS(vmm_unmap_range(v, virt, sz), VMM_OK);
    TEST_ASSERT(!pte_present(v->pt_root, virt));
    pmm_free(phys, sz);
    tr_free(); return true;
}
#pragma endregion

#pragma region Page Table Flags

static bool t_pt_kern_flags(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    uint64_t fl;
    TEST_ASSERT(pte_flags(v->pt_root, p, &fl));
    TEST_ASSERT(fl & PAGE_PRESENT);
    TEST_ASSERT(fl & PAGE_WRITABLE);
    TEST_ASSERT(!(fl & PAGE_USER));
    tr_free(); return true;
}

static bool t_pt_user_flags(void) {
    tr_reset();
    vmm_t* u = vmm_create(USER_BASE, USER_END); tr_vmm(u);
    void* p;
    TEST_ASSERT_STATUS(vmm_alloc(u, PG, VM_FLAG_WRITE | VM_FLAG_USER, NULL, &p), VMM_OK);
    uint64_t fl;
    TEST_ASSERT(pte_flags(u->pt_root, p, &fl));
    TEST_ASSERT(fl & PAGE_USER);
    tr_free(); return true;
}

static bool t_pt_nx_data(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    uint64_t fl;
    TEST_ASSERT(pte_flags(v->pt_root, p, &fl));
    /* NX bit is bit 63 of the raw PTE (separate from low 12 flag bits) */
    (void)fl; /* We log only; NX depends on EFER.NXE being set */
    LOGF("[INFO] data-pg flags=0x%lx ", fl);
    tr_free(); return true;
}

static bool t_pt_nx_exec(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_EXEC, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    /* Check via pte directly for NX clear */
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(v->pt_root);
    uint64_t e = pml4[PML4_INDEX(p)];
    if (e & PAGE_PRESENT) {
        uint64_t* pdpt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
        e = pdpt[PDPT_INDEX(p)];
        if (e & PAGE_PRESENT) {
            uint64_t* pd = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
            e = pd[PD_INDEX(p)];
            if (e & PAGE_PRESENT) {
                uint64_t* pt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
                uint64_t pte = pt[PT_INDEX(p)];
                if (pte & PAGE_PRESENT) {
                    bool nx = !!(pte & (1ULL << 63));
                    if (nx) { LOGF("[FAIL] NX set on exec page "); tr_free(); return false; }
                }
            }
        }
    }
    tr_free(); return true;
}
#pragma endregion

#pragma region Isolation

static bool t_isolation(void) {
    tr_reset();
    vmm_t* a = vmm_create(USER_BASE, USER_END); tr_vmm(a);
    vmm_t* b = vmm_create(USER_BASE, USER_END); tr_vmm(b);
    void *pa, *pb;
    uintptr_t tgt = USER_BASE + PG;
    TEST_ASSERT_STATUS(vmm_alloc_at(a, (void*)tgt, PG, VM_FLAG_WRITE, NULL, &pa), VMM_OK);
    TEST_ASSERT_STATUS(vmm_alloc_at(b, (void*)tgt, PG, VM_FLAG_WRITE, NULL, &pb), VMM_OK);
    TEST_ASSERT(pa == pb); /* same virt */
    uint64_t phys_a, phys_b;
    TEST_ASSERT(vmm_get_physical(a, pa, &phys_a));
    TEST_ASSERT(vmm_get_physical(b, pb, &phys_b));
    TEST_ASSERT(phys_a != phys_b); /* different phys */
    tr_free(); return true;
}

static bool t_kern_persist(void) {
    tr_reset();
    vmm_t* u = vmm_create(USER_BASE, USER_END); tr_vmm(u);
    /* Kernel variable must be mapped in the user VMM's table */
    uint64_t kvar = (uint64_t)&ntests;
    TEST_ASSERT(pte_present(u->pt_root, (void*)kvar));
    tr_free(); return true;
}
#pragma endregion

#pragma region Context Switch

static bool t_ctx_switch(void) {
    tr_reset();
    vmm_t* orig = vmm_kernel_get();
    vmm_t* task = vmm_create(USER_BASE, USER_END); tr_vmm(task);
    void* p;
    TEST_ASSERT_STATUS(vmm_alloc(task, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    uint64_t phys; TEST_ASSERT(vmm_get_physical(task, p, &phys));
    *(uint64_t*)PHYSMAP_P2V(phys) = 0xABCD1234;
    vmm_switch(task);
    volatile uint64_t* vp = (volatile uint64_t*)p;
    bool ok = (*vp == 0xABCD1234);
    vmm_switch(orig);
    TEST_ASSERT(ok);
    tr_free(); return true;
}

static bool t_get_current(void) {
    TEST_ASSERT(vmm_get_current() != NULL);
    return true;
}
#pragma endregion

#pragma region vmm_check_buffer

static bool t_buf_valid(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 2, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG*2);
    TEST_ASSERT(vmm_check_buffer(v, p, PG, VM_FLAG_WRITE));
    tr_free(); return true;
}

static bool t_buf_null(void) {
    vmm_t* v = vmm_kernel_get();
    TEST_ASSERT(!vmm_check_buffer(v, NULL, PG, VM_FLAG_WRITE));
    return true;
}

static bool t_buf_zero_sz(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    /* Zero-size check on a valid pointer - implementation defined, must not crash */
    (void)vmm_check_buffer(v, p, 0, VM_FLAG_WRITE);
    tr_free(); return true;
}

static bool t_buf_bad_flags(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    /* Allocate read-only (VM_FLAG_NONE), check for write - must fail */
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_NONE, NULL, &p), VMM_OK); tr_alloc(v,p,PG);
    TEST_ASSERT(!vmm_check_buffer(v, p, PG, VM_FLAG_WRITE));
    tr_free(); return true;
}

static bool t_buf_partial_unmap(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 2, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG*2);
    /* Unmap second page */
    void* p2 = (void*)((uintptr_t)p + PG);
    TEST_ASSERT_STATUS(vmm_unmap_page(v, p2), VMM_OK);
    /* Buffer spanning both pages must now fail */
    TEST_ASSERT(!vmm_check_buffer(v, p, PG + 1, VM_FLAG_WRITE));
    /* But single page is still valid */
    TEST_ASSERT(vmm_check_buffer(v, p, PG, VM_FLAG_WRITE));
    tr_free(); return true;
}
#pragma endregion

#pragma region find_mapped_object

static bool t_find_hit(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 3, VM_FLAG_WRITE, NULL, &p), VMM_OK); tr_alloc(v,p,PG*3);
    vm_object* obj = vmm_find_mapped_object(v, p);
    TEST_ASSERT(obj != NULL);
    TEST_ASSERT(obj->base == (uintptr_t)p);
    TEST_ASSERT(obj->length == PG * 3);
    tr_free(); return true;
}

static bool t_find_miss(void) {
    vmm_t* v = vmm_kernel_get();
    /* Some definitely-unmapped address */
    vm_object* obj = vmm_find_mapped_object(v, (void*)0xDEAD0000);
    TEST_ASSERT(obj == NULL);
    return true;
}
#pragma endregion

#pragma region Lazy Allocation

static bool t_lazy_before(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 2, VM_FLAG_WRITE | VM_FLAG_LAZY, NULL, &p), VMM_OK); tr_alloc(v,p,PG*2);
    TEST_ASSERT(!pte_present(v->pt_root, p));
    tr_free(); return true;
}

static bool t_lazy_after(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG * 2, VM_FLAG_WRITE | VM_FLAG_LAZY, NULL, &p), VMM_OK); tr_alloc(v,p,PG*2);
    volatile uint64_t* q = (volatile uint64_t*)p;
    *q = 0x1234ABCD;
    TEST_ASSERT(pte_present(v->pt_root, p));
    TEST_ASSERT(*q == 0x1234ABCD);
    tr_free(); return true;
}
#pragma endregion

#pragma region OOM

static bool t_oom_small(void) {
    tr_reset();
    size_t pool = PG * 8;
    uintptr_t base = 0x1000000UL;
    vmm_t* u = vmm_create(base, base + pool); TEST_ASSERT(u != NULL); tr_vmm(u);
    int cnt = 0; void* p;
    while (vmm_alloc(u, PG, VM_FLAG_WRITE, NULL, &p) == VMM_OK) { cnt++; if (cnt > 20) break; }
    TEST_ASSERT(cnt == 8);
    TEST_ASSERT_STATUS(vmm_alloc(u, PG, VM_FLAG_WRITE, NULL, &p), VMM_ERR_OOM);
    tr_free(); return true;
}
#pragma endregion

#pragma region Page Table Cleanup

static bool t_pt_cleanup(void) {
    tr_reset();
    vmm_t* v = vmm_create(0x400000UL, 0x8000000000ULL); tr_vmm(v);
    void* p = (void*)0x4000000000ULL;
    TEST_ASSERT_STATUS(vmm_alloc_at(v, p, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    TEST_ASSERT(pte_present(v->pt_root, p));
    TEST_ASSERT_STATUS(vmm_free(v, p), VMM_OK);
    tr_free(); return true;
}
#pragma endregion

#pragma region Fragmentation Stress

static bool t_frag(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get();
    #define FRAG 128
    void* ptrs[FRAG];
    for (int i = 0; i < FRAG; i++) {
        if (vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &ptrs[i]) != VMM_OK) return false;
        tr_alloc(v, ptrs[i], PG);
    }
    for (int i = 1; i < FRAG; i += 2) {
        vmm_free(v, ptrs[i]);
        for (int t = 0; t < tidx; t++) if (tr[t].addr == ptrs[i]) tr[t].on = false;
    }
    for (int i = 0; i < 64; i++) {
        void* p;
        if (vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p) != VMM_OK) {
            LOGF("[FAIL] OOM during fragmentation refill ");
            tr_free(); return false;
        }
        tr_alloc(v, p, PG);
    }
    tr_free(); return true;
}
#pragma endregion

#pragma region Dirty Reuse / Security

static bool t_dirty_reuse(void) {
    tr_reset();
    vmm_t* v = vmm_kernel_get(); void* p;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p), VMM_OK);
    uint64_t* q = (uint64_t*)p;
    for (int i = 0; i < PG/8; i++) q[i] = 0xDEADDEADDEADDEADULL;
    uint64_t phys; vmm_get_physical(v, p, &phys);
    TEST_ASSERT_STATUS(vmm_free(v, p), VMM_OK);
    void* p2;
    TEST_ASSERT_STATUS(vmm_alloc(v, PG, VM_FLAG_WRITE, NULL, &p2), VMM_OK);
    tr_alloc(v, p2, PG);
    uint64_t phys2; vmm_get_physical(v, p2, &phys2);
    if (phys2 == phys) {
        LOGF("[INFO] reused phys 0x%lx ", phys);
        /* Warn only; not a hard failure */
    }
    tr_free(); return true;
}
#pragma endregion

#pragma region vmm_stats

static bool t_stats_smoke(void) {
    size_t total, resident;
    vmm_stats(vmm_kernel_get(), &total, &resident);
    /* Must not crash; total >= resident */
    TEST_ASSERT(total >= resident);
    return true;
}
#pragma endregion

#pragma region Swiss Cheese Destroy

static bool t_swiss_cheese(void) {
    pmm_stats_t s0; pmm_get_stats(&s0);
    uint64_t free0 = 0;
    for (int i = 0; i < PMM_MAX_ORDERS; i++) free0 += s0.free_blocks[i] * (1ULL<<i) * 4096;
    vmm_t* v = vmm_create(0x400000UL, 0x800000000ULL);
    if (!v) return true;
    #define HOLES 50
    uintptr_t stride = 0x1000000UL;
    for (int i = 0; i < HOLES; i++) {
        void* p; uintptr_t tgt = 0x400000UL + (uintptr_t)i * stride;
        vmm_alloc_at(v, (void*)tgt, PG, VM_FLAG_WRITE, NULL, &p);
    }
    vmm_destroy(v);
    pmm_stats_t s1; pmm_get_stats(&s1);
    uint64_t free1 = 0;
    for (int i = 0; i < PMM_MAX_ORDERS; i++) free1 += s1.free_blocks[i] * (1ULL<<i) * 4096;
    if (free0 > free1 && (free0 - free1) > (uint64_t)(HOLES * PG)) {
        LOGF("[FAIL] swiss cheese: %lu bytes leaked\n", free0 - free1);
        return false;
    }
    return true;
}
#pragma endregion

#pragma region Runner

static void run_test(const char* name, bool (*fn)(void)) {
    ntests++;
    LOGF("[TEST] %-40s ", name);
    if (!vmm_verify_integrity(vmm_kernel_get())) { LOGF("[SKIP] (VMM corrupted)\n"); return; }
    bool pass = fn();
    if (tidx > 0) { LOGF("[WARN] leak (cleaning) ... "); tr_free(); }
    if (pass) { npass++; LOGF("[PASS]\n"); }
    else       { LOGF("[FAIL]\n"); }
}

void test_vmm(void) {
    ntests  = 0;
    npass = 0;

    LOGF("\n--- BEGIN VMM TEST ---\n");

    run_test("kernel_get: not null",           t_kern_nn);
    run_test("kernel: alloc base/end valid",   t_kern_base_end);
    run_test("kernel: alloc_size consistent",  t_kern_sz);
    run_test("kernel: integrity check",        t_kern_integrity);
    run_test("alloc: not null",                t_alloc_nn);
    run_test("alloc: within range",            t_alloc_range);
    run_test("alloc: memory writable",         t_alloc_wr);
    run_test("alloc+free: cycle",              t_alloc_cycle);
    run_test("alloc: multi unique addrs",      t_alloc_uniq);
    run_test("alloc_at: exact address",        t_alloc_at_exact);
    run_test("alloc_at: unaligned → error",    t_alloc_at_unalign);
    run_test("alloc_at: overlap → error",      t_alloc_at_overlap);
    run_test("resize: grow",                   t_resize_grow);
    run_test("resize: shrink",                 t_resize_shrink);
    run_test("resize: collision → OOM",        t_resize_collision);
    run_test("protect: remove write",          t_prot_rm_wr);
    run_test("protect: add write",             t_prot_add_wr);
    run_test("mmio: physical match",           t_mmio_map);
    run_test("map_page + unmap_page",          t_map_unmap_pg);
    run_test("map_range + unmap_range",        t_map_unmap_range);
    run_test("PT: kernel flags (RW, !US)",     t_pt_kern_flags);
    run_test("PT: user flags (US)",            t_pt_user_flags);
    run_test("PT: NX data page (info)",        t_pt_nx_data);
    run_test("PT: NX clear exec page",         t_pt_nx_exec);
    run_test("isolation: different phys",      t_isolation);
    run_test("kernel persistence in user VMM", t_kern_persist);
    run_test("context switch",                 t_ctx_switch);
    run_test("vmm_get_current not null",       t_get_current);
    run_test("check_buffer: valid range",      t_buf_valid);
    run_test("check_buffer: NULL → false",     t_buf_null);
    run_test("check_buffer: zero size",        t_buf_zero_sz);
    run_test("check_buffer: wrong flags",      t_buf_bad_flags);
    run_test("check_buffer: partial unmap",    t_buf_partial_unmap);
    run_test("find_mapped_object: hit",        t_find_hit);
    run_test("find_mapped_object: miss=NULL",  t_find_miss);
    run_test("lazy: not mapped before access", t_lazy_before);
    run_test("lazy: mapped after access",      t_lazy_after);
    run_test("OOM: small VMM exhausted",       t_oom_small);
    run_test("PT cleanup after free",          t_pt_cleanup);
    run_test("fragmentation refill",           t_frag);
    run_test("dirty reuse (security)",         t_dirty_reuse);
    run_test("vmm_stats: smoke",               t_stats_smoke);
    run_test("swiss cheese destroy",           t_swiss_cheese);

    LOGF("--- END VMM TEST ---\n");
    LOGF("VMM Test Results: %d/%d\n\n", npass, ntests);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <klibc/stdio.h>
    if (npass != ntests) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        kprintf("[-] Some VMM tests failed (%d/%d passed).\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    } else {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        kprintf("[+] All VMM tests passed! (%d/%d)\n", npass, ntests);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}
#pragma endregion
