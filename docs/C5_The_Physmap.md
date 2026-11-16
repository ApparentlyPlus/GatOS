# Chapter 5: The Physmap

In the previous chapter, we established a solid C environment: serial ports for early output, a simple `klibc`, a multiboot2 parser that safely relocates its data into the higher half, and `unmap_identity` to drop the identity map. We are fully in the higher half now, and nothing below `KERNEL_VIRTUAL_BASE` is mapped anymore.

The plan for this chapter is to map all of physical memory into a dedicated region of virtual space, the physmap, so that every physical address has a predictable, kernel-accessible virtual counterpart. Once the physmap is in place, every subsystem we write afterwards (allocators, DMA buffers, device drivers) can reach any physical page through a simple offset calculation. It is one of the first things a real kernel needs.

Starting from this chapter, I will be assuming knowledge of everything the docs have covered so far. This means bootstrapping, linkage, higher half, virtual memory, etc. Therefore, I will not be holding the reader's hand, as I have been doing in the past chapters. I am treating you as a fellow OS dev! Be proud!


## What is the Physmap?

The physmap is a direct, linear mapping of all available physical memory into the kernel's higher-half virtual address space. In practice, this means that every physical address has a fixed, predictable virtual counterpart.

Linux uses the same idea, commonly referred to as the **direct mapping of all physical memory**. On x86-64, Linux reserves a large region of the higher-half kernel space starting at `PAGE_OFFSET` (usually `0xFFFF880000000000`) where every physical frame is mapped linearly. For example, physical address `0x1000` might be accessible at virtual address `0xFFFF880000001000`. This covers all normal RAM, while special mappings (I/O memory, highmem, vmalloc, etc.) are placed in separate regions of the kernel address space.

>[!CAUTION]
> `KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000` **is different from** `PHYSMAP_VIRTUAL_BASE = 0xFFFF800000000000`. 
>
>However, they are both located in the higher half of the canonical address space. As a result, they are completely inaccessible to userspace programs, which operate exclusively in the lower half.
>
> Careful, this is NOT Linux's `0xFFFF880000000000` from above. GatOS puts the physmap right at the start of the higher half.
>
> The gap between the physmap base and the kernel base is approximately **131,070 GiB (~128 TiB)** of virtual address space. This vast separation ensures that the physmap region can accommodate any realistic amount of physical memory without ever overlapping with the kernel’s virtual space.
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
* `PAGE_PWT` / `PAGE_PCD`: Write-through and cache-disable. Combined, they mark a range as uncacheable, which is important for MMIO regions like the framebuffer where you want writes to reach the hardware rather than sit in a cache line. (Technically, that's a lie. These two bits are an index into a CPU table called the PAT. We'll abuse that later for the framebuffer.)
* `PAGE_HUGE`: When set at the PD level, the entry maps 2 MiB directly instead of pointing to a PT. Our boot tables already use it, and so will most of the physmap.
* `PAGE_NO_EXECUTE`: Disables instruction fetches from this page (NX bit).

GatOS also declares a few other important constants:

```c
#define PAGE_SIZE     0x1000UL
#define PAGE_2MB      0x200000UL
#define PAGE_ENTRIES  512
#define ADDR_MASK     0x000FFFFFFFFFF000UL
```

* `PAGE_SIZE`: Standard x86-64 page size: 4 KiB.
* `PAGE_2MB`: Size of a huge page at the PD level.
* `PAGE_ENTRIES`: Number of entries in each paging structure: 512.
* `ADDR_MASK`: Bitmask to isolate the full physical address from a page table entry, stripping the metadata bits.
* Plus the `PREALLOC_*` counts we met in Chapter 4.

Finally, `paging.h` also defines some measurement unit helpers:

```c
#define MEASUREMENT_UNIT_BYTES  (1)
#define MEASUREMENT_UNIT_KB     (1024)
#define MEASUREMENT_UNIT_MB     (1024*1024)
#define MEASUREMENT_UNIT_GB     (1024*1024*1024)
```

These are purely for readability when calculating memory sizes.

## The Core Idea

Creating the physmap boils down to a very simple idea. Right now, our kernel occupies `[KERNEL_VIRTUAL_BASE, KEND]`. However, thanks to the preallocated page tables, we actually have mappings that extend all the way up to `KERNEL_VIRTUAL_BASE + 1GB`, which is well past `KEND`. This is a huge advantage: the range `[KEND, KERNEL_VIRTUAL_BASE + 1GB]` is mapped but completely empty, essentially giving us leeway to use it however we want.

Since we're running in 64-bit C, and the multiboot2 struct has been copied into the higher half and parsed, we can now see exactly how much physical memory exists on the system. With that information, we can figure out exactly how many page tables it will take to map all of RAM into virtual memory.

Once we know the number of tables, we can sum up their sizes (remember, each table is `4KB`) and "reserve" that exact amount of space from `[KEND, KERNEL_VIRTUAL_BASE + 1GB]`. After reserving it, we adjust `KEND` to account for this new reserved area, ensuring the kernel knows this space is off-limits for other purposes. Whatever is left between this new `KEND` and `KERNEL_VIRTUAL_BASE + 1GB` simply won't exist in the new tables, since nothing needs it anymore.

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

This means that in order to avoid breaking anything, we rebuild the kernel's own mappings inside the new tables (sourced from the reserved pool) so the kernel's virtual range continues to function exactly as before, even once everything the assembler set up is thrown away.

## Big Pages, Fewer Tables

Before we count anything, let's think about *what* we're counting.

The obvious way to map RAM is with normal `4KB` pages. One PT holds 512 entries, so one PT maps `2MB`. That sounds fine until you do the math for a real machine. With `64GB` of RAM, that's 32,768 PTs, or `128MB` of page tables. Just to *describe* the memory!

Remember `PAGE_HUGE` from Chapter 3? Our boot tables used it to map a whole `2MB` from a single PD entry, no PT needed. The physmap does exactly the same. Now one PD maps `1GB`, and those same `64GB` take 64 PDs, a whopping `256KB`. On QEMU's default 128 MiB machine, the entire table pool (kernel tables included) comes out to `44KB`.

The physmap never needs anything finer than that. Its whole job is "physical address `X` lives at `PHYSMAP_VIRTUAL_BASE + X`", so there's no reason to map it in tiny pieces.

Well, almost.

>[!WARNING]
> The page table bits aren't the only thing that decides how memory gets cached. The CPU has another set of registers where the firmware marks ranges of physical memory with their own caching rules (they're called MTRRs, and GatOS never touches them).
>
> In the first MiB, firmware tends to use several different rules, a few KB apart (old video memory, the BIOS ROM, and friends). Intel calls a `2MB` page that spans more than one of those rules undefined behavior. So the first `2MB` of the physmap get one ordinary PT with `4KB` pages instead. The rest is `2MB` huge pages.

## Where the Numbers Live

Remember `kmem` from the previous chapter? Its two other fields get filled in here:

* `kmem->ram`: where usable RAM ends. Everything below this is fair game for the physmap.
* `kmem->physmap_end`: where the physmap ends. Usually that's `ram`, but you'll see in a second why it can be more.

Inside `paging.c`, the struct behind `kmem` is called `layout`. It's the one file that writes to it, so the code below uses `layout.[field]` directly. Everything else `build_physmap` needs is private to `paging.c` too, in a small struct it reads back later:

```c
typedef struct {
    uint64_t wc_start;    // [wc_start, wc_end) is mapped write combining (framebuffer)
    uint64_t wc_end;
    uintptr_t tables_base;
    uint64_t total_PDs;
    uint64_t total_PDPTs;
    uint64_t total_kernel_PTs;
} physmap_t;

static physmap_t physmap = {0};
```

`tables_base` is where our reserved tables start, and the three counts say how many of each we reserved. The `wc_*` pair is about the framebuffer, which is where `physmap_end` comes in. The framebuffer lives at a physical address the firmware tells us about, and on real hardware that address is almost always outside of RAM. It sits in an MMIO region, usually well above the top of usable memory.

>[!IMPORTANT]
>What is MMIO?
>
>Not all physical addresses correspond to actual RAM. Some regions are reserved for memory-mapped I/O (MMIO), where reads and writes interact directly with hardware devices instead of memory. A common example is the framebuffer, where writing to its address updates pixels on the screen. These regions are special because they appear to be memory but aren't.
>
>They must still be mapped into **virtual memory** if we want to interact with them, but they should be treated carefully. Caching, permissions, and access patterns differ from normal RAM, and incorrect handling can lead to undefined behavior.
>
>You can think of MMIO as special addresses that need to be mapped to be interacted with, but are NOT actual memory. They are hardware! Therefore, we also learn that the virtual address space is not *just* for memory, but for other devices too!

So instead of mapping the framebuffer somewhere special, we just stretch the physmap until it covers the framebuffer too. With `2MB` pages, the stretch costs next to nothing. The gap between the top of RAM and a framebuffer near `4GB` is a few PDs. And now the console can reach the framebuffer the same way everything else reaches physical memory, through `PHYSMAP_P2V`.

## Reserving the Required Tablespace

To implement the physmap, we first need to reserve a contiguous block of virtual memory to store all the page tables that will map physical RAM, as discussed.

We do this with a function called `reserve_tables`, which takes our multiboot parser as a parameter. This allows us to query the total RAM, determine how many page tables are needed, and reserve the corresponding virtual space by moving `KEND`. Let's break down the implementation piece by piece.

### Aligning Values

```c
uintptr_t align_up(uintptr_t val, uintptr_t align) {
    return (val + align - 1) & ~(align - 1);
}
```

This helper function (a `static inline` in [`layout.h`](/src/arch/x86_64/memory/layout.h), because pretty much every subsystem uses it) rounds a value `val` up to the nearest multiple of `align`. Alignment is critical because x86-64 page tables and pages must always start on properly aligned addresses. Its sibling, `align_down`, rounds down instead.

### How Much to Map

```c
uint64_t total_RAM = align_up(multiboot_total_ram(multiboot, MEASUREMENT_UNIT_BYTES), PAGE_SIZE);
uint64_t map_end = total_RAM;

uint64_t wc_start = 0, wc_end = 0;
multiboot_framebuffer_t* fb = multiboot_framebuffer(multiboot);
if (fb) {
    uint64_t fb_end = fb->addr + (uint64_t)fb->height * fb->pitch;
    wc_start = align_down(fb->addr, PAGE_2MB);
    wc_end = align_up(fb_end, PAGE_2MB);
    if (multiboot_hits_avail(multiboot, wc_start, wc_start + PAGE_2MB)) wc_start += PAGE_2MB;
    if (wc_end > wc_start && multiboot_hits_avail(multiboot, wc_end - PAGE_2MB, wc_end)) wc_end -= PAGE_2MB;
    if (fb_end > map_end) map_end = fb_end;
}
map_end = align_up(map_end, PAGE_2MB);
```

**What's happening here:**

* `multiboot_total_ram` gives us the end of the highest usable RAM region. That's where RAM ends, so that's where the physmap ends too, at least for now.
* If GRUB gave us a framebuffer, we work out where it ends. Its size is `height * pitch` rather than `height * width * (bpp / 8)`, because `pitch` is the actual stride in bytes between rows, which the firmware may pad for alignment reasons.
* `[wc_start, wc_end)` is the range of `2MB` pages that will get special caching for the framebuffer (more on that below). Beware of one catch though - a `2MB` page at either end of the framebuffer might also hold some usable RAM. We must *never* give RAM that special caching, so `multiboot_hits_avail` checks for exactly that, and such a page is left out of the range.
* If the framebuffer ends past RAM, the physmap stretches to cover it.
* Finally, the end gets rounded up to a whole `2MB` page.

### Determining Page Table Counts

```c
uint64_t total_PDs = CEIL_DIV(map_end, PAGE_1GB);
uint64_t total_PDPTs = CEIL_DIV(total_PDs, PAGE_ENTRIES);
PANIC_ASSERT(total_PDPTs < PML4_INDEX(KERNEL_VIRTUAL_BASE) - PML4_INDEX(PHYSMAP_VIRTUAL_BASE));

// Low PT + PDs + PDPTs + PML4 + the kernel's own PDPT and PD
uint64_t table_bytes = (1 + total_PDs + total_PDPTs + 1 + 2) * PAGE_SIZE;
```

Each PD maps `1GB` with `2MB` pages, and each PDPT holds 512 PDs (`512GB`). On top of those, the pool holds:

* the one PT for the first `2MB`,
* one PML4 for the new address space,
* and a PDPT and a PD for the kernel's own mapping.

>[!NOTE]
> We use `CEIL_DIV` to round up, ensuring we allocate enough tables even if the amount of memory isn't an exact multiple of a table's reach. Here, not having `math.h` is costing us the gimmick of defining `CEIL_DIV` as:
>```c
>#define CEIL_DIV(x, y) (((x) + (y) - 1) / (y))
>```

The assertion is there for machines with absurd amounts of memory. Each PDPT takes one PML4 slot, starting at the physmap's slot (`256`), and they must not run into the kernel's slot (`511`). That leaves room for 255 of them, about `130TB` of physmap in the worst case. Short of a machine capable of holding my uncompressed `node_modules` folder in RAM, I think we're fine.

### The Kernel's Own Tables

```c
// The kernel range comes from the same pool (so the boot tables stay scratch), one PT per
// 2MiB. The PTs are part of the range they map, so add one PT of slack for them.
uint64_t kernel_PTs = CEIL_DIV(layout.kend + table_bytes, PAGE_2MB) + 1;
PANIC_ASSERT(kernel_PTs <= PAGE_ENTRIES);
table_bytes += kernel_PTs * PAGE_SIZE;
```

After `build_physmap` runs, we want the kernel range itself to be mapped **from this same pool**, not from the throwaway boot tables. That's what lets us discard the boot map entirely. Unlike the physmap, the kernel gets normal `4KB` pages, one PT per `2MB` of kernel range (`kernel_PTs`).

There's a chicken-and-egg problem here, though. The kernel range has to cover the pool, and the kernel PTs live in the pool. So reserving PTs makes the range bigger, which can need more PTs! The way out is the `+ 1`. Even the most PTs we could ever have (512 of them, one full PD) only add `2MB` to the range, and one extra PT covers exactly `2MB`. So one PT of slack always settles it.

The assertion makes sure all those PTs fit under a single PD (`512` entries), which keeps the kernel tables to exactly one `PDPT` → one `PD` → `kernel_PTs` `PTs`.

### Reserving the Space

```c
layout.ram = total_RAM;
layout.physmap_end = map_end;
physmap.wc_start = wc_start;
physmap.wc_end = wc_end;
physmap.total_PDs = total_PDs;
physmap.total_PDPTs = total_PDPTs;
physmap.total_kernel_PTs = kernel_PTs;
physmap.tables_base = KERNEL_P2V(layout.kend);

kend_grow(multiboot, table_bytes);
PANIC_ASSERT(layout.kend < PAGE_1GB);
```

Everything gets stored, and `tables_base` is set to the current `KEND` in the higher half, marking the start of the reserved space for our new tables.

Then `kend_grow` moves `KEND` forward to cover the tables:

```c
uint64_t kend_grow(multiboot_parser_t* mb, uint64_t bytes) {
    uint64_t start = layout.kend;
    bytes = align_up(bytes, PAGE_SIZE);
    PANIC_ASSERT(multiboot_is_avail(mb, start, start + bytes));
    layout.kend += bytes;
    return start;
}
```

It rounds the size up to whole pages, checks that the memory right after the kernel really is usable RAM (and not something the firmware owns), moves `KEND`, and hands back where the new space starts. We'll use it again later for other memory that has to sit right after the kernel.

The last assertion checks that the grown kernel still fits in the first `1GB`. Remember, the boot tables only map that much, and we're about to write our tables through them.

## Building the Physmap

The final step is constructing the physmap itself. The groundwork is already in place: we have reserved space for all page tables. The main thing to be careful about now is preserving the kernel range while mapping the entirety of physical RAM. Rather than leaning on the boot tables for that, we rebuild the kernel's mappings from the reserved pool as well, so the boot tables can be discarded outright. Everything lives in a function called `build_physmap`.

### Function Setup

```c
if (layout.physmap_end == 0) {
    LOGF("[ERROR] No physmap has been built.\n");
    return;
}

pat_init();
```

We begin by checking that `reserve_tables` has run. If it hasn't, the required tablespace hasn't been reserved yet, and we cannot proceed safely. Ignore `pat_init` for now, it gets its own section below.

### Laying Out the Tables

```c
typedef uint64_t pte_t;
typedef pte_t page_table_t[PAGE_ENTRIES];
const uint64_t RW = PAGE_PRESENT | PAGE_WRITABLE;

page_table_t* LOWPT = (page_table_t*)physmap.tables_base;
page_table_t* PDs   = LOWPT + 1;
page_table_t* PDPTs = PDs + physmap.total_PDs;
page_table_t* PML4  = PDPTs + physmap.total_PDPTs;
page_table_t* KPDPT = PML4 + 1;
page_table_t* KPD   = KPDPT + 1;
page_table_t* KPTs  = KPD + 1;

kmemset(LOWPT, 0, (uintptr_t)(KPTs + physmap.total_kernel_PTs) - (uintptr_t)LOWPT);
```

We define `pte_t` for individual page entries and `page_table_t` for a whole table of 512 of them. The nice part about that type is the pointer arithmetic. `LOWPT + 1` is exactly one table further, `PDs + physmap.total_PDs` is right past the last PD, and so on. So each pointer just picks up where the previous one ended. The `K`-prefixed pointers are the kernel's own tables.

Then we zero the whole pool in one go. This region is accessible because it is within that `1GB` preallocated boundary. Otherwise, we would page fault here.

### Filling the PDs

```c
for (uint64_t pa = 0; pa < layout.physmap_end; pa += PAGE_2MB) {
    uint64_t cache = (pa >= physmap.wc_start && pa < physmap.wc_end) ? fb_cache_flags() : 0;
    PDs[pa / PAGE_1GB][PD_INDEX(pa)] = pa | RW | PAGE_HUGE | cache;
}
```

**What's happening here:**

* We step through physical memory `2MB` at a time. `pa / PAGE_1GB` picks the PD (each one covers `1GB`), and `PD_INDEX(pa)` picks the entry inside it.
* Every entry is a `2MB` page (`PAGE_HUGE`), writable, mapping physical `pa`.
* Pages inside the framebuffer range get `fb_cache_flags()` on top. That's the special caching we've been teasing, and it's coming up next.

### The First 2MB

```c
for (uint64_t i = 0; i < PAGE_ENTRIES; i++)
    LOWPT[0][i] = (i * PAGE_SIZE) | RW;
PDs[0][0] = KERNEL_V2P(LOWPT) | RW;
```

Then we go back and swap the very first `2MB` page for a real PT with 512 `4KB` pages, because of the MTRR business from earlier. Notice the `KERNEL_V2P`. Page table entries always hold *physical* addresses, and our tables live in the kernel range, so that's how we get there.

### Wiring the PDPTs

```c
for (uint64_t i = 0; i < physmap.total_PDs; i++)
    PDPTs[i / PAGE_ENTRIES][i % PAGE_ENTRIES] = KERNEL_V2P(&PDs[i]) | RW;
```

One level up, same idea: each PDPT entry points to a PD.

### Write-Combining the Framebuffer (PAT)

The easy way to keep the framebuffer out of the cache is `PAGE_PWT | PAGE_PCD`, which makes it fully uncacheable. It works fine, but it's slow. Every single pixel write becomes its own trip over the bus, and redrawing a 1080p screen is about two million pixel writes.

What we want instead is **write-combining (WC)**. Writes still skip the cache, but the CPU can collect a bunch of them in a buffer and send them out together in one burst. We almost never *read* from the framebuffer, so this is perfect for it.

Problem is, there's no `PAGE_WRITE_COMBINE` bit. Enter the **Page Attribute Table (PAT)**. Remember `IA32_EFER` from Chapter 3, the model specific register (MSR) we flipped to enable long mode? The PAT is another MSR, and it holds 8 "memory type" entries, each one describing a way memory can be cached.

And remember that lie from earlier? `PWT` and `PCD` (plus a third `PAT` bit we don't use) don't really mean "write-through" and "cache-disable". Together, they form an index into this table:

| PAT index | PTE bits | Default type |
| --- | --- | --- |
| 0 | none | Write-back (normal RAM) |
| 1 | `PWT` | Write-through |
| 2 | `PCD` | Uncacheable, mostly (don't worry about it) |
| 3 | `PWT \| PCD` | Uncacheable |

So we just overwrite an entry we don't need. GatOS never uses write-through, so entry 1 gets turned into write-combining:

```c
static void pat_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 16))) return;   // No PAT support, keep the defaults

    uint64_t pat = read_msr(MSR_PAT);
    pat &= ~(0xFFULL << 8);          // Clear entry 1
    pat |= (0x01ULL << 8);           // 0x01 = write-combining
    write_msr(MSR_PAT, pat);
    patwc = true;
}

static inline uint64_t fb_cache_flags(void) {
    return patwc ? PAGE_PWT : (PAGE_PWT | PAGE_PCD);
}
```

`cpuid`, `read_msr` and `write_msr` are tiny C wrappers around the `cpuid`, `rdmsr` and `wrmsr` instructions we already used in Chapter 3. Nothing new there.

`pat_init` runs right at the start of `build_physmap`, before any entry gets written. If CPUID says the CPU has a PAT (leaf 1, bit 16 of `EDX`), we patch entry 1 and set `patwc`. From then on, anything mapped with *only* `PAGE_PWT` is write-combining. No PAT? `fb_cache_flags` falls back to good old uncacheable. Slower, but it works everywhere.

Normal RAM uses entry 0, so it's untouched. Entry 3 stays uncacheable for actual device registers.

>[!CAUTION]
> Don't go using WC for everything now. Since the CPU is allowed to hold writes back and send them out in batches, they don't necessarily reach the hardware when (or in the order) you wrote them. For pixels, who cares. For a device where you write a command and then immediately read back an answer, that's a recipe for a very bad time.

### Remapping the Kernel from the Pool

With the physmap tables done, we build the kernel's *own* tables out of the pool. This is the piece that lets us throw the boot tables away completely: instead of inheriting whatever `boot32.S` left behind, we remap the entire kernel range `[0, KEND)` from scratch, at `4KB` granularity, into tables we fully control.

```c
for (uint64_t p = 0; p < layout.kend; p += PAGE_SIZE)
    KPTs[p / PAGE_2MB][PT_INDEX(p)] = p | RW;
for (uint64_t i = 0; i < physmap.total_kernel_PTs; i++)
    KPD[0][i] = KERNEL_V2P(&KPTs[i]) | RW;
KPDPT[0][PDPT_INDEX(KERNEL_VIRTUAL_BASE)] = KERNEL_V2P(KPD) | RW;
```

**What's happening here:**

* The first loop walks the kernel's physical range one `4KB` page at a time. Page `p` lands in PT number `p / PAGE_2MB` (each PT covers `2MB`), at slot `PT_INDEX(p)`.
* The second loop wires each `KPT` into the single `KPD`, one entry per `2MB`.
* Finally, we plug that `KPD` into the `KPDPT` at the slot the kernel's virtual base decodes to. `PDPT_INDEX(KERNEL_VIRTUAL_BASE)` is index `510`, exactly the value we saw back in Chapter 3.

By the end of this we have a complete, self-contained kernel mapping sitting in the pool, ready to be hung off the new PML4, with no reference to the boot tables anywhere.

### Integrating Kernel and Physmap into the PML4

This is the critical part we need to handle carefully. Using [`virt_breakdown.py`](tools/virt_breakdown.py) to analyze the virtual addresses of `KERNEL_VIRTUAL_BASE` and `PHYSMAP_VIRTUAL_BASE` gives the following:

```
> Enter address: 0xFFFFFFFF80000000
Virtual Address: 0xFFFFFFFF80000000
  PML4 Index : 0x01FF (511)
  PDPT Index : 0x01FE (510)   <--- Kernel PDPT (our KPDPT)
  PD   Index : 0x0000 (0)
  PT   Index : 0x0000 (0)

> Enter address: 0xFFFF800000000000
Virtual Address: 0xFFFF800000000000
  PML4 Index : 0x0100 (256)
  PDPT Index : 0x0000 (0)     <--- First physmap PDPT
  PD   Index : 0x0000 (0)
  PT   Index : 0x0000 (0)
```

From this breakdown, we can see that:

1. The kernel resides under PML4 index `511`, which will point to the `KPDPT` we just built from the pool.
2. The physmap starts at PML4 index `256`. Each PDPT covers `512GB`, so the physmap takes slot `256`, then `257` if it needs a second PDPT, and so on. On any machine you're likely to run this on, that's just the one.

```c
size_t physmap_index = PML4_INDEX(PHYSMAP_VIRTUAL_BASE);
for (uint64_t i = 0; i < physmap.total_PDPTs; i++)
    PML4[0][physmap_index + i] = KERNEL_V2P(&PDPTs[i]) | RW;
PML4[0][PML4_INDEX(KERNEL_VIRTUAL_BASE)] = KERNEL_V2P(KPDPT) | RW;
```

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

Remember that `PANIC_ASSERT` on `total_PDPTs` back in `reserve_tables`? This loop is why it's there. Too many physmap PDPTs, and the loop would walk straight into slot `511`, right over the kernel.

### Activating the New PML4

```c
PML4_switch(KERNEL_V2P(PML4));
```

Finally, we load the physical address of the new PML4 into `CR3` to switch the CPU to the new page tables. There's also no need to manually flush the TLB. Writing `CR3` throws out all the cached translations by itself, which is the same trick `flush_tlb` uses.

From this instruction on, the boot tables are dead weight. Every address the kernel uses is now routed through tables it built itself.

## Putting It All Together

With all the pieces in place, here is the relevant slice of `kernel_main` as of this chapter:

```c
static uint8_t multiboot_buffer[8 * 1024];

void kernel_main(void* mb_info) {
    serial_init_port(SERIAL_COM1);
    serial_init_port(SERIAL_COM2);
    QEMU_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

    multiboot_parser_t multiboot = {0};
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

    if (!multiboot.initialized) {
        QEMU_LOG("[KERNEL] Failed to initialize multiboot2 parser!", TOTAL_DBG);
        return;
    }

    QEMU_LOG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

    unmap_identity();
    QEMU_LOG("Removed the identity map, only the higher half remains", TOTAL_DBG);

    reserve_tables(&multiboot);
    QEMU_LOG("Reserved the required space for page tables in the kernel region", TOTAL_DBG);

    build_physmap();
    QEMU_LOG("Built physmap at PHYSMAP_VIRTUAL_BASE", TOTAL_DBG);
}
```

This version of `kernel_main` ties together all previous steps: the multiboot structure is parsed and relocated into the higher half before we strip the identity map, `reserve_tables` claims the space for the new page tables right after the kernel, and `build_physmap` constructs the final address space and switches to it.

>[!NOTE]
> In the actual `kernel_bootstrap()`, you'll also spot an `idt_init()` right before the multiboot parsing. That's next chapter's business.

Next chapter? Interrupts, Panics, and Spinlocks. Stay tuned!
