/*
 * vmm.c - Virtual Memory Manager Implementation
 *
 * Each vmm_t owns a page table and a vm_object list. Pass NULL to most functions for the
 * kernel VMM. This was hell to write :D
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>
#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/cpu/cpu.h>
#include <kernel/memory/slab.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/spinlock.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <klibc/avl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Dead weight without a heap, see GATA_CAP_MEM in caps.h. vmm_add_mmio is the exception: a stats
// counter the bootstrap's memory-map walk calls regardless, so it gets an always available stub below.
#ifdef GATA_CAP_MEM

// Magic numbers for validation and corruption detection
#define VMM_MAGIC 0xC0FFEEEE
#define VM_OBJECT_MAGIC 0xACCE55ED
#define VM_OBJECT_RED_ZONE 0xDEADC0DE


// Extended vm_object with validation
typedef struct vmo_ext {
    uint32_t magic;
    uint32_t red_zone_pre;
    vm_object public;
    uint32_t red_zone_post;
    avl_node_t vma_node;
} vmo_ext;

// Extended VMM with validation
typedef struct {
    uint32_t magic;
    vmm_t public;
    bool is_kernel;
    avl_tree_t vma_tree; // VMA tree, sorted by base address
    spinlock_t lock;
} vmm_ctx;

static vmm_ctx* kernel_vmm = NULL;
static vmm_t* current_vmm = NULL;
static slab_cache_t* vmm_cache = NULL;
static slab_cache_t* vmo_cache = NULL;
static volatile size_t mmio_bytes = 0;

// SSE free page zero: this file is compiled with -mno-sse (interrupt path).
// rep stosq matches what kmemset does for aligned power of 2 sizes, without SSE.
static inline void zero_page(void *dst) {
    uint64_t cnt = PAGE_SIZE / sizeof(uint64_t);
    __asm__ volatile("rep stosq" : "+D"(dst), "+c"(cnt) : "a"(0ULL) : "memory");
}

#pragma region Validation Helpers

static inline bool vmm_validate(vmm_ctx* vmm) {
    if (!vmm) return false;

    if (vmm->magic != VMM_MAGIC) {
        LOGF("[VMM ERROR] Invalid VMM magic: 0x%x (expected 0x%x)\n",
             vmm->magic, VMM_MAGIC);
        return false;
    }

    return true;
}

static inline bool vm_object_validate(vmo_ext* obj) {
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
    uintptr_t ba = AVL_ENTRY(a, vmo_ext, vma_node)->public.base;
    uintptr_t bb = AVL_ENTRY(b, vmo_ext, vma_node)->public.base;
    if (ba < bb) return -1;
    if (ba > bb) return 1;
    return 0;
}

/*
 * vma_insert - Insert obj into the VMM's VMA tree and maintain public.next / public.objects
 */
static void vma_insert(vmm_ctx* vmm, vmo_ext* obj) {
    avl_insert(&vmm->vma_tree, &obj->vma_node);

    avl_node_t* nx = avl_next(&obj->vma_node);
    avl_node_t* pv = avl_prev(&obj->vma_node);

    obj->public.next = nx ? &AVL_ENTRY(nx, vmo_ext, vma_node)->public : NULL;

    if (pv)
        AVL_ENTRY(pv, vmo_ext, vma_node)->public.next = &obj->public;
    else
        vmm->public.objects = &obj->public;
}

/*
 * vma_remove - Remove obj from the VMM's VMA tree and repair public.next / public.objects
 */
static void vma_remove(vmm_ctx* vmm, vmo_ext* obj) {
    avl_node_t* pv = avl_prev(&obj->vma_node);

    if (pv)
        AVL_ENTRY(pv, vmo_ext, vma_node)->public.next = obj->public.next;
    else
        vmm->public.objects = obj->public.next;

    avl_remove(&vmm->vma_tree, &obj->vma_node);
}

static vmo_ext* vma_find_exact(vmm_ctx* vmm, uintptr_t base) {
    vmo_ext key = {0};
    key.public.base = base;
    avl_node_t* n = avl_find(&vmm->vma_tree, &key.vma_node);
    return n ? AVL_ENTRY(n, vmo_ext, vma_node) : NULL;
}

/*
 * vma_find_containing - Find the VMA containing addr (base <= addr < base+length)
 */
static vmo_ext* vma_find_containing(vmm_ctx* vmm, uintptr_t addr) {
    vmo_ext key = {0};
    key.public.base = addr;
    avl_node_t* n = avl_floor(&vmm->vma_tree, &key.vma_node);
    if (!n) return NULL;
    vmo_ext* obj = AVL_ENTRY(n, vmo_ext, vma_node);
    if (addr < obj->public.base + obj->public.length) return obj;
    return NULL;
}

/*
 * vma_find_gap - Find a gap of at least 'length' bytes, aligned to 'virt_align'
 */
static uintptr_t vma_find_gap(vmm_ctx* vmm, size_t length, size_t virt_align) {
    uintptr_t cand = align_up(vmm->public.alloc_base, virt_align);

    while (cand && cand + length > cand && cand + length <= vmm->public.alloc_end) {
        vmo_ext key = {0};
        key.public.base = cand;

        avl_node_t* fn = avl_floor(&vmm->vma_tree, &key.vma_node);
        if (fn) {
            vmo_ext* f = AVL_ENTRY(fn, vmo_ext, vma_node);
            uintptr_t fend = f->public.base + f->public.length;
            if (cand < fend) { cand = align_up(fend, virt_align); continue; }
        }

        avl_node_t* cn = avl_ceil(&vmm->vma_tree, &key.vma_node);
        if (!cn)
            break;

        vmo_ext* c = AVL_ENTRY(cn, vmo_ext, vma_node);
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
static bool vma_overlaps(vmm_ctx* vmm, uintptr_t start, size_t length) {
    vmo_ext key = {0};
    key.public.base = start;

    avl_node_t* fn = avl_floor(&vmm->vma_tree, &key.vma_node);
    if (fn) {
        vmo_ext* f = AVL_ENTRY(fn, vmo_ext, vma_node);
        if (f->public.base + f->public.length > start) return true;
    }
    avl_node_t* cn = avl_ceil(&vmm->vma_tree, &key.vma_node);
    if (cn) {
        vmo_ext* c = AVL_ENTRY(cn, vmo_ext, vma_node);
        if (c->public.base < start + length) return true;
    }
    return false;
}

/*
 * vmm_get_instance - Get VMM instance (NULL means kernel VMM)
 */
static inline vmm_ctx* vmm_get_instance(vmm_t* vmm) {
    if (vmm) {
        // public is embedded inside vmm_ctx; recover the outer struct
        vmm_ctx* internal =
            (vmm_ctx*)((uint8_t*)vmm - offsetof(vmm_ctx, public));
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
static inline uint64_t vmm_convert_vm_flags(size_t vm_flags) {
    uint64_t pt_flags = PAGE_PRESENT;

    if (vm_flags & VM_FLAG_WRITE) {
        pt_flags |= PAGE_WRITABLE;
    }

    if (vm_flags & VM_FLAG_USER) {
        pt_flags |= PAGE_USER;
    }

    if (!(vm_flags & VM_FLAG_EXEC) && cpu_is_feature_enabled(CF_NX)) {
         pt_flags |= PAGE_NO_EXECUTE;
    }

    // Device registers must be uncacheable (PAT entry 3 stays UC)
    if (vm_flags & VM_FLAG_DEVICE) {
        pt_flags |= PAGE_PCD | PAGE_PWT;
    }

    return pt_flags;
}

uint64_t vmm_alloc_page_table(void) {
    uint64_t phys = 0;
    if (pmm_alloc(PAGE_SIZE, &phys) != PMM_OK) {
        return 0;
    }

    zero_page((void*)PHYSMAP_P2V(phys));

    return phys;
}

// Levels as the walkers count them: 0 = PML4, 1 = PDPT, 2 = PD, 3 = PT
static inline size_t index_at(uintptr_t virt, int level) {
    return (virt >> (39 - 9 * level)) & PT_ENTRY_MASK;
}

// Bytes one entry covers at a level: 512 GiB, 1 GiB, 2 MiB, 4 KiB
static inline uint64_t span_at(int level) {
    return 1ULL << (39 - 9 * level);
}

/*
 * walk - The entry that maps virt, at whatever level the mapping ends (a 2MiB page ends at the PD).
 * NULL if nothing maps virt. Either way, *level is the level the walk stopped at
 */
static uint64_t* walk(uint64_t pt_root, uintptr_t virt, int* level) {
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(pt_root);

    for (int l = 0; ; l++) {
        uint64_t* e = &table[index_at(virt, l)];
        *level = l;
        if (!(*e & PAGE_PRESENT)) return NULL;
        if (l == 3 || (l == 2 && (*e & PAGE_HUGE))) return e;
        table = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(*e));
    }
}

/*
 * map_one - Maps one page at virt, a 2MiB one when level is 2, a 4KiB one when it's 3
 */
static vmm_status_t map_one(uint64_t pt_root, uint64_t phys, uintptr_t virt, uint64_t pt_flags, int level, bool user) {
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(pt_root);

    for (int l = 0; l < level; l++) {
        uint64_t* e = &table[index_at(virt, l)];
        if (!(*e & PAGE_PRESENT)) {
            uint64_t t = vmm_alloc_page_table();
            if (!t) return VMM_ERR_NO_MEMORY;
            *e = t | PAGE_PRESENT | PAGE_WRITABLE | (user ? PAGE_USER : 0);
        } else if (*e & PAGE_HUGE) {
            return VMM_ERR_ALREADY_MAPPED;
        }
        table = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(*e));
    }

    uint64_t* e = &table[index_at(virt, level)];
    if (*e & PAGE_PRESENT) return VMM_ERR_ALREADY_MAPPED;

    *e = (phys & ADDR_MASK & ~(span_at(level) - 1)) | pt_flags | (level == 2 ? PAGE_HUGE : 0);
    return VMM_OK;
}

/*
 * table_empty - Check if a page table is empty (no present entries)
 */
static bool table_empty(uint64_t* table) {
    for (size_t i = 0; i < PAGE_ENTRIES; ++i) {
        if (table[i] & PAGE_PRESENT) return false;
    }
    return true;
}

/*
 * unmap_one - Clears the entry that maps virt and frees every table that leaves empty.
 * Returns the physical page it mapped, and its size in *size (0 if nothing was mapped)
 */
static uint64_t unmap_one(uint64_t pt_root, uintptr_t virt, size_t* size) {
    uint64_t* path[4];
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(pt_root);
    int l = 0;
    *size = 0;

    for (;; l++) {
        path[l] = table;
        uint64_t e = table[index_at(virt, l)];
        if (!(e & PAGE_PRESENT)) return 0;
        if (l == 3 || (l == 2 && (e & PAGE_HUGE))) break;
        table = (uint64_t*)PHYSMAP_P2V(PT_ENTRY_ADDR(e));
    }

    *size = span_at(l);
    uint64_t phys = PT_ENTRY_ADDR(path[l][index_at(virt, l)]) & ~(*size - 1);
    path[l][index_at(virt, l)] = 0;
    invlpg((void*)virt);

    // The higher half's PML4 entries are copied into every process, so those never go
    int floor = virt >= PHYSMAP_VIRTUAL_BASE ? 1 : 0;
    for (; l > floor && table_empty(path[l]); l--) {
        pmm_free(PHYSMAP_V2P((uintptr_t)path[l]), PAGE_SIZE);
        path[l - 1][index_at(virt, l - 1)] = 0;
    }

    return phys;
}

/*
 * phys_of - Physical address virt maps to
 */
static bool phys_of(uint64_t pt_root, uintptr_t virt, uint64_t* out_phys) {
    int level;
    uint64_t* e = walk(pt_root, virt, &level);
    if (!e) return false;
    uint64_t span = span_at(level);
    *out_phys = (PT_ENTRY_ADDR(*e) & ~(span - 1)) + (virt & (span - 1));
    return true;
}

/*
 * free_tables - Frees a table and every table below it. Never the pages they map
 */
static void free_tables(uint64_t table_phys, int level) {
    uint64_t* table = (uint64_t*)PHYSMAP_P2V(table_phys);

    for (size_t i = 0; level < 3 && i < PAGE_ENTRIES; i++) {
        uint64_t e = table[i];
        if ((e & PAGE_PRESENT) && !(e & PAGE_HUGE)) free_tables(PT_ENTRY_ADDR(e), level + 1);
    }
    pmm_free(table_phys, PAGE_SIZE);
}

/*
 * map_run - Maps [virt, virt + length) to [phys, phys + length), with 2MiB pages wherever both sides line up
 */
static vmm_status_t map_run(vmm_ctx* vmm, uint64_t phys, uintptr_t virt, size_t length, size_t flags, bool huge) {
    uint64_t pt_flags = vmm_convert_vm_flags(flags);
    bool user = !vmm->is_kernel && (flags & VM_FLAG_USER);

    for (size_t off = 0; off < length; ) {
        bool big = huge && !((virt + off) & (PAGE_2MB - 1)) && !((phys + off) & (PAGE_2MB - 1)) &&
                   length - off >= PAGE_2MB;
        vmm_status_t st = map_one(vmm->public.pt_root, phys + off, virt + off, pt_flags, big ? 2 : 3, user);
        if (st != VMM_OK) {
            size_t sz;
            for (size_t rb = 0; rb < off; ) {
                unmap_one(vmm->public.pt_root, virt + rb, &sz);
                rb += sz ? sz : PAGE_SIZE;
            }
            return st;
        }
        off += big ? PAGE_2MB : PAGE_SIZE;
    }
    return VMM_OK;
}

/*
 * release - Unmaps [start, end), giving the pages back to the PMM when owned is set
 */
static void release(vmm_ctx* vmm, uintptr_t start, uintptr_t end, bool owned) {
    for (uintptr_t virt = start; virt < end; ) {
        int level;
        if (!walk(vmm->public.pt_root, virt, &level)) {
            virt = align_down(virt, span_at(level)) + span_at(level);
            continue;
        }

        size_t size;
        uint64_t phys = unmap_one(vmm->public.pt_root, virt, &size);
        if (owned) pmm_free(phys, size);
        virt += size;
    }
}

/*
 * vmm_alloc_vm_object - Allocate a vm_object structure with validation
 */
vmo_ext* vmm_alloc_vm_object(void) {
    void* obj;
    if (slab_alloc(vmo_cache, &obj) != SLAB_OK)
        return NULL;

    vmo_ext* internal = (vmo_ext*)obj;
    kmemset(internal, 0, sizeof(vmo_ext));

    internal->magic = VM_OBJECT_MAGIC;
    internal->red_zone_pre = VM_OBJECT_RED_ZONE;
    internal->red_zone_post = VM_OBJECT_RED_ZONE;

    return internal;
}

/*
 * vmm_free_vm_object - Free a vm_object structure
 */
void vmm_free_vm_object(vmo_ext* obj) {
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

static vmm_status_t vmm_copy_kmaps(uint64_t dest_pt_root) {
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
 * place - Puts a new object at [base, base + length) (lock held)
 */
static vmm_status_t place(vmm_ctx* vmm, uintptr_t base, size_t length, size_t flags, void* arg) {
    bool foreign = flags & VM_FLAG_FOREIGN;
    if (foreign && ((uint64_t)arg & (PAGE_SIZE - 1))) {
        LOGF("[VMM ERROR] MMIO address 0x%lx is not page-aligned\n", (uint64_t)arg);
        return VMM_ERR_NOT_ALIGNED;
    }

    vmo_ext* obj = vmm_alloc_vm_object();
    if (!obj) return VMM_ERR_NO_MEMORY;

    obj->public.base = base;
    obj->public.length = length;
    obj->public.flags = flags;

    if (!(flags & VM_FLAG_LAZY)) {
        uint64_t phys = (uint64_t)arg;
        if (!foreign) {
            if (pmm_alloc_exact(length, &phys) != PMM_OK) {
                vmm_free_vm_object(obj);
                return VMM_ERR_NO_MEMORY;
            }
            kmemset((void*)PHYSMAP_P2V(phys), 0, length);
        }

        vmm_status_t st = map_run(vmm, phys, base, length, flags, !foreign);
        if (st != VMM_OK) {
            if (!foreign) pmm_free_range(phys, length);
            vmm_free_vm_object(obj);
            return st;
        }
    }

    // Inserted last, so vmm_find_mapped_object never sees a half built object
    vma_insert(vmm, obj);
    if (foreign) __atomic_add_fetch(&mmio_bytes, length, __ATOMIC_RELAXED);
    return VMM_OK;
}

/*
 * vmm_alloc - Allocate virtual memory anywhere in the VMM's range
 */
vmm_status_t vmm_alloc(vmm_t* vmm_pub, size_t length, size_t flags, void* arg, void** out_addr) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (length == 0 || !out_addr) return VMM_ERR_INVALID;

    *out_addr = NULL;
    size_t len = align_up(length, PAGE_SIZE);
    if (len < length) return VMM_ERR_OOM;

    // Prefer 2MB aligned virtual base for large allocations so huge pages can fire
    size_t align = (!(flags & (VM_FLAG_FOREIGN | VM_FLAG_LAZY)) && len >= PAGE_2MB) ? PAGE_2MB : PAGE_SIZE;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    uintptr_t base = vma_find_gap(vmm, len, align);
    vmm_status_t st = base ? place(vmm, base, len, flags, arg) : VMM_ERR_OOM;
    spinlock_release(&vmm->lock, lock_flags);

    if (st == VMM_OK) *out_addr = (void*)base;
    return st;
}

/*
 * vmm_alloc_at - Allocate virtual memory at a specific address
 */
vmm_status_t vmm_alloc_at(vmm_t* vmm_pub, void* desired_addr, size_t length, size_t flags, void* arg, void** out_addr) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (length == 0 || !out_addr || !desired_addr) return VMM_ERR_INVALID;

    *out_addr = NULL;
    uintptr_t base = (uintptr_t)desired_addr;
    if (base & (PAGE_SIZE - 1)) {
        LOGF("[VMM] vmm_alloc_at: address 0x%lx not page-aligned\n", base);
        return VMM_ERR_NOT_ALIGNED;
    }

    size_t len = align_up(length, PAGE_SIZE);
    if (len < length || len > UINTPTR_MAX - base) return VMM_ERR_OOM;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    vmm_status_t st;

    if (base < vmm->public.alloc_base || base + len > vmm->public.alloc_end) {
        LOGF("[VMM] vmm_alloc_at: range 0x%lx-0x%lx outside allocatable space\n", base, base + len);
        st = VMM_ERR_OOM;
    } else if (vma_overlaps(vmm, base, len)) {
        LOGF("[VMM] vmm_alloc_at: range 0x%lx-0x%lx overlaps with existing object\n", base, base + len);
        st = VMM_ERR_ALREADY_MAPPED;
    } else {
        st = place(vmm, base, len, flags, arg);
    }

    spinlock_release(&vmm->lock, lock_flags);
    if (st == VMM_OK) *out_addr = desired_addr;
    return st;
}

/*
 * vmm_free - Free a previously allocated virtual memory range
 */
vmm_status_t vmm_free(vmm_t* vmm_pub, void* addr) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    vmo_ext* cur = vma_find_exact(vmm, (uintptr_t)addr);
    if (!cur || !vm_object_validate(cur)) {
        spinlock_release(&vmm->lock, lock_flags);
        return cur ? VMM_ERR_INVALID : VMM_ERR_NOT_FOUND;
    }

    bool owned = !(cur->public.flags & VM_FLAG_FOREIGN);
    release(vmm, cur->public.base, cur->public.base + cur->public.length, owned);
    if (!owned) __atomic_sub_fetch(&mmio_bytes, cur->public.length, __ATOMIC_RELAXED);

    vma_remove(vmm, cur);
    vmm_free_vm_object(cur);

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

#pragma endregion

#pragma region Non Kernel VMM Instance Management

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

    vmm_ctx* vmm = (vmm_ctx *)vmm_mem;
    kmemset(vmm, 0, sizeof(vmm_ctx));

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
        vmm_status_t status = vmm_copy_kmaps(pt_root);
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

void vmm_destroy(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    if (vmm == kernel_vmm) {
        LOGF("[VMM ERROR] Cannot destroy kernel VMM\n");
        return;
    }

    // Acquiring the lock prevents any concurrent alloc from racing teardown
    bool lock_flags = spinlock_acquire(&vmm->lock);

    avl_node_t* n = avl_min(&vmm->vma_tree);
    while (n) {
        vmo_ext* cur = AVL_ENTRY(n, vmo_ext, vma_node);
        if (!vm_object_validate(cur)) {
            LOGF("[VMM ERROR] Corrupted vm_object during destroy\n");
            break;
        }
        avl_node_t* nx = avl_next(n);
        release(vmm, cur->public.base, cur->public.base + cur->public.length,
                !(cur->public.flags & VM_FLAG_FOREIGN));
        vmm_free_vm_object(cur);
        n = nx;
    }
    vmm->vma_tree.root = NULL;

    // Whatever's left in the lower half was mapped without an object (vmm_map_page), so only the
    // tables go. The higher half (entries 256-511) is the kernel's, shared by every process
    uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(vmm->public.pt_root);
    for (size_t i = 0; i < 256; ++i) {
        if (pml4[i] & PAGE_PRESENT) free_tables(PT_ENTRY_ADDR(pml4[i]), 1);
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
 * vmm_fault - Page fault handler, backs lazy objects on their first touch
 * Returns NULL for any other fault, so the dispatcher deals with it
 */
static cpu_context_t* vmm_fault(cpu_context_t* ctx) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    // The active address space first, then the kernel's
    vmm_t* spaces[2] = { vmm_get_current(), vmm_kernel_get() };

    for (int i = 0; i < 2; i++) {
        if (!spaces[i]) continue;
        vm_object* obj = vmm_find_mapped_object(spaces[i], (void*)cr2);
        if (!obj || !(obj->flags & VM_FLAG_LAZY)) continue;

        uint64_t phys;
        if (pmm_alloc(PAGE_SIZE, &phys) != PMM_OK) return NULL;
        zero_page((void*)PHYSMAP_P2V(phys));

        vmm_status_t status = vmm_map_page(spaces[i], phys, (void*)align_down(cr2, PAGE_SIZE), obj->flags);
        if (status == VMM_OK) return ctx;

        // Already mapped means another CPU backed it first, which is fine
        pmm_free(phys, PAGE_SIZE);
        return status == VMM_ERR_ALREADY_MAPPED ? ctx : NULL;
    }

    return NULL;
}

/*
 * vmm_switch - Switch to a different address space
 */
void vmm_switch(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    PML4_switch(vmm->public.pt_root);
    current_vmm = &vmm->public;
}

vmm_t* vmm_get_current(void) {
    // If not explicitly set yet, assume kernel VMM
    if (!current_vmm && kernel_vmm) {
        return &kernel_vmm->public;
    }
    return current_vmm;
}

#pragma endregion

#pragma region Kernel VMM Management

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

    if(cpu_has_feature(CF_NX)){
        cpu_enable_feature(CF_NX);
    }

    // Kernel VMM is allocated from PMM directly for a stable physical address
    uint64_t vmm_phys = 0;
    if (pmm_alloc(sizeof(vmm_ctx), &vmm_phys) != PMM_OK) {
        return VMM_ERR_NO_MEMORY;
    }

    vmm_ctx* vmm = (vmm_ctx*)PHYSMAP_P2V(vmm_phys);
    kmemset(vmm, 0, sizeof(vmm_ctx));

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

    vmm_cache = slab_cache_create("vmm_ctx", sizeof(vmm_ctx), _Alignof(vmm_ctx));
    vmo_cache = slab_cache_create("vmo_ext", sizeof(vmo_ext), _Alignof(vmo_ext));

    if (!vmm_cache || !vmo_cache) {
        LOGF("[VMM] Failed to create slab caches\n");
        return VMM_ERR_NO_MEMORY;
    }

    irq_register(INT_PAGE_FAULT, vmm_fault);

    LOGF("[VMM] Kernel VMM initialized, managing 0x%lx - 0x%lx (%zu MiB)\n",
         alloc_base, alloc_end, (alloc_end - alloc_base) / MEASUREMENT_UNIT_MB);

    return VMM_OK;
}

vmm_t* vmm_kernel_get(void) {
    return kernel_vmm ? &kernel_vmm->public : NULL;
}

#pragma endregion

#pragma region Introspection

/*
 * vmm_get_alloc_base - Get the base of the allocatable range
 */
uintptr_t vmm_get_alloc_base(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return 0;
    return vmm->public.alloc_base;
}

/*
 * vmm_get_alloc_end - Get the end of the allocatable range
 */
uintptr_t vmm_get_alloc_end(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return 0;
    return vmm->public.alloc_end;
}

/*
 * vmm_get_alloc_size - Get the size of the allocatable range
 */
size_t vmm_get_alloc_size(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return 0;
    return vmm->public.alloc_end - vmm->public.alloc_base;
}

#pragma endregion

#pragma region Address Translation and Query

bool vmm_get_physical(vmm_t* vmm_pub, void* virt, uint64_t* out_phys) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm || !out_phys) return false;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    bool result = phys_of(vmm->public.pt_root, (uintptr_t)virt, out_phys);
    spinlock_release(&vmm->lock, lock_flags);
    return result;
}

/*
 * vmm_find_mapped_object - Find vm_object containing a virtual address
 */
vm_object* vmm_find_mapped_object(vmm_t* vmm_pub, void* addr) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm || !addr) return NULL;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    vmo_ext* cur = vma_find_containing(vmm, (uintptr_t)addr);
    if (cur && !vm_object_validate(cur)) {
        LOGF("[VMM ERROR] Corrupted vm_object in list\n");
        spinlock_release(&vmm->lock, lock_flags);
        return NULL;
    }
    spinlock_release(&vmm->lock, lock_flags);
    return cur ? &cur->public : NULL;
}

bool vmm_check_flags(vmm_t* vmm_pub, void* addr, size_t required_flags) {
    vm_object* obj = vmm_find_mapped_object(vmm_pub, addr);
    if (!obj) return false;

    return (obj->flags & required_flags) == required_flags;
}

/*
 * vmm_check_buffer - True if every page of [ptr, ptr+size) is mapped with the requested VM_FLAG_* perms
 */
bool vmm_check_buffer(vmm_t* vmm_pub, const void* ptr, size_t size, size_t required_flags) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm || !ptr || size == 0) return false;

    uintptr_t start = (uintptr_t)ptr;
    if (size > (UINTPTR_MAX - start)) return false;
    uintptr_t end = start + size;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    bool ok = true;
    vmo_ext* obj = NULL;

    for (uintptr_t cur = align_down(start, PAGE_SIZE); ok && cur < end; cur += PAGE_SIZE) {
        if (!obj || cur >= obj->public.base + obj->public.length)
            obj = vma_find_containing(vmm, cur);

        if (!obj || (obj->public.flags & required_flags) != required_flags) {
            ok = false;
            break;
        }

        // A page that isn't there yet is fine for lazy objects, the fault handler brings it in
        int level;
        uint64_t* e = walk(vmm->public.pt_root, cur, &level);
        if (!e) {
            ok = obj->public.flags & VM_FLAG_LAZY;
        } else {
            if ((required_flags & VM_FLAG_WRITE) && !(*e & PAGE_WRITABLE)) ok = false;
            if ((required_flags & VM_FLAG_USER) && !(*e & PAGE_USER)) ok = false;
        }
    }

    spinlock_release(&vmm->lock, lock_flags);
    return ok;
}

#pragma endregion

#pragma region Page Table Manipulation

/*
 * vmm_map_page - Maps a physical address to the specified virtual address
 */
vmm_status_t vmm_map_page(vmm_t* vmm_pub, uint64_t phys, void* virt, size_t flags) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if ((phys & (PAGE_SIZE - 1)) || ((uintptr_t)virt & (PAGE_SIZE - 1))) {
        return VMM_ERR_NOT_ALIGNED;
    }

    bool lock_flags = spinlock_acquire(&vmm->lock);
    vmm_status_t status = map_run(vmm, phys, (uintptr_t)virt, PAGE_SIZE, flags, false);
    spinlock_release(&vmm->lock, lock_flags);
    return status;
}

/*
 * vmm_unmap_page - Unmaps a virtual page (a whole 2MiB one if that's what maps it)
 */
vmm_status_t vmm_unmap_page(vmm_t* vmm_pub, void* virt) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    size_t size;
    unmap_one(vmm->public.pt_root, (uintptr_t)virt, &size);
    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_map_range - Maps a physical range to a virtual range starting at a specific virtual address
 */
vmm_status_t vmm_map_range(vmm_t* vmm_pub, uint64_t phys, void* virt, size_t length, size_t flags) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if ((phys & (PAGE_SIZE - 1)) || ((uintptr_t)virt & (PAGE_SIZE - 1))) {
        return VMM_ERR_NOT_ALIGNED;
    }

    bool lock_flags = spinlock_acquire(&vmm->lock);
    vmm_status_t status = map_run(vmm, phys, (uintptr_t)virt, align_up(length, PAGE_SIZE), flags, !(flags & (VM_FLAG_FOREIGN | VM_FLAG_LAZY)));
    spinlock_release(&vmm->lock, lock_flags);
    return status;
}

/*
 * vmm_unmap_range - Unmaps a virtual range, starting at a specific virtual address
 */
vmm_status_t vmm_unmap_range(vmm_t* vmm_pub, void* virt, size_t length) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;

    bool lock_flags = spinlock_acquire(&vmm->lock);
    release(vmm, (uintptr_t)virt, (uintptr_t)virt + align_up(length, PAGE_SIZE), false);
    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

/*
 * vmm_resize - Grows or shrinks an object in place. New pages come zeroed
 */
vmm_status_t vmm_resize(vmm_t* vmm_pub, void* addr, size_t new_length) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr || new_length == 0) return VMM_ERR_INVALID;

    new_length = align_up(new_length, PAGE_SIZE);
    bool lock_flags = spinlock_acquire(&vmm->lock);
    vmm_status_t st = VMM_OK;

    vmo_ext* cur = vma_find_exact(vmm, (uintptr_t)addr);
    if (!cur || !vm_object_validate(cur)) {
        LOGF("[VMM ERROR] vmm_resize: No object found at address 0x%lx\n", (uintptr_t)addr);
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    uintptr_t base = cur->public.base;
    size_t old_length = cur->public.length;
    int level;

    if (cur->public.flags & VM_FLAG_FOREIGN) {
        LOGF("[VMM ERROR] vmm_resize: Cannot resize MMIO region\n");
        st = VMM_ERR_INVALID;
    } else if (new_length < old_length) {
        
        // Cutting through the middle of a 2MiB page would need it split, so that's refused
        uint64_t* e = walk(vmm->public.pt_root, base + new_length, &level);
        if (e && level == 2 && ((base + new_length) & (PAGE_2MB - 1))) {
            LOGF("[VMM ERROR] vmm_resize: Can't shrink into the middle of a 2MiB page\n");
            st = VMM_ERR_INVALID;
        } else {
            release(vmm, base + new_length, base + old_length, true);
            cur->public.length = new_length;
        }
    } else if (new_length > old_length) {
        size_t growth = new_length - old_length;
        avl_node_t* nx = avl_next(&cur->vma_node);
        uintptr_t limit = nx ? AVL_ENTRY(nx, vmo_ext, vma_node)->public.base : vmm->public.alloc_end;

        if (base + new_length > limit) {
            LOGF("[VMM ERROR] vmm_resize: Growth would overlap with next object\n");
            st = VMM_ERR_OOM;
        } else if (cur->public.flags & VM_FLAG_LAZY) {
            cur->public.length = new_length;
        } else {
            uint64_t phys;
            if (pmm_alloc_exact(growth, &phys) != PMM_OK) {
                st = VMM_ERR_NO_MEMORY;
            } else {
                kmemset((void*)PHYSMAP_P2V(phys), 0, growth);
                st = map_run(vmm, phys, base + old_length, growth, cur->public.flags, false);
                if (st == VMM_OK) cur->public.length = new_length;
                else pmm_free_range(phys, growth);
            }
        }
    }

    spinlock_release(&vmm->lock, lock_flags);
    return st;
}

#pragma endregion

#pragma region Protection

/*
 * vmm_protect - Change the flags of an object in place (no unmap + remap)
 */
vmm_status_t vmm_protect(vmm_t* vmm_pub, void* addr, size_t new_flags) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return VMM_ERR_NOT_INIT;
    if (!addr) return VMM_ERR_INVALID;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    vmo_ext* cur = vma_find_exact(vmm, (uintptr_t)addr);
    if (!cur) {
        spinlock_release(&vmm->lock, lock_flags);
        return VMM_ERR_NOT_FOUND;
    }

    vm_object* obj = &cur->public;
    obj->flags = new_flags;
    uint64_t pt_flags = vmm_convert_vm_flags(new_flags);

    // Lazy pages that aren't there yet pick the new flags up when they fault in
    for (uintptr_t virt = obj->base; virt < obj->base + obj->length; ) {
        int level;
        uint64_t* e = walk(vmm->public.pt_root, virt, &level);
        if (e) {
            *e = (*e & ADDR_MASK) | pt_flags | (*e & PAGE_HUGE);
            invlpg((void*)virt);
        }
        virt = align_down(virt, span_at(level)) + span_at(level);
    }

    spinlock_release(&vmm->lock, lock_flags);
    return VMM_OK;
}

#pragma endregion

/*
 * vmm_mmio_total - Total bytes currently mapped as MMIO across all VMM instances
 */
size_t vmm_mmio_total(void) {
    return __atomic_load_n(&mmio_bytes, __ATOMIC_RELAXED);
}

#endif // GATA_CAP_MEM

// vmm_add_mmio sits outside the GATA_CAP_MEM gate: the bootstrap's memory-map walk calls it
// unconditionally while populating the PMM, and without a heap the count just goes nowhere
#ifdef GATA_CAP_MEM
void vmm_add_mmio(size_t bytes) {
    __atomic_add_fetch(&mmio_bytes, bytes, __ATOMIC_RELAXED);
}
#else
void vmm_add_mmio(size_t bytes) {
    (void)bytes;
}
#endif

#ifdef GATA_CAP_MEM

#pragma region Debugging

/*
 * vmm_dump - Dumps the current VMM layout
 */
void vmm_dump(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    LOGF("%s VMM at %p\n", vmm->is_kernel ? "Kernel" : "User", vmm);
    log_indent(1);
    LOGF("range     0x%lx - 0x%lx\n", vmm->public.alloc_base, vmm->public.alloc_end);
    LOGF("root      0x%lx (phys)\n", vmm->public.pt_root);

    int count = 0;
    for (avl_node_t* it = avl_min(&vmm->vma_tree); it; it = avl_next(it), count++) {
        vmo_ext* current = AVL_ENTRY(it, vmo_ext, vma_node);
        if (!vm_object_validate(current)) {
            LOGF("[corrupted object at index %d]\n", count);
            break;
        }
        LOGF("object %-3d 0x%016lx  0x%08lx bytes  flags 0x%02lx\n", count,
             current->public.base, current->public.length, current->public.flags);
    }
    if (count == 0) LOGF("no objects\n");
    log_indent(-1);

    spinlock_release(&vmm->lock, lock_flags);
}

/*
 * vmm_stats - Get the number of pages handled by the VMM and the number of mapped pages
 */
void vmm_stats(vmm_t* vmm_pub, size_t* out_total, size_t* out_resident) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    bool lock_flags = spinlock_acquire(&vmm->lock);

    size_t total = 0;
    size_t resident = 0;

    for (avl_node_t* it = avl_min(&vmm->vma_tree); it; it = avl_next(it)) {
        vmo_ext* current = AVL_ENTRY(it, vmo_ext, vma_node);
        if (!vm_object_validate(current)) {
            LOGF("[VMM ERROR] Corrupted vm_object during stats\n");
            break;
        }

        uintptr_t end = current->public.base + current->public.length;
        total += current->public.length;
        for (uintptr_t virt = current->public.base; virt < end; ) {
            int level;
            if (walk(vmm->public.pt_root, virt, &level)) resident += span_at(level);
            virt = align_down(virt, span_at(level)) + span_at(level);
        }
    }

    if (out_total) *out_total = total;
    if (out_resident) *out_resident = resident;

    spinlock_release(&vmm->lock, lock_flags);
}

bool vmm_verify_integrity(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) {
        LOGF("[VMM VERIFY] Failed to get VMM instance\n");
        return false;
    }

    bool lock_flags = spinlock_acquire(&vmm->lock);


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
    vmo_ext* prev = NULL;
    int count = 0;

    while (it) {
        vmo_ext* current = AVL_ENTRY(it, vmo_ext, vma_node);

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

    spinlock_release(&vmm->lock, lock_flags);
    return true;
}

#pragma endregion
#endif // GATA_CAP_MEM
