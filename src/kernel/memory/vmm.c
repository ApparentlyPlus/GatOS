/*
 * vmm.c - Virtual Memory Manager Implementation
 *
 * This implementation manages multiple virtual address spaces using vmm_t instances.
 * Each instance maintains its own page table and vm_object list. A special kernel VMM
 * can be accessed by passing NULL to most functions. This was hell to write :D
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/spinlock.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <klibc/avl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Magic numbers for validation and corruption detection
#define VMM_MAGIC 0xC0FFEEEE
#define VM_OBJECT_MAGIC 0xACCE55ED
#define VM_OBJECT_RED_ZONE 0xDEADC0DE

// Extended vm_object with validation
typedef struct vm_object_internal {
    uint32_t magic;
    uint32_t red_zone_pre;
    vm_object public;
    uint32_t red_zone_post;
    size_t   pg_size;
    uint64_t phys_base;
    avl_node_t vma_node;
} vm_object_internal;

// Extended VMM with validation
typedef struct {
    uint32_t magic;
    vmm_t public;
    bool is_kernel;
    avl_tree_t vma_tree; // VMA tree, sorted by base address
    spinlock_t lock;
} vmm_internal;

static vmm_internal* kernel_vmm = NULL;
static vmm_t* current_vmm = NULL;
static slab_cache_t* vmm_cache = NULL;
static slab_cache_t* vmo_cache = NULL;

#pragma region Validation Helpers

/*
 * vmm_validate - Validate VMM structure integrity
 */
static inline bool vmm_validate(vmm_internal* vmm) {
    if (!vmm) return false;

    if (vmm->magic != VMM_MAGIC) {
        LOGF("[VMM ERROR] Invalid VMM magic: 0x%x (expected 0x%x)\n",
             vmm->magic, VMM_MAGIC);
        return false;
    }

    return true;
}

/*
 * vm_object_validate - Validate vm_object structure integrity
 */
static inline bool vm_object_validate(vm_object_internal* obj) {
    if (!obj) return false;

    if (obj->magic != VM_OBJECT_MAGIC) {
        LOGF("[VMM ERROR] Invalid vm_object magic: 0x%x (expected 0x%x)\n",
             obj->magic, VM_OBJECT_MAGIC);
        return false;
    }

    if (obj->red_zone_pre != VM_OBJECT_RED_ZONE) {
        LOGF("[VMM ERROR] vm_object pre-red-zone corrupted: 0x%x\n",
             obj->red_zone_pre);
        return false;
    }

    if (obj->red_zone_post != VM_OBJECT_RED_ZONE) {
        LOGF("[VMM ERROR] vm_object post-red-zone corrupted: 0x%x\n",
             obj->red_zone_post);
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Internal Functions

/*
 * vma_cmp - Compare two VMA nodes
 */
static int vma_cmp(const avl_node_t* a, const avl_node_t* b) {
    uintptr_t ba = AVL_ENTRY(a, vm_object_internal, vma_node)->public.base;
    uintptr_t bb = AVL_ENTRY(b, vm_object_internal, vma_node)->public.base;
    if (ba < bb) return -1;
    if (ba > bb) return  1;
    return 0;
}

/*
 * vma_insert - Insert obj into the VMM's VMA tree and maintain public.next / public.objects
 */
static void vma_insert(vmm_internal* vmm, vm_object_internal* obj) {
    avl_insert(&vmm->vma_tree, &obj->vma_node);

    avl_node_t* nx = avl_next(&obj->vma_node);
    avl_node_t* pv = avl_prev(&obj->vma_node);

    obj->public.next = nx ? &AVL_ENTRY(nx, vm_object_internal, vma_node)->public : NULL;

    if (pv)
        AVL_ENTRY(pv, vm_object_internal, vma_node)->public.next = &obj->public;
    else
        vmm->public.objects = &obj->public;
}

/*
 * vma_remove - Remove obj from the VMM's VMA tree and repair public.next / public.objects
 */
static void vma_remove(vmm_internal* vmm, vm_object_internal* obj) {
    avl_node_t* pv = avl_prev(&obj->vma_node);

    if (pv)
        AVL_ENTRY(pv, vm_object_internal, vma_node)->public.next = obj->public.next;
    else
        vmm->public.objects = obj->public.next;

    avl_remove(&vmm->vma_tree, &obj->vma_node);
}

/*
 * vma_find_exact - Find the VMA with exact base address
 */
static vm_object_internal* vma_find_exact(vmm_internal* vmm, uintptr_t base) {
    vm_object_internal key = {0};
    key.public.base = base;
    avl_node_t* n = avl_find(&vmm->vma_tree, &key.vma_node);
    return n ? AVL_ENTRY(n, vm_object_internal, vma_node) : NULL;
}

/*
 * vma_find_containing - Find the VMA containing addr (base <= addr < base+length)
 */
static vm_object_internal* vma_find_containing(vmm_internal* vmm, uintptr_t addr) {
    vm_object_internal key = {0};
    key.public.base = addr;
    avl_node_t* n = avl_floor(&vmm->vma_tree, &key.vma_node);
    if (!n) return NULL;
    vm_object_internal* obj = AVL_ENTRY(n, vm_object_internal, vma_node);
    if (addr < obj->public.base + obj->public.length) return obj;
    return NULL;
}

/*
 * vma_find_gap - Find a gap of at least 'length' bytes in the VMM's address space, aligned to 'virt_align'
 */
static uintptr_t vma_find_gap(vmm_internal* vmm, size_t length, size_t virt_align) {
    uintptr_t cand = align_up(vmm->public.alloc_base, virt_align);

    while (cand && cand + length > cand && cand + length <= vmm->public.alloc_end) {
        vm_object_internal key = {0};
        key.public.base = cand;

        avl_node_t* fn = avl_floor(&vmm->vma_tree, &key.vma_node);
        if (fn) {
            vm_object_internal* f = AVL_ENTRY(fn, vm_object_internal, vma_node);
            uintptr_t fend = f->public.base + f->public.length;
            if (cand < fend) { cand = align_up(fend, virt_align); continue; }
        }

        avl_node_t* cn = avl_ceil(&vmm->vma_tree, &key.vma_node);
        if (!cn)
            break;

        vm_object_internal* c = AVL_ENTRY(cn, vm_object_internal, vma_node);
        if (cand + length <= c->public.base)
            break;

        cand = align_up(c->public.base + c->public.length, virt_align);
    }

    if (!cand || cand + length <= cand || cand + length > vmm->public.alloc_end)
        return 0;
    return cand;
}

/* 
 * vma_overlaps - Returns true if [start, start+length) overlaps any existing VMA
 */
static bool vma_overlaps(vmm_internal* vmm, uintptr_t start, size_t length) {
    vm_object_internal key = {0};
    key.public.base = start;

    avl_node_t* fn = avl_floor(&vmm->vma_tree, &key.vma_node);
    if (fn) {
        vm_object_internal* f = AVL_ENTRY(fn, vm_object_internal, vma_node);
        if (f->public.base + f->public.length > start) return true;
    }
    avl_node_t* cn = avl_ceil(&vmm->vma_tree, &key.vma_node);
    if (cn) {
        vm_object_internal* c = AVL_ENTRY(cn, vm_object_internal, vma_node);
        if (c->public.base < start + length) return true;
    }
    return false;
}

/*
 * vmm_get_instance - Get VMM instance (NULL means kernel VMM)
 */
static inline vmm_internal* vmm_get_instance(vmm_t* vmm) {
    if (vmm) {
        // public is embedded inside vmm_internal; recover the outer struct
        vmm_internal* internal =
            (vmm_internal*)((uint8_t*)vmm - offsetof(vmm_internal, public));
        if (!vmm_validate(internal)) return NULL;
        return internal;
    }

    // NULL means kernel VMM
    if (!vmm_validate(kernel_vmm)) return NULL;
    return kernel_vmm;
}

/*
 * vmm_convert_vm_flags - Convert VM_FLAG_* to architecture-specific page table flags
 */
static inline uint64_t vmm_convert_vm_flags(size_t vm_flags,
                                            bool is_kernel_vmm) {
    uint64_t pt_flags = PAGE_PRESENT;

    if (vm_flags & VM_FLAG_WRITE) {
        pt_flags |= PAGE_WRITABLE;
    }

    if (vm_flags & VM_FLAG_USER) {
        pt_flags |= PAGE_USER;
    }

    // For non-kernel address spaces, intermediate tables also need the user bit
    if (!is_kernel_vmm && (vm_flags & VM_FLAG_USER)) {
        pt_flags |= PAGE_USER;
    }

    if (!(vm_flags & VM_FLAG_EXEC) && cpu_is_feature_enabled(CPU_FEAT_NX)) {
         pt_flags |= PAGE_NO_EXECUTE;
    }

    return pt_flags;
}

/*
 * vmm_alloc_page_table - Allocate and zero a page table
 */
uint64_t vmm_alloc_page_table(void) {
    uint64_t phys = 0;
    if (pmm_alloc(PAGE_SIZE, &phys) != PMM_OK) {
        return 0;
    }

    uint64_t* table = (uint64_t*)PHYSMAP_P2V(phys);
    kmemset(table, 0, PAGE_SIZE);

    return phys;
}

/*
 * vmm_get_or_create_table - Get or create a page table entry
 */
uint64_t* vmm_get_or_create_table(uint64_t* parent_table, size_t index,
                                  bool create, bool set_user) {
    uint64_t entry = parent_table[index];

    if (entry & PAGE_PRESENT) {
        uint64_t table_phys = PT_ENTRY_ADDR(entry);
        return (uint64_t*)PHYSMAP_P2V(table_phys);
    }

    if (!create) {
        return NULL;
    }

    uint64_t new_table_phys = vmm_alloc_page_table();
    if (!new_table_phys) {
        return NULL;
    }

    uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
    if (set_user) {
        flags |= PAGE_USER;
    }

    parent_table[index] = (new_table_phys & ADDR_MASK) | flags;

    return (uint64_t*)PHYSMAP_P2V(new_table_phys);
}

/*
 * arch_map_page - Map a single page in the page tables (x86_64 version)
 */
vmm_status_t arch_map_page(uint64_t pt_root, uint64_t phys, void* virt,
                           uint64_t pt_flags, bool is_user_vmm) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    // intermediate tables should be marked user if final flags request it and
    // this is not kernel VMM
    bool set_user = is_user_vmm && (pt_flags & PAGE_USER);

    uint64_t* pdpt =
        vmm_get_or_create_table(pml4, PML4_INDEX(virt), true, set_user);
    if (!pdpt) return VMM_ERR_NO_MEMORY;

    uint64_t* pd =
        vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), true, set_user);
    if (!pd) return VMM_ERR_NO_MEMORY;

    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), true, set_user);
    if (!pt) return VMM_ERR_NO_MEMORY;

    size_t pt_index = PT_INDEX(virt);

    if (pt[pt_index] & PAGE_PRESENT) {
        return VMM_ERR_ALREADY_MAPPED;
    }

    pt[pt_index] = PT_ENTRY_ADDR(phys) | pt_flags;

    return VMM_OK;
}

/*
 * arch_map_huge_page - Map a single 2MB page in the page tables
 */
static vmm_status_t arch_map_huge_page(uint64_t pt_root, uint64_t phys, void* virt,
                                       uint64_t pt_flags, bool is_user_vmm) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);
    bool set_user = is_user_vmm && (pt_flags & PAGE_USER);

    uint64_t* pdpt = vmm_get_or_create_table(pml4, PML4_INDEX(virt), true, set_user);
    if (!pdpt) return VMM_ERR_NO_MEMORY;

    uint64_t* pd = vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), true, set_user);
    if (!pd) return VMM_ERR_NO_MEMORY;

    size_t pd_index = PD_INDEX(virt);
    if (pd[pd_index] & PAGE_PRESENT)
        return VMM_ERR_ALREADY_MAPPED;

    pd[pd_index] = (phys & ~(uint64_t)(PAGE_2MB - 1) & ADDR_MASK) | pt_flags | PAGE_HUGE;
    return VMM_OK;
}

/*
 * arch_unmap_page - Unmap a single 4KB or 2MB page from the page tables (x86_64 version)
 * Returns the physical base of the unmapped page, or 0 if not mapped.
 */
uint64_t arch_unmap_page(uint64_t pt_root, void* virt) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    uint64_t* pdpt =
        vmm_get_or_create_table(pml4, PML4_INDEX(virt), false, false);
    if (!pdpt) return 0;

    uint64_t* pd =
        vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false, false);
    if (!pd) return 0;

    uint64_t pde = pd[PD_INDEX(virt)];
    if (pde & PAGE_HUGE) {
        if (!(pde & PAGE_PRESENT)) return 0;
        uint64_t phys = pde & ~(uint64_t)(PAGE_2MB - 1) & ADDR_MASK;
        pd[PD_INDEX(virt)] = 0;
        invlpg(virt);

        if (vmm_table_is_empty(pd)) {
            uint64_t pd_phys = PHYSMAP_V2P((uint64_t)pd);
            pmm_free(pd_phys, PAGE_SIZE);
            pdpt[PDPT_INDEX(virt)] = 0;
            if (vmm_table_is_empty(pdpt)) {
                uint64_t pdpt_phys = PHYSMAP_V2P((uint64_t)pdpt);
                pmm_free(pdpt_phys, PAGE_SIZE);
                pml4[PML4_INDEX(virt)] = 0;
            }
        }
        return phys;
    }

    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false, false);
    if (!pt) return 0;

    size_t pt_index = PT_INDEX(virt);

    if (!(pt[pt_index] & PAGE_PRESENT)) {
        return 0;
    }

    uint64_t phys = PT_ENTRY_ADDR(pt[pt_index]);
    pt[pt_index] = 0;
    invlpg(virt);

    if (vmm_table_is_empty(pt)) {
        uint64_t pt_phys = PHYSMAP_V2P((uint64_t)pt);
        pmm_free(pt_phys, PAGE_SIZE);

        pd[PD_INDEX(virt)] = 0;

        if (vmm_table_is_empty(pd)) {
            uint64_t pd_phys = PHYSMAP_V2P((uint64_t)pd);
            pmm_free(pd_phys, PAGE_SIZE);

            pdpt[PDPT_INDEX(virt)] = 0;

            if (vmm_table_is_empty(pdpt)) {
                uint64_t pdpt_phys = PHYSMAP_V2P((uint64_t)pdpt);
                pmm_free(pdpt_phys, PAGE_SIZE);
                pml4[PML4_INDEX(virt)] = 0;
            }
        }
    }

    return phys;
}

/*
 * arch_update_page_flags - Update flags for an existing page mapping (in-place)
 */
vmm_status_t arch_update_page_flags(uint64_t pt_root, void* virt,
                                    uint64_t new_flags) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    uint64_t* pdpt =
        vmm_get_or_create_table(pml4, PML4_INDEX(virt), false, false);
    if (!pdpt) return VMM_ERR_NOT_FOUND;

    uint64_t* pd =
        vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false, false);
    if (!pd) return VMM_ERR_NOT_FOUND;

    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false, false);
    if (!pt) return VMM_ERR_NOT_FOUND;

    size_t pt_index = PT_INDEX(virt);

    if (!(pt[pt_index] & PAGE_PRESENT)) {
        return VMM_ERR_NOT_FOUND;
    }

    uint64_t phys = PT_ENTRY_ADDR(pt[pt_index]);
    pt[pt_index] = phys | new_flags;
    invlpg(virt);

    return VMM_OK;
}

/*
 * vmm_get_mapped_phys - Get physical address from virtual address
 */
bool vmm_get_mapped_phys(uint64_t pt_root, void* virt, uint64_t* out_phys) {
    if (!out_phys) return false;

    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    uint64_t* pdpt =
        vmm_get_or_create_table(pml4, PML4_INDEX(virt), false, false);
    if (!pdpt) return false;

    uint64_t* pd =
        vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false, false);
    if (!pd) return false;

    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false, false);
    if (!pt) return false;

    uint64_t entry = pt[PT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) {
        return false;
    }

    uint64_t page_base = PT_ENTRY_ADDR(entry);
    uint64_t offset = (uintptr_t)virt & (PAGE_SIZE - 1);
    *out_phys = page_base + offset;

    return true;
}

/*
 * vmm_alloc_vm_object - Allocate a vm_object structure with validation
 */
vm_object_internal* vmm_alloc_vm_object(void) {
    void* obj;
    if (slab_alloc(vmo_cache, &obj) != SLAB_OK)
        return NULL;

    vm_object_internal* internal = (vm_object_internal*)obj;
    kmemset(internal, 0, sizeof(vm_object_internal));

    internal->magic = VM_OBJECT_MAGIC;
    internal->red_zone_pre = VM_OBJECT_RED_ZONE;
    internal->red_zone_post = VM_OBJECT_RED_ZONE;
    internal->pg_size  = PAGE_SIZE;
    internal->phys_base = 0;

    return internal;
}

/*
 * vmm_free_vm_object - Free a vm_object structure
 */
void vmm_free_vm_object(vm_object_internal* obj) {
    if (!obj) return;

    if (!vm_object_validate(obj)) {
        LOGF("[VMM ERROR] Attempted to free corrupted vm_object at %p\n", obj);
        return;
    }

    // Clear validation fields to detect use-after-free in debug runs
    obj->magic = 0;
    obj->red_zone_pre = 0;
    obj->red_zone_post = 0;

    slab_free(vmo_cache, obj);
}

/*
 * vmm_destroy_page_table - Recursively free page tables
 */
void vmm_destroy_page_table(uint64_t table_phys, bool purge, int level) {
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(table_phys);

    if (purge && level > 1) {
        for (size_t i = 0; i < PAGE_ENTRIES; ++i) {
            uint64_t entry = table[i];
            if (!(entry & PAGE_PRESENT)) continue;

            uint64_t child_phys = PT_ENTRY_ADDR(entry);
            vmm_destroy_page_table(child_phys, purge, level - 1);
            table[i] = 0;
        }
    }

    // If purge asked or the table is empty, free its physical page
    if (purge || vmm_table_is_empty(table)) {
        pmm_free(table_phys, PAGE_SIZE);
    }
}

/*
 * vmm_copy_kernel_mappings - Copy kernel mappings from kernel VMM to a new page table
 */
static vmm_status_t vmm_copy_kernel_mappings(uint64_t dest_pt_root) {
    if (!kernel_vmm) return VMM_ERR_NOT_INIT;

    // PML4 kernel entries are static so no lock needed here
    uint64_t* src_pml4 = (uint64_t*)PHYSMAP_P2V(kernel_vmm->public.pt_root);
    uint64_t* dest_pml4 = (uint64_t*)PHYSMAP_P2V(dest_pt_root);

    // Entries 256-511 map 0xFFFF800000000000 and above
    for (size_t i = 256; i < PAGE_ENTRIES; ++i) {
        dest_pml4[i] = src_pml4[i];
    }

    return VMM_OK;
}

#pragma endregion

#pragma region Core Allocation/Deallocation

/*
 * vmm_alloc - Allocate a virtual memory range and back it with physical memory
 */
vmm_status_t vmm_alloc(vmm_t* vmm_pub, size_t length, size_t flags, void* arg,
                       void** out_addr) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (length == 0) return VMM_ERR_INVALID;
    if (!out_addr) return VMM_ERR_INVALID;

    *out_addr = NULL;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    if (flags & VM_FLAG_MMIO) {
        uint64_t mmio_phys = (uint64_t)arg;
        if (mmio_phys & (PAGE_SIZE - 1)) {
            LOGF("[VMM ERROR] MMIO address 0x%lx is not page-aligned\n",
                 mmio_phys);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NOT_ALIGNED;
        }
    }

    size_t orig_length = length;
    length = align_up(length, PAGE_SIZE);
    if (length < orig_length) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_OOM;
    }

    // Prefer 2MB-aligned virtual base for large allocations so huge pages can fire
    size_t virt_align = (!(flags & (VM_FLAG_MMIO | VM_FLAG_LAZY)) && length >= PAGE_2MB)
                        ? PAGE_2MB : PAGE_SIZE;

    uintptr_t found_base = vma_find_gap(vmm, length, virt_align);
    if (!found_base) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_OOM;
    }

    vm_object_internal* obj = vmm_alloc_vm_object();
    if (!obj) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NO_MEMORY;
    }

    obj->public.base   = found_base;
    obj->public.length = length;
    obj->public.flags  = flags;
    vma_insert(vmm, obj);

    if (flags & VM_FLAG_LAZY) {
        *out_addr = (void*)obj->public.base;
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_OK;
    }

    uint64_t phys_base = 0;
    if (flags & VM_FLAG_MMIO) {
        phys_base = (uint64_t)arg;
    } else {
        pmm_status_t status = pmm_alloc(length, &phys_base);
        if (status != PMM_OK) {
            vma_remove(vmm, obj);
            vmm_free_vm_object(obj);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NO_MEMORY;
        }
        kmemset((void*)PHYSMAP_P2V(phys_base), 0, length);
    }

    obj->phys_base = (flags & VM_FLAG_MMIO) ? 0 : phys_base;
    bool is_user_vmm  = !vmm->is_kernel;
    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);
    bool allow_huge   = !(flags & (VM_FLAG_MMIO | VM_FLAG_LAZY));
    bool any_huge     = false;

    size_t offset = 0;
    while (offset < length) {
        uintptr_t virt_cur = obj->public.base + offset;
        uint64_t  phys_cur = phys_base + offset;
        size_t    remain   = length - offset;
        bool use_2mb = allow_huge &&
                       (virt_cur & (PAGE_2MB - 1)) == 0 &&
                       (phys_cur & (PAGE_2MB - 1)) == 0 &&
                       remain >= PAGE_2MB;
        size_t step = use_2mb ? PAGE_2MB : PAGE_SIZE;

        vmm_status_t ms = use_2mb
            ? arch_map_huge_page(vmm->public.pt_root, phys_cur,
                                 (void*)virt_cur, pt_flags, is_user_vmm)
            : arch_map_page(vmm->public.pt_root, phys_cur,
                            (void*)virt_cur, pt_flags, is_user_vmm);

        if (ms != VMM_OK) {
            for (size_t rb = 0; rb < offset; rb += PAGE_SIZE)
                arch_unmap_page(vmm->public.pt_root, (void*)(obj->public.base + rb));
            if (!(flags & VM_FLAG_MMIO)) pmm_free(phys_base, length);
            vma_remove(vmm, obj);
            vmm_free_vm_object(obj);
            spinlock_release(&vmm->lock, lock_flags);
            return ms;
        }

        if (use_2mb) any_huge = true;
        offset += step;
    }

    // pg_size > PAGE_SIZE means "has huge pages"
    obj->pg_size = any_huge ? PAGE_2MB : PAGE_SIZE;

    *out_addr = (void*)obj->public.base;
    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_alloc_at - Allocate virtual memory at a specific address
 */
vmm_status_t vmm_alloc_at(vmm_t* vmm_pub, void* desired_addr, size_t length,
                          size_t flags, void* arg, void** out_addr) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (length == 0) return VMM_ERR_INVALID;
    if (!out_addr) return VMM_ERR_INVALID;
    if (!desired_addr) return VMM_ERR_INVALID;

    *out_addr = NULL;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    uintptr_t desired = (uintptr_t)desired_addr;
    if (desired & (PAGE_SIZE - 1)) {
        LOGF("[VMM] vmm_alloc_at: address 0x%lx not page-aligned\n", desired);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_ALIGNED;
    }

    size_t orig_length = length;
    length = align_up(length, PAGE_SIZE);
    if (length < orig_length) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_OOM;
    }

    if (length > (UINTPTR_MAX - desired) ||
        desired < vmm->public.alloc_base ||
        desired + length > vmm->public.alloc_end) {
        LOGF("[VMM] vmm_alloc_at: range 0x%lx-0x%lx outside allocatable space\n",
            desired, desired + length);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_OOM;
    }

    if (flags & VM_FLAG_MMIO) {
        uint64_t mmio_phys = (uint64_t)arg;
        if (mmio_phys & (PAGE_SIZE - 1)) {
            LOGF("[VMM] vmm_alloc_at: MMIO address 0x%lx not page-aligned\n",
                 mmio_phys);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NOT_ALIGNED;
        }
    }

    if (vma_overlaps(vmm, desired, length)) {
        LOGF("[VMM] vmm_alloc_at: range 0x%lx-0x%lx overlaps with existing object\n",
             desired, desired + length);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_ALREADY_MAPPED;
    }

    vm_object_internal* obj = vmm_alloc_vm_object();
    if (!obj) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NO_MEMORY;
    }

    obj->public.base   = desired;
    obj->public.length = length;
    obj->public.flags  = flags;
    vma_insert(vmm, obj);

    uint64_t phys_base = 0;
    if (flags & VM_FLAG_MMIO) {
        phys_base = (uint64_t)arg;
    } else {
        pmm_status_t pmm_status = pmm_alloc(length, &phys_base);
        if (pmm_status != PMM_OK) {
            vma_remove(vmm, obj);
            vmm_free_vm_object(obj);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NO_MEMORY;
        }
    }

    obj->phys_base = (flags & VM_FLAG_MMIO) ? 0 : phys_base;
    bool is_user_vmm  = !vmm->is_kernel;
    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);
    bool allow_huge   = !(flags & (VM_FLAG_MMIO | VM_FLAG_LAZY));
    bool any_huge     = false;

    size_t offset = 0;
    while (offset < length) {
        uintptr_t virt_cur = desired + offset;
        uint64_t  phys_cur = phys_base + offset;
        size_t    remain   = length - offset;
        bool use_2mb = allow_huge &&
                       (virt_cur & (PAGE_2MB - 1)) == 0 &&
                       (phys_cur & (PAGE_2MB - 1)) == 0 &&
                       remain >= PAGE_2MB;
        size_t step = use_2mb ? PAGE_2MB : PAGE_SIZE;

        vmm_status_t ms = use_2mb
            ? arch_map_huge_page(vmm->public.pt_root, phys_cur,
                                 (void*)virt_cur, pt_flags, is_user_vmm)
            : arch_map_page(vmm->public.pt_root, phys_cur,
                            (void*)virt_cur, pt_flags, is_user_vmm);

        if (ms != VMM_OK) {
            for (size_t rb = 0; rb < offset; rb += PAGE_SIZE)
                arch_unmap_page(vmm->public.pt_root, (void*)(desired + rb));
            if (!(flags & VM_FLAG_MMIO)) pmm_free(phys_base, length);
            vma_remove(vmm, obj);
            vmm_free_vm_object(obj);
            spinlock_release(&vmm->lock, lock_flags);
            return ms;
        }

        if (use_2mb) any_huge = true;
        offset += step;
    }

    obj->pg_size = any_huge ? PAGE_2MB : PAGE_SIZE;

    *out_addr = desired_addr;
    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_free - Free a previously allocated virtual memory range
 */
vmm_status_t vmm_free(vmm_t* vmm_pub, void* addr) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    vm_object_internal* cur = vma_find_exact(vmm, (uintptr_t)addr);
    if (!cur) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }
    if (!vm_object_validate(cur)) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_INVALID;
    }

    for (uintptr_t virt = cur->public.base;
         virt < cur->public.base + cur->public.length; virt += PAGE_SIZE) {
        arch_unmap_page(vmm->public.pt_root, (void*)virt);
    }
    if (cur->phys_base && !(cur->public.flags & VM_FLAG_MMIO))
        pmm_free(cur->phys_base, cur->public.length);

    vma_remove(vmm, cur);
    vmm_free_vm_object(cur);

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

#pragma endregion

#pragma region Non Kernel VMM Instance Management

/*
 * vmm_create - Create a new VMM instance
 */
vmm_t* vmm_create(uintptr_t alloc_base, uintptr_t alloc_end) {
    if (alloc_end <= alloc_base) return NULL;
    
    alloc_base = align_up(alloc_base, PAGE_SIZE);
    alloc_end = align_down(alloc_end, PAGE_SIZE);

    if (alloc_end <= alloc_base) return NULL;

    if(!pmm_is_initialized()) {
        LOGF("[VMM] The PMM must be online first\n");
        return NULL;
    }

    if(!slab_is_initialized()){
        LOGF("[VMM] The Slab Allocator must be online first\n");
        return NULL;
    }

    void* vmm_mem;
    if (slab_alloc(vmm_cache, &vmm_mem) != SLAB_OK) {
        return NULL;
    }

    vmm_internal* vmm = (vmm_internal *)vmm_mem;
    kmemset(vmm, 0, sizeof(vmm_internal));

    vmm->magic = VMM_MAGIC;
    vmm->is_kernel = false;
    avl_init(&vmm->vma_tree, vma_cmp);
    spinlock_init(&vmm->lock, "user_vmm");

    uint64_t pt_root = vmm_alloc_page_table();
    if (!pt_root) {
        slab_free(vmm_cache, vmm_mem);
        return NULL;
    }

    if (kernel_vmm) {
        vmm_status_t status = vmm_copy_kernel_mappings(pt_root);
        if (status != VMM_OK) {
            pmm_free(pt_root, PAGE_SIZE);
            slab_free(vmm_cache, vmm_mem);
            return NULL;
        }
    }
    
    vmm->public.pt_root = pt_root;
    vmm->public.objects = NULL;
    vmm->public.alloc_base = alloc_base;
    vmm->public.alloc_end = alloc_end;

    LOGF("[VMM] User VMM initialized, managing 0x%lx - 0x%lx (%zu MiB)\n",
           alloc_base, alloc_end, (alloc_end - alloc_base)/MEASUREMENT_UNIT_MB);
    
    return &vmm->public;
}

/*
 * vmm_destroy - Destroy a VMM instance and free all resources
 */
void vmm_destroy(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    if (vmm == kernel_vmm) {
        LOGF("[VMM ERROR] Cannot destroy kernel VMM\n");
        return;
    }

    // Acquiring the lock prevents any concurrent alloc from racing teardown
    bool lock_flags = spinlock_acquire(&vmm->lock);

    avl_node_t* n = avl_min(&vmm->vma_tree);
    while (n) {
        vm_object_internal* cur = AVL_ENTRY(n, vm_object_internal, vma_node);
        if (!vm_object_validate(cur)) {
            LOGF("[VMM ERROR] Corrupted vm_object during destroy\n");
            break;
        }
        avl_node_t* nx = avl_next(n);
        if (cur->phys_base && !(cur->public.flags & VM_FLAG_MMIO))
            pmm_free(cur->phys_base, cur->public.length);
        vmm_free_vm_object(cur);
        n = nx;
    }
    vmm->vma_tree.root = NULL;

    // Only free lower half (entries 0-255); DO NOT touch kernel mappings (256-511)
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(vmm->public.pt_root);

    for (size_t i = 0; i < 256; ++i) {
        uint64_t entry = pml4[i];
        if (!(entry & PAGE_PRESENT)) continue;

        uint64_t pdpt_phys = PT_ENTRY_ADDR(entry);
        vmm_destroy_page_table(pdpt_phys, true, 3);
        pml4[i] = 0;
    }

    pmm_free(vmm->public.pt_root, PAGE_SIZE);

    // Clear magic to help detect use-after-free
    vmm->magic = 0;

    spinlock_release(&vmm->lock, lock_flags);

    slab_free(vmm_cache, vmm);

    LOGF("[VMM] User VMM Destroyed\n");
}

/*
 * vmm_switch - Switch to a different address space
 */
void vmm_switch(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    PML4_switch(vmm->public.pt_root);
    current_vmm = &vmm->public;
}

/*
 * vmm_get_current - Get the currently active VMM instance
 */
vmm_t* vmm_get_current(void) {
    // If not explicitly set yet, assume kernel VMM
    if (!current_vmm && kernel_vmm) {
        return &kernel_vmm->public;
    }
    return current_vmm;
}

#pragma endregion

#pragma region Kernel VMM Management

/*
 * vmm_kernel_init - Initialize the kernel VMM specifically
 */
vmm_status_t vmm_kernel_init(uintptr_t alloc_base, uintptr_t alloc_end) {
    if (kernel_vmm) return VMM_ERR_ALREADY_INIT;

    if (!pmm_is_initialized()) {
        LOGF("[VMM] The PMM must be online first\n");
        return VMM_ERR_NOT_INIT;
    }

    if (!slab_is_initialized()) {
        LOGF("[VMM] The Slab allocator must be online first\n");
        return VMM_ERR_NOT_INIT;
    }

    if(cpu_has_feature(CPU_FEAT_NX)){
        cpu_enable_feature(CPU_FEAT_NX);
    }

    // Kernel VMM is allocated from PMM directly for a stable physical address
    uint64_t vmm_phys = 0;
    if (pmm_alloc(sizeof(vmm_internal), &vmm_phys) != PMM_OK) {
        return VMM_ERR_NO_MEMORY;
    }

    vmm_internal* vmm = (vmm_internal*)PHYSMAP_P2V(vmm_phys);
    kmemset(vmm, 0, sizeof(vmm_internal));

    vmm->magic = VMM_MAGIC;
    vmm->is_kernel = true;
    avl_init(&vmm->vma_tree, vma_cmp);
    spinlock_init(&vmm->lock, "kernel_vmm");

    vmm->public.pt_root = (uint64_t)KERNEL_V2P(getPML4());
    vmm->public.objects = NULL;
    vmm->public.alloc_base = alloc_base;
    vmm->public.alloc_end = alloc_end;

    kernel_vmm = vmm;
    current_vmm = &kernel_vmm->public;

    vmm_cache = slab_cache_create("vmm_internal", sizeof(vmm_internal), _Alignof(vmm_internal));
    vmo_cache = slab_cache_create("vm_object_internal", sizeof(vm_object_internal), _Alignof(vm_object_internal));

    if (!vmm_cache || !vmo_cache) {
        LOGF("[VMM] Failed to create slab caches\n");
        return VMM_ERR_NO_MEMORY;
    }

    LOGF("[VMM] Kernel VMM initialized, managing 0x%lx - 0x%lx (%zu MiB)\n",
         alloc_base, alloc_end, (alloc_end - alloc_base) / MEASUREMENT_UNIT_MB);

    return VMM_OK;
}

/*
 * vmm_kernel_get - Get the kernel VMM instance
 */
vmm_t* vmm_kernel_get(void) {
    return kernel_vmm ? &kernel_vmm->public : NULL;
}

#pragma endregion

#pragma region Introspection

/*
 * vmm_get_alloc_base - Get the base of the allocatable range
 */
uintptr_t vmm_get_alloc_base(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return 0;
    return vmm->public.alloc_base;
}

/*
 * vmm_get_alloc_end - Get the end of the allocatable range
 */
uintptr_t vmm_get_alloc_end(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return 0;
    return vmm->public.alloc_end;
}

/*
 * vmm_get_alloc_size - Get the size of the allocatable range
 */
size_t vmm_get_alloc_size(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return 0;
    return vmm->public.alloc_end - vmm->public.alloc_base;
}

/*
 * vmm_table_is_empty - Check if a page table is empty (no present entries)
 */
bool vmm_table_is_empty(uint64_t* table) {
    for (size_t i = 0; i < PAGE_ENTRIES; ++i) {
        if (table[i] & PAGE_PRESENT) return false;
    }
    return true;
}

#pragma endregion

#pragma region Address Translation and Query

/*
 * vmm_get_physical - Get the physical address mapped to a virtual address
 */
bool vmm_get_physical(vmm_t* vmm_pub, void* virt, uint64_t* out_phys) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return false;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    bool result = vmm_get_mapped_phys(vmm->public.pt_root, virt, out_phys);
    spinlock_release(&vmm->lock, lock_flags);
    return result;
}

/*
 * vmm_find_mapped_object - Find vm_object containing a virtual address
 */
vm_object* vmm_find_mapped_object(vmm_t* vmm_pub, void* addr) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm || !addr) return NULL;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    vm_object_internal* cur = vma_find_containing(vmm, (uintptr_t)addr);
    if (cur && !vm_object_validate(cur)) {
        LOGF("[VMM ERROR] Corrupted vm_object in list\n");
        spinlock_release(&vmm->lock, lock_flags);
        return NULL;
    }
    spinlock_release(&vmm->lock, lock_flags);
    return cur ? &cur->public : NULL;
}

/*
 * vmm_check_flags - Check if a specific address has specific flags
 */
bool vmm_check_flags(vmm_t* vmm_pub, void* addr, size_t required_flags) {
    vm_object* obj = vmm_find_mapped_object(vmm_pub, addr);
    if (!obj) return false;

    return (obj->flags & required_flags) == required_flags;
}

/*
 * vmm_walk_pte - Walk the page table hierarchy rooted at pt_root and return
 * the leaf PTE entry for the given virtual address.
 */
static uint64_t vmm_walk_pte(uint64_t pt_root, void* virt) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    uint64_t* pdpt = vmm_get_or_create_table(pml4, PML4_INDEX(virt), false, false);
    if (!pdpt) return 0;

    uint64_t* pd = vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false, false);
    if (!pd) return 0;

    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false, false);
    if (!pt) return 0;

    return pt[PT_INDEX(virt)];
}

/*
 * vmm_check_buffer - Check that every page of [ptr, ptr+size) is mapped in
 * the hardware page tables with the requested VM_FLAG_* permissions.
 */
bool vmm_check_buffer(vmm_t* vmm_pub, const void* ptr, size_t size, size_t required_flags) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm || !ptr || size == 0) return false;

    uintptr_t start = (uintptr_t)ptr;
    if (size > (UINTPTR_MAX - start)) return false;
    uintptr_t end   = start + size;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    uintptr_t current = start & ~(uintptr_t)(PAGE_SIZE - 1);
    vm_object_internal* last_obj = NULL;

    while (current < end) {
        if (!last_obj || current < last_obj->public.base ||
            current >= last_obj->public.base + last_obj->public.length) {
            last_obj = vma_find_containing(vmm, current);
        }

        if (!last_obj) {
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        if ((last_obj->public.flags & required_flags) != required_flags) {
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        uint64_t pte = vmm_walk_pte(vmm->public.pt_root, (void*)current);

        if (!(pte & PAGE_PRESENT)) {
            // Non-lazy objects must have all pages present in hardware tables
            if (!(last_obj->public.flags & VM_FLAG_LAZY)) {
                spinlock_release(&vmm->lock, lock_flags);
                return false;
            }
        } else {
            if ((required_flags & VM_FLAG_WRITE) && !(pte & PAGE_WRITABLE)) {
                spinlock_release(&vmm->lock, lock_flags);
                return false;
            }
            if ((required_flags & VM_FLAG_USER) && !(pte & PAGE_USER)) {
                spinlock_release(&vmm->lock, lock_flags);
                return false;
            }
        }

        current += PAGE_SIZE;
    }

    spinlock_release(&vmm->lock, lock_flags);
    return true;
}

#pragma endregion

#pragma region Page Table Manipulation

/*
 * vmm_map_page - Maps a physical address to the specified virtual address
 */
vmm_status_t vmm_map_page(vmm_t* vmm_pub, uint64_t phys, void* virt,
                          size_t flags) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    if ((phys & (PAGE_SIZE - 1)) || ((uintptr_t)virt & (PAGE_SIZE - 1))) {
        return VMM_ERR_NOT_ALIGNED;
    }

    bool lock_flags = spinlock_acquire(&vmm->lock);

    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);
    bool is_user_vmm = !vmm->is_kernel;

    vmm_status_t status =
        arch_map_page(vmm->public.pt_root, phys, virt, pt_flags, is_user_vmm);
    spinlock_release(&vmm->lock, lock_flags);
    return status;
}

/*
 * vmm_unmap_page - Unmaps a virtual page and handles cleanup
 */
vmm_status_t vmm_unmap_page(vmm_t* vmm_pub, void* virt) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    arch_unmap_page(vmm->public.pt_root, virt);
    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_map_range - Maps a physical range to a virtual range starting at a specific virtual address
 */
vmm_status_t vmm_map_range(vmm_t* vmm_pub, uint64_t phys, void* virt,
                           size_t length, size_t flags) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    length = align_up(length, PAGE_SIZE);

    bool lock_flags = spinlock_acquire(&vmm->lock);

    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);
    bool is_user_vmm = !vmm->is_kernel;

    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        vmm_status_t status = arch_map_page(vmm->public.pt_root, phys + offset,
                                            (void*)((uintptr_t)virt + offset),
                                            pt_flags, is_user_vmm);

        if (status != VMM_OK) {
            for (size_t rollback = 0; rollback < offset;
                 rollback += PAGE_SIZE) {
                arch_unmap_page(vmm->public.pt_root,
                                (void*)((uintptr_t)virt + rollback));
            }
            spinlock_release(&vmm->lock, lock_flags);
            return status;
        }
    }

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_unmap_range - Unmaps a virtual range, starting at a specific virtual address
 */
vmm_status_t vmm_unmap_range(vmm_t* vmm_pub, void* virt, size_t length) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    length = align_up(length, PAGE_SIZE);

    bool lock_flags = spinlock_acquire(&vmm->lock);

    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        arch_unmap_page(vmm->public.pt_root, (void*)((uintptr_t)virt + offset));
    }

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_resize - Resize an existing virtual memory region
 */
vmm_status_t vmm_resize(vmm_t* vmm_pub, void* addr, size_t new_length) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;
    if (new_length == 0) return VMM_ERR_INVALID;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    new_length = align_up(new_length, PAGE_SIZE);

    vm_object_internal* cur = vma_find_exact(vmm, (uintptr_t)addr);
    if (!cur || !vm_object_validate(cur)) {
        LOGF("[VMM ERROR] vmm_resize: No object found at address 0x%lx\n",
             (uintptr_t)addr);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    if ((cur->public.flags & VM_FLAG_MMIO) || cur->pg_size > PAGE_SIZE) {
        LOGF("[VMM ERROR] vmm_resize: Cannot resize MMIO or huge page region\n");
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_INVALID;
    }

    size_t old_length = cur->public.length;

    if (new_length == old_length) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_OK;
    }

    if (new_length > old_length) {
        size_t growth = new_length - old_length;
        uintptr_t new_end = cur->public.base + new_length;

        avl_node_t* nx = avl_next(&cur->vma_node);
        vm_object_internal* next_obj = nx ? AVL_ENTRY(nx, vm_object_internal, vma_node) : NULL;
        if (next_obj) {
            if (new_end > next_obj->public.base) {
                LOGF(
                    "[VMM ERROR] vmm_resize: Growth would overlap with next "
                    "object\n");
                spinlock_release(&vmm->lock, lock_flags);
                return VMM_ERR_OOM;
            }
        } else {
            if (new_end > vmm->public.alloc_end) {
                LOGF(
                    "[VMM ERROR] vmm_resize: Growth exceeds allocation "
                    "range\n");
                spinlock_release(&vmm->lock, lock_flags);
                return VMM_ERR_OOM;
            }
        }

        uint64_t phys_base = 0;
        pmm_status_t pmm_status = pmm_alloc(growth, &phys_base);
        if (pmm_status != PMM_OK) {
            LOGF(
                "[VMM ERROR] vmm_resize: Failed to allocate %zu bytes of "
                "physical memory\n",
                growth);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NO_MEMORY;
        }

        bool is_user_vmm = !vmm->is_kernel;
        uint64_t pt_flags = vmm_convert_vm_flags(cur->public.flags, vmm->is_kernel);
        size_t mapped_offset = 0;

        for (size_t offset = 0; offset < growth; offset += PAGE_SIZE) {
            vmm_status_t map_status =
                arch_map_page(vmm->public.pt_root, phys_base + offset,
                              (void*)(cur->public.base + old_length + offset),
                              pt_flags, is_user_vmm);

            if (map_status != VMM_OK) {
                LOGF("[VMM ERROR] vmm_resize: Mapping failed at offset 0x%lx\n",
                     offset);

                for (size_t rb = 0; rb < offset; rb += PAGE_SIZE) {
                    arch_unmap_page(
                        vmm->public.pt_root,
                        (void*)(cur->public.base + old_length + rb));
                }

                pmm_free(phys_base, growth);

                spinlock_release(&vmm->lock, lock_flags);
                return map_status;
            }

            mapped_offset = offset + PAGE_SIZE;
        }

        cur->public.length = new_length;
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_OK;
    }

    else {
        size_t shrinkage = old_length - new_length;
        uintptr_t shrink_start = cur->public.base + new_length;

        for (uintptr_t virt = shrink_start; virt < shrink_start + shrinkage;
             virt += PAGE_SIZE) {
            arch_unmap_page(vmm->public.pt_root, (void*)virt);
        }

        if (cur->phys_base && !(cur->public.flags & VM_FLAG_MMIO))
            pmm_free(cur->phys_base + new_length, shrinkage);

        cur->public.length = new_length;
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_OK;
    }
}

#pragma endregion

#pragma region Protection

/*
 * vmm_protect - Change the permission flags of a specific virtual address
 *
 * This improved version uses in-place flag updates instead of unmap+remap,
 * which is significantly more efficient.
 */
vmm_status_t vmm_protect(vmm_t* vmm_pub, void* addr, size_t new_flags) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    vm_object_internal* cur = vma_find_containing(vmm, (uintptr_t)addr);
    vm_object* obj = cur ? &cur->public : NULL;

    if (!obj) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    if (obj->base != (uintptr_t)addr) {
        LOGF("[VMM ERROR] vmm_protect requires exact base address match\n");
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_INVALID;
    }

    obj->flags = new_flags;

    uint64_t pt_flags = vmm_convert_vm_flags(new_flags, vmm->is_kernel);

    for (uintptr_t virt = obj->base; virt < obj->base + obj->length;
         virt += PAGE_SIZE) {
        vmm_status_t status =
            arch_update_page_flags(vmm->public.pt_root, (void*)virt, pt_flags);
        if (status != VMM_OK) {
            LOGF("[VMM WARNING] Failed to update flags for page at 0x%lx\n",
                 virt);
        }
    }

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

#pragma endregion

#pragma region Debugging

/*
 * vmm_dump - Dumps the current VMM layout
 */
void vmm_dump(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    LOGF("\n=== VMM Dump ===\n");
    LOGF("VMM at %p (magic: 0x%x, is_kernel: %d)\n", vmm, vmm->magic,
         vmm->is_kernel);
    LOGF("Alloc range: 0x%lx - 0x%lx (size: 0x%lx)\n", vmm->public.alloc_base,
         vmm->public.alloc_end, vmm->public.alloc_end - vmm->public.alloc_base);
    LOGF("Page table root (phys): 0x%lx\n", vmm->public.pt_root);
    LOGF("\nVM Objects:\n");

    avl_node_t* it = avl_min(&vmm->vma_tree);
    int count = 0;

    while (it) {
        vm_object_internal* current = AVL_ENTRY(it, vm_object_internal, vma_node);
        if (!vm_object_validate(current)) {
            LOGF("[CORRUPTED OBJECT AT INDEX %d]\n", count);
            break;
        }
        LOGF("  [%d] base=0x%016lx, length=0x%08lx, flags=0x%02lx", count,
             current->public.base, current->public.length, current->public.flags);
        count++;
        it = avl_next(it);
    }

    if (count == 0) LOGF("  (no objects)\n");
    LOGF("Total objects: %d\n", count);
    LOGF("================\n");

    spinlock_release(&vmm->lock, lock_flags);
}

/*
 * vmm_stats - Get the number of pages handled by the VMM and the number of mapped pages
 */
void vmm_stats(vmm_t* vmm_pub, size_t* out_total, size_t* out_resident) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    size_t total = 0;
    size_t resident = 0;

    avl_node_t* it = avl_min(&vmm->vma_tree);
    while (it) {
        vm_object_internal* current = AVL_ENTRY(it, vm_object_internal, vma_node);
        if (!vm_object_validate(current)) {
            LOGF("[VMM ERROR] Corrupted vm_object during stats\n");
            break;
        }
        total += current->public.length;
        for (uintptr_t virt = current->public.base;
             virt < current->public.base + current->public.length; virt += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_get_mapped_phys(vmm->public.pt_root, (void*)virt, &phys))
                resident += PAGE_SIZE;
        }
        it = avl_next(it);
    }

    if (out_total) *out_total = total;
    if (out_resident) *out_resident = resident;

    spinlock_release(&vmm->lock, lock_flags);
}

/*
 * vmm_dump_pte_chain - Dumps the page table entries to get to the specified virtual address
 */
void vmm_dump_pte_chain(uint64_t pt_root, void* virt) {
    // Takes pt_root directly - no VMM lock needed.
    uint64_t v = (uint64_t)virt;
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    LOGF("Dumping PTE chain for virt=0x%lx (pt_root phys=0x%lx)\n", v, pt_root);
    size_t i;

    i = PML4_INDEX(virt);
    uint64_t e = pml4[i];
    LOGF("PML4[%3zu] = 0x%016lx\n", i, e);
    if (!(e & PAGE_PRESENT)) return;

    uint64_t pdpt_phys = PT_ENTRY_ADDR(e);
    uint64_t* pdpt = (uint64_t*)PHYSMAP_P2V(pdpt_phys);

    i = PDPT_INDEX(virt);
    e = pdpt[i];
    LOGF("PDPT[%3zu] = 0x%016lx\n", i, e);
    if (!(e & PAGE_PRESENT)) return;

    uint64_t pd_phys = PT_ENTRY_ADDR(e);
    uint64_t* pd = (uint64_t*)PHYSMAP_P2V(pd_phys);

    i = PD_INDEX(virt);
    e = pd[i];
    LOGF("PD  [%3zu] = 0x%016lx\n", i, e);
    if (!(e & PAGE_PRESENT)) return;

    uint64_t pt_phys = PT_ENTRY_ADDR(e);
    uint64_t* pt = (uint64_t*)PHYSMAP_P2V(pt_phys);

    i = PT_INDEX(virt);
    e = pt[i];
    LOGF("PT  [%3zu] = 0x%016lx\n", i, e);

    if (e & PAGE_PRESENT) {
        uint64_t phys = PT_ENTRY_ADDR(e);
        uint64_t offset = (uintptr_t)virt & 0xFFF;
        LOGF("Physical address: 0x%lx\n", phys + offset);
    }
}

/*
 * vmm_verify_integrity - Verify integrity of VMM and all vm_objects
 */
bool vmm_verify_integrity(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) {
        LOGF("[VMM VERIFY] Failed to get VMM instance\n");
        return false;
    }

    bool lock_flags = spinlock_acquire(&vmm->lock);

    LOGF("[VMM VERIFY] Checking VMM at %p\n", vmm);

    if (!vmm_validate(vmm)) {
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }

    if (vmm->public.alloc_end <= vmm->public.alloc_base) {
        LOGF("[VMM VERIFY] Invalid alloc range: 0x%lx - 0x%lx\n",
             vmm->public.alloc_base, vmm->public.alloc_end);
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }

    if (!vmm->public.pt_root) {
        LOGF("[VMM VERIFY] NULL page table root\n");
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }

    avl_node_t* it = avl_min(&vmm->vma_tree);
    vm_object_internal* prev = NULL;
    int count = 0;

    while (it) {
        vm_object_internal* current = AVL_ENTRY(it, vm_object_internal, vma_node);

        if (!vm_object_validate(current)) {
            LOGF("[VMM VERIFY] Object %d failed validation\n", count);
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        if (current->public.base & (PAGE_SIZE - 1)) {
            LOGF("[VMM VERIFY] Object %d: unaligned base 0x%lx\n", count,
                 current->public.base);
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        if (current->public.length & (PAGE_SIZE - 1)) {
            LOGF("[VMM VERIFY] Object %d: unaligned length 0x%lx\n", count,
                 current->public.length);
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        if (current->public.base < vmm->public.alloc_base ||
            current->public.base + current->public.length > vmm->public.alloc_end) {
            LOGF("[VMM VERIFY] Object %d: out of bounds (0x%lx - 0x%lx)\n",
                 count, current->public.base,
                 current->public.base + current->public.length);
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        if (prev) {
            uintptr_t prev_end = prev->public.base + prev->public.length;
            if (current->public.base < prev_end) {
                LOGF("[VMM VERIFY] Object %d overlaps with previous (0x%lx < 0x%lx)\n",
                     count, current->public.base, prev_end);
                spinlock_release(&vmm->lock, lock_flags);
                return false;
            }
        }

        prev = current;
        it = avl_next(it);
        count++;
    }

    LOGF("[VMM VERIFY] All checks passed (%d objects)\n", count);
    spinlock_release(&vmm->lock, lock_flags);
    return true;
}

#pragma endregion