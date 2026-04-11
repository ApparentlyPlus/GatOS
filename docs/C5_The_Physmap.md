# Chapter 5: The Physmap

In the previous chapter, we established a solid C environment: serial ports for early output, a simple `klibc`, a multiboot2 parser that safely relocates its data into the higher half, and `cleanup_kpt` to strip away every mapping outside the kernel's virtual range. We are fully in the higher half now, and everything below `KERNEL_VIRTUAL_BASE` has been unmapped.

The plan for this chapter is to map all of physical memory into a dedicated region of virtual space, the physmap, so that every physical address has a predictable, kernel-accessible virtual counterpart. Once the physmap is in place, every subsystem we write afterwards (allocators, DMA buffers, device drivers) can reach any physical page through a simple offset calculation. It is one of the first things a real kernel needs.

Starting from this chapter, I will be assuming knowledge of everything the docs have covered so far. This means bootstrapping, linkage, higher half, virtual memory, etc. Therefore, I will not be holding the reader's hand, as I have been doing in the past chapters. I am treating you as a fellow OS dev! Be proud!

>[!NOTE]
>Most of what's discussed in this chapter, along with a few concepts we covered earlier, is implemented in the branch [`paging-refactored`](https://github.com/ApparentlyPlus/GatOS/tree/paging-refactored).


## What is the Physmap?

The physmap is a direct, linear mapping of all available physical memory into the kernel's higher-half virtual address space. In practice, this means that every physical address has a fixed, predictable virtual counterpart.

Linux uses the same idea, commonly referred to as the **direct mapping of all physical memory**. On x86-64, Linux reserves a large region of the higher-half kernel space starting at `PAGE_OFFSET` (usually `0xFFFF880000000000`) where every physical frame is mapped linearly. For example, physical address `0x1000` might be accessible at virtual address `0xFFFF880000001000`. This covers all normal RAM, while special mappings (I/O memory, highmem, vmalloc, etc.) are placed in separate regions of the kernel address space.

>[!CAUTION]
> `KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000` **is different from** `PHYSMAP_VIRTUAL_BASE = 0xFFFF880000000000`. 
>
>However, they are both located in the higher half of the canonical address space. As a result, they are completely inaccessible to userspace programs, which operate exclusively in the lower half.
>
> The gap between the physmap base and the kernel base is approximately **122,878 GiB (~122 TiB)** of virtual address space. This vast separation ensures that the physmap region can accommodate any realistic amount of physical memory without ever overlapping with the kernel’s virtual space.
>
> In practice, this guarantees that the physmap remains both **consistently accessible** and **safely isolated** within high memory.

There are multiple advantages to loading the entirety of RAM into higher-half virtual space, including:

* **Constant-Time Translation:** Any physical address can be trivially converted into a virtual address with a fixed offset (and vice versa).
* **Simplified Memory Access:** Kernel subsystems like the page frame allocator, slab allocator, or DMA buffers can directly access any physical page without extra mapping steps.
* **Uniform Addressing:** Drivers and low-level subsystems don't need to worry about temporary mappings or special-purpose page tables. Everything is already in place.
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

We have seen in past chapters that whenever we map a new page, we declare some attributes like `PAGE_PRESENT` or `PAGE_WRITABLE`. Here is the full set of flags GatOS defines in `paging.h`:

```c
#define PAGE_PRESENT        (1ULL << 0)
#define PAGE_WRITABLE       (1ULL << 1)
#define PAGE_USER           (1ULL << 2)
#define PAGE_PWT            (1ULL << 3)  // Page Write Through
#define PAGE_PCD            (1ULL << 4)  // Page Cache Disable
#define PAGE_ACCESSED       (1ULL << 5)
#define PAGE_DIRTY          (1ULL << 6)
#define PAGE_HUGE           (1ULL << 7)
#define PAGE_GLOBAL         (1ULL << 8)
#define PAGE_NO_EXECUTE     (1ULL << 63)
```

These correspond directly to hardware-defined bits in the x86-64 page table entries.

* `PAGE_PRESENT`: Marks the page as present in memory; required for valid mappings.
* `PAGE_WRITABLE`: Allows writes to the page (otherwise it's read-only).
* `PAGE_USER`: Grants access from user-space (otherwise kernel-only).
* `PAGE_PWT` / `PAGE_PCD`: Write-through and cache-disable. Combined, they mark a range as uncacheable, which is important for MMIO regions like the framebuffer where you want writes to reach the hardware rather than sit in a cache line.
* `PAGE_HUGE`: When set at the PD level, the entry maps 2 MiB directly instead of pointing to a PT. We will use this for the framebuffer mapping.
* `PAGE_NO_EXECUTE`: Disables instruction fetches from this page (NX bit).

GatOS also declares a few other important constants:

```c
#define PAGE_SIZE     0x1000UL
#define PAGE_2MB      0x200000UL
#define PAGE_ENTRIES  512
#define FRAME_MASK    0xFFFFF000
#define ADDR_MASK     0x000FFFFFFFFFF000UL

#define PREALLOC_PML4s  1
#define PREALLOC_PDPTs  1
#define PREALLOC_PDs    1
#define PREALLOC_PTs    512
```

* `PAGE_SIZE`: Standard x86-64 page size: 4 KiB.
* `PAGE_2MB`: Size of a huge page at the PD level.
* `PAGE_ENTRIES`: Number of entries in each paging structure: 512.
* `FRAME_MASK`: Bitmask to align addresses to 4 KiB boundaries or strip page flags in a 32-bit context.
* `ADDR_MASK`: Bitmask to isolate the full physical address from a page table entry, stripping the metadata bits.
* The `PREALLOC_*` constants declare how many of each table type was statically reserved at boot time.

Finally, `paging.h` also defines some measurement unit helpers:

```c
#define MEASUREMENT_UNIT_BYTES  1
#define MEASUREMENT_UNIT_KB     1024
#define MEASUREMENT_UNIT_MB     1024*1024
#define MEASUREMENT_UNIT_GB     1024*1024*1024
```

These are purely for readability when calculating memory sizes.

## The Core Idea

Creating the physmap boils down to a very simple idea. Right now, our kernel occupies `[KERNEL_VIRTUAL_BASE, KEND]`. However, thanks to the preallocated page tables, we actually have mappings that extend all the way up to `KERNEL_VIRTUAL_BASE + 1GB`, which is well past `KEND`. This is a huge advantage: the range `[KEND, KERNEL_VIRTUAL_BASE + 1GB]` is mapped but completely empty, essentially giving us leeway to use it however we want.

Since we're running in 64-bit C, and the multiboot2 struct has been copied into the higher half and parsed, we can now see exactly how much physical memory exists on the system. With that information, we can figure out exactly how many page tables (`PML4s`, `PDPTs`, `PDs`, and `PTs`) it will take to map all of RAM into virtual memory.

Once we know the number of tables, we can sum up their sizes (remember, each table is `4KB`) and "reserve" that exact amount of space from `[KEND, KERNEL_VIRTUAL_BASE + 1GB]`. After reserving it, we adjust `KEND` to account for this new reserved area, ensuring the kernel knows this space is off-limits for other purposes. Any leftover space between this new `KEND` and `KERNEL_VIRTUAL_BASE + 1GB` can then be unmapped, since it's no longer needed.

Here's a little ASCII diagram to help you visualize it:

```
Our higher half virtual memory now:

KERNEL_VIRTUAL_BASE                                      KERNEL_VIRTUAL_BASE + 1GB
        |-----------------------------------------------------------|
        |                       Pre-mapped                          |
        |-----------------------------------------------------------|
        |      Kernel      |        Free / Usable for Physmap       |
        | [KV_BASE, KEND]  |          [KEND, KV_BASE + 1GB]         |
        |------------------|----------------------------------------|

After reserving space for physmap page tables:

        |------------------|-------------|--------------------------|
        |      Kernel      |  Reserved   |  Free / Rest of the 1GB  |
        | [KV_BASE, KEND]  |  Tables     |    (to be cleaned up)    |
        |------------------|-------------|--------------------------|

After integrating reserved space into the kernel (KEND updated):

        |-----------------------------|-----------------------------|
        |           Kernel            |          Unmapped           |
        |     [KV_BASE, NEW_KEND]     | (Cleaned up - inaccessible) |
        |-----------------------------|-----------------------------|

```

The final step is to actually populate these new page tables and point `cr3` to them. Here, we have to be careful: the old page tables contain the kernel range where we are currently executing. The new tables map the entirety of physical RAM, but not for execution. This means the execution will still happen in `[KERNEL_VIRTUAL_BASE, KEND]`, and our physical memory will be accessible starting at `PHYSMAP_VIRTUAL_BASE`.

This means that in order to avoid breaking anything, we need to incorporate the old kernel tables into the new tables, ensuring that the kernel's virtual range continues to function exactly as before.

## The `physmap_t` Struct

Before we begin implementing the physmap, it's a good idea to define a struct that holds all the information we need to access, modify, or create it. This way, we can easily make changes later if necessary. We define it as follows:

```c
typedef struct{
    uint64_t total_RAM;   // RAM (physical boundary)
    uint64_t fb_phys;     // framebuffer physical base (crash console)
    uint64_t fb_size;     // framebuffer size in bytes
    uint64_t total_pages;
    uintptr_t tables_base;
    uint64_t total_PTs;
    uint64_t total_PDs;
    uint64_t total_PDPTs;
    uint64_t total_PML4s;
} physmap_t;

static physmap_t physmap = {0};
```

This struct centralizes all the key data for the physmap.

The `total_RAM`, `total_pages`, and table count fields are what you would expect: they track the total RAM size and how many page table structures we need to cover it. The two additions worth highlighting are `fb_phys` and `fb_size`. The framebuffer lives at a physical address the firmware tells us about, and on real hardware that address is almost always outside of RAM — it sits in an MMIO region well above the top of usable memory. Mapping RAM alone therefore won't cover it. We store the framebuffer's physical base and byte size here so that `build_physmap` can add a separate MMIO mapping for it. More on that shortly.

Initializing `physmap` to zero ensures that all fields start in a clean, predictable state. Once this struct is in place, the next step is to populate it with real values derived from the multiboot2 memory map.

>[!IMPORTANT]
>What is MMIO?
>
>Not all physical addresses correspond to actual RAM. Some regions are reserved for memory-mapped I/O (MMIO), where reads and writes interact directly with hardware devices instead of memory. A common example is the framebuffer, where writing to its address updates pixels on the screen. These regions are special because they appear to be memory but aren't.
>
>They must still be mapped into **virtual memory** if we want to interact with them, but they should be treated carefully. Caching, permissions, and access patterns differ from normal RAM, and incorrect handling can lead to undefined behavior.
>
>You can think of MMIO as special addresses that need to be mapped to be interacted with, but are NOT actual memory. They are hardware! Therefore, we also learn that the virtual adress space is not *just* for memory, but for other devices too!

## Reserving the Required Tablespace

To implement the physmap, we first need to reserve a contiguous block of virtual memory to store all the page tables that will map physical RAM, as discussed.

We do this with a function called `reserve_required_tablespace`, which takes our multiboot parser as a parameter. This allows us to query the total RAM, determine how many page tables are needed, and reserve the corresponding virtual space by moving `KEND`. Let's break down the implementation piece by piece.


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
> We use `CEIL_DIV` to round up, ensuring we allocate enough tables even if the number of pages isn't an exact multiple of 512. Here, not having `math.h` is costing us the gimmick of defining `CEIL_DIV` as:
>```c
>#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y))
>```

### Calculating the Total Table Space

```c
uint64_t table_bytes = (total_PTs + total_PDs + total_PDPTs + total_PML4s) * 4 * MEASUREMENT_UNIT_KB;
table_bytes = align_up(table_bytes, PAGE_SIZE);
```

Here we compute the total amount of memory needed to store all the tables. Each table is `4KB` in size, so we multiply the total number of tables by `4KB`. We then align the total table size to a page boundary using `align_up` to ensure proper alignment in virtual memory.

### Safety Checks

```c
PANIC_ASSERT(KEND + table_bytes < (1UL << 30) && KEND + table_bytes < total_RAM);
```

Before committing to the reserved range, we confirm two things: that the extended kernel region still fits within the 1 GiB preallocated window, and that it doesn't extend past the end of RAM itself. If either condition fails, we have either underestimated the required space or the machine simply doesn't have enough contiguous memory in the right place — either way, there is no safe path forward, so a panic is warranted.

We also walk the multiboot memory map to verify that the reserved table range does not land inside a region the firmware has marked unavailable:

```c
for (size_t i = 0; i < multiboot->memory_map_length; i++) {
    uintptr_t region_start, region_end;
    uint32_t region_type;
    if (multiboot_get_memory_region(multiboot, i, &region_start, &region_end, &region_type) != 0)
        continue;
    PANIC_ASSERT(region_type != MULTIBOOT_MEMORY_AVAILABLE
                    && KEND + table_bytes > region_start
                    && KEND + table_bytes < region_end);
}
```

### Probing the Framebuffer

```c
uint64_t fb_phys = 0, fb_size = 0;
multiboot_framebuffer_t* fb = multiboot_get_framebuffer(multiboot);
if (fb) { fb_phys = fb->addr; fb_size = (uint64_t)fb->height * fb->pitch; }
```

While we have the multiboot structure open, we grab the framebuffer's physical address and size. This does not affect `table_bytes` at all, there is no extra space being reserved here. We are simply recording where the framebuffer lives so that `build_physmap` can add a separate MMIO mapping for it later. The size is calculated as `height * pitch` rather than `height * width * (bpp / 8)` because `pitch` is the actual stride in bytes between rows, which the firmware may pad for alignment reasons.

### Updating the `physmap` Struct

```c
physmap.total_RAM = total_RAM;
physmap.fb_phys = fb_phys;
physmap.fb_size = fb_size;
physmap.total_pages = total_pages;
physmap.total_PTs = total_PTs;
physmap.total_PDs = total_PDs;
physmap.total_PDPTs = total_PDPTs;
physmap.total_PML4s = total_PML4s;
physmap.tables_base = (uintptr_t)get_kend(true);
```

All the calculated values are stored in `physmap`. `tables_base` is set to the current `KEND` virtual address, marking the start of the reserved space for our new tables.

### Reserving the Space

```c
KEND += table_bytes;
return table_bytes;
```

Finally, we increase `KEND` by the total size of the reserved tables, effectively carving out this region in the kernel's virtual memory. The function returns the total number of bytes reserved.

## Unmapping the Excess

After moving `KEND` to account for the reserved page table space, we call `cleanup_kpt(0x0, get_kend(false))`. We covered this function's implementation in full in the previous chapter: it zeroes out every page table entry outside the kernel's higher-half range, including the identity map at `PML4[0]` that the assembler boot code originally set up. By the time it returns, the only valid virtual region is the kernel itself together with the newly reserved tablespace, exactly what we want before handing things off to `build_physmap`.

The only difference here is that we make sure to move that call *after* `reserve_required_tablespace`, so that the required tablespace has been incorporated into the kernel region. Then, we can nuke everything else. Remember, `get_kend` returns the *current* kernel end, meaning it returns a runtime adjusted value, not the linker symbol.

## Building the Physmap

The final step is constructing the physmap itself. The groundwork is already in place: we have reserved space for all page tables and cleaned up unnecessary mappings. The main thing to be careful about now is integrating the old page tables to preserve the kernel range while mapping the entirety of physical RAM. Everything lives in a function called `build_physmap`.

### Function Setup

```c
if (physmap.total_RAM == 0) {
    LOGF("[ERROR] No physmap has been built.\n");
    return;
}
```

We begin by checking that `physmap` has been properly populated. If it hasn't, it means the required tablespace hasn't been reserved yet, and we cannot proceed safely.

### Calculating Table Base Addresses

```c
uintptr_t pt_base    = physmap.tables_base;
uintptr_t pd_base    = pt_base   + physmap.total_PTs   * PAGE_SIZE;
uintptr_t pdpt_base  = pd_base   + physmap.total_PDs   * PAGE_SIZE;
uintptr_t pml4_base  = pdpt_base + physmap.total_PDPTs * PAGE_SIZE;
```

Here we calculate the starting addresses for each level of the new page tables within the reserved region. Each base is offset by the total size of the lower-level tables to ensure nothing overlaps.

### Typedefs and Table Pointers

```c
typedef uint64_t pte_t;
typedef pte_t page_table_t[PAGE_ENTRIES];

page_table_t* PTs    = (page_table_t*)pt_base;
page_table_t* PDs    = (page_table_t*)pd_base;
page_table_t* PDPTs  = (page_table_t*)pdpt_base;
page_table_t* PML4   = (page_table_t*)pml4_base;
```

We define `pte_t` for individual page entries and `page_table_t` for arrays of 512 entries. We then create typed pointers to each table level based on the base addresses calculated above.

### Clearing Reserved Space

```c
kmemset((void*)physmap.tables_base, 0,
    (physmap.total_PTs + physmap.total_PDs +
     physmap.total_PDPTs + physmap.total_PML4s) * PAGE_SIZE);
```

We zero out the entire reserved tables region to ensure a clean starting point for the new mappings. This region is accesible because it is within that 1GB preallocated boundary. Otherwise, we would page fault here. `physmap.tables_base` points to the new `KEND`, so right after our kernel's *linker* defined region.

### Filling Page Tables (PTs)

```c
uint64_t pa = 0;
while (pa < physmap.total_RAM) {
    uint64_t pti = (pa >> 12) / PAGE_ENTRIES;
    uint64_t pte = (pa >> 12) % PAGE_ENTRIES;
    PTs[pti][pte] = pa | (PAGE_PRESENT | PAGE_WRITABLE);
    pa += PAGE_SIZE;
}
```

**What's happening here:**

* We step through physical memory `4KB` at a time. For each address, we compute which PT it belongs to (`pti`) and which slot within that PT (`pte`), then write the mapping.
* The stop condition `pa < physmap.total_RAM` means the last PT is left partially filled if RAM isn't an exact multiple of 2 MiB — that's fine, the unused entries stay at zero from the `kmemset` above.

### Mapping the Framebuffer

If the framebuffer's physical address falls outside of RAM, the PT loop above won't cover it. Rather than extending RAM-style 4 KiB mappings all the way up to wherever the firmware placed the framebuffer, we handle it separately using 2 MiB huge pages.

```c
if (physmap.fb_phys && physmap.fb_phys >= physmap.total_RAM) {
    uint64_t fb_end = physmap.fb_phys + physmap.fb_size;
    uint64_t pdpt_s = (physmap.fb_phys >> 30) & 0x1FF;
    uint64_t pdpt_e = ((fb_end - 1) >> 30) & 0x1FF;
    if (pdpt_s == pdpt_e) {
        kmemset(fb_pd, 0, sizeof(fb_pd));
        uint64_t base2m = physmap.fb_phys & ~(uint64_t)(0x1FFFFF);
        uint64_t end2m  = (fb_end + 0x1FFFFF) & ~(uint64_t)(0x1FFFFF);
        for (uint64_t pa2m = base2m; pa2m < end2m; pa2m += 0x200000)
            fb_pd[(pa2m >> 21) & 0x1FF] =
                pa2m | (PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE | PAGE_PWT | PAGE_PCD);
        PDPTs[0][pdpt_s] = KERNEL_V2P(fb_pd) | (PAGE_PRESENT | PAGE_WRITABLE);
    }
}
```

**What's happening here:**

* We compute the PDPT index range the framebuffer spans. If the entire framebuffer fits within a single 1 GiB PDPT region (`pdpt_s == pdpt_e`), we can cover it with a single PD — that's `fb_pd`.
* We round the framebuffer down and up to 2 MiB boundaries, then write one `PAGE_HUGE` entry per 2 MiB chunk into `fb_pd`. Each of those entries maps 2 MiB directly without a PT level beneath it, which is what `PAGE_HUGE` means at the PD layer.
* `PAGE_PWT | PAGE_PCD` disable caching entirely. A framebuffer is MMIO — writes must reach the display hardware, not stall in a cache line.
* We wire the relevant `PDPTs[0]` entry to point to `fb_pd`.

If the framebuffer happens to fall inside RAM (unusual, but possible on some embedded platforms), the PT loop already mapped it and we just go back to fix the cache attributes on those entries to make them uncacheable.

>[!TIP]
>Keeep an eye out for this `fb_pd` variable. 
>
>Here's a little trivia for you: A framebuffer spans the entire screen. If we do NOT know what screen our kernel will run on, how can we map it into virtual memory? We need a number of page tables that we cannot know at compile time. A 720p display framebuffer is much smaller than a 4k framebuffer, and mapping either of them requires certain page tables.
>
> One could argue we could do the same thing we did with the physmap. Just see how big of a framebuffer it is at runtime, then reserve the required tablespace out of that preallocated 1GB, then use it to map the framebuffer into virtual memory. And they would be right!
>
>But... is there a simpler solution? Hmmmm...

### Filling Page Directories (PDs)

```c
uint64_t used_pt = 0;
for (uint64_t i = 0; i < physmap.total_PDs; i++)
    for (int e = 0; e < PAGE_ENTRIES && used_pt < physmap.total_PTs; e++)
        PDs[i][e] = KERNEL_V2P(&PTs[used_pt++]) | (PAGE_PRESENT | PAGE_WRITABLE);
```

**What's happening here:**

* Each PD entry points to a PT. `used_pt` tracks how many PTs have already been assigned.
* `KERNEL_V2P(&PTs[used_pt])` converts the virtual address of the PT into its physical address, since that is what the MMU reads from a PD entry.

### Filling PDPTs

```c
uint64_t used_pd = 0;
for (uint64_t i = 0; i < physmap.total_PDPTs; i++)
    for (int e = 0; e < PAGE_ENTRIES && used_pd < physmap.total_PDs; e++)
        PDPTs[i][e] = KERNEL_V2P(&PDs[used_pd++]) | (PAGE_PRESENT | PAGE_WRITABLE);
```

Same pattern, one level up: each PDPT entry points to a PD.

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
> 1. We don't know in advance how much memory the physmap will occupy. Using the same PDPT could overwrite existing kernel entries.
> 2. Keeping two separate PDPTs provides a clean separation between the kernel and the physmap, making it easier to manage, debug, and extend.
> 3. It also simplifies context switches and reduces the risk of accidentally remapping critical kernel memory.
>
> By maintaining two independent PDPTs, we ensure both ranges are mapped safely and predictably, and our higher-half memory layout remains organized.

Implementing this in C is quite trivial, after you've grasped the logic:

```c
kmemset(PML4, 0, PAGE_SIZE);
uint64_t* old_pml4   = getPML4();
size_t kernel_index  = PML4_INDEX(KERNEL_VIRTUAL_BASE);
size_t physmap_index = PML4_INDEX(PHYSMAP_VIRTUAL_BASE);
PANIC_ASSERT(kernel_index != physmap_index);
PML4[0][kernel_index]  = old_pml4[kernel_index];
PML4[0][physmap_index] = KERNEL_V2P(&PDPTs[0]) | (PAGE_PRESENT | PAGE_WRITABLE);
```

**What's happening here:**

* The PML4 is cleared to start fresh.
* We copy the old kernel entry into its exact index so the kernel remains mapped in the higher-half.
* Then, we place the physmap PDPT at its designated virtual address in the PML4.
* The assertion confirms that the two indexes are distinct — if they weren't, writing the physmap entry would silently clobber the kernel mapping, which would be catastrophic.

No loops are needed here because we only need two specific slots: one for the kernel, one for the physmap.

### Activating the New PML4

```c
PML4_switch(KERNEL_V2P(pml4_base));
flush_tlb();
```

Finally, we load the physical address of the new PML4 into `CR3` to switch the CPU to the new page tables, and flush the TLB to ensure all stale entries are cleared.

## The Static `fb_pd` Array

You might have noticed in the framebuffer mapping code that `fb_pd` was used without any allocation. It is declared at file scope in `paging.c`:

```c
static uint64_t fb_pd[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
```

That's it. One `4KB` array in `.bss`, zero allocation needed. Because `fb_pd` is `static` with no initializer, the compiler places it in the BSS segment, which the linker zeroes before the kernel starts executing. It is already in memory the moment we enter `build_physmap`, properly aligned, and requires nothing from the heap or the physmap itself.

A single PD covers 512 entries × 2 MiB each = 1 GiB of virtual address space. That is more than enough to cover any framebuffer at any resolution. In practice, even a 4K display at 32 bpp (`3840 × 2160 × 4 bytes ≈ 31 MiB`) fits in a handful of those entries. The rest stay zero and are never touched.

The alternative would be to pull the same trick we did with physmap: use that 1GB preallocated virtual memory to reserve the tables we need. But isn't a single `4KB` BSS table smarter and easier?

## Putting It All Together

With all the pieces in place, here is the relevant slice of `kernel_main` as it stands today:

```c
static uint8_t multiboot_buffer[8 * 1024];

void kernel_main(void* mb_info) {
    serial_init_port(COM1_PORT);
    serial_init_port(COM2_PORT);
    QEMU_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

    multiboot_parser_t multiboot = {0};
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

    if (!multiboot.initialized) {
        QEMU_LOG("[KERNEL] Failed to initialize multiboot2 parser!", TOTAL_DBG);
        return;
    }

    QEMU_LOG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

    reserve_required_tablespace(&multiboot);
    QEMU_LOG("Reserved the required space for page tables in the kernel region", TOTAL_DBG);

    cleanup_kpt(0x0, get_kend(false));
    QEMU_LOG("Unmapped all memory besides the higher half kernel range", TOTAL_DBG);

    build_physmap();
    QEMU_LOG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);
}
```

This version of `kernel_main` ties together all previous steps: the multiboot structure is parsed and relocated into the higher half before we strip the identity map, `reserve_required_tablespace` claims the virtual space for the new page tables, `cleanup_kpt` removes everything we no longer need, and `build_physmap` constructs the final address space layout.

Next chapter? Interrupts, Panics, and Spinlocks. Stay tuned!