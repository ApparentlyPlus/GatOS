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
#include <libc/string.h>
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
    struct vm_object_internal* next_internal;
} vm_object_internal;

// Extended VMM with validation
typedef struct {
    uint32_t magic;
    vmm_t public;
    bool is_kernel;
    vm_object_internal* objects_internal;
    spinlock_t lock;
} vmm_internal;

// Global kernel VMM
static vmm_internal* g_kernel_vmm = NULL;

// Slab caches for VMM internal structures
static slab_cache_t* g_vmm_internal_cache = NULL;
static slab_cache_t* g_vm_object_internal_cache = NULL;

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
 * vmm_get_instance - Get VMM instance (NULL means kernel VMM)
 */
static inline vmm_internal* vmm_get_instance(vmm_t* vmm) {
    // If the caller passed a public pointer, convert to internal
    if (vmm) {
        // turn public->internal pointer: public is embedded inside vmm_internal
        vmm_internal* internal =
            (vmm_internal*)((uint8_t*)vmm - offsetof(vmm_internal, public));
        if (!vmm_validate(internal)) return NULL;
        return internal;
    }

    // NULL means kernel VMM
    if (!vmm_validate(g_kernel_vmm)) return NULL;
    return g_kernel_vmm;
}

/*
 * vmm_convert_vm_flags - Convert VM_FLAG_* to architecture-specific page table flags
 */
static inline uint64_t vmm_convert_vm_flags(size_t vm_flags,
                                            bool is_kernel_vmm) {
    // Start with present bit always set for mapped pages
    uint64_t pt_flags = PAGE_PRESENT;

    // Writable?
    if (vm_flags & VM_FLAG_WRITE) {
        pt_flags |= PAGE_WRITABLE;
    }

    // User-accessible?
    if (vm_flags & VM_FLAG_USER) {
        pt_flags |= PAGE_USER;
    }

    // For non-kernel address spaces, make sure intermediate tables also get the user bit 
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
    memset(table, 0, PAGE_SIZE);

    return phys;
}

/*
 * vmm_get_or_create_table - Get or create a page table entry
 */
uint64_t* vmm_get_or_create_table(uint64_t* parent_table, size_t index,
                                  bool create, bool set_user) {
    uint64_t entry = parent_table[index];

    // If present, return existing table virtual address
    if (entry & PAGE_PRESENT) {
        uint64_t table_phys = PT_ENTRY_ADDR(entry);
        return (uint64_t*)PHYSMAP_P2V(table_phys);
    }

    // Not present: if caller didn't ask to create, return NULL
    if (!create) {
        return NULL;
    }

    // Otherwise allocate a fresh page table and insert it
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

    // If already mapped, bail out
    if (pt[pt_index] & PAGE_PRESENT) {
        return VMM_ERR_ALREADY_MAPPED;
    }

    // Fill in the PTE: keep physical bits and provided flags
    pt[pt_index] = PT_ENTRY_ADDR(phys) | pt_flags;

    return VMM_OK;
}

/*
 * arch_unmap_page - Unmap a single page from the page tables (x86_64 version)
 */
uint64_t arch_unmap_page(uint64_t pt_root, void* virt) {
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(pt_root);

    uint64_t* pdpt =
        vmm_get_or_create_table(pml4, PML4_INDEX(virt), false, false);
    if (!pdpt) return 0;

    uint64_t* pd =
        vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false, false);
    if (!pd) return 0;

    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false, false);
    if (!pt) return 0;

    size_t pt_index = PT_INDEX(virt);

    // Not present => nothing to do
    if (!(pt[pt_index] & PAGE_PRESENT)) {
        return 0;
    }

    uint64_t phys = PT_ENTRY_ADDR(pt[pt_index]);
    pt[pt_index] = 0;

    // Clean up empty page tables up the chain. This is a bit chatty, but it
    // avoids leaked page-table pages.
    if (vmm_table_is_empty(pt)) {
        uint64_t pt_phys = PHYSMAP_V2P((uint64_t)pt);
        pmm_free(pt_phys, PAGE_SIZE);

        // Clear the PD entry that pointed to this PT
        pd[PD_INDEX(virt)] = 0;

        if (vmm_table_is_empty(pd)) {
            uint64_t pd_phys = PHYSMAP_V2P((uint64_t)pd);
            pmm_free(pd_phys, PAGE_SIZE);

            // Clear PDPT entry
            pdpt[PDPT_INDEX(virt)] = 0;

            if (vmm_table_is_empty(pdpt)) {
                uint64_t pdpt_phys = PHYSMAP_V2P((uint64_t)pdpt);
                pmm_free(pdpt_phys, PAGE_SIZE);

                // Clear PML4 entry
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

    // Keep physical page base, replace flags
    uint64_t phys = PT_ENTRY_ADDR(pt[pt_index]);
    pt[pt_index] = phys | new_flags;

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
    if (slab_alloc(g_vm_object_internal_cache, &obj) != SLAB_OK)
        return NULL;
    
    vm_object_internal* internal = (vm_object_internal*)obj;
    memset(internal, 0, sizeof(vm_object_internal));
    
    // Initialize validation fields...
    internal->magic = VM_OBJECT_MAGIC;
    internal->red_zone_pre = VM_OBJECT_RED_ZONE;
    internal->red_zone_post = VM_OBJECT_RED_ZONE;
    internal->next_internal = NULL;
    
    return internal;
}

/*
 * vmm_free_vm_object - Free a vm_object structure
 */
void vmm_free_vm_object(vm_object_internal* obj) {
    if (!obj) return;

    // Defensive: validate before freeing so we can log suspected corruption
    if (!vm_object_validate(obj)) {
        LOGF("[VMM ERROR] Attempted to free corrupted vm_object at %p\n", obj);
        return;
    }

    // Clear validation fields to detect use-after-free in debug runs
    obj->magic = 0;
    obj->red_zone_pre = 0;
    obj->red_zone_post = 0;

    slab_free(g_vm_object_internal_cache, obj);
}

/*
 * vmm_destroy_page_table - Recursively free page tables
 */
void vmm_destroy_page_table(uint64_t table_phys, bool purge, int level) {
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(table_phys);

    // If purge requested and we're not at leaf, walk children and destroy
    if (purge && level > 1) {
        for (size_t i = 0; i < PAGE_ENTRIES; ++i) {
            uint64_t entry = table[i];
            if (!(entry & PAGE_PRESENT)) continue;

            uint64_t child_phys = PT_ENTRY_ADDR(entry);

            // Recurse before clearing our own entry
            vmm_destroy_page_table(child_phys, purge, level - 1);

            // clear entry after child's freed
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
    if (!g_kernel_vmm) return VMM_ERR_NOT_INIT;

    // Technically we should lock kernel VMM here, but PML4 kernel entries are static
    uint64_t* src_pml4 = (uint64_t*)PHYSMAP_P2V(g_kernel_vmm->public.pt_root);
    uint64_t* dest_pml4 = (uint64_t*)PHYSMAP_P2V(dest_pt_root);

    // Copy upper half (kernel space) entries from PML4
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

    // Validate MMIO alignment
    if (flags & VM_FLAG_MMIO) {
        uint64_t mmio_phys = (uint64_t)arg;
        if (mmio_phys & (PAGE_SIZE - 1)) {
            LOGF("[VMM ERROR] MMIO address 0x%lx is not page-aligned\n",
                 mmio_phys);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NOT_ALIGNED;
        }
    }

    // Align length to page size
    length = align_up(length, PAGE_SIZE);

    // Walk existing vm_objects to find a gap big enough
    vm_object_internal* cur = vmm->objects_internal;
    vm_object_internal* prev = NULL;
    uintptr_t found_base = 0;

    // Try to find a gap between existing objects
    while (cur) {
        if (!vm_object_validate(cur)) {
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_INVALID;
        }

        // base is either alloc_base or end of previous object
        uintptr_t base = prev ? (prev->public.base + prev->public.length)
                              : vmm->public.alloc_base;
        uintptr_t gap_end = cur->public.base;

        if (base + length <= gap_end) {
            found_base = base;
            break;
        }

        prev = cur;
        cur = cur->next_internal;
    }

    // If no gap found, place after last object if it fits
    if (!found_base) {
        uintptr_t base = prev ? (prev->public.base + prev->public.length)
                              : vmm->public.alloc_base;
        if (base + length <= vmm->public.alloc_end) {
            found_base = base;
        } else {
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_OOM;  // Out of virtual address space
        }
    }

    // Create new vm_object
    vm_object_internal* obj = vmm_alloc_vm_object();
    if (!obj) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NO_MEMORY;
    }

    obj->public.base = found_base;
    obj->public.length = length;
    obj->public.flags = flags;
    obj->public.next = cur ? &cur->public : NULL;
    obj->next_internal = cur;

    // Insert into linked list
    if (prev) {
        prev->public.next = &obj->public;
        prev->next_internal = obj;
    } else {
        vmm->public.objects = &obj->public;
        vmm->objects_internal = obj;
    }

    // Back with physical memory (immediate backing)
    uint64_t phys_base = 0;
    if (flags & VM_FLAG_MMIO) {
        // MMIO: use provided physical address (caller must ensure
        // page-alignment)
        phys_base = (uint64_t)arg;
    } else {
        // Normal memory: allocate contiguous block from PMM
        pmm_status_t status = pmm_alloc(length, &phys_base);
        if (status != PMM_OK) {
            // Revert list insertion
            if (prev) {
                prev->public.next = cur ? &cur->public : NULL;
                prev->next_internal = cur;
            } else {
                vmm->public.objects = cur ? &cur->public : NULL;
                vmm->objects_internal = cur;
            }
            vmm_free_vm_object(obj);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NO_MEMORY;
        }
    }

    // Map physical -> virtual page-by-page with rollback on failure
    bool is_user_vmm = !vmm->is_kernel;

    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);

        vmm_status_t map_status = arch_map_page(
            vmm->public.pt_root, phys_base + offset,
            (void*)(obj->public.base + offset), pt_flags, is_user_vmm);

        if (map_status != VMM_OK) {
            // Mapping failed - rollback what we already mapped
            for (size_t rb = 0; rb < offset; rb += PAGE_SIZE) {
                arch_unmap_page(vmm->public.pt_root,
                                (void*)(obj->public.base + rb));
            }

            // Free physical memory if we allocated it (not MMIO)
            if (!(flags & VM_FLAG_MMIO)) {
                pmm_free(phys_base, length);
            }

            // Remove vm_object from list
            if (prev) {
                prev->public.next = cur ? &cur->public : NULL;
                prev->next_internal = cur;
            } else {
                vmm->public.objects = cur ? &cur->public : NULL;
                vmm->objects_internal = cur;
            }
            vmm_free_vm_object(obj);

            spinlock_release(&vmm->lock, lock_flags);
            return map_status;
        }
    }

    // Success
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

    // Align to page boundary
    uintptr_t desired = (uintptr_t)desired_addr;
    if (desired & (PAGE_SIZE - 1)) {
        LOGF("[VMM] vmm_alloc_at: address 0x%lx not page-aligned\n", desired);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_ALIGNED;
    }

    // Align length
    length = align_up(length, PAGE_SIZE);

    // Check if desired range is within allocatable space
    if (desired < vmm->public.alloc_base ||
        desired + length > vmm->public.alloc_end) {
        LOGF(
            "[VMM] vmm_alloc_at: range 0x%lx-0x%lx outside allocatable space\n",
            desired, desired + length);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_OOM;
    }

    // Validate MMIO alignment if needed
    if (flags & VM_FLAG_MMIO) {
        uint64_t mmio_phys = (uint64_t)arg;
        if (mmio_phys & (PAGE_SIZE - 1)) {
            LOGF("[VMM] vmm_alloc_at: MMIO address 0x%lx not page-aligned\n",
                 mmio_phys);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NOT_ALIGNED;
        }
    }

    // Check for overlap with existing objects
    vm_object_internal* cur = vmm->objects_internal;
    vm_object_internal* insert_after = NULL;

    while (cur) {
        if (!vm_object_validate(cur)) {
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_INVALID;
        }

        uintptr_t obj_start = cur->public.base;
        uintptr_t obj_end = cur->public.base + cur->public.length;
        uintptr_t desired_end = desired + length;

        // Overlap check (closed-open ranges)
        if (!(desired_end <= obj_start || desired >= obj_end)) {
            LOGF(
                "[VMM] vmm_alloc_at: range 0x%lx-0x%lx overlaps with existing "
                "object\n",
                desired, desired_end);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_ALREADY_MAPPED;
        }

        // Track insertion point to keep list sorted
        if (obj_start < desired) {
            insert_after = cur;
        }

        cur = cur->next_internal;
    }

    // Create new vm_object
    vm_object_internal* obj = vmm_alloc_vm_object();
    if (!obj) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NO_MEMORY;
    }

    obj->public.base = desired;
    obj->public.length = length;
    obj->public.flags = flags;

    // Insert into the list keeping order
    if (insert_after) {
        obj->public.next = insert_after->public.next;
        obj->next_internal = insert_after->next_internal;
        insert_after->public.next = &obj->public;
        insert_after->next_internal = obj;
    } else {
        // Insert at head
        obj->public.next = vmm->public.objects;
        obj->next_internal = vmm->objects_internal;
        vmm->public.objects = &obj->public;
        vmm->objects_internal = obj;
    }

    // Allocate and map physical memory
    uint64_t phys_base = 0;
    if (flags & VM_FLAG_MMIO) {
        phys_base = (uint64_t)arg;
    } else {
        pmm_status_t pmm_status = pmm_alloc(length, &phys_base);
        if (pmm_status != PMM_OK) {
            // Remove from list and free
            if (insert_after) {
                insert_after->public.next = obj->public.next;
                insert_after->next_internal = obj->next_internal;
            } else {
                vmm->public.objects = obj->public.next;
                vmm->objects_internal = obj->next_internal;
            }
            vmm_free_vm_object(obj);
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_NO_MEMORY;
        }
    }

    // Map pages with rollback on failure
    bool is_user_vmm = !vmm->is_kernel;
    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);

    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        vmm_status_t map_status =
            arch_map_page(vmm->public.pt_root, phys_base + offset,
                          (void*)(desired + offset), pt_flags, is_user_vmm);

        if (map_status != VMM_OK) {
            // Rollback mappings we've done so far
            for (size_t rb = 0; rb < offset; rb += PAGE_SIZE) {
                arch_unmap_page(vmm->public.pt_root, (void*)(desired + rb));
            }

            // Free physical memory if allocated from PMM
            if (!(flags & VM_FLAG_MMIO)) {
                pmm_free(phys_base, length);
            }

            // Remove from list
            if (insert_after) {
                insert_after->public.next = obj->public.next;
                insert_after->next_internal = obj->next_internal;
            } else {
                vmm->public.objects = obj->public.next;
                vmm->objects_internal = obj->next_internal;
            }
            vmm_free_vm_object(obj);

            spinlock_release(&vmm->lock, lock_flags);
            return map_status;
        }
    }

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

    uintptr_t target = (uintptr_t)addr;
    vm_object_internal* prev = NULL;
    vm_object_internal* cur = vmm->objects_internal;

    // Find the object with matching base
    while (cur) {
        if (!vm_object_validate(cur)) {
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_INVALID;
        }

        if (cur->public.base == target) {
            break;
        }
        prev = cur;
        cur = cur->next_internal;
    }

    if (!cur) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    // Unmap and free physical memory if not MMIO
    for (uintptr_t virt = cur->public.base;
         virt < cur->public.base + cur->public.length; virt += PAGE_SIZE) {
        uint64_t phys = arch_unmap_page(vmm->public.pt_root, (void*)virt);

        // If arch_unmap_page returned a physical page, free it (we free
        // per-page)
        if (phys && !(cur->public.flags & VM_FLAG_MMIO)) {
            pmm_free(phys, PAGE_SIZE);
        }
    }

    // Flush TLB after unmapping to ensure no stale entries
    flush_tlb();

    // Remove from linked list
    if (prev) {
        prev->public.next = cur->public.next;
        prev->next_internal = cur->next_internal;
    } else {
        vmm->public.objects = cur->public.next;
        vmm->objects_internal = cur->next_internal;
    }

    // Free the vm_object structure
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
    
    // Align to page boundaries
    alloc_base = align_up(alloc_base, PAGE_SIZE);
    alloc_end = align_down(alloc_end, PAGE_SIZE);
    
    if (alloc_end <= alloc_base) return NULL;

    // Ensure PMM is initialized
    if(!pmm_is_initialized()) {
        LOGF("[VMM] The PMM must be online first\n");
        return NULL;
    }

    if(!slab_is_initialized()){
        LOGF("[VMM] The Slab Allocator must be online first\n");
        return NULL;
    }
    
    // Allocate VMM structure
    void* vmm_mem;
    if (slab_alloc(g_vmm_internal_cache, &vmm_mem) != SLAB_OK) {
        return NULL;
    }
    
    vmm_internal* vmm = (vmm_internal *)vmm_mem;
    memset(vmm, 0, sizeof(vmm_internal));
    
    vmm->magic = VMM_MAGIC;
    vmm->is_kernel = false;
    vmm->objects_internal = NULL;
    spinlock_init(&vmm->lock, "user_vmm");
    
    // Create page table root
    uint64_t pt_root = vmm_alloc_page_table();
    if (!pt_root) {
        slab_free(g_vmm_internal_cache, vmm_mem);
        return NULL;
    }

    // Copy kernel mappings for userspace VMMs
    if (g_kernel_vmm) {
        vmm_status_t status = vmm_copy_kernel_mappings(pt_root);
        if (status != VMM_OK) {
            pmm_free(pt_root, PAGE_SIZE);
            slab_free(g_vmm_internal_cache, vmm_mem);
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

    // Do not allow tearing down kernel VMM via this path
    if (vmm == g_kernel_vmm) {
        LOGF("[VMM ERROR] Cannot destroy kernel VMM\n");
        return;
    }

    // No need to acquire lock here as we are tearing the whole thing down,
    // but for production, it prevents anyone else from starting an alloc
    bool lock_flags = spinlock_acquire(&vmm->lock);

    // Free all vm_objects and their backing memory
    vm_object_internal* cur = vmm->objects_internal;
    while (cur) {
        if (!vm_object_validate(cur)) {
            LOGF("[VMM ERROR] Corrupted vm_object during destroy\n");
            break;
        }

        vm_object_internal* next = cur->next_internal;

        // If not MMIO, attempt to free physical pages mapped for this object
        if (!(cur->public.flags & VM_FLAG_MMIO)) {
            for (uintptr_t virt = cur->public.base;
                 virt < cur->public.base + cur->public.length;
                 virt += PAGE_SIZE) {
                uint64_t phys;
                if (vmm_get_mapped_phys(vmm->public.pt_root, (void*)virt,
                                        &phys)) {
                    pmm_free(phys, PAGE_SIZE);
                }
            }
        }

        vmm_free_vm_object(cur);
        cur = next;
    }

    // Free userspace page tables (PML4 entries 0-255 only)
    // DO NOT touch kernel mappings (entries 256-511)
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(vmm->public.pt_root);

    // Only free the lower half (user mappings)
    for (size_t i = 0; i < 256; ++i) {
        uint64_t entry = pml4[i];
        if (!(entry & PAGE_PRESENT)) continue;

        uint64_t pdpt_phys = PT_ENTRY_ADDR(entry);

        // Recursively destroy PDPT and everything below it
        vmm_destroy_page_table(pdpt_phys, true, 3);

        // Clear the PML4 entry to be tidy
        pml4[i] = 0;
    }

    // Free the PML4 page itself
    pmm_free(vmm->public.pt_root, PAGE_SIZE);

    // Clear magic before freeing structure to help detect use-after-free
    vmm->magic = 0;

    spinlock_release(&vmm->lock, lock_flags);

    // Free VMM structure itself using slab allocator
    slab_free(g_vmm_internal_cache, vmm);

    LOGF("[VMM] User VMM Destroyed\n");
}

/*
 * vmm_switch - Switch to a different address space
 */
void vmm_switch(vmm_t* vmm_pub) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    // Switch to the root page table of the VMM
    PMT_switch(vmm->public.pt_root);
}

#pragma endregion

#pragma region Kernel VMM Management

/*
 * vmm_kernel_init - Initialize the kernel VMM specifically
 */
vmm_status_t vmm_kernel_init(uintptr_t alloc_base, uintptr_t alloc_end) {
    if (g_kernel_vmm) return VMM_ERR_ALREADY_INIT;

    // Ensure PMM is online
    if (!pmm_is_initialized()) {
        LOGF("[VMM] The PMM must be online first\n");
        return VMM_ERR_NOT_INIT;
    }

    // Ensure slab is online
    if (!slab_is_initialized()) {
        LOGF("[VMM] The Slab allocator must be online first\n");
        return VMM_ERR_NOT_INIT;
    }

    // Enable NX bit if supported
    if(cpu_has_feature(CPU_FEAT_NX)){
        cpu_enable_feature(CPU_FEAT_NX);
    }

    // Allocate kernel VMM structure using PMM (kernel wants a stable physical
    // allocation)
    uint64_t vmm_phys = 0;
    if (pmm_alloc(sizeof(vmm_internal), &vmm_phys) != PMM_OK) {
        return VMM_ERR_NO_MEMORY;
    }

    vmm_internal* vmm = (vmm_internal*)PHYSMAP_P2V(vmm_phys);
    memset(vmm, 0, sizeof(vmm_internal));

    vmm->magic = VMM_MAGIC;
    vmm->is_kernel = true;
    vmm->objects_internal = NULL;
    spinlock_init(&vmm->lock, "kernel_vmm");

    // Use the currently active PML4 as the kernel page table root
    vmm->public.pt_root = (uint64_t)KERNEL_V2P(getPML4());
    vmm->public.objects = NULL;
    vmm->public.alloc_base = alloc_base;
    vmm->public.alloc_end = alloc_end;

    g_kernel_vmm = vmm;

    // Create slab caches for VMM structures
    g_vmm_internal_cache = slab_cache_create(
        "vmm_internal", sizeof(vmm_internal), _Alignof(vmm_internal));

    g_vm_object_internal_cache =
        slab_cache_create("vm_object_internal", sizeof(vm_object_internal),
                          _Alignof(vm_object_internal));

    if (!g_vmm_internal_cache || !g_vm_object_internal_cache) {
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
    return g_kernel_vmm ? &g_kernel_vmm->public : NULL;
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

    uintptr_t target = (uintptr_t)addr;
    vm_object_internal* cur = vmm->objects_internal;

    while (cur) {
        if (!vm_object_validate(cur)) {
            LOGF("[VMM ERROR] Corrupted vm_object in list\n");
            spinlock_release(&vmm->lock, lock_flags);
            return NULL;
        }

        if (target >= cur->public.base &&
            target < cur->public.base + cur->public.length) {
            spinlock_release(&vmm->lock, lock_flags);
            return &cur->public;
        }
        cur = cur->next_internal;
    }

    spinlock_release(&vmm->lock, lock_flags);
    return NULL;
}

/*
 * vmm_check_flags - Check if a specific address has specific flags
 */
bool vmm_check_flags(vmm_t* vmm_pub, void* addr, size_t required_flags) {
    vm_object* obj = vmm_find_mapped_object(vmm_pub, addr);
    if (!obj) return false;

    return (obj->flags & required_flags) == required_flags;
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

    // Check alignment
    if ((phys & (PAGE_SIZE - 1)) || ((uintptr_t)virt & (PAGE_SIZE - 1))) {
        return VMM_ERR_NOT_ALIGNED;
    }

    bool lock_flags = spinlock_acquire(&vmm->lock);

    // Convert VM flags to page table flags
    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);
    bool is_user_vmm = !vmm->is_kernel;

    // Map the page
    vmm_status_t status =
        arch_map_page(vmm->public.pt_root, phys, virt, pt_flags, is_user_vmm);
    if (status != VMM_OK) {
        spinlock_release(&vmm->lock, lock_flags);
        return status;
    }

    // Flush TLB
    flush_tlb();

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_unmap_page - Unmaps a virtual page and handles cleanup
 */
vmm_status_t vmm_unmap_page(vmm_t* vmm_pub, void* virt) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    arch_unmap_page(vmm->public.pt_root, virt);
    flush_tlb();
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

    // Align to page boundaries
    length = align_up(length, PAGE_SIZE);

    bool lock_flags = spinlock_acquire(&vmm->lock);

    // Convert VM flags to page table flags
    uint64_t pt_flags = vmm_convert_vm_flags(flags, vmm->is_kernel);
    bool is_user_vmm = !vmm->is_kernel;

    // Map each page; if any mapping fails, roll back what we've done so far.
    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        vmm_status_t status = arch_map_page(vmm->public.pt_root, phys + offset,
                                            (void*)((uintptr_t)virt + offset),
                                            pt_flags, is_user_vmm);

        if (status != VMM_OK) {
            // Unmap what we've mapped so far
            for (size_t rollback = 0; rollback < offset;
                 rollback += PAGE_SIZE) {
                arch_unmap_page(vmm->public.pt_root,
                                (void*)((uintptr_t)virt + rollback));
            }
            spinlock_release(&vmm->lock, lock_flags);
            return status;
        }
    }

    // Flush TLB once after all mappings
    flush_tlb();

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_unmap_range - Unmaps a virtual range, starting at a specific virtual address
 */
vmm_status_t vmm_unmap_range(vmm_t* vmm_pub, void* virt, size_t length) {
    vmm_internal* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    // Align to page boundaries
    length = align_up(length, PAGE_SIZE);

    bool lock_flags = spinlock_acquire(&vmm->lock);

    // Unmap each page
    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        arch_unmap_page(vmm->public.pt_root, (void*)((uintptr_t)virt + offset));
    }

    // Flush TLB once after all unmappings
    flush_tlb();

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

    // Align new length to page size
    new_length = align_up(new_length, PAGE_SIZE);

    uintptr_t target = (uintptr_t)addr;
    vm_object_internal* prev = NULL;
    vm_object_internal* cur = vmm->objects_internal;

    // Find the object with matching base address
    while (cur) {
        if (!vm_object_validate(cur)) {
            spinlock_release(&vmm->lock, lock_flags);
            return VMM_ERR_INVALID;
        }

        if (cur->public.base == target) {
            break;
        }
        prev = cur;
        cur = cur->next_internal;
    }

    if (!cur) {
        LOGF("[VMM ERROR] vmm_resize: No object found at address 0x%lx\n",
             target);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    // Cannot resize MMIO regions
    if (cur->public.flags & VM_FLAG_MMIO) {
        LOGF("[VMM ERROR] vmm_resize: Cannot resize MMIO region\n");
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_INVALID;
    }

    size_t old_length = cur->public.length;

    // No change needed
    if (new_length == old_length) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_OK;
    }

    // Growing the region
    if (new_length > old_length) {
        size_t growth = new_length - old_length;
        uintptr_t new_end = cur->public.base + new_length;

        // Check for available space: either before next object or before
        // alloc_end
        if (cur->next_internal) {
            if (new_end > cur->next_internal->public.base) {
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

        // Allocate physical memory for the new pages
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

        // Map the new pages
        bool is_user_vmm = !vmm->is_kernel;
        uint64_t pt_flags =
            vmm_convert_vm_flags(cur->public.flags, vmm->is_kernel);
        size_t mapped_offset = 0;

        for (size_t offset = 0; offset < growth; offset += PAGE_SIZE) {
            vmm_status_t map_status =
                arch_map_page(vmm->public.pt_root, phys_base + offset,
                              (void*)(cur->public.base + old_length + offset),
                              pt_flags, is_user_vmm);

            if (map_status != VMM_OK) {
                LOGF("[VMM ERROR] vmm_resize: Mapping failed at offset 0x%lx\n",
                     offset);

                // Rollback: unmap all successfully mapped pages
                for (size_t rb = 0; rb < offset; rb += PAGE_SIZE) {
                    arch_unmap_page(
                        vmm->public.pt_root,
                        (void*)(cur->public.base + old_length + rb));
                }

                // Free the entire physical allocation
                pmm_free(phys_base, growth);

                spinlock_release(&vmm->lock, lock_flags);
                return map_status;
            }

            mapped_offset = offset + PAGE_SIZE;
        }

        // If we reach here, update the object's length
        cur->public.length = new_length;

        // Flush TLB
        flush_tlb();

        spinlock_release(&vmm->lock, lock_flags);
        return VMM_OK;
    }

    // Shrinking the region
    else {
        size_t shrinkage = old_length - new_length;
        uintptr_t shrink_start = cur->public.base + new_length;

        // Unmap and free physical pages for the tail portion
        for (uintptr_t virt = shrink_start; virt < shrink_start + shrinkage;
             virt += PAGE_SIZE) {
            uint64_t phys = arch_unmap_page(vmm->public.pt_root, (void*)virt);

            if (phys) {
                pmm_free(phys, PAGE_SIZE);
            }
        }

        // Update the object's length
        cur->public.length = new_length;

        // Flush TLB after unmapping
        flush_tlb();

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

    // Find the vm_object
    // NOTE: vmm_find_mapped_object acquires the lock, so we must use an internal version.
    // However, for simplicity and since we follow order list -> cache, we can just do it here.
    
    vm_object_internal* cur = vmm->objects_internal;
    vm_object* obj = NULL;
    uintptr_t target = (uintptr_t)addr;

    while (cur) {
        if (target >= cur->public.base &&
            target < cur->public.base + cur->public.length) {
            obj = &cur->public;
            break;
        }
        cur = cur->next_internal;
    }

    if (!obj) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    // Must match base address exactly
    if (obj->base != (uintptr_t)addr) {
        LOGF("[VMM ERROR] vmm_protect requires exact base address match\n");
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_INVALID;
    }

    // Update object flags
    obj->flags = new_flags;

    // Convert new flags to page table flags
    uint64_t pt_flags = vmm_convert_vm_flags(new_flags, vmm->is_kernel);

    // Update page table entries in-place (more efficient than unmap+remap)
    for (uintptr_t virt = obj->base; virt < obj->base + obj->length;
         virt += PAGE_SIZE) {
        vmm_status_t status =
            arch_update_page_flags(vmm->public.pt_root, (void*)virt, pt_flags);
        if (status != VMM_OK) {
            LOGF("[VMM WARNING] Failed to update flags for page at 0x%lx\n",
                 virt);
        }
    }

    // Flush TLB for the entire range
    flush_tlb();

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

    LOGF("=== VMM Dump ===\n");
    LOGF("VMM at %p (magic: 0x%x, is_kernel: %d)\n", vmm, vmm->magic,
         vmm->is_kernel);
    LOGF("Alloc range: 0x%lx - 0x%lx (size: 0x%lx)\n", vmm->public.alloc_base,
         vmm->public.alloc_end, vmm->public.alloc_end - vmm->public.alloc_base);
    LOGF("Page table root (phys): 0x%lx\n", vmm->public.pt_root);
    LOGF("\nVM Objects:\n");

    vm_object_internal* current = vmm->objects_internal;
    int count = 0;

    while (current) {
        if (!vm_object_validate(current)) {
            LOGF("[CORRUPTED OBJECT AT INDEX %d]\n", count);
            break;
        }

        LOGF("  [%d] base=0x%016lx, length=0x%08lx, flags=0x%02lx", count,
             current->public.base, current->public.length,
             current->public.flags);

        count++;
        current = current->next_internal;
    }

    if (count == 0) {
        LOGF("  (no objects)\n");
    }
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

    vm_object_internal* current = vmm->objects_internal;
    while (current) {
        if (!vm_object_validate(current)) {
            LOGF("[VMM ERROR] Corrupted vm_object during stats\n");
            break;
        }

        total += current->public.length;

        // Count resident pages (actually mapped)
        for (uintptr_t virt = current->public.base;
             virt < current->public.base + current->public.length;
             virt += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_get_mapped_phys(vmm->public.pt_root, (void*)virt, &phys)) {
                resident += PAGE_SIZE;
            }
        }

        current = current->next_internal;
    }

    if (out_total) *out_total = total;
    if (out_resident) *out_resident = resident;

    spinlock_release(&vmm->lock, lock_flags);
}

/*
 * vmm_dump_pte_chain - Dumps the page table entries to get to the specified virtual address
 */
void vmm_dump_pte_chain(uint64_t pt_root, void* virt) {
    // Note: This takes pt_root directly, so it doesn't need a VMM lock.
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

    // Check VMM magic
    if (!vmm_validate(vmm)) {
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }

    // Check allocation range sanity
    if (vmm->public.alloc_end <= vmm->public.alloc_base) {
        LOGF("[VMM VERIFY] Invalid alloc range: 0x%lx - 0x%lx\n",
             vmm->public.alloc_base, vmm->public.alloc_end);
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }

    // Check page table root
    if (!vmm->public.pt_root) {
        LOGF("[VMM VERIFY] NULL page table root\n");
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }

    // Verify all vm_objects
    vm_object_internal* current = vmm->objects_internal;
    vm_object_internal* prev = NULL;
    int count = 0;

    while (current) {
        // Validate object structure
        if (!vm_object_validate(current)) {
            LOGF("[VMM VERIFY] Object %d failed validation\n", count);
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        // Check alignment
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

        // Check bounds
        if (current->public.base < vmm->public.alloc_base ||
            current->public.base + current->public.length >
                vmm->public.alloc_end) {
            LOGF("[VMM VERIFY] Object %d: out of bounds (0x%lx - 0x%lx)\n",
                 count, current->public.base,
                 current->public.base + current->public.length);
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }

        // Check for overlap with previous
        if (prev) {
            uintptr_t prev_end = prev->public.base + prev->public.length;
            if (current->public.base < prev_end) {
                LOGF(
                    "[VMM VERIFY] Object %d overlaps with previous (0x%lx < "
                    "0x%lx)\n",
                    count, current->public.base, prev_end);
                spinlock_release(&vmm->lock, lock_flags);
                return false;
            }
        }

        prev = current;
        current = current->next_internal;
        count++;

        // Sanity check: prevent infinite loop
        if (count > 10000) {
            LOGF("[VMM VERIFY] Too many objects (possible loop)\n");
            spinlock_release(&vmm->lock, lock_flags);
            return false;
        }
    }

    LOGF("[VMM VERIFY] All checks passed (%d objects)\n", count);
    spinlock_release(&vmm->lock, lock_flags);
    return true;
}

#pragma endregion