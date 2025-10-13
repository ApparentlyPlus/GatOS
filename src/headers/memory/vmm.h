/*
 * vmm.h - Virtual Memory Manager
 *
 * The Virtual Memory Manager (VMM) provides comprehensive virtual memory management
 * for the operating system, serving as the highest-level abstraction over hardware
 * memory management units. It manages multiple address spaces, page table structures,
 * and virtual memory objects while providing a unified interface for memory allocation,
 * protection, and mapping operations.
 *
 * Each VMM instance manages a complete address space with its own page table hierarchy
 * and maintains a linked list of VM objects representing contiguous virtual memory regions.
 * The kernel VMM instance is initialized first and manages the kernel's address space,
 * while additional instances can be created for user processes.
 *
 * The VMM should be initialized LAST, as it relies on both the PMM and the Slab Allocator.
 *
 * Author: u/ApparentlyPlus
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// VM Object Flags
#define VM_FLAG_NONE  0
#define VM_FLAG_WRITE (1 << 0)
#define VM_FLAG_EXEC  (1 << 1)
#define VM_FLAG_USER  (1 << 2)
#define VM_FLAG_MMIO  (1 << 3)

// Return codes
typedef enum {
    VMM_OK = 0,             // Success
    VMM_ERR_INVALID,        // Invalid arguments
    VMM_ERR_OOM,            // Out of memory (virtual address space)
    VMM_ERR_NOT_INIT,       // Kernel VMM not initialized
    VMM_ERR_ALREADY_INIT,   // Kernel VMM already initialized
    VMM_ERR_NOT_FOUND,      // VM object or mapping not found
    VMM_ERR_NOT_ALIGNED,    // Address not page-aligned
    VMM_ERR_NO_MEMORY,      // Physical memory allocation failed
    VMM_ERR_ALREADY_MAPPED, // Page is already mapped
} vmm_status_t;

typedef struct vm_object vm_object;

// VM Object - represents a virtual memory range
// Note: This is the public interface. Internal implementation adds validation fields.
struct vm_object{
    uintptr_t base;        // Virtual base address (page-aligned)
    size_t length;         // Length in bytes (page-aligned)
    size_t flags;          // VM_FLAG_* bitfield
    vm_object* next;       // Next object in linked list
};

// VMM Instance - manages one address space
// Note: This is the public interface. Internal implementation adds validation fields.
typedef struct {
    uint64_t pt_root;      // Page table root (physical address)
    vm_object* objects;    // Linked list of vm_objects
    uintptr_t alloc_base;  // Base address for allocations
    uintptr_t alloc_end;   // End address for allocations
} vmm_t;

// Core Allocation/Deallocation

vmm_status_t vmm_alloc(vmm_t* vmm, size_t length, size_t flags, void* arg, void** out_addr);
vmm_status_t vmm_free(vmm_t* vmm, void* addr);

// Non-kernel VMM Instance Management

vmm_t* vmm_create(uintptr_t alloc_base, uintptr_t alloc_end);
void vmm_destroy(vmm_t* vmm);
void vmm_switch(vmm_t* vmm);

// Kernel VMM Management

vmm_status_t vmm_kernel_init(uintptr_t alloc_base, uintptr_t alloc_end);
vmm_t* vmm_kernel_get(void);

// Introspection

uintptr_t vmm_get_alloc_base(vmm_t* vmm);
uintptr_t vmm_get_alloc_end(vmm_t* vmm);
size_t vmm_get_alloc_size(vmm_t* vmm);
bool vmm_table_is_empty(uint64_t *table);

// Address Translation & Query

bool vmm_get_physical(vmm_t* vmm, void* virt, uint64_t* out_phys);
vm_object* vmm_find_mapped_object(vmm_t* vmm, void* addr);
bool vmm_check_flags(vmm_t* vmm, void* addr, size_t required_flags);

// Page Table Manipulation

vmm_status_t vmm_map_page(vmm_t* vmm, uint64_t phys, void* virt, size_t flags);
vmm_status_t vmm_unmap_page(vmm_t* vmm, void* virt);
vmm_status_t vmm_map_range(vmm_t* vmm, uint64_t phys, void* virt, size_t length, size_t flags);
vmm_status_t vmm_unmap_range(vmm_t* vmm, void* virt, size_t length);

// Protection & Permissions

vmm_status_t vmm_protect(vmm_t* vmm, void* addr, size_t new_flags);

// Debugging

void vmm_dump(vmm_t* vmm);
void vmm_stats(vmm_t* vmm, size_t* out_total, size_t* out_resident);
void vmm_dump_pte_chain(uint64_t pt_root, void* virt);
bool vmm_verify_integrity(vmm_t* vmm_pub);

#endif