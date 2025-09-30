# Chapter 5: The Physmap

In the previous chapter, we established a solid C environment: setting up a fully functional `printf`, parsing the multiboot2 structure, and finally removing the lower-half identity mapping.

Now, we will revisit paging with a new goal: mapping the entirety of physical memory (as reported by the multiboot2 structure) into virtual space. Unlike before, I won’t spend any time explaining how paging works; Chapter 2 already covered that in depth.

From here on, I’ll assume familiarity with everything we’ve built so far. That means no re-explaining functions or concepts introduced in earlier chapters. Starting with this chapter, I’ll address you as a fellow OS developer, so expect explanations for the more obvious details to be left out.

>[!NOTE]
>Most of what's discussed in this chapter, along with a few concepts we covered earlier, is implemented in the branch [`paging-refactored`](https://github.com/ApparentlyPlus/GatOS/tree/paging-refactored).


## What is the Physmap?

The physmap is a direct, linear mapping of all available physical memory into the kernel’s higher-half virtual address space. In practice, this means that every physical address has a fixed, predictable virtual counterpart.

Linux uses the same idea, commonly referred to as the **direct mapping of all physical memory**. On x86-64, Linux reserves a large region of the higher-half kernel space starting at `PAGE_OFFSET` (usually `0xFFFF880000000000`) where every physical frame is mapped linearly. For example, physical address `0x1000` might be accessible at virtual address `0xFFFF880000001000`. This covers all normal RAM, while special mappings (I/O memory, highmem, vmalloc, etc.) are placed in separate regions of the kernel address space.

There are multiple advantages to loading the entirety of RAM into higher-half virtual space, including:

* **Constant-Time Translation:** Any physical address can be trivially converted into a virtual address with a fixed offset (and vice versa).
* **Simplified Memory Access:** Kernel subsystems like the page frame allocator, slab allocator, or DMA buffers can directly access any physical page without extra mapping steps.
* **Uniform Addressing:** Drivers and low-level subsystems don’t need to worry about temporary mappings or special-purpose page tables. Everything is already in place.
* **Debugging Convenience:** Having RAM linearly mapped makes tools like memory dumps, page frame debugging, or direct memory inspection significantly easier.

In GatOS, the physmap virtual base is defined as:

```c
#define PHYSMAP_VIRTUAL_BASE 0xFFFF800000000000
```

We also define the following conversion macros:

```c
#ifdef __ASSEMBLER__

#define PHYSMAP_V2P(a) ((a) - PHYSMAP_VIRTUAL_BASE)
#define PHYSMAP_P2V(a) ((a) + PHYSMAP_VIRTUAL_BASE)

#else

#define PHYSMAP_V2P(a) ((uintptr_t)(a) & ~PHYSMAP_VIRTUAL_BASE)
#define PHYSMAP_P2V(a) ((uintptr_t)(a) | PHYSMAP_VIRTUAL_BASE)

#endif
```

## Page Attributes and Constants

We have seen in past chapters that whenever we map a new page, we declare some attributes like `PAGE_PRESENT` or `PAGE_WRITABLE`. Here's a list of attributes that GatOS defines in `paging.h`:

```c
#define PAGE_PRESENT        (1ULL << 0)
#define PAGE_WRITABLE       (1ULL << 1)
#define PAGE_USER           (1ULL << 2)
#define PAGE_NO_EXECUTE     (1ULL << 63)
```

These correspond directly to hardware-defined bits in the x86-64 page table entries.

* `PAGE_PRESENT`: Marks the page as present in memory; required for valid mappings.
* `PAGE_WRITABLE`: Allows writes to the page (otherwise it’s read-only).
* `PAGE_USER`: Grants access from user-space (otherwise kernel-only).
* `PAGE_NO_EXECUTE`: Disables instruction fetches from this page (NX bit).

GatOS also declares a few other constants:

```c
#define PAGE_SIZE     0x1000UL
#define PAGE_ENTRIES  512
#define PAGE_MASK     0xFFFFF000
#define ADDR_MASK     0x000FFFFFFFFFF000UL

#define PREALLOC_PML4s  1
#define PREALLOC_PDPTs  1
#define PREALLOC_PDs    1
#define PREALLOC_PTs    512
```

These constants define the structural layout of paging. 

* `PAGE_SIZE`: Standard x86-64 page size: 4 KiB.
* `PAGE_ENTRIES`: Number of entries in each paging structure: 512.
* `PAGE_MASK`: Bitmask (`0xFFFFF000`) to align addresses to 4 KiB boundaries or strip page flags.
* `ADDR_MASK`: Bitmask (`0x000FFFFFFFFFF000UL`) to isolate the physical address portion of a page table entry (removing metadata bits).
* The `PREALLOC_*` constants simply declare how many of each table type was reserved at boot time.

Finally, in `multiboot2.h`, we also define some measurement units:

```c
#define MEASUREMENT_UNIT_BYTES  1
#define MEASUREMENT_UNIT_KB     1024
#define MEASUREMENT_UNIT_MB     1024*1024
#define MEASUREMENT_UNIT_GB     1024*1024*1024
```

These units are simply helpers for readability.

## The Core Idea

Creating the physmap boils down to a very simple idea. Right now, our kernel occupies `[KERNEL_VIRTUAL_BASE, KEND]`. However, thanks to the preallocated page tables, we actually have mappings that extend all the way up to `KERNEL_VIRTUAL_BASE + 1GB`, which is well past `KEND`. This is a huge advantage: the range `[KEND, KERNEL_VIRTUAL_BASE + 1GB]` is mapped but completely empty, essentially giving us leaway to use it however we want.

Since we’re running in 64-bit C, and the multiboot2 struct has been copied into the higher half and parsed, we can now see exactly how much physical memory exists on the system. With that information, we can figure out exactly how many page tables (`PML4s`, `PDPTs`, `PDs`, and `PTs`) it will take to map all of RAM into virtual memory.

Once we know the number of tables, we can sum up their sizes (remember, each table is `4KB`) and “reserve” that exact amount of space from `[KEND, KERNEL_VIRTUAL_BASE + 1GB]`. After reserving it, we adjust `KEND` to account for this new reserved area, ensuring the kernel knows this space is off-limits for other purposes. Any leftover space between this new `KEND` and `KERNEL_VIRTUAL_BASE + 1GB` can then be unmapped, since it’s no longer needed.

The final step is to actually populate these new page tables and point `cr3` to them. Here, we have to be careful: the old page tables contain the kernel range where we are currently executing. The new tables map the entirety of physical RAM, but not for execution. This means the execution will still happen in `[KERNEL_VIRTUAL_BASE, KEND]`, and our physical memory will be accessible starting at `PHYSMAP_VIRTUAL_BASE`. 

This means that in order to avoid breaking anything, we need to incorporate the old kernel tables into the new tables, ensuring that the kernel’s virtual range continues to function exactly as before.


## The `physmapInfo` Struct

Before we begin implementing the physmap, it’s a good idea to define a struct that holds all the information we need to access, modify, or create it. This way, we can easily make changes later if necessary. We define it as follows:

```c
typedef struct{
    uint64_t total_RAM;
    uint64_t total_pages;
    uintptr_t tables_base;
    uint64_t total_PTs;
    uint64_t total_PDs;
    uint64_t total_PDPTs;
    uint64_t total_PML4s;
} physmapInfo;

static physmapInfo physmapStruct = {0};
```

This struct centralizes all the key data for the physmap. 

Initializing `physmapStruct` to zero ensures that all fields start in a clean, predictable state, preventing any leftover or random values from interfering with our calculations. Once this struct is in place, the next step is to populate it with real values derived from the multiboot2 memory map, which will guide the allocation and setup of the new page tables.

## Reserving the Required Tablespace

To implement the physmap, we first need to reserve a contiguous block of virtual memory to store all the page tables that will map physical RAM, as discussed.

We do this with a function called `reserve_required_tablespace`, which takes our multiboot parser as a parameter. This allows us to query the total RAM, determine how many page tables are needed, and reserve the corresponding virtual space by moving `KEND`. Let’s break down the implementation piece by piece.


### Aligning Values

```c
uintptr_t align_up(uintptr_t val, uintptr_t align) {
    return (val + align - 1) & ~(align - 1);
}
```

This helper function rounds a value `val` up to the nearest multiple of `align`. We use it to ensure that both physical memory totals and reserved table space are aligned to page boundaries (`4KB`). Alignment is critical because x86-64 page tables and pages must always start on properly aligned addresses.


### Calculating RAM and Pages

```c
uint64_t total_RAM = align_up(multiboot_get_total_RAM(multiboot, MEASUREMENT_UNIT_BYTES), PAGE_SIZE);
uint64_t total_pages = total_RAM / PAGE_SIZE;
```

Here, we query the multiboot parser to get the total amount of physical RAM in bytes. We immediately round it up to the nearest page boundary using `align_up` to avoid fractional pages. Dividing by `PAGE_SIZE` then gives us the total number of `4KB` pages in the system.

### Determining Page Table Counts

```c
uint64_t total_PTs    = CEIL_DIV(total_pages, PAGE_ENTRIES);
uint64_t total_PDs    = CEIL_DIV(total_PTs, PAGE_ENTRIES);
uint64_t total_PDPTs  = CEIL_DIV(total_PDs, PAGE_ENTRIES);
uint64_t total_PML4s  = CEIL_DIV(total_PDPTs, PAGE_ENTRIES);
```

Next, we calculate how many page tables of each level are needed to map all of RAM:

* `total_PTs`: Number of standard page tables (maps individual pages).
* `total_PDs`: Number of page directories, each pointing to 512 page tables.
* `total_PDPTs`: Number of page-directory pointer tables, each pointing to 512 directories.
* `total_PML4s`: Number of top-level PML4 tables, each pointing to 512 PDPTs.

>[!NOTE]
> We use `CEIL_DIV` to round up, ensuring we allocate enough tables even if the number of pages isn’t an exact multiple of 512. Here, not having `math.h` is costing us the gimmick of defining `CEIL_DIV` as:
>```c
>#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y))
>```

### Calculating the Total Table Space

```c
uint64_t table_bytes = (total_PTs + total_PDs + total_PDPTs + total_PML4s) * 4 * MEASUREMENT_UNIT_KB;
table_bytes = align_up(table_bytes, PAGE_SIZE); //align to 4kb
```

Here we compute the total amount of memory needed to store all the tables. Each table is `4KB` in size, so we multiply the total number of tables by `4KB`. We then align the total table size to a page boundary using `align_up` to ensure proper alignment in virtual memory.


### Updating the Physmap Struct

```c
physmapStruct.total_RAM = total_RAM;
physmapStruct.total_pages = total_pages;
physmapStruct.total_PTs = total_PTs;
physmapStruct.total_PDs = total_PDs;
physmapStruct.total_PDPTs = total_PDPTs;
physmapStruct.total_PML4s = total_PML4s;
physmapStruct.tables_base = (uintptr_t)get_kend(true);
```

All the calculated values are stored in `physmapStruct`. This centralizes all the information about the physmap, making it easy to reference or modify later. `tables_base` is set to the current `KVIRT_END`, marking the start of the reserved space for our new tables, and pointing to the first table.

### Reserving the Space

```c
KEND += table_bytes;
return table_bytes;
```

Finally, we increase `KEND` by the total size of the reserved tables, effectively carving out this region in the kernel’s virtual memory. The function returns the total number of bytes reserved.

## Unmapping the Excess

After moving `KEND` to account for the reserved page table space, we no longer need the old, now-unused mappings beyond the kernel range. To clean these up, we use the function `cleanup_kernel_page_tables`, which carefully preserves the kernel’s higher-half mapping while zeroing out everything else. Let’s break it down segment by segment.

### Function Signature

```c
void cleanup_kernel_page_tables(uintptr_t start, uintptr_t end)
```

This function takes two parameters: `start` and `end`, which define the kernel’s **physical** memory range that we want to preserve. Everything outside this range can be safely unmapped.

### Getting Page Table Pointers

```c
uint64_t* PML4 = getPML4();
uint64_t* PDPT = PML4 + 512 * PREALLOC_PML4s;
uint64_t* PD = PDPT + 512 * PREALLOC_PDPTs;
uint64_t* PT = PD + 512 * PREALLOC_PDs;
```

Here, we retrieve the top-level PML4 pointer and calculate the locations of the PDPT, PD, and PT structures in memory. These offsets rely on the preallocated table counts defined earlier.

---

### Sanity Checks

```c
uintptr_t kernel_size = end - start;
if (kernel_size > (1UL << 30)) return; // > 1 GiB not allowed
if ((start & 0xFFF) != 0 || (end & 0xFFF) != 0) return; // alignment check
```

We perform basic sanity checks: the kernel size must not exceed 1 GiB, and both `start` and `end` must be page-aligned. This ensures that we don’t accidentally zero out invalid memory or misalign page table entries.

---

### Computing Higher-Half Virtual Addresses

```c
uintptr_t virt_start = start + KERNEL_VIRTUAL_BASE;
uintptr_t virt_end   = end   + KERNEL_VIRTUAL_BASE;
```

Since the kernel runs in the higher-half, we compute the corresponding virtual addresses for the start and end of the kernel range. All subsequent calculations are done in terms of higher-half virtual addresses.

---

### Calculating Page Table Indices

```c
size_t hh_pml4 = (virt_start >> 39) & 0x1FF;
size_t hh_pdpt = (virt_start >> 30) & 0x1FF;
size_t hh_pd_start = (virt_start >> 21) & 0x1FF;
size_t hh_pd_end   = ((virt_end - 1) >> 21) & 0x1FF;

uintptr_t start_page = start >> 12;
uintptr_t end_page   = (end - 1) >> 12;
size_t total_pages = end_page - start_page + 1;
size_t total_pds = hh_pd_end + 1;
```

These lines compute the indices into the PML4, PDPT, and PD tables for the kernel’s virtual memory range. They also calculate the first and last physical pages covered and the total number of pages to preserve.

---

### Cleaning PML4 Entries

```c
for (size_t i = 0; i < 512; i++) {
    if (i != hh_pml4) {
        PML4[i] = 0;
    }
}
PML4[hh_pml4] = KERNEL_V2P(PDPT) | (PAGE_PRESENT | PAGE_WRITABLE);
```

We zero out all PML4 entries except the one corresponding to the higher-half kernel. Then, we set that entry to point to our PDPT with present and writable flags.

---

### Cleaning PDPT Entries

```c
for (size_t i = 0; i < 512; i++) {
    if (i != hh_pdpt) {
        PDPT[i] = 0;
    }
}
PDPT[hh_pdpt] = KERNEL_V2P(PD) | (PAGE_PRESENT | PAGE_WRITABLE);
```

Similarly, all PDPT entries except the one for the kernel are cleared. The remaining entry is updated to point to the PD.

---

### Cleaning PD Entries

```c
for (size_t i = 0; i < 512; i++) {
    if (!(i >= hh_pd_start && i <= hh_pd_end)) {
        PD[i] = 0;
    }
}
for (size_t pd_index = hh_pd_start; pd_index <= hh_pd_end; ++pd_index) {
    PD[pd_index] = KERNEL_V2P(PT + ((pd_index - hh_pd_start) << 9)) | (PAGE_PRESENT | PAGE_WRITABLE);
}
```

All PD entries outside the kernel range are cleared. For the ones covering the kernel, we set them to point to the corresponding PTs with present and writable flags.

---

### Cleaning PT Entries

```c
for (size_t i = 0; i < (512 * (hh_pd_end - hh_pd_start + 1)); i++) {
    if (i >= total_pages) {
        PT[i] = 0;
    }
}
for (uintptr_t i = 0; i < total_pages; ++i) {
    uintptr_t phys = (start_page + i) << 12;
    PT[i] = phys | (PAGE_PRESENT | PAGE_WRITABLE);
}
```

We first clear any PT entries beyond the kernel’s pages, then populate the PTs with mappings to the kernel’s physical pages, ensuring all higher-half kernel memory remains mapped and writable.

---

### Flushing the TLB

```c
flush_tlb();
```

Finally, we flush the TLB to ensure the CPU doesn’t use stale translations. This step is crucial to guarantee that our newly cleaned page tables are correctly recognized by the MMU.


## Building the Physmap

The final step in our approach is constructing the physmap itself. The groundwork is already in place: we have reserved space for all page tables and cleaned up unnecessary mappings. The main thing to be careful about now is integrating the old page tables to preserve the kernel range while mapping the entirety of physical RAM. We will implement everything in a function called `build_physmap`.

---

### Function Setup

```c
if(physmapStruct.total_RAM == 0){
    printf("[ERROR] No physmapStruct has been built. The required tablespace has not been reserved.");
    return;
}
```

We begin by checking that `physmapStruct` has been properly populated. If it hasn’t, it means the required tablespace hasn’t been reserved yet, and we cannot proceed.

---

### Calculating Table Base Addresses

```c
uintptr_t pt_base    = physmapStruct.tables_base;
uintptr_t pd_base    = pt_base + physmapStruct.total_PTs * PAGE_SIZE;
uintptr_t pdpt_base  = pd_base + physmapStruct.total_PDs * PAGE_SIZE;
uintptr_t pml4_base  = pdpt_base + physmapStruct.total_PDPTs * PAGE_SIZE;
```

Here we calculate the starting addresses for each level of the new page tables within the reserved region. Each base is offset by the total size of the lower-level tables to ensure tables do not overlap.

---

### Typedefs and Table Pointers

```c
typedef uint64_t pte_t;
typedef pte_t page_table_t[PAGE_ENTRIES];

page_table_t* PTs    = (page_table_t*)pt_base;
page_table_t* PDs    = (page_table_t*)pd_base;
page_table_t* PDPTs  = (page_table_t*)pdpt_base;
page_table_t* PML4   = (page_table_t*)pml4_base;
```

We define `pte_t` for individual page entries and `page_table_t` for arrays of 512 entries. We then create typed pointers to the PTs, PDs, PDPTs, and PML4 tables based on the base addresses calculated above.

---

### Clearing Reserved Space

```c
memset((void*)physmapStruct.tables_base, 0, 
    (physmapStruct.total_PTs
        +physmapStruct.total_PDs
        +physmapStruct.total_PDPTs
        +physmapStruct.total_PML4s) * PAGE_SIZE);
```

We zero out the entire reserved tables region to ensure a clean starting point for the new mappings.

---

### Filling Page Tables (PTs)

```c
uintptr_t phys_addr = 0;
for (uint64_t pt_index = 0; pt_index < physmapStruct.total_PTs; pt_index++) {
    for (int e = 0; e < PAGE_ENTRIES && phys_addr < physmapStruct.total_RAM; e++) {
        PTs[pt_index][e] = phys_addr | (PAGE_PRESENT | PAGE_WRITABLE);
        phys_addr += PAGE_SIZE;
    }
}
```

**What's happening here:**

* `phys_addr` keeps track of the current physical address we are mapping.
* The outer loop iterates over each PT in the reserved region. Remember, each PT can hold 512 entries (`PAGE_ENTRIES`).
* The inner loop fills each entry of the current PT with the next physical page address, marking it as present and writable.
* The `&& phys_addr < total_RAM` condition ensures we stop once all physical memory has been mapped, even if the last PT is not fully used.

Effectively, this loop “flattens” all physical memory into page-sized chunks and stores them sequentially in the PTs.

---

### Filling Page Directories (PDs)

```c
uint64_t used_pt = 0;
for (uint64_t i = 0; i < physmapStruct.total_PDs; i++) {
    for (int e = 0; e < PAGE_ENTRIES && used_pt < physmapStruct.total_PTs; e++) {
        PDs[i][e] = KERNEL_V2P(&PTs[used_pt]) | (PAGE_PRESENT | PAGE_WRITABLE);
        used_pt++;
    }
}
```

**What's happening here:**

* Each PD entry points to a PT. `used_pt` tracks how many PTs we have already assigned.
* The outer loop iterates over all PDs we reserved, and the inner loop fills each PD with pointers to up to 512 PTs.
* `KERNEL_V2P(&PTs[used_pt])` converts the virtual address of the PT into a physical address, which the page directory requires.

In short, this loop organizes the PTs into 512-entry blocks, letting the page directory reference every PT sequentially.

---

### Filling PDPTs

```c
uint64_t used_pd = 0;
for (uint64_t i = 0; i < physmapStruct.total_PDPTs; i++) {
    for (int e = 0; e < PAGE_ENTRIES && used_pd < physmapStruct.total_PDs; e++) {
        PDPTs[i][e] = KERNEL_V2P(&PDs[used_pd]) | (PAGE_PRESENT | PAGE_WRITABLE);
        used_pd++;
    }
}
```

**What's happening here:**

* Each PDPT entry points to a PD. `used_pd` tracks how many PDs we have already assigned.
* The outer loop iterates over all PDPTs we reserved, and the inner loop fills each PDPT with up to 512 PDs.
* Again, `KERNEL_V2P` converts the virtual pointer to a physical one, which the MMU requires.

This loop essentially groups PDs into 512-entry blocks, forming the next level of the hierarchy.

---

### Integrating Kernel and Physmap into the PML4

This is the critical part we need to handle carefully. Using [`virt_breakdown.py`](tools/virt_breakdown.py) to analyze the virtual addresses of `KERNEL_VIRTUAL_BASE` and `PHYSMAP_VIRTUAL_BASE` gives the following:

```
> Enter address: 0xFFFFFFFF80000000
Virtual Address: 0xFFFFFFFF80000000
  PML4 Index : 0x01FF (511)
  PDPT Index : 0x01FE (510)   <--- Old PDPT
  PD   Index : 0x0000 (0)
  PT   Index : 0x0000 (0)

> Enter address: 0xFFFF800000000000
Virtual Address: 0xFFFF800000000000
  PML4 Index : 0x0100 (256)
  PDPT Index : 0x0000 (0)     <--- New PDPT
  PD   Index : 0x0000 (0)
  PT   Index : 0x0000 (0)
```

From this breakdown, we can see that:

1. The kernel resides under PML4 index `511`, pointing to the old PDPT that contains all our existing kernel mappings.
2. The physmap resides under PML4 index `256`, pointing to a brand-new PDPT that we will populate with the physmap page tables.

In other words, our new PML4 must contain **two separate entries**:

* `PML4[511]` points to the old PDPT (kernel mappings).
* `PML4[256]` points to the new PDPT (physmap).

>[!IMPORTANT]
> **Why not just use the same PDPT for both the kernel and the physmap?**
> 
> It might seem tempting to point both PML4 entries to the same PDPT and rely on a single PDPT to handle both mappings. However, this is risky:
>
> 1. We don’t know in advance how much memory the physmap will occupy. Using the same PDPT could overwrite existing kernel entries.
> 2. Keeping two separate PDPTs provides a clean separation between the kernel and the physmap, making it easier to manage, debug, and extend.
> 3. It also simplifies context switches and reduces the risk of accidentally remapping critical kernel memory.
>
> By maintaining two independent PDPTs, we ensure both ranges are mapped safely and predictably, and our higher-half memory layout remains organized.

Implementing this in C is quite trivial, after you've grasped the logic:

```c
memset(PML4, 0, PAGE_SIZE);

uint64_t *old_pml4 = getPML4();
size_t kernel_index = (KERNEL_VIRTUAL_BASE >> 39) & 0x1FF;
PML4[0][kernel_index] = old_pml4[kernel_index];

size_t physmap_index = (PHYSMAP_VIRTUAL_BASE >> 39) & 0x1FF;
PML4[0][physmap_index] = KERNEL_V2P(&PDPTs[0]) | (PAGE_PRESENT | PAGE_WRITABLE);
```

**What's happening here:**

* The PML4 is cleared to start fresh.
* We copy the old kernel entry into its exact index so the kernel remains mapped in the higher-half.
* Then, we place the physmap PDPT at its designated virtual address in the PML4.

No loops are needed here because the PML4 only has 512 entries, and we only need two specific slots: one for the kernel, one for the physmap.

---

### Activating the New PML4

```c
uintptr_t pml4_phys = KERNEL_V2P(pml4_base);
asm volatile("mov %0, %%cr3" :: "r"(pml4_phys));
flush_tlb();
```

Finally, we load the physical address of the new PML4 into `CR3` to switch the CPU to the new page tables, and flush the TLB to ensure all stale entries are cleared.

## Putting It All Together

With all the pieces in place, we can now refactor GatOS's `kernel_main` to match the `v1.5.7` release. Everything we’ve discussed — multiboot parsing, extending the kernel region, cleaning up old mappings, and building the physmap — comes together here:

```c
#include <memory/paging.h>
#include <libc/string.h>
#include <multiboot2.h>
#include <vga_console.h>
#include <vga_stdio.h>
#include <misc.h>
#include <serial.h>
#include <debug.h>

#define TOTAL_DBG 7

static char* KERNEL_VERSION = "v1.5.7";
static uint8_t multiboot_buffer[8 * 1024];

void kernel_main(void* mb_info) {
    DEBUG_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

    console_clear();
    print_banner(KERNEL_VERSION);

    multiboot_parser_t multiboot = {0};
    
    // Initialize multiboot parser (copies all data to higher half)
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

    if (!multiboot.initialized) {
        printf("[KERNEL] Failed to initialize multiboot2 parser!\n");
        return;
    }

    DEBUG_LOG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

    // Extend kernel region to reserve space for the page tables mapping all physical memory
    reserve_required_tablespace(&multiboot);
    printf("[MEM] Kernel region extended to include page tables.\n");
    DEBUG_LOG("Reserved the required space for page tables in the kernel region", TOTAL_DBG);

    // Clean up everything outside the kernel range
    cleanup_kernel_page_tables(0x0, get_kend(false));
    printf("[MEM] Cleaned up page tables, unmapped everything besides the kernel range.\n");
    DEBUG_LOG("Unmapped all memory besides the kernel range", TOTAL_DBG);
    
    // Remove the identity mapping; only the higher half remains
    unmap_identity();
    printf("[MEM] Unmapped identity mapping, only higher half remains.\n");
    DEBUG_LOG("Unmapped identity mapping, only higher half remains", TOTAL_DBG);

    // Build the physmap to map all physical RAM into virtual space
    build_physmap();
    printf("[MEM] Built physmap, all physical memory is now accessible.\n");
    DEBUG_LOG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);

    // Final sanity check to ensure kernel is correctly positioned
    check_kernel_position();
    DEBUG_LOG("Reached kernel end", TOTAL_DBG);
}
```

> [!NOTE]
> Some helper functions, like `print_banner` or `check_kernel_position`, are implemented in a separate helper file, [`misc.c`](/src/impl/kernel/misc.c).

This version of `kernel_main` now ties together all previous steps: the kernel runs entirely in the higher half, the physmap provides access to all physical memory, and the old mappings have been safely removed.

In the next chapter, we will finally dive into the long-awaited interrupts. Until then, take care!