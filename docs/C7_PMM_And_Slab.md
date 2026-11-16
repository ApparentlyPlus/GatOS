# Chapter 7: Physical Memory and Slab Allocators

If you have made it this far into the docs, I'll let you in on a little secret. Kernel development is 10% laying the groundwork (assembly, GDT, Serial, Page Tables, interrupts, etc.) and 90% writing smart subsystems and drivers around it.

We are practically done with the former, and are now entering the latter.

The low level work is mostly behind us now. The CPU is initialized, the physmap works, interrupts are wired up, spinlocks are there to protect our shared data structures, and we even have a crash console that will continue functioning no matter how badly everything else explodes.

But there is still one very obvious thing missing from GatOS: We cannot allocate memory.

I deliberately postponed memory management because it is one of the most complicated, difficult to understand, and error-prone parts of kernel development. If you get memory wrong, the rest of the kernel becomes a nightmare. If you get it right, everything built on top of it suddenly becomes dramatically easier.

And this is really the point where kernel development changes.

Modern kernels do not just "allocate memory" dynamically. They manage physical memory, virtual memory, address spaces, page allocators, heap allocators, fragmentation, synchronization, caching, permissions, and a dozen other interconnected systems simultaneously.

Before we start writing allocators, though, we first need to understand how GatOS actually thinks about memory.

## The Memory Subsystem

Broadly speaking, we can group everything memory related into what we will refer to as the *memory subsystem*. This is not a single component or file somewhere in the kernel, but rather a conceptual grouping of every system whose job is specifically to manage memory in some way.

One thing that always frustrated me when I was first writing GatOS was how abstract and disconnected most explanations of kernel memory management felt. People would throw around terms like physical allocators, virtual memory managers, heaps, slabs, paging, and regions without ever properly explaining what each component was actually responsible for, why it existed, or how all of them eventually come together to implement something as seemingly simple as `malloc()` and `free()`.

It made the whole thing unnecessarily difficult to conceptualize.

So before we start implementing anything, I want to dedicate this section to building a proper mental model for how a modern-ish kernel handles memory requests across its different layers and components. Because once you understand what each piece is responsible for, the entire system suddenly starts fitting together in a much more logical way.

There are essentially four critical components to a robust memory subsystem:

1. **The Physical Memory Manager** (which we will refer to as the PMM)
2. **The Slab Allocator**
3. **The Virtual Memory Manager** (which we will refer to as the VMM)
4. **The Heap Manager** (or commonly known as "the heap")

The order here is intentional. This is the order these systems are initialized in the kernel, and shortly you will see why. For now, though, it is easier to think about them in isolation. The picture will slowly emerge as you read on.

### What does the PMM do?

The PMM's job is exceedingly simple: it keeps track of which physical pages are currently in use, and which ones are free. 

>[!NOTE]
> Keep in mind that there is only **one** PMM in the entire system.

To understand how it works, you first need to understand why the PMM is **the reason** we built the physmap, as well as **the only** subsystem that routinely interacts with it.

To manage physical pages, the PMM must read and write data into them directly. But physical addresses cannot be dereferenced by the CPU after the identity map is gone. 

Since we are now operating exclusively in the higher half, we have built the physmap for this exact reason: since it maps physical (low) address `X` to virtual (high) address `PHYSMAP_VIRTUAL_BASE + X`, the PMM can reach any physical page through the physmap.

This is the key idea: the PMM uses the physmap **internally** to access physical pages, but its external interface deals exclusively in physical addresses. When it allocates a page, it returns the physical address (**not** the physmap virtual address) using `PHYSMAP_V2P`.

> [!IMPORTANT]
> So for example, you will never see the PMM return something like `0xffffffff90000000` as an allocation result, because that is not a physical address. The PMM always returns addresses within the physical RAM range. The physmap is **strictly an internal mechanism**.

At its core, PMM allocation is nothing more than marking a page as "used" and returning its physical address, and deallocation is marking it as "free". For large allocations / deallocations, numerous pages are marked as "used" and "free" respectively.

We will go into the PMM's algorithms in more detail later in this chapter.

### What does the Slab Allocator do?

Picture this: we want to dynamically allocate an integer at runtime, assuming the PMM is online. Given what we know so far, we could do something like:

```c
// Placeholder for the output address (the physical page the PMM will allocate)
uint64_t low_address = 0x0;

// Get the status code to check if the allocation succeeded
pmm_status_t sample_alloc = pmm_alloc(sizeof(int), &low_address);

if(sample_alloc == PMM_OK){
    // The PMM returns a physical address, so we access it through the physmap
    int *val = PHYSMAP_P2V(low_address);

    // Set a sample value
    *val = 1337;

    // Print it
    printf("The allocated value is %d", *val);
}
```

Notice the use of `PHYSMAP_P2V` here. The PMM returns a physical address, which the CPU cannot dereference directly, so we translate it to its physmap virtual address before use. The PMM has already marked the page as "used" internally, so this is perfectly safe.

> [!IMPORTANT]
> The physmap is a kernel construct, so all kernel subsystems have access to it. The distinction is that the PMM is the only subsystem that *manages* it — writing metadata into it and handing out pages from it. Other subsystems may access PMM-returned addresses through the physmap, but they should never reach into the physmap for anything the PMM did not explicitly hand them.

So, what's the problem? If we were to try this, it would work. What gives?

The issue is subtle. PMMs almost universally allocate memory in fixed-size blocks; recall from earlier chapters that we call these physical frames, and they are the same size as virtual pages: 4096 bytes, or 4 KiB. So while the code above works, what happens internally is that the PMM **rounds up** our requested 4 bytes (`sizeof(int)`) to a **full 4 KiB page** and reserves it. 

A full page.

In practice, we are consuming an entire physical frame of RAM just to store a 32-bit integer, utilizing roughly 1% of the memory we actually allocated.

Ouch.

Of course, this doesn't only happen with integers. Any data structure smaller than a full page, primitive or not, suffers from the same waste. This is why the PMM doesn't act as our main allocator. It is very good at exactly one thing: handing out physical pages. Nothing more, nothing less.

As you can imagine, the slab allocator is what we use to fix this problem. It sits on top of the PMM: it requests whole physical pages from it, then subdivides them into small, fixed-size objects. Each page it carves up like that is called a *slab*, hence the name. 

When you allocate an integer, the slab allocator hands you 4 bytes out of an already-allocated page, **not a fresh page of its own**. When that page fills up, it requests another from the PMM. This way, physical pages are packed tightly with real data, and the per-allocation waste is negligible.

>[!NOTE]
> Keep in mind that there is only **one** Slab Allocator in the entire system.

### What does the Virtual Memory Manager do?

The short answer is: a shit load of things. It's the most complex piece of the memory subsystem, and it gets a whole chapter of its own, so here's just the gist.

Recall from earlier chapters that everything the kernel touches (its code, its data sections, the physmap, everything) exists only as virtual memory. The CPU never operates on physical addresses directly; it always goes through the page tables. The subsystem that owns and manages those page tables is the VMM.

Ask it for memory, and it picks a free range of virtual addresses, gets physical frames from the PMM to back them, and writes the page table entries that tie the two together. Free it, and it does the reverse.

Here's where the slab comes in. The VMM also has to *remember* everything it mapped: which ranges, with which permissions. That bookkeeping is a pile of small structs, a few dozen bytes each, and small structs are the slab's whole thing.

And since every VMM keeps its own page tables, we can have several of them and swap between them. That becomes extremely useful once we introduce processes, where each process needs its own private view of memory.

>[!NOTE]
>Keep in mind that there can be **many** VMMs in the system.

### What does the Heap Manager do?

The heap manager is the final layer of the memory stack. The VMM thinks in pages, and the heap thinks in arbitrary sizes. It carves the VMM's pages into allocations of whatever size you ask for, and remembers how big each one was. That's `malloc`, `calloc`, `realloc`, `free`, and friends. And it's also how a 4 byte allocation finally stops eating a whole page.

> [!NOTE]
> The heap is the **only** component of the memory subsystem that gets implemented twice: once for the kernel (`kheap`, with `kmalloc` and `kfree`) and once for userland (in `ulibc`, with `malloc` and `free`), which asks the kernel for memory through syscalls.

Hopefully by now you have a general grasp of why we need each component in the memory subsystem. Now that we have motivated the architecture, let's get specific with the PMM and the slab.

## Why We Need a Physical Memory Manager

Right now, we have the physmap. We built it in Chapter 5. It maps every physical address into virtual space at `PHYSMAP_VIRTUAL_BASE`, meaning we can read and write any physical memory from C using `PHYSMAP_P2V(phys_addr)`. From a hardware perspective, we have total access to all of RAM.

The problem is that *access* and *ownership* are different things. Say the VMM needs a new page table. It needs a physical page to put it in. What physical page should it use? It has no idea. If it just picks an arbitrary address, it might clobber the kernel's `.text` section. Or a page the PMM already handed to someone else. Or the multiboot structure we are still reading from. Without the PMM, allocating a physical page is basically just gambling with your kernel's stability.

The PMM fixes this by maintaining a precise record of which physical pages are free and which are in use. It is the gatekeeper. You want a physical page? You ask the PMM. Nobody else gets to hand out physical memory. Nobody else gets to decide what is free.

All of this lives in [`kernel/memory/pmm.h`](/src/kernel/memory/pmm.h) and [`kernel/memory/pmm.c`](/src/kernel/memory/pmm.c).

## The Buddy Allocator

The simplest physical memory manager you could build is a bitmap: one bit per page, `1` means allocated, `0` means free. Allocating a page is `O(n)` scan of the bitmap to find a free bit. Freeing is `O(1)`. It works. It is also bad in a way that is easy to overlook until you hit it.

Suppose the VMM asks for a 16 KB block. That is four consecutive pages. With a bitmap, you need to scan for four *consecutive* free bits. Worse, as allocation and deallocation happen over time, your RAM starts looking like Swiss cheese: plenty of free pages in aggregate, but none of them contiguous. This is called **external fragmentation**, and it can cause large allocation requests to fail even when there is technically enough free memory.

The buddy allocator solves this by organizing free memory into power-of-two-sized blocks. Every free block lives in a free list indexed by its *order*, where order `N` represents a block of size `4KB * 2^N`. The smallest block is one page, so:

```
Order 0  →  4 KB
Order 1  →  8 KB
Order 2  →  16 KB
Order 3  →  32 KB
...
Order 10 →  4 MB
```

When you free a block, you check whether its *buddy* — the adjacent block that, combined with yours, forms the next-larger block — is also free. If it is, you merge them into a single block at the next order up and try again. This coalescing ensures that freed memory consolidates back into large contiguous blocks over time, preventing long-term fragmentation. The name "buddy" comes from that pairing relationship: every block has exactly one buddy, and only that buddy can be merged with it.

The beauty of the buddy system is how it computes the buddy address. Given a block at physical address `addr` and order `N`, the buddy is:

```c
uint64_t buddy = phys ^ order_to_size(order);
```

XOR with the block size. That is it. 

Think about why this works: at order `N`, the block size is `2^N * 4KB`. In binary, that is a single bit at position `N + 12` (since `4KB = 2^12`). XOR-ing the address with that value flips exactly that bit, which toggles between the lower and upper half of the aligned pair. If you are at the start of a 16 KB block, XOR with 16 KB gives you the address 16 KB ahead. If you are 16 KB ahead, XOR brings you back. No division, no subtraction, no branching. A single instruction.

>[!NOTE]
>This is pure algorithmic design. It is not the point of the documents to explain exactly how this works intuitively. If stuff feels a little too unintuitive, feel free to google for further reading. I bet there are some neat YouTube videos that will visualize this beautifully.

### Orders, Sizes, and the Max Order

Converting between sizes and orders is straightforward:

```c
static inline uint64_t order_to_size(uint32_t order) {
    return (uint64_t)PAGE_SIZE << order;
}

static uint32_t size_to_order(uint64_t size) {
    uint32_t order = 0;
    while (order_to_size(order) < size) order++;
    return order;
}
```

`size_to_order` keeps going up an order until the block is at least as large as the request. Whatever order we land on is the smallest block that can satisfy the request. If you ask for 5 KB, you get order 1 (8 KB). If you ask for exactly 4 KB, you get order 0. There is no such thing as a "5 KB block" in a buddy allocator. The granularity is always a power of two.

The downside of this is **internal fragmentation**: asking for 5 KB wastes 3 KB. For large kernel allocations — page tables, DMA buffers, large kernel structures — this overhead is acceptable. For small allocations like linked list nodes or process descriptors that are a few dozen bytes, it is catastrophic. That is the entire reason the Slab allocator exists, and we will get to it shortly.

`max_order` is computed in `pmm_init`, as the largest order whose block still fits in the managed range:

```c
max_order = 0;
while (max_order + 1 < PMM_MAX_ORDERS && order_to_size(max_order + 1) <= end)
    max_order++;
```

This determines the largest single block we can ever hand out. For a 4 GB machine, `max_order` ends up at 20, meaning the largest block is 4 GB. You probably do not need a single 4 GB contiguous allocation at kernel level, but the allocator can handle it. 

Psychos exist, you know.

## The Free Block Header

Here is a question worth sitting with: when a block is free, what lives at its physical address?

Nothing important. It is free.

This is the key insight the PMM exploits. Instead of maintaining a separate structure for the free list links of each block (which would require its own allocation — circular and gross), we store them *directly inside the free block itself*. The block's first few bytes are the header. When the block is allocated, the caller overwrites it anyway. When it is free, nobody else is using those bytes.

```c
typedef struct {
    uint32_t magic;
    uint32_t order;
    uint64_t next_phys;
    uint64_t prev_phys;
} pmm_free_header_t;
```

`magic` is `0xFEEDBEEF`. Any time the PMM pulls a block off a free list, it checks this field first. If it sees anything other than the magic value, it knows the block has been corrupted — either a use-after-free, a buffer overflow from an adjacent allocation, or something more exotic. Without this check, corruption in the free lists manifests as bizarre allocation behavior much later, in completely unrelated code. The magic gives us an early warning.

`order` tells us which free list this block belongs to. This sounds redundant, but it is a cross-check. If we think we are looking at an order-3 block and the header says order-7, something has gone wrong. Again, early warning.

`next_phys` and `prev_phys` thread the block into a **doubly-linked list**. Both are physical addresses. At first glance, you might wonder why we need a doubly-linked list here. A singly-linked list would be simpler for push and pop.

The reason is coalescing: when we merge a freed block with its buddy, we need to remove the buddy from wherever it currently sits in the free list. With only a `next` pointer, that requires scanning the entire list to find the predecessor. With `prev`, we splice it out in O(1). When you are managing the system's physical memory, O(n) on every free operation is a non-starter.

All accesses to these headers go through the physmap, via a tiny helper:

```c
static inline pmm_free_header_t* hdr_of(uint64_t phys) {
    return (pmm_free_header_t*)PHYSMAP_P2V(phys);
}
```

This is the payoff for Chapter 5. The physmap was not just a convenience feature — it is a prerequisite for the PMM's ability to read and write free block metadata without any additional setup.

## The Page Map

There's a catch with keeping everything inside the blocks, though. Say we're freeing a block and want to coalesce it. We compute its buddy's address, and now we need to know: is that buddy free, and if so, at which order?

The obvious answer is "read its header and check the magic". But think about what's at that address if the buddy is *allocated*. Whatever its owner put there. Could be a page table, could be a string, could be anything. And if those first bytes happen to look like `0xFEEDBEEF` followed by the right order, we'd "unlink" a block that was never on any list, and now someone else's memory is in our free lists. Unlikely? Sure. But memory allocators run billions of times, and unlikely things love to happen at 3 AM.

>[!WARNING]
>This doesn't necessarily need to happen at random. Someone could deliberately (and maliciously) induce such behavior to exploit a potential bug in our coalescing logic.

So the PMM keeps one small piece of bookkeeping *outside* the blocks, the **page map**, one byte per physical page.

```c
// Page map entry, 0 means "not the start of a free block", otherwise PG_FREE | order
#define PG_FREE 0x80

static uint8_t* pmap = NULL;
```

Only the first page of each free block is marked, with `PG_FREE` plus its order. Every other byte is `0`. That's enough to answer our question without ever touching the buddy's memory: `pmap[buddy / PAGE_SIZE] == (PG_FREE | order)` means "a free block of exactly this order starts here". Anything else means the buddy is not ours to take.

One byte per page sounds like a lot, but it really isn't. On QEMU's default 128 MiB machine that's `32 KiB`, and even `64 GB` of RAM only needs `16 MiB` of page map. How big it has to be is `pmm_map_size`, which is just the number of pages:

```c
uint64_t pmm_map_size(uint64_t end) {
    return end / PAGE_SIZE;
}
```

Where does it live, though? We have no allocator yet, that's the whole point of this chapter! Remember `kend_grow` from Chapter 5? It already solved this exact problem for the page tables once before. We can grab some RAM right after the kernel and move `KEND` past it. The page map does the same thing, as you'll see in the init code.

### Validation

The header is still checked whenever the PMM relies on it, as a second opinion next to the page map:

```c
static bool valid_hdr(uint64_t phys, uint32_t order) {
    pmm_free_header_t* h = hdr_of(phys);

    if (h->magic != PMM_FREE_BLOCK_MAGIC || h->order != order) {
        LOGF("[PMM ERROR] Bad free header at 0x%lx (magic 0x%x, order %u, expected %u)\n", phys, h->magic, h->order, order);
        stats.corruption_detected++;
        return false;
    }

    if ((h->next_phys != EMPTY_SENTINEL && h->next_phys >= range_end) ||
        (h->prev_phys != EMPTY_SENTINEL && h->prev_phys >= range_end)) {
        LOGF("[PMM ERROR] Bad free list links at 0x%lx\n", phys);
        stats.corruption_detected++;
        return false;
    }

    return true;
}
```

If the page map says a block is free but its header disagrees, someone scribbled over free memory. The PMM logs a detailed error to `COM2` (which ends up in `debug.log`), increments a corruption counter in the stats, and returns false. The caller then handles the failure safely rather than proceeding with garbage data. A memory allocator that silently continues after detecting corruption is worse than one that panics — it just moves the crash somewhere completely unrelated and makes it ten times harder to diagnose. The PMM is paranoid by design.

## Pushing and Popping

Free list management is handled by `push_head`, `unlink` and `pop_head`. These are the lowest-level operations the PMM performs, and everything else is built on top of them.

`push_head` inserts a block at the front of its order's free list:

```c
static void push_head(uint32_t order, uint64_t phys) {
    uint64_t old = free_heads[order];
    pmm_free_header_t* h = hdr_of(phys);

    h->magic = PMM_FREE_BLOCK_MAGIC;
    h->order = order;
    h->next_phys = old;
    h->prev_phys = EMPTY_SENTINEL;

    if (old != EMPTY_SENTINEL) hdr_of(old)->prev_phys = phys;

    free_heads[order] = phys;
    pmap[phys / PAGE_SIZE] = PG_FREE | order;
    stats.free_blocks[order]++;
}
```

We write the header into the block at `phys`, wire up its `next_phys` to the current head, and update the old head's `prev_phys` to point back. Then `free_heads[order]` becomes the new block. All of this is standard doubly-linked list insertion at the head. Then the page map gets its mark, and `stats.free_blocks[order]++` keeps our debug counters accurate.

`unlink` takes a block out of its list, wherever it is, using its own links:

```c
static void unlink(uint32_t order, uint64_t phys) {
    pmm_free_header_t* h = hdr_of(phys);

    if (h->prev_phys == EMPTY_SENTINEL) free_heads[order] = h->next_phys;
    else hdr_of(h->prev_phys)->next_phys = h->next_phys;

    if (h->next_phys != EMPTY_SENTINEL) hdr_of(h->next_phys)->prev_phys = h->prev_phys;

    h->magic = 0;
    pmap[phys / PAGE_SIZE] = 0;
    stats.free_blocks[order]--;
}
```

No previous block means we're the head, so the head moves to our next. Otherwise our previous block skips over us. Same on the other side. Then we wipe the magic and the page map mark, so nothing mistakes this block for a free one anymore.

`pop_head` is just `unlink` on the head, with a validation check first:

```c
static uint64_t pop_head(uint32_t order) {
    uint64_t head = free_heads[order];
    if (head == EMPTY_SENTINEL) return EMPTY_SENTINEL;

    // A broken head would hand out garbage, so we cut the whole list loose instead
    if (!valid_hdr(head, order)) {
        free_heads[order] = EMPTY_SENTINEL;
        return EMPTY_SENTINEL;
    }

    unlink(order, head);
    return head;
}
```

If the head is corrupt, we can't trust its links either, so the rest of that list is unreachable anyway. We drop it and return `EMPTY_SENTINEL`, which the caller treats as "no block available". We lose that list's memory, which is not great from an allocation perspective, but infinitely better than returning garbage.

For the coalescing path, there is `take_buddy`, which is where the page map earns its keep:

```c
static bool take_buddy(uint32_t order, uint64_t buddy) {
    if (pmap[buddy / PAGE_SIZE] != (PG_FREE | order)) return false;
    if (!valid_hdr(buddy, order)) return false;
    unlink(order, buddy);
    return true;
}
```

The page map check comes first, and it is not a corruption check. A buddy that's allocated, or free but split into smaller blocks, simply isn't marked with this order. Returning `false` tells the caller "the buddy is not available at this order", which is a perfectly normal situation during coalescing. Only once the page map says yes do we look at the header at all.

## Allocating: Block Splitting

`pmm_alloc` is the public entry point. It validates arguments, turns the requested size into an order, and delegates to the internal helper:

```c
pmm_status_t pmm_alloc(size_t size_bytes, uint64_t* out_phys) {
    if (!out_phys || size_bytes == 0) return PMM_ERR_INVALID;
    if (!inited) return PMM_ERR_NOT_INIT;

    uint32_t order = size_to_order(size_bytes);
    if (order > max_order) return PMM_ERR_OOM;

    bool flags = spinlock_acquire(&pmm_lock);
    stats.alloc_calls++;
    pmm_status_t status = alloc_order(order, out_phys);
    spinlock_release(&pmm_lock, flags);
    return status;
}
```

Notice the spinlock acquire and release wrapping the actual work. This is exactly what Chapter 6 was about. The PMM's free lists are shared state that could be torn apart by an interrupt handler calling `pmm_alloc` at the wrong moment. The spinlock, combined with interrupt disabling inside `spinlock_acquire`, makes the entire operation atomic from the system's perspective.

The actual allocation logic lives in `alloc_order`:

```c
static pmm_status_t alloc_order(uint32_t order, uint64_t* out_phys) {
    for (uint32_t o = order; o <= max_order; o++) {
        uint64_t block = pop_head(o);
        if (block == EMPTY_SENTINEL) continue;

        // Hand the upper halves back until the block is the size we want
        while (o > order) {
            o--;
            push_head(o, block + order_to_size(o));
        }

        *out_phys = block;
        return PMM_OK;
    }
    return PMM_ERR_OOM;
}
```

Start at the requested order. If the free list there is empty, move up to the next order. Keep going until we find a non-empty list. Pop a block from it. Then, if we took a larger block than needed, split it down: cut it in half, push the upper half to the free list at one order lower, and keep halving until we are at the order we wanted.

Let's trace through a concrete example. Say we want a 4 KB block (order 0) and the free lists look like:

```
Order 0: empty
Order 1: empty
Order 2: empty
Order 3: [0x100000 → 0xA00000 → sentinel]
```

1. `o = 0`: empty, skip.
2. `o = 1`: empty, skip.
3. `o = 2`: empty, skip.
4. `o = 3`: non-empty! Pop `0x100000`. Now the inner `while (o > order)` loop runs:
   - `o--` → `o = 2`, push `0x100000 + 16KB = 0x104000` to order 2.
   - `o--` → `o = 1`, push `0x100000 + 8KB = 0x102000` to order 1.
   - `o--` → `o = 0`, push `0x100000 + 4KB = 0x101000` to order 0.
   - Now `o == order`, exit the inner loop.
5. Return `0x100000`.

After this, the free lists look like:

```
Order 0: [0x101000 → sentinel]
Order 1: [0x102000 → sentinel]
Order 2: [0x104000 → sentinel]
Order 3: [0xA00000 → sentinel]
```

The 32 KB block was split into one 4 KB block (returned), plus three leftover "buddy" blocks at different orders, ready for future allocations. Nobody wasted anything. The next time you ask for an 8 KB block, it comes from `0x102000` with no splitting needed.

## Freeing: Coalescing

Freeing is where the buddy system earns its keep. `pmm_free` checks the arguments (in range, aligned to the block size), takes the lock, and hands the block to `free_order`:

```c
static pmm_status_t free_order(uint64_t phys, uint32_t order) {
    if (free_overlaps(phys, order)) {
        LOGF("[PMM ERROR] Double free at 0x%lx\n", phys);
        return PMM_ERR_DOUBLE_FREE;
    }

    stats.free_calls++;

    // Coalesce like a pro heheh
    while (order < max_order) {
        uint64_t buddy = phys ^ order_to_size(order);
        if (buddy + order_to_size(order) > range_end) break;
        if (!take_buddy(order, buddy)) break;

        stats.coalesce_success++;
        if (buddy < phys) phys = buddy;
        order++;
    }

    push_head(order, phys);
    return PMM_OK;
}
```

Ignore `free_overlaps` for a moment, we'll get to it right after. Let's trace this. Say we free `0x101000` (a 4 KB block, order 0). The free lists currently look like what we left them from the allocation trace above.

1. `order = 0`. `buddy = 0x101000 ^ 4KB = 0x100000`. Is `0x100000` a free order-0 block? No (it was allocated), so the page map has no mark there and `take_buddy` returns `false`. We push `0x101000` to order 0 and return.

Now suppose `0x100000` gets freed too. Free `0x100000` at order 0:

1. `order = 0`. `buddy = 0x100000 ^ 4KB = 0x101000`. Is `0x101000` a free order-0 block? Yes! `take_buddy` unlinks it. `stats.coalesce_success++`. `phys = min(0x100000, 0x101000) = 0x100000`. `order = 1`.
2. `order = 1`. `buddy = 0x100000 ^ 8KB = 0x102000`. Free at order 1? Yes! Unlink it. Coalesce again. `phys = 0x100000`. `order = 2`.
3. `order = 2`. `buddy = 0x100000 ^ 16KB = 0x104000`. Free at order 2? Yes! Unlink it. `phys = 0x100000`. `order = 3`.
4. `order = 3`. `max_order` check. Suppose `max_order = 3` for this example. Exit loop. Push `0x100000` to order 3.

The original 32 KB block is completely reassembled. Two separate frees, the second of which triggered a whole chain of coalescing, rebuilt the large block. This is exactly the kind of defragmentation that makes the buddy system worth the complexity.

The line `if (buddy < phys) phys = buddy;` is critical. When two buddies merge, the combined block starts at the *lower* address. If the freed block is the upper half (`phys > buddy`), we have to adopt the buddy's address as the new block start before bumping the order.

### Catching Double Frees

Now, `free_overlaps`. What happens if someone frees the same block twice?

Without a check, the block goes onto a free list a second time. Two allocations later, two different owners get the same physical memory, and they slowly overwrite each other's data. That's one of the nastiest bugs you can have, because it breaks something completely unrelated to the actual mistake. So `free_order` refuses to free anything that's already (even partly) free:

```c
// True if phys lies inside any free block
static bool free_covering(uint64_t phys) {
    for (uint32_t o = 0; o <= max_order; o++) {
        uint64_t head = align_down(phys, order_to_size(o));
        uint8_t m = pmap[head / PAGE_SIZE];
        if ((m & PG_FREE) && (uint32_t)(m & ~PG_FREE) >= o) return true;
    }
    return false;
}

// True if any page of [phys, phys + size of order) is already free
static bool free_overlaps(uint64_t phys, uint32_t order) {
    if (free_covering(phys)) return true;
    for (uint64_t p = phys + PAGE_SIZE; p < phys + order_to_size(order); p += PAGE_SIZE)
        if (pmap[p / PAGE_SIZE] & PG_FREE) return true;
    return false;
}
```

**What's happening here:**

* A free block that contains `phys` has to start at `phys` rounded down to its own size, because buddy blocks are always aligned to their size. So `free_covering` tries every order: round `phys` down, and check whether a free block of at least that order starts there. That's at most one check per order, around 20 of them.
* That catches a block that already sits inside a free block. But the block we're freeing could also *contain* free blocks. Say someone frees 8 KB, but its second page was already freed on its own. The loop in `free_overlaps` catches that by checking the page map for every page after the first.

Both are pure page map lookups, so the cost is a handful of byte reads per free. Cheap insurance.

### Exact Sizes

There are two more public functions, `pmm_alloc_exact` and `pmm_free_range`. Remember the internal fragmentation from earlier? Ask `pmm_alloc` for 12 KB and you get a 16 KB block, 4 KB of it wasted. That's fine for most callers, but some (the VMM, for one) want *exactly* the number of pages they asked for. `pmm_alloc_exact` does the same allocation, then gives the unused tail straight back with `split_range` (more on that below). `pmm_free_range` is its counterpart: it frees any page aligned range, by cutting it into the largest aligned blocks that fit and freeing each one through `free_order`.

## Initialization: Two Phases

The PMM's `pmm_init` function sets up data structures and records the managed range, but it does **not** mark any memory free:

```c
pmm_status_t pmm_init(uint64_t end, uint64_t map_phys) {
    if (inited) return PMM_ERR_ALREADY_INIT;
    spinlock_init(&pmm_lock, "pmm_global");

    end = align_down(end, PAGE_SIZE);
    if (end == 0) return PMM_ERR_INVALID;

    range_end = end;
    pmap = (uint8_t*)PHYSMAP_P2V(map_phys);
    kmemset(pmap, 0, pmm_map_size(end));

    // ... max_order, from earlier ...

    for (uint32_t i = 0; i < PMM_MAX_ORDERS; ++i)
        free_heads[i] = EMPTY_SENTINEL;

    kmemset(&stats, 0, sizeof(pmm_stats_t));
    exclusion_count = 0;
    inited = true;
    // ...
}
```

The PMM manages `[0, end)` of physical memory, and `map_phys` is where its page map lives. All free lists start empty and the whole page map is zeroed. The PMM is initialized but has nothing to give out. This is intentional. Before we can populate the free lists, we need to register exclusion zones — regions of physical memory that must never be allocated, regardless of what the firmware's memory map says.

### Exclusion Zones

The most important exclusion zone is everything up to `KEND`:

```c
pmm_exclude_range(0, kmem->kend);
```

That one range covers a lot. The kernel image itself, of course. If we skipped it and then populated the free lists with all available RAM, the PMM might hand out physical pages that the kernel's `.text`, `.data`, or `.bss` sections live on. The first allocation that touched such a page would silently corrupt the kernel. The failure would show up much later, somewhere completely unrelated, and you would be staring at a page fault in a function that looks completely correct wondering what on earth happened.

But since `KEND` has grown twice by now, it also covers the physmap's page tables and the PMM's own page map. And starting at `0` throws in the first MiB for free: page `0`, which would look exactly like a NULL pointer if we ever handed it out, and the firmware leftovers from Chapter 5 (old video memory, the BIOS ROM and friends).

Exclusion zones are stored in a simple static table:

```c
static pmm_exclusion_t exclusions[PMM_MAX_EXCLUSIONS];
static uint32_t exclusion_count = 0;
```

`PMM_MAX_EXCLUSIONS` is eight. This is intentional — we do not expect more than a handful of excluded regions. Right now there's just the one. If we ever needed to exclude additional ranges (ACPI tables, firmware reserved regions), we add entries here before calling `pmm_populate`.

### Populating the Free Lists

After registering exclusions, we walk the multiboot memory map and mark each available region free:

```c
for (size_t i = 0; i < mb->memory_map_length; i++) {
    uintptr_t region_start, region_end;
    uint32_t region_type;
    if (multiboot_mem_region(mb, i, &region_start, &region_end, &region_type) != 0)
        continue;
    if (region_type != MULTIBOOT_MEMORY_AVAILABLE){
        // Handle non RAM...
        continue;
    }
    pmm_populate((uint64_t)region_start, (uint64_t)region_end);
}
```

`pmm_populate` acquires the PMM lock and calls down to the internal `mark_free`. This is where the exclusion logic actually runs:

```c
static void mark_free(uint64_t start, uint64_t end) {
    if (end > range_end) end = range_end;
    start = align_up(start, PAGE_SIZE);
    end = align_down(end, PAGE_SIZE);
    if (start >= end) return;

    for (uint32_t i = 0; i < exclusion_count; i++) {
        uint64_t ex_start = exclusions[i].start;
        uint64_t ex_end = exclusions[i].end;
        if (start < ex_end && ex_start < end) {
            if (start < ex_start) mark_free(start, ex_start);
            if (ex_end < end) mark_free(ex_end, end);
            return;
        }
    }

    split_range(start, end);
}
```

First, the range is clipped to whole pages inside what we manage. Then, before marking any range free, we check whether it overlaps any exclusion zone. If it does, we split the range at the exclusion boundary and recursively mark the non-excluded pieces free. If the range fully contains an exclusion zone, the two recursive calls together cover everything except the excluded portion. If the range is fully contained within an exclusion zone, neither recursive call fires, and we return having marked nothing free.

The final call to `split_range` handles a range that has passed all exclusion checks:

```c
static void split_range(uint64_t start, uint64_t end) {
    while (start < end) {
        uint32_t o = max_order;
        while (order_to_size(o) > end - start || (start & (order_to_size(o) - 1)))
            o--;
        push_head(o, start);
        start += order_to_size(o);
    }
}
```

This is a greedy partition: starting at `start`, pick the largest order block that both fits in the remaining range *and* is naturally aligned to that block size. Push it. Advance by the block size. Repeat until the range is exhausted. The alignment check `(start & (order_to_size(o) - 1))` is mandatory — the buddy system requires all blocks to be naturally aligned to their size. If a 32 KB block does not start on a 32 KB boundary, its buddy computation breaks. And the inner loop always stops by order 0 at the latest, since `start` and `end` are page aligned.

The result is that any arbitrary aligned range gets partitioned into the fewest possible blocks of the largest possible sizes. Maximum efficiency, no waste.

## The Full Picture

Stepping back, here is the relevant slice of `kernel_bootstrap()` that initializes the PMM:

```c
// The page map goes right after the kernel, like the physmap tables did
pmm_status_t pmm_status = pmm_init(kmem->ram, kend_grow(mb, pmm_map_size(kmem->ram)));
if(pmm_status != PMM_OK) {
    QEMU_LOG("[PMM] Failed to initialize physical memory manager", TOTAL_DBG);
    return false;
}

// Nothing below KEND is ever handed out
pmm_exclude_range(0, kmem->kend);

// Populate freelists from firmware reported available regions
for (size_t i = 0; i < mb->memory_map_length; i++) {
    uintptr_t region_start, region_end;
    uint32_t region_type;
    if (multiboot_mem_region(mb, i, &region_start, &region_end, &region_type) != 0)
        continue;
    if (region_type != MULTIBOOT_MEMORY_AVAILABLE){
        vmm_add_mmio(region_end - region_start);
        continue;
    }
    pmm_populate((uint64_t)region_start, (uint64_t)region_end);
}
QEMU_LOG("Initialized physical memory manager", TOTAL_DBG);
```

The PMM manages `[0, kmem->ram)`, all of RAM. Note that it's `ram` and not `physmap_end`. The physmap may stretch further to cover the framebuffer, but that's not memory anyone should ever allocate. Everything in that range is potentially allocatable, subject to exclusions. The page map is carved out with `kend_grow` *before* the exclusion is registered, so the exclusion covers it too, and there is no window in which the kernel's (or the page map's) physical pages could be handed out accidentally.

After this block runs, every usable physical page the firmware knows about, minus everything below `KEND`, is represented in the PMM's free lists and ready to allocate.

>[!NOTE]
>You might notice that non-available memory regions are passed to `vmm_add_mmio` rather than silently skipped. This tallies up the total MMIO reservation the VMM needs to account for in its virtual address space layout. We will cover what that actually does in the next chapter.

## The Slab Allocator

The PMM gives us physical pages. The minimum allocation is `4KB`. For a 32-byte object, that is 99.2% waste. For a 64-byte object, 98.4%. For any small, frequently-allocated kernel data structure, allocating whole pages from the PMM is completely impractical.

Let's put some numbers on it. A system with 1000 active processes, each with a process descriptor of, say, 256 bytes: if we backed each one with a PMM page, we would consume 4 MB just for process descriptors. With the Slab allocator, we consume about 256 KB, `1000 * 256 bytes`, packed efficiently. A 16x improvement just from packing objects together.

This is not a new problem. Jeff Bonwick introduced the Slab allocator in 1994 in a paper literally titled "The Slab Allocator: An Object-Caching Kernel Memory Allocator." 

The core idea: for each type of object you allocate frequently, maintain a dedicated pool that divides PMM pages into slots of that object's size. Hand out slots, take them back, return whole pages to the PMM only when they are completely empty. No per-object PMM calls. No power-of-two rounding waste.

GatOS's implementation lives in [`kernel/memory/slab.h`](/src/kernel/memory/slab.h) and [`kernel/memory/slab.c`](/src/kernel/memory/slab.c).

>[!NOTE]
> Fun fact (again)! The Linux kernel's slab allocator has gone through several generations. The original "SLAB" allocator was eventually supplemented by "SLUB" (a simplified, more performant version) and "SLOB" (a simple, compact version for memory-constrained systems). All three exist in the Linux kernel simultaneously, selectable at compile time. We are implementing something much closer to the original SLAB concept, which is perfectly appropriate for a single-core kernel.

### Two Core Structures

Everything in the Slab allocator flows through two structures: the **cache** and the **slab**.

A **cache** (`slab_cache_t`) represents a pool of identically-sized objects. You create one cache per type of thing you want to allocate efficiently. All VMM region tracking structures go in one cache. All process descriptors go in another. All scheduler queue nodes in a third. The cache is the logical container — it holds all the slabs backing that object type and manages allocation from them:

```c
struct slab_cache {
    uint32_t magic;
    char name[SLAB_CACHE_NAME_LEN];
    size_t obj_size;
    size_t user_size;
    size_t align;

    uint32_t cache_id;

    slab_t* slabs_empty;
    slab_t* slabs_partial;
    slab_t* slabs_full;

    spinlock_t lock;

    cache_stats_t stats;
    slab_cache_t* next;
};
```

`user_size` is what the caller asked for. `obj_size` is the actual slot size after accounting for the allocation header and alignment padding. The distinction matters: if you create a cache for 32-byte objects but internal overhead bumps the slot size to 48 bytes, allocating an object wastes 16 bytes per slot. These two fields track both numbers so callers can inspect them and the allocator can make accurate memory accounting decisions.

`slabs_empty`, `slabs_partial`, and `slabs_full` are the three doubly-linked lists of slabs. We will talk about these states shortly — they are the heart of how the allocator decides where to allocate from.

`spinlock_t lock` protects the per-cache state. This is a finer-grained lock than the PMM's single global lock. Multiple caches can be accessed concurrently by different parts of the kernel without contending on a single lock. There is also a global `slab_list_lock` that protects the global list of all caches, separate from any individual cache lock.

A **slab** (`slab_t`) represents one PMM page that has been carved into fixed-size object slots:

```c
typedef struct slab {
    uint32_t magic;
    uint32_t in_use;
    uint32_t capacity;
    uint32_t obj_size;
    void* freelist;
    struct slab* next;
    struct slab* prev;
    slab_cache_t* cache;
    uint64_t slab_phys;
    uint8_t  list_id;
} slab_t;
```

`capacity` is the total number of object slots that fit in this slab. `in_use` is how many are currently allocated. `freelist` points to the first free slot inside the slab. `slab_phys` is the physical address of the underlying PMM page, needed when returning the page on slab destruction. `list_id` records which of the three lists this slab currently lives on — `SLAB_LIST_EMPTY`, `SLAB_LIST_PARTIAL`, or `SLAB_LIST_FULL`.

The `next` and `prev` pointers make this another doubly-linked list for O(1) removal during list transitions. You are seeing a theme here.

### Slab Layout: Header at the Front

This is one of the more elegant parts of the Slab allocator. The `slab_t` header is not stored in a separate allocation. It lives at the very beginning of the PMM page it describes:

```c
static slab_t* slab_allocate_page(slab_cache_t* cache) {
    uint64_t phys = 0;
    pmm_status_t pmm_status = pmm_alloc(PAGE_SIZE, &phys);
    // ...
    slab_t* slab = (slab_t*)PHYSMAP_P2V(phys);
    kmemset(slab, 0, PAGE_SIZE);
    slab->magic = SLAB_MAGIC;
    slab->obj_size = cache->obj_size;
    slab->cache = cache;
    slab->slab_phys = phys;
    // ...
}
```

`PHYSMAP_P2V(phys)` gives us the virtual address of the newly allocated PMM page, and we cast it straight to `slab_t*`. The slab header occupies the first `sizeof(slab_t)` bytes. Object slots begin immediately after, starting from `(uint8_t*)slab + metadata_size`. No separate allocation. No extra PMM call. One PMM page, one slab header, the rest is slots.

The number of slots is computed from how much space remains after the header:

```c
size_t metadata_size = sizeof(slab_t);
uintptr_t base = (uintptr_t)slab;
uintptr_t first_obj_start = base + metadata_size;
uintptr_t first_user_ptr = first_obj_start + sizeof(slab_alloc_header_t);

uintptr_t aligned_user_ptr = align_up(first_user_ptr, cache->align);
metadata_size = (size_t)(aligned_user_ptr - base - sizeof(slab_alloc_header_t));

size_t available = PAGE_SIZE - metadata_size;
slab->capacity = (uint32_t)(available / cache->obj_size);
```

This is a bit subtle. The pointer returned to the user must be aligned to `cache->align`. But between the slab header and the first user pointer, there are also the metadata bytes of the allocation header. So we compute where the first user pointer would land after the slab header and the allocation header, round *that* up to the alignment boundary, then back-calculate how much metadata space is needed in total.

If this alignment bump pushed `first_user_ptr` forward by some bytes, those bytes become padding at the front of the slab. After that, slots repeat at `cache->obj_size` intervals, and every subsequent user pointer is automatically aligned because `obj_size` itself was aligned up during cache creation.

### Creating a Cache

`slab_cache_create` is where all the sizing decisions happen:

```c
slab_cache_t* slab_cache_create(const char* name, size_t obj_size, size_t align) {
    // ...
    cache->user_size = obj_size;

    size_t total_size = obj_size + sizeof(slab_alloc_header_t);

    if (total_size < SLAB_MIN_OBJ_SIZE) {
        total_size = SLAB_MIN_OBJ_SIZE;
    }

    cache->obj_size = align_up(total_size, align);
    cache->align = align;
    // ...
}
```

The slot size is the user's requested size *plus* the allocation header. The allocation header is prepended to every slot and stores metadata used during `slab_free` to validate that we are freeing the right thing to the right cache:

```c
typedef struct {
    uint32_t magic;
    uint32_t cache_id;
} slab_alloc_header_t;
```

`magic` (`0xA110C8ED` — "allocated") confirms the slot is currently in use. `cache_id` identifies which cache it came from. If you try to free an object to a different cache than the one it was allocated from, we catch it:

```c
if (header->cache_id != cache->cache_id) {
    LOGF("[SLAB ERROR] Cache ID mismatch\n");
    // ...
}
```

The minimum slot size (`SLAB_MIN_OBJ_SIZE`) ensures that even if the user asks for a tiny object (smaller than what a free header needs) the slot is still large enough to hold the free-list metadata when it is in the free state. More on what that free-list metadata looks like next.

So where does the `slab_cache_t` itself live? We can't allocate it from the slab, that's the allocator we're in the middle of building. We could ask the PMM for a page, but `sizeof(slab_cache_t)` is a tiny fraction of `4KB`, so that's exactly the waste the slab exists to avoid.

>[!NOTE]
>This is a classic bootstrapping problem in OS development. You need an allocator to build the data structures that describe the allocator.

GatOS takes the easy way out. There can only ever be 16 caches (`SLAB_MAX_CACHES`), so they all live in a static table in BSS:

```c
static slab_cache_t cache_pool[SLAB_MAX_CACHES];

static slab_cache_t* slab_alloc_cache_struct(void) {
    for (int i = 0; i < SLAB_MAX_CACHES; i++) {
        if (cache_pool[i].magic == SLAB_CACHE_MAGIC) continue;
        kmemset(&cache_pool[i], 0, sizeof(slab_cache_t));
        return &cache_pool[i];
    }
    return NULL;
}
```

A slot whose `magic` is set belongs to a live cache, so we skip it. The first one that isn't gets cleared and handed out. Destroying a cache clears its magic, which frees the slot up again. No allocation, no bootstrapping problem, and the whole table costs a couple KiB that are part of the kernel image anyway.

### The Object Freelist

Within a slab, free slots are tracked using an embedded freelist. This is the same trick the PMM uses for free blocks, applied one level down. When a slot is free, its first bytes contain:

```c
typedef struct slab_free_obj {
    uint32_t magic;
    uint32_t red_zone_pre;
    struct slab_free_obj* next;
    uint32_t red_zone_post;
} slab_free_obj_t;
```

`magic` is `0xFEEDF00D`. `next` points to the next free slot in this slab (as a virtual pointer, since we are in C land and the physmap is active). The two `red_zone` fields flanking `next` are both `0xDEADFA11`. If a write overflow from an adjacent allocated slot clobbers either red zone, we detect it the next time `validate_free_obj` runs.

When a new slab is created, all slots are chained into a freelist in reverse order:

```c
uint8_t* obj_base = (uint8_t*)slab + metadata_size;
slab->freelist = NULL;

for (size_t i = 0; i < slab->capacity; i++) {
    slab_free_obj_t* obj = (slab_free_obj_t*)(obj_base + i * cache->obj_size);
    obj->magic = SLAB_FREE_MAGIC;
    obj->red_zone_pre = SLAB_RED_ZONE;
    obj->red_zone_post = SLAB_RED_ZONE;
    obj->next = (slab_free_obj_t*)slab->freelist;
    slab->freelist = obj;
}
```

This builds a stack (LIFO) pointing from the last slot back to the first. Allocation pops from the front. Freeing pushes back. The result is that recently freed slots tend to be reused first, which is good for cache locality. Hot slots stay hot.

When allocating, we pop the head of the slab's freelist, validate the free object header, then zero the slot and write the allocation header:

```c
slab_free_obj_t* obj = (slab_free_obj_t*)slab->freelist;
if (!validate_free_obj(obj)) {
    LOGF("[SLAB ERROR] Corrupted free object in cache '%s'\n", cache->name);
    spinlock_release(&cache->lock, flags);
    return SLAB_ERR_CORRUPTION;
}

slab->freelist = obj->next;
slab->in_use++;

kmemset(obj, 0, cache->obj_size);

slab_alloc_header_t* header = (slab_alloc_header_t*)obj;
header->magic = SLAB_ALLOC_MAGIC;
header->cache_id = cache->cache_id;
```

`kmemset` clears the entire slot, including the old free object header. Then we write the allocation header at the same location. The pointer returned to the caller is `sizeof(slab_alloc_header_t)` bytes past the start of the slot, so the caller never sees the header. Their allocation starts clean and zero-initialized.

When freeing, we subtract back to find the start of the slot, validate the allocation header, then re-initialize the free object header:

```c
void* obj_start = (void*)((uint8_t*)obj - sizeof(slab_alloc_header_t));

slab_t* slab = get_slab_from_obj(obj_start);
// ...

slab_alloc_header_t* header = (slab_alloc_header_t*)obj_start;
if (header->magic != SLAB_ALLOC_MAGIC) {
    LOGF("[SLAB ERROR] Invalid allocation magic (double-free or corruption)\n");
    // ...
}

slab_free_obj_t* free_obj = (slab_free_obj_t*)obj_start;
free_obj->magic = SLAB_FREE_MAGIC;
free_obj->red_zone_pre = SLAB_RED_ZONE;
free_obj->red_zone_post = SLAB_RED_ZONE;
free_obj->next = (slab_free_obj_t*)slab->freelist;
slab->freelist = free_obj;
slab->in_use--;
```

A double-free (freeing the same object twice) will see `SLAB_FREE_MAGIC` where it expects `SLAB_ALLOC_MAGIC`, because the first free already converted the slot back to a free object header. Caught immediately.

### Finding the Slab from an Object Pointer

Here is a genuinely clever trick. When freeing an object, we need to know which slab it came from. The obvious approach is to store a back-pointer to the slab in the allocation header. But there is a cleaner way that costs nothing:

```c
static slab_t* get_slab_from_obj(void* obj) {
    if (!obj) return NULL;

    uintptr_t addr = (uintptr_t)obj;
    uintptr_t slab_addr = addr & ~(PAGE_SIZE - 1);
    slab_t* slab = (slab_t*)slab_addr;

    if (!slab_validate(slab)) {
        return NULL;
    }

    return slab;
}
```

`addr & ~(PAGE_SIZE - 1)` masks off the low 12 bits, rounding down to the nearest 4 KB page boundary. Because the slab header lives at the very start of its PMM page, and PMM pages are always 4 KB aligned, this lands directly on the `slab_t` header. We validate the magic to confirm it is actually a slab, and we are done.

No back-pointers. No extra storage. Page alignment does the work for free. This is one of those tricks that feels obvious in retrospect but takes a moment to appreciate the first time you see it.

### The Three List States

Every slab belongs to exactly one of three lists in its cache at all times:

* **Empty** (`slabs_empty`): `in_use == 0`. Every slot is free. The slab is dormant but ready.
* **Partial** (`slabs_partial`): `0 < in_use < capacity`. Mix of allocated and free slots.
* **Full** (`slabs_full`): `in_use == capacity`. Every slot is allocated. No free slots at all.

Allocation always prefers partial slabs:

```c
slab_t* slab = NULL;

if (cache->slabs_partial) {
    slab = cache->slabs_partial;
} else if (cache->slabs_empty) {
    slab = cache->slabs_empty;
} else {
    slab = slab_allocate_page(cache);
    if (!slab) {
        spinlock_release(&cache->lock, flags);
        return SLAB_ERR_NO_MEMORY;
    }
    slab_add_to_list(&cache->slabs_empty, slab, SLAB_LIST_EMPTY);
}
```

Why prefer partial over empty? Because using a partial slab increases the density of allocations within a page. If we always preferred empty slabs, we would spread allocations thinly across many pages, never filling any one page enough to return it to the PMM. By packing into partial slabs first, we are more likely to eventually fill them completely, and we are more likely to eventually empty them completely (making them returnable). Both extremes (fully full, fully empty) are better than a sea of barely-used partial slabs.

We only pull from an empty slab when no partial exists, and we only call `pmm_alloc` when there are no empty slabs at all.

After allocating from a slab, its fill level may have changed, so we put it on the list it now belongs on. That's `slab_relist`'s job:

```c
static void slab_relist(slab_cache_t* cache, slab_t* slab) {
    slab_t** lists[] = { NULL, &cache->slabs_empty, &cache->slabs_partial, &cache->slabs_full };
    uint64_t* counts[] = { NULL, &cache->stats.empty_slabs, &cache->stats.partial_slabs, &cache->stats.full_slabs };

    uint8_t to = slab->in_use == 0 ? SLAB_LIST_EMPTY
               : slab->in_use == slab->capacity ? SLAB_LIST_FULL
               : SLAB_LIST_PARTIAL;
    uint8_t from = slab->list_id;
    if (to == from) return;

    if (from != SLAB_LIST_NONE) {
        slab_remove_from_list(lists[from], slab);
        (*counts[from])--;
    }
    slab_add_to_list(lists[to], slab, to);
    (*counts[to])++;
}
```

**What's happening here:**

* The two arrays line up with the `SLAB_LIST_*` ids, so `lists[id]` is that list and `counts[id]` is its counter in the stats. Index `0` is `SLAB_LIST_NONE`, a slab that isn't on any list yet, hence the `NULL`.
* `to` is where the slab *should* be, judging by `in_use`. `from` is where it is now.
* If those match, nothing to do. That's the common case: most allocations don't change a slab's state.
* Otherwise, we take it off its old list and put it on the new one, counters included.

The `list_id` field on the slab is critical here. Without it, moving a slab between lists would require knowing which list it came from, which would mean scanning all three. With `list_id`, we know exactly where to remove it from in O(1).

### Freeing and Page Reclamation

Freeing an object from a slab updates `in_use`, pushes the slot back onto the slab's freelist, and calls the same `slab_relist`. Then there's one extra step:

```c
// An empty slab goes back to the empty list
slab_relist(cache, slab);
if (slab->in_use == 0 && cache->stats.empty_slabs > 1) {
    slab_remove_from_list(&cache->slabs_empty, slab);
    cache->stats.empty_slabs--;
    slab_free_page(slab);
}
```

When a slab becomes empty, `slab_relist` moves it to `slabs_empty`. But then there is an extra check: `if (cache->stats.empty_slabs > 1)`. If there is already an empty slab in the cache, we do not need this one too. We free it back to the PMM.

Why `> 1` rather than `> 0`? We keep *one* empty slab around as a warm buffer for the next allocation burst. If we returned every empty slab immediately, a sequence of alloc-free-alloc-free would hammer the PMM with page allocations and frees, which is expensive. Keeping one empty slab in reserve absorbs that pattern at no ongoing cost (one idle PMM page per cache is not a big deal). Keeping *two or more* empty slabs would just waste memory.

And when a full slab has one object freed, `slab_relist` drops it back to partial, making it a candidate for future allocations again.

`slab_free_page` is the reverse of `slab_allocate_page`:

```c
static void slab_free_page(slab_t* slab) {
    if (!slab_validate(slab)) return;
    slab_cache_t* cache = slab->cache;
    // ...
    slab->magic = 0;
    pmm_free(slab->slab_phys, PAGE_SIZE);
}
```

We clear the magic before calling `pmm_free` to help detect dangling references. If anything tries to access this slab after freeing its page, the magic check will fail and we will know.

### Why Slab Must Come Before the VMM

The Virtual Memory Manager we will build in the next chapter needs to track virtual memory regions: their base address, length, flags, associated physical backing, and relationships with other regions. These tracking structures are small objects (a few dozen bytes each) and there can be many thousands of them on a system with heavy virtual memory usage.

If the VMM were initialized before the Slab allocator, it would have two bad options: allocate an entire PMM page per tracking structure (catastrophically wasteful), or implement its own bespoke small allocator (reinventing the wheel badly). Neither is acceptable.

The Slab allocator, by contrast, has zero dependency on the VMM. It uses PMM pages directly and accesses them through the physmap. It needs nothing that only the VMM can provide.

This forces the ordering:

```
PMM → Slab → VMM
```

Each layer is a prerequisite for the next. Violating this order means the VMM cannot bootstrap its own data structures, and you get a panic or, worse, silent corruption. You'll see this exact order in the init sequence at the end of the chapter.

`slab_init` itself is intentionally simple:

```c
slab_status_t slab_init(void) {
    spinlock_init(&slab_list_lock, "slab_list");
    bool flags = spinlock_acquire(&slab_list_lock);

    if (slab_inited) {
        spinlock_release(&slab_list_lock, flags);
        return SLAB_ERR_ALREADY_INIT;
    }

    if (!pmm_is_initialized()) {
        LOGF("[SLAB] PMM must be initialized before slab allocator\n");
        spinlock_release(&slab_list_lock, flags);
        return SLAB_ERR_NOT_INIT;
    }

    caches = NULL;
    next_cache_id = 1;
    kmemset(&stats, 0, sizeof(slab_stats_t));
    slab_inited = true;

    LOGF("[SLAB] Slab (System Wide) Allocator initialized\n");

    spinlock_release(&slab_list_lock, flags);
    return SLAB_OK;
}
```

It checks the PMM is up (`pmm_is_initialized()`), initializes the global state, and sets `slab_inited = true`. No memory allocation happens here, no PMM pages are claimed. The Slab allocator initializes lazily, so PMM pages are only claimed when the first allocation from a cache happens and a new slab needs to be backed.

### Integrity Checks

Both allocators include deep verification functions: `pmm_verify_integrity` and `slab_verify_integrity`. These are not meant for production use on every allocation. They are for debugging, tests, or called when you suspect something has gone wrong.

`pmm_verify_integrity` walks every free list at every order. For every block, it checks that it's in range and aligned to its size, that the page map marks it as a free block of that order, that its header is valid, and that its `prev_phys` really points at the block we just came from. Then it compares what it counted against `stats.free_blocks[order]`. Any discrepancy means the stats have drifted from reality, which usually indicates a push or pop happened without the corresponding stats update.

```c
for (uint64_t cur = free_heads[order]; cur != EMPTY_SENTINEL; cur = hdr_of(cur)->next_phys) {
    if (cur >= range_end || (cur & (order_to_size(order) - 1)) ||
        pmap[cur / PAGE_SIZE] != (PG_FREE | order) || !valid_hdr(cur, order) ||
        hdr_of(cur)->prev_phys != prev || ++count > stats.free_blocks[order]) {
        LOGF("[PMM] Order %u: broken block at 0x%lx\n", order, cur);
        ok = false;
        break;
    }
    prev = cur;
}
```

That last condition doubles as the infinite loop guard. A list where two blocks point to each other in a cycle would keep counting forever, so the moment the count goes past what the stats say, we stop.

Finally, it checks the page map from the other side. Every free mark in the page map has to belong to a block that's actually on a list, so the total number of marks must match the total number of listed blocks. A mismatch means a block that's marked free but unreachable, or the reverse.

`slab_verify_integrity` goes through every cache, every slab in every list, validates the slab headers, checks that empty slabs actually have `in_use == 0`, that full slabs actually have `in_use == capacity`, and that the free object count in each slab matches `capacity - in_use`. It even has an infinite loop guard:

```c
if (slab_num > 10000) {
    LOGF("[SLAB VERIFY] Cache '%s': %s list has too many slabs (loop?)\n", ...);
    all_ok = false;
    break;
}
```

You can call these anywhere during development. Wrapping suspicious initialization sequences with them quickly narrows down where corruption is introduced:

```c
PANIC_ASSERT(pmm_verify_integrity());
// ... do some operations ...
PANIC_ASSERT(pmm_verify_integrity()); // still good?
```

They are slow (O(n) in the number of free blocks or slab objects), but correctness during development is worth a few milliseconds of boot time.

## Putting It All Together

Here is the section of `kernel_bootstrap()` that this chapter describes, with the reasoning made explicit:

```c
// PMM: the foundation. Physical pages. Nothing works without this.
pmm_status_t pmm_status = pmm_init(kmem->ram, kend_grow(mb, pmm_map_size(kmem->ram)));
if(pmm_status != PMM_OK) { ... return false; }

pmm_exclude_range(0, kmem->kend);

for (size_t i = 0; i < mb->memory_map_length; i++) {
    // ... 
    pmm_populate(region_start, region_end);
}
QEMU_LOG("Initialized physical memory manager", TOTAL_DBG);

// Slab: small object allocation. VMM depends on this.
slab_status_t slab_status = slab_init();
if(slab_status != SLAB_OK) { ... return false; }
QEMU_LOG("Initialized slab allocator", TOTAL_DBG);

// VMM can now use Slab for its tracking structures...
```

Each step enables the next. The physmap (Chapter 5) was a prerequisite for the PMM. The PMM is a prerequisite for the Slab. The Slab is a prerequisite for the VMM. Spinlocks (Chapter 6) were a prerequisite for all three — without them, concurrent access would corrupt every data structure we just built.

You can also appreciate now why the crash console needed no allocator at all. It was built before any of this existed, operating on nothing but a framebuffer pointer and a handful of static variables. This chapter's entire infrastructure was built *on top of* that guarantee. If something in the PMM initialization fails, `panic` fires and the crash console shows you exactly where and why. No heap, no slab, no VMM required.

In the next chapter, you better strap in, because we are going straight for the boss fight. We are in a position to explain, motivate, and build the most complex subsystem in kernel dev, and it's none other than the VMM.