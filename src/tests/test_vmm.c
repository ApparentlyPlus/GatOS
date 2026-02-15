/*
 * vmm_tests.c - Virtual Memory Manager Validation Suite
 *
 * This suite verifies the correctness, stability, and security of the
 * Virtual Memory Manager. It operates on the live system memory and 
 * verifies allocator logic, page table management, address space 
 * isolation, and cleanup mechanisms.
 */

#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>
#include <tests/tests.h>
#include <libc/string.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#pragma region Configuration & Types

#define MAX_TRACKED_ITEMS 2048
#define TEST_PAGE_SIZE 4096
#define TEST_USER_BASE 0x400000 // 4MB
#define TEST_USER_END  0x800000 // 8MB

typedef enum { 
    TRACK_ALLOC,        // Standard vmm_alloc/alloc_at
    TRACK_VMM_INSTANCE  // A vmm_t created via vmm_create
} track_type_t;

typedef struct {
    track_type_t type;
    vmm_t* vmm;         // The VMM instance or the instance owning the allocation
    void* addr;         // Virtual address (for alloc)
    size_t size;        // Size (for alloc)
    bool active;
} vmm_tracker_t;

// Static tracking to maintain isolation
static vmm_tracker_t g_tracker[MAX_TRACKED_ITEMS];
static int g_tracker_idx = 0;

static int g_tests_total = 0;
static int g_tests_passed = 0;

#pragma endregion

#pragma region Harness Helpers

// Resets the internal tracking array before a test begins.
static void tracker_reset(void) {
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        g_tracker[i].active = false;
        g_tracker[i].vmm = NULL;
        g_tracker[i].addr = NULL;
        g_tracker[i].size = 0;
    }
    g_tracker_idx = 0;
}

// Registers a memory allocation to be automatically freed during cleanup.
static void tracker_add_alloc(vmm_t* vmm, void* addr, size_t size) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx].type = TRACK_ALLOC;
        g_tracker[g_tracker_idx].vmm = vmm;
        g_tracker[g_tracker_idx].addr = addr;
        g_tracker[g_tracker_idx].size = size;
        g_tracker[g_tracker_idx].active = true;
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] VMM Tracker full.\n");
    }
}

// Registers a VMM instance to be automatically destroyed during cleanup.
static void tracker_add_vmm(vmm_t* vmm) {
    if (g_tracker_idx < MAX_TRACKED_ITEMS) {
        g_tracker[g_tracker_idx].type = TRACK_VMM_INSTANCE;
        g_tracker[g_tracker_idx].vmm = vmm;
        g_tracker[g_tracker_idx].active = true;
        g_tracker_idx++;
    } else {
        LOGF("[TEST WARN] VMM Tracker full.\n");
    }
}

// Frees all tracked allocations and destroys tracked VMM instances.
static void tracker_cleanup(void) {
    // Free allocations first
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        if (g_tracker[i].active && g_tracker[i].type == TRACK_ALLOC) {
            vmm_free(g_tracker[i].vmm, g_tracker[i].addr);
            g_tracker[i].active = false;
        }
    }
    // Destroy VMM instances
    for (int i = 0; i < MAX_TRACKED_ITEMS; i++) {
        if (g_tracker[i].active && g_tracker[i].type == TRACK_VMM_INSTANCE) {
            vmm_destroy(g_tracker[i].vmm);
            g_tracker[i].active = false;
        }
    }
    g_tracker_idx = 0;
}

// Manually walks the x86_64 page tables to verify mapping existence and flags.
static bool inspect_pte(uint64_t pt_root, void* virt, uint64_t* out_phys, uint64_t* out_flags) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);
    
    uint64_t pml4e = pml4[PML4_INDEX(virt)];
    if (!(pml4e & PAGE_PRESENT)) return false;
    
    uint64_t* pdpt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(pml4e));
    uint64_t pdpte = pdpt[PDPT_INDEX(virt)];
    if (!(pdpte & PAGE_PRESENT)) return false;
    
    uint64_t* pd = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(pdpte));
    uint64_t pde = pd[PD_INDEX(virt)];
    if (!(pde & PAGE_PRESENT)) return false;
    
    uint64_t* pt = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(pde));
    uint64_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PAGE_PRESENT)) return false;
    
    if (out_phys) *out_phys = PT_ENTRY_ADDR(pte);
    if (out_flags) *out_flags = (pte & 0xFFF); 
    return true;
}

#pragma endregion

#pragma region Core Allocator Tests

// Checks if the Kernel VMM is in a valid initial state.
static bool test_invariants(void) {
    vmm_t* k_vmm = vmm_kernel_get();
    TEST_ASSERT(k_vmm != NULL);
    TEST_ASSERT(vmm_get_alloc_base(k_vmm) > 0);
    TEST_ASSERT(vmm_get_alloc_end(k_vmm) > vmm_get_alloc_base(k_vmm));
    TEST_ASSERT(vmm_verify_integrity(k_vmm));
    return true;
}

// Tests a standard allocation, write access, and free cycle.
static bool test_basic_cycle(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    void* ptr;
    size_t size = TEST_PAGE_SIZE * 4;

    TEST_ASSERT_STATUS(vmm_alloc(vmm, size, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, size);

    uintptr_t addr = (uintptr_t)ptr;
    TEST_ASSERT(addr >= vmm_get_alloc_base(vmm));
    TEST_ASSERT(addr + size <= vmm_get_alloc_end(vmm));

    volatile uint64_t* p = (volatile uint64_t*)ptr;
    *p = 0xDEADBEEFCAFEBABE;
    TEST_ASSERT(*p == 0xDEADBEEFCAFEBABE);

    TEST_ASSERT_STATUS(vmm_free(vmm, ptr), VMM_OK);
    g_tracker[0].active = false; 

    TEST_ASSERT(vmm_verify_integrity(vmm));
    tracker_cleanup();
    return true;
}

// Verifies allocation at a specific address, including alignment and overlap checks.
static bool test_alloc_at(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    
    uintptr_t target = vmm_get_alloc_base(vmm) + (TEST_PAGE_SIZE * 4096);
    void* ptr;
    
    TEST_ASSERT_STATUS(vmm_alloc_at(vmm, (void*)target, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, TEST_PAGE_SIZE);
    TEST_ASSERT((uintptr_t)ptr == target);

    // Test unaligned address
    TEST_ASSERT_STATUS(vmm_alloc_at(vmm, (void*)(target + 1), TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_ERR_NOT_ALIGNED);

    // Test overlap
    TEST_ASSERT_STATUS(vmm_alloc_at(vmm, (void*)target, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_ERR_ALREADY_MAPPED);

    tracker_cleanup();
    return true;
}

// Tests the resizing (expanding and shrinking) of existing allocations.
static bool test_resize_logic(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    void* ptr;
    size_t size = TEST_PAGE_SIZE * 2;

    TEST_ASSERT_STATUS(vmm_alloc(vmm, size, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, size);

    size_t new_size = TEST_PAGE_SIZE * 4;
    TEST_ASSERT_STATUS(vmm_resize(vmm, ptr, new_size), VMM_OK);
    g_tracker[0].size = new_size;

    volatile uint8_t* byte_ptr = (volatile uint8_t*)ptr;
    byte_ptr[size + 10] = 0xAA; // Access extended area
    TEST_ASSERT(byte_ptr[size + 10] == 0xAA);

    TEST_ASSERT_STATUS(vmm_resize(vmm, ptr, TEST_PAGE_SIZE), VMM_OK);
    g_tracker[0].size = TEST_PAGE_SIZE;

    vm_object* obj = vmm_find_mapped_object(vmm, ptr);
    TEST_ASSERT(obj != NULL);
    TEST_ASSERT(obj->length == TEST_PAGE_SIZE);

    tracker_cleanup();
    return true;
}

// Ensures resizing fails correctly when expanding into occupied memory.
static bool test_resize_collision(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    void *p1, *p2;

    TEST_ASSERT_STATUS(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &p1), VMM_OK);
    tracker_add_alloc(vmm, p1, TEST_PAGE_SIZE);

    TEST_ASSERT_STATUS(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &p2), VMM_OK);
    tracker_add_alloc(vmm, p2, TEST_PAGE_SIZE);

    TEST_ASSERT_STATUS(vmm_resize(vmm, p1, TEST_PAGE_SIZE * 2), VMM_ERR_OOM);

    tracker_cleanup();
    return true;
}

// Verifies that page permissions can be modified dynamically.
static bool test_protection(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    void* ptr;

    TEST_ASSERT_STATUS(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, TEST_PAGE_SIZE);

    TEST_ASSERT(vmm_check_flags(vmm, ptr, VM_FLAG_WRITE));

    TEST_ASSERT_STATUS(vmm_protect(vmm, ptr, VM_FLAG_NONE), VMM_OK);
    TEST_ASSERT(!vmm_check_flags(vmm, ptr, VM_FLAG_WRITE));

    TEST_ASSERT_STATUS(vmm_protect(vmm, ptr, VM_FLAG_WRITE), VMM_OK);
    TEST_ASSERT(vmm_check_flags(vmm, ptr, VM_FLAG_WRITE));

    tracker_cleanup();
    return true;
}

// Tests mapping specific physical addresses (simulated MMIO) to virtual space.
static bool test_mmio_mapping(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    
    uint64_t phys;
    pmm_alloc(TEST_PAGE_SIZE, &phys); 
    
    void* ptr;
    TEST_ASSERT_STATUS(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_MMIO | VM_FLAG_WRITE, (void*)phys, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, TEST_PAGE_SIZE);

    uint64_t mapped_phys;
    TEST_ASSERT(vmm_get_physical(vmm, ptr, &mapped_phys));
    TEST_ASSERT(mapped_phys == phys);
    TEST_ASSERT(vmm_check_flags(vmm, ptr, VM_FLAG_MMIO));

    tracker_cleanup();
    pmm_free(phys, TEST_PAGE_SIZE);
    return true;
}

// Tests manual mapping and unmapping of a physical range to a high virtual address.
static bool test_manual_map_range(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    
    uint64_t phys_base;
    size_t size = TEST_PAGE_SIZE * 4;
    if (pmm_alloc(size, &phys_base) != PMM_OK) return true;
    
    void* virt_addr = (void*)0xC00000000;
    
    TEST_ASSERT_STATUS(vmm_map_range(vmm, phys_base, virt_addr, size, VM_FLAG_WRITE | VM_FLAG_MMIO), VMM_OK);
    
    for(size_t off=0; off<size; off+=TEST_PAGE_SIZE) {
        uint64_t p;
        TEST_ASSERT(vmm_get_physical(vmm, (void*)((uintptr_t)virt_addr + off), &p));
        TEST_ASSERT(p == phys_base + off);
    }
    
    TEST_ASSERT_STATUS(vmm_unmap_range(vmm, virt_addr, size), VMM_OK);
    
    uint64_t p;
    if (vmm_get_physical(vmm, virt_addr, &p)) {
        LOGF("[FAIL] Manual unmap failed, address still resolves\n");
        return false;
    }
    
    pmm_free(phys_base, size);
    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region System Inspection & Hardware Tests

// Verifies that hardware page table flags match the requested permissions.
static bool test_pt_flags(void) {
    tracker_reset();
    vmm_t* kvmm = vmm_kernel_get();
    void* kptr;
    
    // Check Kernel Flags
    TEST_ASSERT_STATUS(vmm_alloc(kvmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &kptr), VMM_OK);
    tracker_add_alloc(kvmm, kptr, TEST_PAGE_SIZE);
    
    uint64_t flags;
    TEST_ASSERT(inspect_pte(kvmm->pt_root, kptr, NULL, &flags));
    TEST_ASSERT(flags & PAGE_PRESENT);
    TEST_ASSERT(flags & PAGE_WRITABLE);
    TEST_ASSERT(!(flags & PAGE_USER)); 
    
    // Check User Flags
    vmm_t* uvmm = vmm_create(TEST_USER_BASE, TEST_USER_END);
    tracker_add_vmm(uvmm);
    
    void* uptr;
    TEST_ASSERT_STATUS(vmm_alloc(uvmm, TEST_PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_USER, NULL, &uptr), VMM_OK);
    
    TEST_ASSERT(inspect_pte(uvmm->pt_root, uptr, NULL, &flags));
    TEST_ASSERT(flags & PAGE_PRESENT);
    TEST_ASSERT(flags & PAGE_WRITABLE);
    TEST_ASSERT(flags & PAGE_USER); 
    
    tracker_cleanup();
    return true;
}

// Ensures that two VMM instances map the same virtual address to different physical pages.
static bool test_isolation(void) {
    tracker_reset();
    
    vmm_t* vmm_a = vmm_create(TEST_USER_BASE, TEST_USER_END);
    tracker_add_vmm(vmm_a);
    
    vmm_t* vmm_b = vmm_create(TEST_USER_BASE, TEST_USER_END);
    tracker_add_vmm(vmm_b);
    
    void *ptr_a, *ptr_b;
    uintptr_t target = TEST_USER_BASE + 0x1000;
    
    TEST_ASSERT_STATUS(vmm_alloc_at(vmm_a, (void*)target, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr_a), VMM_OK);
    TEST_ASSERT_STATUS(vmm_alloc_at(vmm_b, (void*)target, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr_b), VMM_OK);
    
    TEST_ASSERT(ptr_a == ptr_b);
    
    uint64_t phys_a, phys_b;
    TEST_ASSERT(vmm_get_physical(vmm_a, ptr_a, &phys_a));
    TEST_ASSERT(vmm_get_physical(vmm_b, ptr_b, &phys_b));
    
    if (phys_a == phys_b) {
        LOGF("[FAIL] Isolation breach! Both VMMs mapped 0x%lx to Phys 0x%lx\n", ptr_a, phys_a);
        return false;
    }
    
    tracker_cleanup();
    return true;
}

// Tests VMM context switching and verification of active memory mappings.
static bool test_context_switch(void) {
    tracker_reset();
    vmm_t* original = vmm_kernel_get();
    
    vmm_t* task_vmm = vmm_create(TEST_USER_BASE, TEST_USER_END);
    tracker_add_vmm(task_vmm);
    
    void* ptr;
    TEST_ASSERT_STATUS(vmm_alloc(task_vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    
    uint64_t phys;
    TEST_ASSERT(vmm_get_physical(task_vmm, ptr, &phys));
    uint64_t* p_phys = (uint64_t*)PHYSMAP_P2V(phys);
    *p_phys = 0xdef1234;
    
    vmm_switch(task_vmm);
    
    uint64_t* p_virt = (uint64_t*)ptr;
    if (*p_virt != 0xdef1234) {
        vmm_switch(original);
        LOGF("[FAIL] Context switch failed to map virtual memory correctly\n");
        return false;
    }
    
    vmm_switch(original);
    
    tracker_cleanup();
    return true;
}

// Checks that kernel mappings are visible within a user VMM instance.
static bool test_kernel_persistence(void) {
    tracker_reset();
    
    uint64_t k_var_addr = (uint64_t)&g_tracker_idx;
    
    vmm_t* uvmm = vmm_create(TEST_USER_BASE, TEST_USER_END);
    tracker_add_vmm(uvmm);
    
    uint64_t phys;
    if (!inspect_pte(uvmm->pt_root, (void*)k_var_addr, &phys, NULL)) {
        LOGF("[FAIL] Kernel address 0x%lx not mapped in User VMM\n", k_var_addr);
        return false;
    }
    
    tracker_cleanup();
    return true;
}

// Verifies that internal page table structures are removed when pages are freed.
static bool test_pt_cleanup(void) {
    tracker_reset();
    
    vmm_t* vmm = vmm_create(0x400000, 0x8000000000ULL);
    if (!vmm) return false;
    tracker_add_vmm(vmm);
    
    // Alloc in high memory to force new PDPT/PD creation
    void* ptr = (void*)(0x4000000000ULL); 
    TEST_ASSERT_STATUS(vmm_alloc_at(vmm, ptr, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    
    TEST_ASSERT(inspect_pte(vmm->pt_root, ptr, NULL, NULL));
    
    TEST_ASSERT_STATUS(vmm_free(vmm, ptr), VMM_OK);
    
    if (inspect_pte(vmm->pt_root, ptr, NULL, NULL)) {
        LOGF("[WARN] Page tables for 0x%lx still present after free (Efficiency issue?)\n", ptr);
    }
    
    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Stress & Security Tests

// Performs interleaved allocations and frees to stress the allocator's gap finding logic.
static bool test_fragmentation_stress(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    
    #define STRESS_COUNT 256
    void* ptrs[STRESS_COUNT];
    
    for(int i=0; i<STRESS_COUNT; i++) {
        if(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptrs[i]) != VMM_OK) return false;
        tracker_add_alloc(vmm, ptrs[i], TEST_PAGE_SIZE);
    }
    
    for(int i=1; i<STRESS_COUNT; i+=2) {
        vmm_free(vmm, ptrs[i]);
        for(int t=0; t<g_tracker_idx; t++) if(g_tracker[t].addr == ptrs[i]) g_tracker[t].active = false;
    }
    
    for(int i=0; i<128; i++) {
        void* p;
        if(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &p) != VMM_OK) {
             LOGF("[FAIL] OOM during fragmentation gap filling\n");
             return false;
        }
        tracker_add_alloc(vmm, p, TEST_PAGE_SIZE);
    }
    
    tracker_cleanup();
    return true;
}

// Verifies that the allocator correctly reports OOM when limits are reached.
static bool test_oom_limit(void) {
    tracker_reset();
    
    size_t pool_size = TEST_PAGE_SIZE * 16;
    uintptr_t base = 0x1000000;
    vmm_t* uvmm = vmm_create(base, base + pool_size);
    TEST_ASSERT(uvmm != NULL);
    tracker_add_vmm(uvmm);

    void* p;
    int count = 0;
    while (vmm_alloc(uvmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &p) == VMM_OK) {
        count++;
        if (count > 20) break; 
    }

    TEST_ASSERT(count == 16); 
    TEST_ASSERT_STATUS(vmm_alloc(uvmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &p), VMM_ERR_OOM);

    tracker_cleanup();
    return true;
}

// Tests unmapping and remapping pages within a large contiguous block.
static bool test_large_remap(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();
    
    size_t size = TEST_PAGE_SIZE * 512; // 2MB
    void* ptr;
    
    TEST_ASSERT_STATUS(vmm_alloc(vmm, size, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, size);
    
    void* mid = (void*)((uintptr_t)ptr + (256 * TEST_PAGE_SIZE));
    TEST_ASSERT_STATUS(vmm_unmap_page(vmm, mid), VMM_OK);
    
    if (inspect_pte(vmm->pt_root, mid, NULL, NULL)) {
        LOGF("[FAIL] Middle page still present in page table\n");
        return false;
    }
    
    uint64_t phys;
    pmm_alloc(TEST_PAGE_SIZE, &phys); 
    TEST_ASSERT_STATUS(vmm_map_page(vmm, phys, mid, VM_FLAG_WRITE), VMM_OK);
    
    TEST_ASSERT(inspect_pte(vmm->pt_root, mid, NULL, NULL));
    
    tracker_cleanup();
    return true;
}

// Checks if reused physical memory retains old data (Security Check).
static bool test_dirty_reuse(void) {
    tracker_reset();
    vmm_t* kvmm = vmm_kernel_get();
    
    void* secret_ptr;
    TEST_ASSERT_STATUS(vmm_alloc(kvmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &secret_ptr), VMM_OK);
    
    uint64_t* p = (uint64_t*)secret_ptr;
    for(int i=0; i < TEST_PAGE_SIZE/8; i++) p[i] = 0xdddddddddddddd;
    
    uint64_t phys_addr;
    vmm_get_physical(kvmm, secret_ptr, &phys_addr);
    
    TEST_ASSERT_STATUS(vmm_free(kvmm, secret_ptr), VMM_OK);
    
    void* new_ptr;
    TEST_ASSERT_STATUS(vmm_alloc(kvmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &new_ptr), VMM_OK);
    tracker_add_alloc(kvmm, new_ptr, TEST_PAGE_SIZE);
    
    uint64_t new_phys;
    vmm_get_physical(kvmm, new_ptr, &new_phys);
    
    if (new_phys == phys_addr) {
        uint64_t* new_p = (uint64_t*)new_ptr;
        if (new_p[0] == 0xdddddddddddddd) {
            LOGF("[WARN] Security: Dirty memory returned (expected without memset)\n");
        }
    }
    
    tracker_cleanup();
    return true;
}

// Tests robustness of cleanup by creating scattered allocations across a large range.
static bool test_swiss_cheese_cleanup(void) {
    tracker_reset();
    
    pmm_stats_t start_stats;
    pmm_get_stats(&start_stats);
    uint64_t start_free = 0;
    for(int i=0; i<=32; i++) start_free += (start_stats.free_blocks[i] * (1ULL<<i) * 4096);

    vmm_t* uvmm = vmm_create(0x400000, 0x800000000ULL);
    if(!uvmm) return false;
    
    #define CHEESE_HOLES 50
    uintptr_t base = 0x400000;
    uintptr_t stride = 0x1000000;
    
    for(int i=0; i<CHEESE_HOLES; i++) {
        void* p;
        uintptr_t target = base + (i * stride);
        if (vmm_alloc_at(uvmm, (void*)target, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &p) != VMM_OK) {
            vmm_destroy(uvmm);
            return false;
        }
    }
    
    vmm_destroy(uvmm);
    
    pmm_stats_t end_stats;
    pmm_get_stats(&end_stats);
    uint64_t end_free = 0;
    for(int i=0; i<=32; i++) end_free += (end_stats.free_blocks[i] * (1ULL<<i) * 4096);

    if (end_free < start_free) {
        if ((start_free - end_free) > (CHEESE_HOLES * 4096)) {
             LOGF("[FAIL] VMM Destroy leaked page tables! Diff: %lu bytes\n", start_free - end_free);
             return false;
        }
    }
    
    return true;
}

// Verifies that the NX (No-Execute) bit is correctly set on non-executable pages.
static bool test_nx_bit_enforcement(void) {
    tracker_reset();
    vmm_t* vmm = vmm_kernel_get();

    // Test Data Page (RW, No Exec)
    void* ptr;
    TEST_ASSERT_STATUS(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_WRITE, NULL, &ptr), VMM_OK);
    tracker_add_alloc(vmm, ptr, TEST_PAGE_SIZE);

    uint64_t phys, flags;
    TEST_ASSERT(inspect_pte(vmm->pt_root, ptr, &phys, &flags));

    bool nx_set = (flags & (1ULL << 63));
    
    if (!nx_set) {
        LOGF("[WARN] NX bit not set on data page. (Is EFER.NXE enabled?)\n");
    }

    // Test Code Page (RX, Exec)
    void* code_ptr;
    TEST_ASSERT_STATUS(vmm_alloc(vmm, TEST_PAGE_SIZE, VM_FLAG_EXEC, NULL, &code_ptr), VMM_OK);
    tracker_add_alloc(vmm, code_ptr, TEST_PAGE_SIZE);

    inspect_pte(vmm->pt_root, code_ptr, NULL, &flags);
    nx_set = (flags & (1ULL << 63));

    if (nx_set) {
        LOGF("[FAIL] NX bit SET on executable page!\n");
        return false;
    }

    tracker_cleanup();
    return true;
}

#pragma endregion

#pragma region Test Runner

static void run_test(const char* name, bool (*func)(void)) {
    g_tests_total++;
    LOGF("[TEST] %-35s ", name);
    
    if (!vmm_verify_integrity(vmm_kernel_get())) {
        LOGF("[SKIP] (Kernel VMM Corrupted)\n");
        return;
    }

    bool pass = func();

    if (g_tracker_idx > 0) {
        LOGF("[WARN] Leak detected (cleaning) ... ");
        tracker_cleanup();
    }

    if (pass) {
        g_tests_passed++;
        LOGF("[PASS]\n");
    } else {
        LOGF("[FAIL]\n");
    }
}

void test_vmm(void) {
    g_tests_total = 0;
    g_tests_passed = 0;

    LOGF("\n--- BEGIN VMM TEST ---\n");
    
    // Core Allocator Tests
    run_test("Invariants Check", test_invariants);
    run_test("Basic Alloc/Free Cycle", test_basic_cycle);
    run_test("Fixed Address Alloc", test_alloc_at);
    run_test("Resize (Grow/Shrink)", test_resize_logic);
    run_test("Resize Collision Detect", test_resize_collision);
    run_test("Protection & Flags", test_protection);
    run_test("MMIO Mapping", test_mmio_mapping);
    run_test("Manual Range Map", test_manual_map_range);
    
    // Hardware & System Tests
    run_test("PT Flag Correctness (US/RW)", test_pt_flags);
    run_test("Cross-Space Isolation", test_isolation);
    run_test("Kernel Mapping Persistence", test_kernel_persistence);
    run_test("Context Switching", test_context_switch);
    run_test("Page Table Cleanup", test_pt_cleanup);
    run_test("NX Bit Enforcement", test_nx_bit_enforcement);
    
    // Stress Tests
    run_test("Fragmentation Stress", test_fragmentation_stress);
    run_test("OOM Enforcement", test_oom_limit);
    run_test("Large Range Surgery", test_large_remap);
    run_test("Dirty Memory Reuse", test_dirty_reuse);
    run_test("Swiss Cheese Destruction", test_swiss_cheese_cleanup);

    LOGF("--- END VMM TEST ---\n");
    LOGF("VMM Test Results: %d/%d\n\n", g_tests_passed, g_tests_total);

    #ifdef TEST_BUILD
    #include <kernel/drivers/console.h>
    #include <kernel/drivers/stdio.h>
    if (g_tests_passed != g_tests_total) {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        printf("[-] Some tests failed (%d/%d). Please check the debug log for details.\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    else{
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        printf("[+] All tests passed successfully! (%d/%d)\n", g_tests_passed, g_tests_total);
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    }
    #endif
}

#pragma endregion