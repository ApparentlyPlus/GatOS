/*
 * vmm.c - Virtual Memory Manager Implementation
 * 
 * This implementation manages multiple virtual address spaces using vmm_t instances.
 * Each instance maintains its own page table and vm_object list. A special kernel VMM
 * can be accessed by passing NULL to most functions. This was hell to write :D
 * 
 * Author: u/ApparentlyPlus
 */

#include <memory/vmm.h>
#include <memory/pmm.h>
#include <memory/paging.h>
#include <libc/string.h>
#include <vga_stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Global kernel VMM
static vmm_t* g_kernel_vmm = NULL;

#pragma region Internal Functions

/*
 * vmm_get_instance - Get VMM instance (NULL means kernel VMM)
 */
static inline vmm_t* vmm_get_instance(vmm_t* vmm) {
    return vmm ? vmm : g_kernel_vmm;
}

/*
 * vmm_convert_vm_flags - Convert VM_FLAG_* to architecture-specific page table flags
 */
static inline uint64_t vmm_convert_vm_flags(size_t vm_flags) {
    uint64_t pt_flags = PAGE_PRESENT;
    
    if (vm_flags & VM_FLAG_WRITE) {
        pt_flags |= PAGE_WRITABLE;
    }
    
    if (vm_flags & VM_FLAG_USER) {
        pt_flags |= PAGE_USER;
    }

    /*
        On x86_64, memory is executable by default
        Set NX bit if VM_FLAG_EXEC is NOT set
        
        IMPORTANT:
        The code below should remain commented out until we enable NX bit support
    */

    //if (!(vm_flags & VM_FLAG_EXEC)) {
    //    pt_flags |= PAGE_NO_EXECUTE;
    //}
    
    return pt_flags;
}

/*
 * vmm_alloc_page_table - Allocate and zero a page table
 */
uint64_t vmm_alloc_page_table(void) {
    uint64_t phys;
    if (pmm_alloc(PAGE_SIZE, &phys) != PMM_OK) {
        return 0;
    }
    
    uint64_t* table = (uint64_t *)PHYSMAP_P2V(phys);
    memset(table, 0, PAGE_SIZE);
    
    return phys;
}

/*
 * vmm_get_or_create_table - Get or create a page table entry. If create is false, returns NULL if not present.
 */
uint64_t* vmm_get_or_create_table(uint64_t* parent_table, size_t index, bool create) {
    uint64_t entry = parent_table[index];
    
    // If present, return existing table
    if (entry & PAGE_PRESENT) {
        uint64_t table_phys = PT_ENTRY_ADDR(entry);
        return (uint64_t *)PHYSMAP_P2V(table_phys);
    }
    
    // Not present
    if (!create) {
        return NULL;
    }
    
    uint64_t new_table_phys = vmm_alloc_page_table();

    // Allocation failed
    if (!new_table_phys) {
        return NULL;
    }
    
    parent_table[index] = (new_table_phys & ADDR_MASK) | PAGE_PRESENT | PAGE_WRITABLE;

    // We should add a PAGE_USER if we are in a non kernel VMM :pepethink:
    
    return (uint64_t *)PHYSMAP_P2V(new_table_phys);
}

/*
 * arch_map_page - Map a single page in the page tables (x86_64 version)
 */
vmm_status_t arch_map_page(uint64_t pt_root, uint64_t phys, void* virt, uint64_t pt_flags) {
    uint64_t* pml4 = (uint64_t *)PHYSMAP_P2V(pt_root);
    
    uint64_t* pdpt = vmm_get_or_create_table(pml4, PML4_INDEX(virt), true);
    if (!pdpt) return VMM_ERR_NO_MEMORY;
    
    uint64_t* pd = vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), true);
    if (!pd) return VMM_ERR_NO_MEMORY;
    
    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), true);
    if (!pt) return VMM_ERR_NO_MEMORY;
    
    size_t pt_index = PT_INDEX(virt);

    // Check if page is already mapped
    if (pt[pt_index] & PAGE_PRESENT) {
        return VMM_ERR_ALREADY_MAPPED;
    }

    pt[pt_index] = PT_ENTRY_ADDR(phys) | pt_flags;
    
    return VMM_OK;
}

/*
 * arch_unmap_page - Unmap a single page from the page tables (x86_64 version)
 */
vmm_status_t arch_unmap_page(uint64_t pt_root, void* virt) {
    uint64_t* pml4 = (uint64_t *)PHYSMAP_P2V(pt_root);
    
    uint64_t* pdpt = vmm_get_or_create_table(pml4, PML4_INDEX(virt), false);
    if (!pdpt) return VMM_OK;
    
    uint64_t* pd = vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false);
    if (!pd) return VMM_OK;
    
    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false);
    if (!pt) return VMM_OK;
    
    size_t pt_index = PT_INDEX(virt);
    
    // If page is not present, nothing to do
    if (!(pt[pt_index] & PAGE_PRESENT)) {
        return VMM_OK;
    }
    
    pt[pt_index] = 0;

    // If the page table is empty after unmapping, free it
    if (vmm_table_is_empty(pt)) {
        uint64_t pt_phys = PHYSMAP_V2P((uint64_t)pt);
        pmm_free(pt_phys, PAGE_SIZE);
        
        // Clear the PD entry pointing to this PT
        pd[PD_INDEX(virt)] = 0;
        
        // Check and free PD if empty
        if (vmm_table_is_empty(pd)) {
            uint64_t pd_phys = PHYSMAP_V2P((uint64_t)pd);
            pmm_free(pd_phys, PAGE_SIZE);
            
            // Clear the PDPT entry pointing to this PD
            pdpt[PDPT_INDEX(virt)] = 0;
            
            // Check and free PDPT if empty
            if (vmm_table_is_empty(pdpt)) {
                uint64_t pdpt_phys = PHYSMAP_V2P((uint64_t)pdpt);
                pmm_free(pdpt_phys, PAGE_SIZE);
                
                // Clear the PML4 entry pointing to this PDPT
                pml4[PML4_INDEX(virt)] = 0;
            }
        }
    }

    return VMM_OK;
}

/*
 * vmm_get_mapped_phys - Get physical address from virtual address
 */
bool vmm_get_mapped_phys(uint64_t pt_root, void* virt, uint64_t* out_phys) {
    if (!out_phys) return false;
    
    uint64_t* pml4 = (uint64_t *)PHYSMAP_P2V(pt_root);
    
    uint64_t* pdpt = vmm_get_or_create_table(pml4, PML4_INDEX(virt), false);
    if (!pdpt) return false;
    
    uint64_t* pd = vmm_get_or_create_table(pdpt, PDPT_INDEX(virt), false);
    if (!pd) return false;
    
    uint64_t* pt = vmm_get_or_create_table(pd, PD_INDEX(virt), false);
    if (!pt) return false;
    
    uint64_t entry = pt[PT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) {
        return false;
    }
    
    uint64_t page_base = PT_ENTRY_ADDR(entry);
    uint64_t offset = (uintptr_t)virt & 0xFFF;
    *out_phys = page_base + offset;
    
    return true;
}

/*
 * vmm_alloc_vm_object - Allocate a vm_object structure
 */
vm_object* vmm_alloc_vm_object(void) {
    uint64_t phys;
    if (pmm_alloc(sizeof(vm_object), &phys) != PMM_OK) {
        return NULL;
    }
    
    vm_object* obj = (vm_object *)PHYSMAP_P2V(phys);
    memset(obj, 0, sizeof(vm_object));
    return obj;
}

/*
 * vmm_free_vm_object - Free a vm_object structure
 */
void vmm_free_vm_object(vm_object* obj) {
    if (!obj) return;
    
    uint64_t phys = PHYSMAP_V2P((uint64_t)obj);
    pmm_free(phys, sizeof(vm_object));
}

/*
 * Recursively free page tables.
 * - Frees a table if empty.
 * - If purge = true, frees lower-level (child) tables even if non-empty.
 */
void vmm_destroy_page_table(uint64_t table_phys, bool purge, int level) {
    uint64_t *table = (uint64_t *)PHYSMAP_P2V(table_phys);

    if (purge) {
        for (size_t i = 0; i < PAGE_ENTRIES; i++) {
            uint64_t entry = table[i];
            if (!(entry & PAGE_PRESENT)) continue;

            uint64_t child_phys = PT_ENTRY_ADDR(entry);

            if (level > 1) {
                // Recursively destroy lower-level table
                vmm_destroy_page_table(child_phys, purge, level - 1);
            }

            pmm_free(child_phys, PAGE_SIZE);
            table[i] = 0;
        }
        pmm_free(table_phys, PAGE_SIZE);
    } else {
        if (vmm_table_is_empty(table))
            pmm_free(table_phys, PAGE_SIZE);
    }
}

/*
 * vmm_copy_kernel_mappings - Copy kernel mappings from kernel VMM to a new page table
 * This ensures userspace VMMs can access kernel code/data when needed.
 */
static vmm_status_t vmm_copy_kernel_mappings(uint64_t dest_pt_root) {
    if (!g_kernel_vmm) return VMM_ERR_NOT_INIT;
    
    uint64_t* src_pml4 = (uint64_t *)PHYSMAP_P2V(g_kernel_vmm->pt_root);
    uint64_t* dest_pml4 = (uint64_t *)PHYSMAP_P2V(dest_pt_root);
    
    // Copy upper half (kernel space) entries from PML4
    // Typically entries 256-511 (0xFFFF800000000000 and above)
    for (size_t i = 256; i < PAGE_ENTRIES; i++) {
        dest_pml4[i] = src_pml4[i];
    }
    
    return VMM_OK;
}

#pragma endregion

#pragma region Core Allocation/Deallocation

/*
 * vmm_alloc - Allocate a virtual memory range and back it with physical memory
 */
vmm_status_t vmm_alloc(vmm_t* vmm, size_t length, size_t flags, void* arg, void** out_addr) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (length == 0) return VMM_ERR_INVALID;
    if (!out_addr) return VMM_ERR_INVALID;
    
    *out_addr = NULL;
    
    // Align length to page size
    length = align_up(length, PAGE_SIZE);
    
    // Find space for the new object
    vm_object* current = vmm->objects;
    vm_object* prev = NULL;
    uintptr_t found = 0;
    
    // Try to find a gap between existing objects
    while (current != NULL) {
        uintptr_t base = prev ? (prev->base + prev->length) : vmm->alloc_base;
        uintptr_t gap_end = current->base;
        
        if (base + length <= gap_end) {
            found = base;
            break;
        }
        
        prev = current;
        current = current->next;
    }
    
    // If no gap found, try after the last object
    if (!found) {
        uintptr_t base = prev ? (prev->base + prev->length) : vmm->alloc_base;
        if (base + length <= vmm->alloc_end) {
            found = base;
        } else {
            return VMM_ERR_OOM; // Out of virtual address space
        }
    }
    
    // Create new vm_object
    vm_object* obj = vmm_alloc_vm_object();
    if (!obj) return VMM_ERR_NO_MEMORY;
    
    obj->base = found;
    obj->length = length;
    obj->flags = flags;
    obj->next = current;
    
    // Insert into linked list
    if (prev) {
        prev->next = obj;
    } else {
        vmm->objects = obj;
    }
    
    // Back with physical memory (immediate backing)
    uint64_t phys;
    if (flags & VM_FLAG_MMIO) {
        // MMIO: use provided physical address
        phys = (uint64_t)arg;
    } else {
        // Normal memory: allocate from PMM
        pmm_status_t status = pmm_alloc(length, &phys);
        if (status != PMM_OK) {
            // Allocation failed, clean up
            if (prev) {
                prev->next = current;
            } else {
                vmm->objects = current;
            }
            vmm_free_vm_object(obj);
            return VMM_ERR_NO_MEMORY;
        }
    }
    
    // Map the physical memory
    vmm_status_t map_status = vmm_map_range(vmm, phys, (void*)obj->base, length, flags);
    if (map_status != VMM_OK) {
        // Mapping failed, clean up
        if (!(flags & VM_FLAG_MMIO)) {
            pmm_free(phys, length);
        }
        if (prev) {
            prev->next = current;
        } else {
            vmm->objects = current;
        }
        vmm_free_vm_object(obj);
        return map_status;
    }
    
    *out_addr = (void*)obj->base;
    return VMM_OK;
}

/*
 * vmm_free - Free a previously allocated virtual memory range
 */
vmm_status_t vmm_free(vmm_t* vmm, void* addr) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;
    
    uintptr_t target = (uintptr_t)addr;
    vm_object* prev = NULL;
    vm_object* current = vmm->objects;
    
    // Find the object with matching base address
    while (current) {
        if (current->base == target) {
            break;
        }
        prev = current;
        current = current->next;
    }
    
    if (!current) {
        return VMM_ERR_NOT_FOUND;
    }
    
    // Free physical memory if not MMIO
    if (!(current->flags & VM_FLAG_MMIO)) {
        for (uintptr_t virt = current->base; virt < current->base + current->length; virt += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_get_mapped_phys(vmm->pt_root, (void*)virt, &phys)) {
                pmm_free(phys, PAGE_SIZE);
            }
        }
    }
    
    // Unmap pages
    vmm_unmap_range(vmm, (void*)current->base, current->length);
    
    // Remove from linked list
    if (prev) {
        prev->next = current->next;
    } else {
        vmm->objects = current->next;
    }
    
    // Free the vm_object
    vmm_free_vm_object(current);
    
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
        return NULL;
    }
    
    // Allocate VMM structure
    uint64_t vmm_phys;
    if (pmm_alloc(sizeof(vmm_t), &vmm_phys) != PMM_OK) {
        return NULL;
    }
    
    vmm_t* vmm = (vmm_t *)PHYSMAP_P2V(vmm_phys);
    memset(vmm, 0, sizeof(vmm_t));
    
    // Create page table root
    uint64_t pt_root = vmm_alloc_page_table();
    if (!pt_root) {
        pmm_free(vmm_phys, sizeof(vmm_t));
        return NULL;
    }

    // Copy kernel mappings for userspace VMMs
    if (g_kernel_vmm) {
        vmm_status_t status = vmm_copy_kernel_mappings(pt_root);
        if (status != VMM_OK) {
            pmm_free(pt_root, PAGE_SIZE);
            pmm_free(vmm_phys, sizeof(vmm_t));
            return NULL;
        }
    }
    
    vmm->pt_root = pt_root;
    vmm->objects = NULL;
    vmm->alloc_base = alloc_base;
    vmm->alloc_end = alloc_end;
    
    return vmm;
}

/*
 * vmm_destroy - Destroy a VMM instance and free all resources
 */
void vmm_destroy(vmm_t* vmm) {
    if (!vmm) return;
    
    // Cannot destroy kernel VMM
    if (vmm == g_kernel_vmm) return;
    
    // Free all vm_objects
    vm_object* current = vmm->objects;
    while (current) {
        vm_object* next = current->next;
        
        // Free physical memory if not MMIO
        if (!(current->flags & VM_FLAG_MMIO)) {
            for (uintptr_t virt = current->base; virt < current->base + current->length; virt += PAGE_SIZE) {
                uint64_t phys;
                if (vmm_get_mapped_phys(vmm->pt_root, (void*)virt, &phys)) {
                    pmm_free(phys, PAGE_SIZE);
                }
            }
        }
        
        vmm_free_vm_object(current);
        current = next;
    }
    
    // Destroy page tables from level 4 (PML4)
    vmm_destroy_page_table(vmm->pt_root, true, 4);
    
    // Free VMM structure itself
    uint64_t vmm_phys = PHYSMAP_V2P((uint64_t)vmm);
    pmm_free(vmm_phys, sizeof(vmm_t));
}

/*
 * vmm_switch - Switch to a different address space
 */
void vmm_switch(vmm_t* vmm) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return;
    
    PMT_switch(vmm->pt_root);
}

#pragma endregion

#pragma region Kernel VMM Management

/*
 * vmm_kernel_init - Initialize the kernel VMM specifically
 */
vmm_status_t vmm_kernel_init(uintptr_t alloc_base, uintptr_t alloc_end) {
    if (g_kernel_vmm) return VMM_ERR_ALREADY_INIT;
    
    g_kernel_vmm = vmm_create(alloc_base, alloc_end);
    if (!g_kernel_vmm) return VMM_ERR_NO_MEMORY;
    
    // Use current page table as kernel page table
    g_kernel_vmm->pt_root = (uint64_t)KERNEL_V2P(getPML4());
    
    return VMM_OK;
}

/*
 * vmm_kernel_get - Get the kernel VMM instance
 */
vmm_t* vmm_kernel_get(void) {
    return g_kernel_vmm;
}

#pragma endregion

#pragma region Introspection

/*
 * vmm_get_alloc_base - Get the base of the allocatable range
 */
uintptr_t vmm_get_alloc_base(vmm_t* vmm) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return 0;
    return vmm->alloc_base;
}

/*
 * vmm_get_alloc_end - Get the end of the allocatable range
 */
uintptr_t vmm_get_alloc_end(vmm_t* vmm) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return 0;
    return vmm->alloc_end;
}

/*
 * vmm_get_alloc_size - Get the size of the allocatable range
 */
size_t vmm_get_alloc_size(vmm_t* vmm) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return 0;
    return vmm->alloc_end - vmm->alloc_base;
}

/*
 * vmm_table_is_empty - Check if a page table is empty (no present entries)
 */
bool vmm_table_is_empty(uint64_t *table) {
    for (size_t i = 0; i < PAGE_ENTRIES; i++) {
        if (table[i] & PAGE_PRESENT)
            return false;
    }
    return true;
}

#pragma endregion

#pragma region Address Translation and Query

/*
 * vmm_get_physical - Get the physical address mapped to a virtual address
 */
bool vmm_get_physical(vmm_t* vmm, void* virt, uint64_t* out_phys) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return false;
    
    return vmm_get_mapped_phys(vmm->pt_root, virt, out_phys);
}

/*
 * vmm_find_mapped_object - Find vm_object containing a virtual address
 * Use to check if an address is valid before accessing it.
 */
vm_object* vmm_find_mapped_object(vmm_t* vmm, void* addr) {
    vmm = vmm_get_instance(vmm);
    if (!vmm || !addr) return NULL;
    
    uintptr_t target = (uintptr_t)addr;
    vm_object* current = vmm->objects;
    
    while (current) {
        if (target >= current->base && target < current->base + current->length) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

/*
 * vmm_check_flags - Check if a specific address has specific flags
 */
bool vmm_check_flags(vmm_t* vmm, void* addr, size_t required_flags) {
    vm_object* obj = vmm_find_mapped_object(vmm, addr);
    if (!obj) return false;
    
    return (obj->flags & required_flags) == required_flags;
}

#pragma endregion

#pragma region Page Table Manipulation

/*
 * vmm_map_page - Maps a physical address to the specified virtual address,
 * with the given flags
 */
vmm_status_t vmm_map_page(vmm_t* vmm, uint64_t phys, void* virt, size_t flags) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    
    // Check alignment
    if ((phys & (PAGE_SIZE - 1)) || ((uintptr_t)virt & (PAGE_SIZE - 1))) {
        return VMM_ERR_NOT_ALIGNED;
    }
    
    // Convert VM flags to page table flags
    uint64_t pt_flags = vmm_convert_vm_flags(flags);
    
    // Map the page
    vmm_status_t status = arch_map_page(vmm->pt_root, phys, virt, pt_flags);
    if (status != VMM_OK) {
        return status;
    }
    
    // Flush TLB
    flush_tlb();
    
    return VMM_OK;
}

/*
 * vmm_unmap_page - Unmaps a virtual page and handles cleanup
 */
vmm_status_t vmm_unmap_page(vmm_t* vmm, void* virt) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    
    vmm_status_t status = arch_unmap_page(vmm->pt_root, virt);
    flush_tlb();
    
    return status;
}

/*
 * vmm_map_range - Maps a physical range to a virtual range starting at a specific virtual address
 * The 2 ranges are the same length. If it fails, nothing is mapped.
 */
vmm_status_t vmm_map_range(vmm_t* vmm, uint64_t phys, void* virt, size_t length, size_t flags) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    
    // Align to page boundaries
    length = align_up(length, PAGE_SIZE);
    
    // Map each page
    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        vmm_status_t status = vmm_map_page(vmm, phys + offset, (void*)((uintptr_t)virt + offset), flags);
        if (status != VMM_OK) {
            // Unmap what we've mapped so far
            vmm_unmap_range(vmm, virt, offset);
            return status;
        }
    }
    
    return VMM_OK;
}

/*
 * vmm_unmap_range - Unmaps a virtual range, starting at a specific virtual address
 */
vmm_status_t vmm_unmap_range(vmm_t* vmm, void* virt, size_t length) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    
    // Align to page boundaries
    length = align_up(length, PAGE_SIZE);
    
    // Unmap each page
    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        vmm_unmap_page(vmm, (void*)((uintptr_t)virt + offset));
    }
    
    return VMM_OK;
}

#pragma endregion

#pragma region Protection

/*
 * vmm_protect - Change the permission flags of a specific virtual address
 */
vmm_status_t vmm_protect(vmm_t* vmm, void* addr, size_t new_flags) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;
    
    // Find the vm_object
    vm_object* obj = vmm_find_mapped_object(vmm, addr);
    if (!obj) return VMM_ERR_NOT_FOUND;
    
    // Must match base address exactly
    if (obj->base != (uintptr_t)addr) return VMM_ERR_INVALID;
    
    // Update object flags
    obj->flags = new_flags;
    
    // Update page table entries
    uint64_t pt_flags = vmm_convert_vm_flags(new_flags);
    for (uintptr_t virt = obj->base; virt < obj->base + obj->length; virt += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_get_mapped_phys(vmm->pt_root, (void*)virt, &phys)) {
            // Remap with new flags
            arch_unmap_page(vmm->pt_root, (void*)virt);
            arch_map_page(vmm->pt_root, phys, (void*)virt, pt_flags);
        }
    }
    
    // Flush TLB for the entire range
    flush_tlb();
    
    return VMM_OK;
}

#pragma endregion

#pragma region Debugging

/*
 * vmm_dump - Dumps the current VMM layout
 */
void vmm_dump(vmm_t* vmm) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return;
    
    vm_object* current = vmm->objects;
    int count = 0;
    
    while (current) {
        printf("VM Object %d: base=0x%lx, length=0x%lx, flags=0x%lx\n",
                 count, current->base, current->length, current->flags);
        count++;
        current = current->next;
    }
}

/*
 * vmm_stats - Get the number of pages handled by the VMM and the
 * number of mapped pages in the VMM's range
 */
void vmm_stats(vmm_t* vmm, size_t* out_total, size_t* out_resident) {
    vmm = vmm_get_instance(vmm);
    if (!vmm) return;
    
    size_t total = 0;
    size_t resident = 0;
    
    vm_object* current = vmm->objects;
    while (current) {
        total += current->length;
        
        // Count resident pages (actually mapped)
        for (uintptr_t virt = current->base; virt < current->base + current->length; virt += PAGE_SIZE) {
            uint64_t phys;
            if (vmm_get_mapped_phys(vmm->pt_root, (void*)virt, &phys)) {
                resident += PAGE_SIZE;
            }
        }
        
        current = current->next;
    }
    
    if (out_total) *out_total = total;
    if (out_resident) *out_resident = resident;
}

/*
 * vmm_dump_pte_chain - Dumbs the page table entries to get to the specified
 * virtual address
 */
void vmm_dump_pte_chain(uint64_t pt_root, void* virt) {
    uint64_t v = (uint64_t)virt;
    uint64_t *pml4 = (uint64_t *)PHYSMAP_P2V(pt_root);

    printf("Dumping PTE chain for virt=0x%lx (pt_root phys=0x%lx)\n", v, pt_root);
    size_t i;

    i = PML4_INDEX(virt);
    uint64_t e = pml4[i];
    printf("PML4[%2zu] = 0x%016lx\n", i, e);
    if (!(e & PAGE_PRESENT)) return;

    uint64_t pdpt_phys = PT_ENTRY_ADDR(e);
    uint64_t *pdpt = (uint64_t *)PHYSMAP_P2V(pdpt_phys);

    i = PDPT_INDEX(virt);
    e = pdpt[i];
    printf("PDPT[%2zu] = 0x%016lx\n", i, e);
    if (!(e & PAGE_PRESENT)) return;

    uint64_t pd_phys = PT_ENTRY_ADDR(e);
    uint64_t *pd = (uint64_t *)PHYSMAP_P2V(pd_phys);

    i = PD_INDEX(virt);
    e = pd[i];
    printf("PD  [%2zu] = 0x%016lx\n", i, e);
    if (!(e & PAGE_PRESENT)) return;

    uint64_t pt_phys = PT_ENTRY_ADDR(e);
    uint64_t *pt = (uint64_t *)PHYSMAP_P2V(pt_phys);

    i = PT_INDEX(virt);
    e = pt[i];
    printf("PT  [%2zu] = 0x%016lx\n", i, e);
}

#pragma endregion