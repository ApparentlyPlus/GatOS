# Chapter 8: The Virtual Memory Manager

Last chapter promised a boss fight, so let's load the healthbar. Welcome! If you made it through the buddy allocator and the slab, you are more ready for this than you think. Grab a coffee, though. It's a long one.

So far, every allocator we have built deals in physical memory. The PMM hands out pages and merges buddies as it goes. The slab packs small objects into those pages, so a 40 byte struct no longer costs us 4 KiB. Both of them reach memory through the physmap.

The virtual address space, on the other hand, has not changed since Chapter 5. It still holds exactly two things, the kernel image at `KERNEL_VIRTUAL_BASE` and the physmap at `PHYSMAP_VIRTUAL_BASE`, and nothing has touched the page tables since. We can allocate physical memory all day long, but we cannot put a single new page anywhere in virtual memory.

This is the VMM's job, and it is, by a mile, the biggest file in the GatOS memory subsystem (the comment at the top of [`vmm.c`](/src/kernel/memory/vmm.c) literally says "This was hell to write :D"). We are going to build it up slowly: first the why, then the bookkeeping, then the page tables, and only at the very end the init code that turns it on.

## What's Wrong With the Physmap?

This is a fair question. We already have every byte of RAM mapped, and getting at any of it is as simple as taking a physical address from the PMM and turning it into a pointer with `PHYSMAP_P2V`. Why would we need yet another subsystem on top of that?

Because the physmap is a mirror, and a mirror can only show you what's already there, mapping physical address `X` to `PHYSMAP_VIRTUAL_BASE + X`, for every `X`, with the same flags, forever. Everything it can't do falls out of that one sentence:

* It can't control permissions, since every page in the physmap is writable and kernel-only. There's no way to say "this page is read-only" or "userspace may touch this page" without also saying it about every other page that happens to sit next to it. Of all the combinations of `PAGE_USER` and `PAGE_WRITABLE`, the physmap uses exactly one.
* It can't reach device registers. They live at physical addresses too (that's MMIO), but the physmap stops at the end of RAM, plus whatever stretch it took to cover the framebuffer. A timer chip at `0xFED00000` is simply not in there.
* It can't give a process a private view, even though we picked the higher half design back in Chapter 2 on the promise that every process gets its own lower half. The physmap lives in the higher half and looks the same to everyone, by design.
* It can't fake memory. Demand paging, which the same chapter also talked about, only gives a page real backing the first time someone touches it, and a mirror of RAM has no concept of "not there yet".

Each of those needs someone to write *new* page table entries at runtime, at addresses of our choosing, with flags of our choosing, and to remember what they wrote so they can take it back later. That someone is the VMM.

>[!NOTE]
> Textbooks will tell you the VMM's other big job is stitching scattered physical frames into one contiguous virtual range, so a 64 KiB buffer doesn't need 64 KiB of contiguous RAM. GatOS only does that for lazy memory (you'll see it near the end). For everything else, it asks the PMM for one contiguous block and maps that instead.
>
> It's simpler, it lets us use 2 MiB pages, and the buddy allocator from last chapter is good at keeping large blocks around. If fragmentation ever becomes a problem, this is the first thing I'll change.

## The Life of One Allocation

All of that is still pretty abstract. Before any code, let's follow one allocation from start to finish, and keep it as a map for the rest of the chapter.

Say a VMM manages the range `[0x400000, 0x410000)`, it has never handed anything out, and someone calls `vmm_alloc` for 4 bytes, writable.

1. The VMM rounds the size up to whole pages, because the page tables only deal in whole pages. 4 bytes become 4 KiB.
2. It looks for the lowest free spot in its range that fits 4 KiB. Nothing is taken, so that's `0x400000`.
3. It asks the PMM for a physical page. Let's say the PMM hands back `0x2A3000`.
4. Virtual `0x400000` has no page tables behind it at all, so the VMM walks down from the PML4. Whenever it hits an empty entry on the way, it fills it with a fresh page from the PMM, used as the next table down. Every table is reached through `PHYSMAP_P2V`, like everything else physical.
5. At the bottom, it writes one PT entry, `0x2A3000 | present | writable`.
6. Finally, it writes down "`[0x400000, 0x401000)` is taken, writable, and it's my own RAM" somewhere it can find it again.

From that point on, any access to virtual `0x400000` lands on physical `0x2A3000`. The caller never needs to know that number. Freeing it runs the same steps backwards. We look up the record, clear the mapping, give `0x2A3000` back to the PMM and forget the record.

Every step on that list is a section of this chapter. Step 6 is the bookkeeping, which is the hardest part to do fast, while step 2 is finding room, and steps 4 and 5 are the page table plumbing. The whole thing glued together is `vmm_alloc`.

>[!NOTE]
> The VMMs in Linux or Windows do a lot more than this. They evict pages to disk (swap) when RAM runs out, share one physical page between several address spaces until someone writes to it (copy-on-write), and map files straight into memory.
>
> GatOS does none of those yet. There's no disk driver to swap to, and copy-on-write only matters once processes can clone themselves. Both are on the list of future improvements at the bottom of [`vmm.h`](/src/kernel/memory/vmm.h).

## Address Spaces

The first thing to pin down is what a VMM *owns*. In GatOS, one VMM instance manages one address space. `vmm_t` describes exactly that:

```c
typedef struct {
    uint64_t pt_root;
    vm_object* objects;
    uintptr_t alloc_base;
    uintptr_t alloc_end;
} vmm_t;
```

`pt_root` is the physical address of this address space's PML4. Once it is loaded into `cr3`, the CPU sees the world through this VMM's page tables. We store the physical address because that is what `cr3` expects, and we can always reach the table through the physmap anyway.

`alloc_base` and `alloc_end` are the window this VMM is allowed to hand out. Anything outside `[alloc_base, alloc_end)` is none of its business. `objects` is the list of everything it has handed out so far (it gets its own section in a minute).

Remember the note from last chapter, that there's one PMM, one slab, but **many** VMMs? There's always exactly one *kernel* VMM, plus one more for every process. Pretty much every function in [`vmm.h`](/src/kernel/memory/vmm.h) takes a `vmm_t*` as its first argument, and passing `NULL` means "the kernel's". You'll see `vmm_alloc(NULL, ...)` all over the kernel.

The two kinds get very different windows:

| VMM | `alloc_base` | `alloc_end` |
| --- | --- | --- |
| Kernel | one page past `KEND` (in the higher half) | `0xFFFFFFFFFFFFF000` |
| Process | `0x400000` (4 MiB) | `0x00007FFFFFFFF000` |

The kernel one starts right after the kernel image, plus a one page gap so nothing the VMM hands out sits flush against the kernel's last bytes. On QEMU's default machine that's `0xffffffff801aa000`, which leaves about 2046 MiB until the top of the address space.

Why squeeze the kernel's dynamic memory into the last 2 GiB, when the higher half is 128 TiB wide? There's a very good reason, but it only makes sense once processes have their own page tables. Hold that thought until the end of the chapter.

The process window covers the whole lower half, minus the first 4 MiB. That gap is the NULL pointer guard. Since nothing is ever mapped near `0x0`, dereferencing a NULL pointer (or `NULL + 0x40`, for a struct field) page faults instead of quietly reading something.

## Remembering What We Mapped

This one took me embarrassingly long to appreciate. The page tables already record what is mapped where, so why would the VMM need its own records on top of them?

Try answering these from the page tables alone:

* When someone calls `vmm_free(addr)`, how long is the thing that starts at `addr`? Page tables have no idea where one allocation ends and the next one begins. Two neighboring 8 KiB allocations look exactly like one 16 KiB allocation.
* When we unmap a page, do we give its physical frame back to the PMM? That depends. If it's RAM we allocated, yes. If it's a device register, absolutely not, because the PMM has never heard of that address and would add it to a free list.
* A page fault hits an address with no page table entry. Is that a bug, or a page we promised to someone and haven't backed yet? A missing entry looks the same either way.

Every allocation therefore gets a record, the `vm_object`:

```c
struct vm_object{
    uintptr_t base;
    size_t length;
    size_t flags;
    vm_object* next;
};
```

A virtual range (`base`, `length`) plus a set of flags. Other kernels call these VMAs (virtual memory areas). You will catch that name in a few GatOS functions too, like `vma_insert`. It means the same thing there. `next` is for walking them in address order, more on that later.

The flags decide how the memory behaves:

| Flag | Meaning |
| --- | --- |
| `VM_FLAG_WRITE` | Writable. Without it the range is read-only. |
| `VM_FLAG_EXEC` | Code may run from it. Without it, the CPU refuses to execute from it (if it can, more on that soon). |
| `VM_FLAG_USER` | Ring 3 may touch it. |
| `VM_FLAG_FOREIGN` | The physical memory behind it isn't ours. Never give it back to the PMM. |
| `VM_FLAG_LAZY` | Don't back it yet. The first touch of each page brings that page in. |
| `VM_FLAG_DEVICE` | It's device registers. Don't cache it. |

These are deliberately *not* the hardware `PAGE_*` bits. A `vm_object` describes what the memory is for. Later on, a small function turns that into whatever bits the CPU wants. `FOREIGN` and `LAZY` don't even have a hardware equivalent. They only mean something to the VMM.

>[!IMPORTANT]
> Keep in mind the defaults here. A `vm_object` with flags `0` is read-only, not executable, kernel-only, owned and eagerly backed. You have to opt *in* to every permission. We did this on purpose, because forgetting a flag should give you a fault you can debug, never a security hole you can't see.

### Public and Private Halves

If you look at the actual code, you'll notice the VMM never allocates a `vm_object` directly. It allocates this:

```c
typedef struct vmo_ext {
    uint32_t magic;
    uint32_t red_zone_pre;
    vm_object public;
    uint32_t red_zone_post;
    avl_node_t vma_node;
} vmo_ext;
```

`vmm_t` gets the same treatment:

```c
typedef struct {
    uint32_t magic;
    vmm_t public;
    bool is_kernel;
    avl_tree_t vma_tree; // VMA tree, sorted by base address
    spinlock_t lock;
} vmm_ctx;
```

The `public` part is what the rest of the kernel gets a pointer to. Everything around it is private to `vmm.c`. If this looks familiar, it should. It's the same paranoia we gave the PMM's free headers and the slab's free objects.

`magic` (`0xACCE55ED` for objects, `0xC0FFEEEE` for VMMs) says "this really is one of ours", and the red zones (`0xDEADC0DE`) on either side of the public part catch anyone who writes past the end of the struct they were handed.

`vmm_get_instance` gets from the public pointer back to the private struct:

```c
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
```

`offsetof(vmm_ctx, public)` is how many bytes into `vmm_ctx` the `public` field starts. Our pointer points at `public`, so step back that many bytes and you're at the start of the `vmm_ctx`. Then check the magic, because a caller could hand us any pointer at all. Every public VMM function starts with this. It is also where the `NULL` means kernel rule lives.

Keep this trick in mind (pointer to a member, step back by its offset, get the container), because it shows up again in about two sections, as the backbone of the whole tracking structure.

Both structs come from the slab, by the way, from two caches called `vmo_ext` and `vmm_ctx`. The previous chapter warned about this dependency. Let's put numbers on it. A `vmo_ext` is 80 bytes. With the slab's 8 byte allocation header on top, each one takes an 88 byte slot, and after the 64 byte `slab_t` header, 45 of those fit in one page.

Without the slab, each object would need a page of its own, and every allocation in the kernel would burn an extra 4 KiB just to *remember* itself. With it, the same 45 objects share one page.

## Keeping Track of Objects

We now have a pile of `vm_object`s that need somewhere to live.

The right data structure depends on what we ask of that pile:

1. Which object starts at `addr`? `vmm_free(addr)` asks this.
2. Which object *contains* `addr`? The page fault handler asks this on every lazy page, and the syscall layer will ask it to check every buffer userspace hands the kernel.
3. Where is a free gap of `N` bytes? `vmm_alloc` asks this every time.
4. What are all the objects, in address order? Destroying an address space, debugging and integrity checks all need this.

>[!IMPORTANT]
> **Why not a bitmap, like the simplest PMM from last chapter?**
>
> One bit per virtual page, `1` for used. It works great for physical memory, because physical memory is small. A process's lower half is 128 TiB. That's `2^47 / 2^12 = 2^35` pages. At one bit each, we are looking at **4 GiB of bitmap**. Per process. On a machine with 128 MiB of RAM.
>
> A bitmap also wouldn't know where objects begin and end, or what flags they have. That is what we needed records for in the first place. So no.

The obvious answer is a linked list sorted by `base`. Number 4 comes free with it, but everything else is a linear scan. A process with a few hundred objects (heap arenas, thread stacks, a few mapped buffers) pays a few hundred pointer chases on every lazy page fault. That doesn't sound like much, but page faults sit right on the hot path of a running program, and "a few hundred" grows with every thread.

A binary search tree, then, sorted by `base`, with `O(log n)` lookups. That sounds like it solves the problem.

It doesn't quite, because of how `vmm_alloc` picks addresses: it takes the *lowest* gap that fits (we'll see the code soon). On a fresh address space, that means every new object lands right after the previous one. Insert `0x400000`, then `0x401000`, then `0x402000`, and so on, into a plain BST:

```
0x400000
    \
    0x401000
        \
        0x402000
            \
            0x403000
                \
                ...
```

Every insert goes right. What we built is essentially a linked list, and lookups are back to `O(n)`, because our allocation pattern happens to be close to the worst case for a plain BST.

What we want is a tree that refuses to get lopsided.

### The AVL Tree

An AVL tree (named after Adelson-Velsky and Landis, two Soviet mathematicians, 1962) is a binary search tree with one extra rule. For every node, the heights of its left and right subtrees differ by at most 1. Height is the number of nodes on the longest path down to a leaf, where a leaf has height 1 and an empty subtree counts as 0.

That rule is enough to keep the whole tree's height within about `1.44 * log2(n)`. A tree with a million nodes is at most 28 levels deep, so every lookup touches at most 28 nodes.

The rule gets enforced after every insert and remove. The tree walks from the changed spot back up to the root, recomputing heights, and wherever a node's two sides differ by 2, it *rotates* that spot back into shape. Let's see the rotation with our worst case. Insert `A = 0x400000`, `B = 0x401000`, `C = 0x402000`:

```
Before:                  After rot_left(A):

A  (left 0, right 2)          B
 \                           / \
  B                         A   C
   \
    C
```

`A`'s left side has height 0 and its right side has height 2. Too much. And since `B` also leans right, this is the simple case: one *left rotation* around `A`. `B` moves up and becomes the root, `A` drops down to be `B`'s left child, and `C` stays where it was. The order is untouched (`A < B < C` still reads left to right), but the height went from 3 to 2.

The real thing lives in [`klibc/avl.c`](/src/klibc/avl.c):

```c
static avl_node_t* rot_left(avl_tree_t* tree, avl_node_t* x) {
    avl_node_t* y = x->right;
    avl_node_t* T = y->left;
    relink(tree, x->parent, x, y);
    y->left = x; x->parent = y;
    x->right = T; if (T) T->parent = x;
    upd_height(x);
    upd_height(y);
    return y;
}
```

**What's happening here:**

* `x` is the node that's too right-heavy (`A` above), `y` is its right child (`B`).
* `T` is `y`'s left subtree. It's everything between `x` and `y` in sorted order, so after the rotation it has to hang off `x`'s right side. In our tiny example it's empty.
* `relink` points whatever used to point at `x` (its parent, or the tree root) at `y` instead.
* `y` adopts `x` as its left child, and `x` adopts `T`.
* Heights get recomputed bottom up, `x` first since it's now below `y`.

`rot_right` is the mirror image. There is also one less obvious case. If `A` is right-heavy but `B` leans *left* (insert `0x400000`, `0x402000`, then `0x401000`), one rotation would just move the lopsidedness to the other side. So we rotate `B` right first, which turns it into the simple case, then rotate `A` left. That's all `fix_up` does:

```c
static void fix_up(avl_tree_t* tree, avl_node_t* n) {
    while (n) {
        upd_height(n);
        int b = bal(n);
        if (b > 1) {
            if (bal(n->left) < 0)
                rot_left(tree, n->left);   /* LR case */
            n = rot_right(tree, n);
        } else if (b < -1) {
            if (bal(n->right) > 0)
                rot_right(tree, n->right); /* RL case */
            n = rot_left(tree, n);
        }
        n = n->parent;
    }
}
```

`bal(n)` is left height minus right height, so anything above `1` is too left-heavy and anything below `-1` is too right-heavy. We walk up, fix, and repeat until we fall off the root.

>[!NOTE]
> As with the buddy XOR trick in the last chapter, this is pure algorithms, and the internet has far better animations of AVL rotations than I could ever draw in ASCII. If the double rotation feels like magic, go watch one.

Removal is the same idea with a few more cases (a node with two children swaps places with its in-order successor first). All of it lives in `avl_remove`.

### Intrusive Trees

Look at how a node is defined:

```c
typedef struct avl_node {
    struct avl_node *left, *right, *parent;
    int height;
} avl_node_t;
```

There is no key and no data pointer in there, only links. So how does the tree know what it is sorting?

It doesn't, and that's the point. The node gets *embedded* inside the thing being sorted. If you look back at `vmo_ext`, there it is, `avl_node_t vma_node`, sitting right at the end. This is called an **intrusive** data structure. We used the same idea for the PMM's free headers, which live inside the free blocks themselves. The tree never allocates anything, and putting an object in the tree costs zero allocations, because the node was already part of the object.

The cost is that the tree hands you back `avl_node_t*` pointers when what you need is the `vmo_ext` around them. Remember that `offsetof` trick?

```c
#define AVL_ENTRY(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))
```

Exactly the same thing `vmm_get_instance` did, wrapped in a macro. `AVL_ENTRY(n, vmo_ext, vma_node)` takes a node pointer and gives back the `vmo_ext` it lives in. Linux calls this `container_of`, and once you've seen it you'll spot it absolutely everywhere in kernel code.

Since the tree doesn't know what it's sorting, it asks us, and the VMM gives it a comparator that compares two nodes by the `base` of the objects around them:

```c
static int vma_cmp(const avl_node_t* a, const avl_node_t* b) {
    uintptr_t ba = AVL_ENTRY(a, vmo_ext, vma_node)->public.base;
    uintptr_t bb = AVL_ENTRY(b, vmo_ext, vma_node)->public.base;
    if (ba < bb) return -1;
    if (ba > bb) return 1;
    return 0;
}
```

The whole AVL implementation is generic, which is why it lives in `klibc` and not in the VMM (the scheduler will reuse it later for something completely different, sleeping threads sorted by wake-up time).

### Floor and Ceil

Looking up the object that *starts* at an address is the classic tree search, `avl_find`. But question 2 from earlier is harder. Given an address somewhere in the middle of an object, which object is it?

Since the tree is sorted by `base`, the object containing `addr`, if there is one, must be the one with the largest `base` that's still `<= addr`. Anything with a bigger `base` starts after `addr`, and anything with a smaller one is further left than that one. That's called the **floor** of `addr`:

```c
avl_node_t* avl_floor(avl_tree_t* tree, avl_node_t* key) {
    avl_node_t* n = tree->root, *res = NULL;
    while (n) {
        int c = tree->cmp(key, n);
        if (c < 0) n = n->left;
        else if (c > 0) { res = n; n = n->right; }
        else return n;
    }
    return res;
}
```

It's a normal search that remembers the last node it passed that was smaller than the key. When the search falls off the tree, that's the floor. `avl_ceil` is the mirror, the smallest node `>=` the key.

`vma_find_containing` finds the object that contains an address using the floor:

```c
static vmo_ext* vma_find_containing(vmm_ctx* vmm, uintptr_t addr) {
    vmo_ext key = {0};
    key.public.base = addr;
    avl_node_t* n = avl_floor(&vmm->vma_tree, &key.vma_node);
    if (!n) return NULL;
    vmo_ext* obj = AVL_ENTRY(n, vmo_ext, vma_node);
    if (addr < obj->public.base + obj->public.length) return obj;
    return NULL;
}
```

Pay attention to `key`, because the comparator only knows how to compare nodes that live inside `vmo_ext`s. To search for a plain address, we build a fake `vmo_ext` on the stack with only `base` filled in, then search using its node.

Then the floor candidate either reaches past `addr` (hit) or ends before it (`addr` sits in a gap, miss).

### The Public List

There is one last piece, the one `vm_object.next` and `vmm_t.objects` are for. The rest of the kernel doesn't know the tree exists, it only sees the public structs. The VMM therefore keeps a plain sorted linked list running through them as well, for anyone who wants to walk the objects in order:

```c
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
```

The tree already knows each node's in-order neighbors (`avl_next`, `avl_prev`), so after inserting into the tree, we just splice the new object in between its two neighbors in the list. No previous neighbor means it's the new head, and `vma_remove` does the reverse. There are two structures, but the tree is the one in charge, while the list just follows along.

## Finding Room

With the tree in place, we can answer question 3: where does a new allocation go?

```c
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
```

The idea is a candidate address, `cand`, that starts at the bottom of the window and keeps hopping forward until it lands somewhere that fits. Two things can be wrong with a candidate:

* It's inside an object, because the floor of `cand` reaches past it. We hop to the end of that object.
* The next object starts too soon, because the ceil of `cand` begins before `cand + length`. We hop to the end of *that* one.

If neither is true, the gap fits. The weird looking `cand + length > cand` checks are overflow guards. Near the top of the address space, adding `length` can wrap around to a tiny number, which would sail right through `<= alloc_end`.

Let's trace it. A process VMM (window starts at `0x400000`) holds two objects:

```
A: [0x400000, 0x402000)   8 KiB
B: [0x405000, 0x406000)   4 KiB
```

We ask for `0x3000` bytes (12 KiB), page aligned.

1. `cand = 0x400000`. Floor is `A`, which ends at `0x402000`, past `cand`. We're inside `A`. Hop: `cand = 0x402000`.
2. `cand = 0x402000`. Floor is still `A`, but `A` ends exactly at `cand`, so we're not inside it. Ceil is `B` at `0x405000`. Does `0x402000 + 0x3000 = 0x405000` fit before it? Exactly. Done, `0x402000`.

The 12 KiB request slotted perfectly into the gap between `A` and `B`. Now ask for `0x4000` (16 KiB) instead:

1. Same as before, `cand = 0x402000`.
2. Ceil is `B`. `0x402000 + 0x4000 = 0x406000`, which runs into `B`. Hop past `B`: `cand = 0x406000`.
3. `cand = 0x406000`. Floor is `B`, ending exactly at `cand`, fine. Ceil? Nothing above. Done, `0x406000`.

It picks the lowest gap that fits, every time. With a plain BST, that pattern would have been a disaster.

>[!NOTE]
> Each hop is two `O(log n)` tree lookups, but the number of hops depends on how many objects are too close together for the request. A badly fragmented address space with thousands of tiny gaps would make this crawl. With 128 TiB of lower half per process, though, you'd have to work pretty hard to get there.

There's also `vmm_alloc_at`, for when the caller wants a *specific* address, and that one doesn't search at all, it just needs to know whether the requested range is free:

```c
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
```

In practice, we ask the same two questions once. Does the object before us reach into our range? Does the object after us start inside it? Two lookups, done.

## Talking to the Page Tables

Everything so far has been bookkeeping, and nothing we have seen writes a single page table entry. Time to fix that.

Back in Chapter 5, `build_physmap` wrote page tables with hand-picked indices (`PD_INDEX(pa)`, `PDPT_INDEX(KERNEL_VIRTUAL_BASE)` and friends), because it knew exactly which tables existed and what they mapped. The VMM is in a much worse spot. It gets an arbitrary virtual address and has to figure out at runtime which tables exist and where the walk ends.

### Walking

First, two helpers that turn the four separate `*_INDEX` macros into one formula:

```c
// Levels as the walkers count them: 0 = PML4, 1 = PDPT, 2 = PD, 3 = PT
static inline size_t index_at(uintptr_t virt, int level) {
    return (virt >> (39 - 9 * level)) & PT_ENTRY_MASK;
}

// Bytes one entry covers at a level: 512 GiB, 1 GiB, 2 MiB, 4 KiB
static inline uint64_t span_at(int level) {
    return 1ULL << (39 - 9 * level);
}
```

This is the address breakdown `virt_breakdown.py` prints, done as arithmetic. The PML4 index starts at bit 39, and every level down starts 9 bits lower: 39, 30, 21, 12. Each index is 9 bits wide (`PT_ENTRY_MASK` is `0x1FF`, 511), and an entry at a level covers `2^(that starting bit)` bytes.

`walk` uses them to find the entry that maps an address:

```c
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
```

**What's happening here:**

* Start at the PML4, through the physmap. `pt_root` is physical, so `PHYSMAP_P2V` it is.
* At each level, look at the entry for our address. If it isn't present, nothing maps `virt`, and we return `NULL`.
* At the PT level, the entry *is* the mapping. At the PD level with `PAGE_HUGE` set, it's a 2 MiB page and the walk ends there too.
* Otherwise, the entry points at the next table down. `PT_ENTRY_ADDR` strips the flag bits off (it's just `& ADDR_MASK`), leaving the physmap to turn it into a pointer.

`*level` always says where the walk stopped, even on a miss. This is more useful than it looks. If the walk stops at level 1 because the PDPT entry is empty, then the whole 1 GiB that entry would have covered is unmapped. A loop scanning a range can skip all of it in one go, as we'll see in `release`.

And this is the payoff for the physmap, once again. Every table, at every level, is just a physical page. Since the physmap reaches every physical page, the walk needs no special mappings and no recursive tricks. It is also the reason `QEMU_DUMP_PML4` only works once the physmap exists.

### Mapping One Page

`map_one` walks the tables the same way, but creates any missing table instead of stopping:

```c
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
```

`level` says where the mapping should end: `3` for a normal 4 KiB page, `2` for a 2 MiB one. The loop walks down to that level. Whenever it finds a missing table on the way, it makes one on the spot, which is what people mean by allocating page tables on the fly. The address space starts out almost empty and grows tables only where something actually gets mapped.

A new table is one page from the PMM, zeroed:

```c
uint64_t vmm_alloc_page_table(void) {
    uint64_t phys = 0;
    if (pmm_alloc(PAGE_SIZE, &phys) != PMM_OK) {
        return 0;
    }

    zero_page((void*)PHYSMAP_P2V(phys));

    return phys;
}
```

The zeroing is not optional, because the PMM hands out whatever was in that page before. If an old byte pattern happens to have bit 0 set in some entry, the CPU sees a present entry pointing at garbage, and now a random address in your kernel maps to a random physical page. `zero_page` is just `kmemset` hard-wired to exactly one page.

A few things in `map_one` need explaining:

* Every intermediate table entry on the way down gets `PAGE_PRESENT | PAGE_WRITABLE`, regardless of what we are mapping. This is fine, because the CPU combines the permissions of *every* level, and the strictest one wins. A read-only page under a writable PD is still read-only. The real permissions live in the final entry. The levels above just have to not get in the way.
* `PAGE_USER` is the exception, because the same "strictest wins" rule means a user page under a table without `PAGE_USER` is unreachable from ring 3. For user mappings, the new intermediate tables get `PAGE_USER` too. Kernel mappings never do.
* If we're walking down and find a 2 MiB page where we expected a table, someone already mapped this whole 2 MiB. We can't put a 4 KiB page "inside" it, so it's `VMM_ERR_ALREADY_MAPPED`.
* The address gets rounded to the page size, which for a 2 MiB page means `~(span_at(2) - 1)` clears the low 21 bits. The hardware expects a huge page's physical address to be 2 MiB aligned, and garbage in those bits means something else entirely to the CPU.

Let's watch it build tables from nothing. Take a brand new process, whose PML4 is completely empty, and map its very first page at `0x400000`. This is what `virt_breakdown.py` says about that address and the one right after it:

```
> Enter address: 0x400000
  PML4 Index : 0x0000 (0)
  PDPT Index : 0x0000 (0)
  PD   Index : 0x0002 (2)
  PT   Index : 0x0000 (0)

> Enter address: 0x401000
  PML4 Index : 0x0000 (0)
  PDPT Index : 0x0000 (0)
  PD   Index : 0x0002 (2)
  PT   Index : 0x0001 (1)
```

`map_one(root, phys, 0x400000, flags, 3, true)`:

1. `l = 0`: `PML4[0]` is empty. Allocate a table, that's our PDPT. `PML4[0] = PDPT | P | W | U`.
2. `l = 1`: `PDPT[0]` is empty. Allocate a table, that's our PD. `PDPT[0] = PD | P | W | U`.
3. `l = 2`: `PD[2]` is empty. Allocate a table, that's our PT. `PD[2] = PT | P | W | U`.
4. The loop ends at `level = 3`. `PT[0]` is empty, so `PT[0] = phys | flags`. Done.

Three new page tables for one page of memory. Ouch.

But now map `0x401000`, which has the same PML4, PDPT and PD index as before. All three tables are already there, the loop walks straight through them, and only `PT[1]` gets written. In fact, the next 510 pages after that cost nothing extra either, until we cross into the next 2 MiB and need a second PT. The first page in a region is expensive, and everything around it rides along for free.

### Unmapping One Page

Unmapping has two jobs that mapping didn't have. It must tell the CPU to forget the old translation, and it should also clean up tables that end up empty.

```c
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
```

**What's happening here:**

* The same walk as before, except it remembers every table it passed in `path`. We'll need them on the way back up.
* Once we find the mapping, we note the physical page and its size (4 KiB or 2 MiB) to return to the caller, then clear the entry.
* `invlpg`. Then there is the TLB, the CPU's cache of recent translations. Clearing an entry in memory doesn't clear the TLB's copy of it. Until that copy gets evicted, the CPU would keep translating `virt` to a page we might have already handed to someone else. `flush_tlb` would fix that by throwing out *every* cached translation. `invlpg` is a single instruction that throws out just the one for `virt`, which is a lot cheaper when you're unmapping one page.
* Then we walk back up. If the table we just cleared an entry in is now completely empty, nobody needs it anymore. Free it to the PMM, and clear the entry in the parent that pointed at it. Then check the parent, and so on.

`table_empty` scans the 512 entries of a table for a present bit.

Back to our fresh process, with `0x400000` and `0x401000` mapped. Unmap `0x401000` first:

1. The walk records `path = [PML4, PDPT, PD, PT]` and stops at `l = 3`. Clear `PT[1]`, `invlpg(0x401000)`.
2. Is the PT empty? No, `PT[0]` still maps `0x400000`. Stop.

Now unmap `0x400000`:

1. Clear `PT[0]`, `invlpg(0x400000)`.
2. `l = 3`: the PT is empty now. Free it, clear `PD[2]`.
3. `l = 2`: the PD is empty now too. Free it, clear `PDPT[0]`.
4. `l = 1`: same story. Free the PDPT, clear `PML4[0]`.
5. `l = 0`: that's `floor`, stop. The PML4 itself belongs to the process and stays.

The process is back to an empty PML4, as if those two pages had never existed, with the three tables from the mapping trace all back in the PMM.

And then there's `floor`. For lower half addresses it's `0`, and the cleanup may go all the way up, like in the trace. For higher half addresses it's `1`, stopping the cleanup before it frees a PDPT, so a PML4 entry is never cleared.

>[!TIP]
> Keep an eye on this `floor` variable.
>
> In the trace above, freeing that PDPT and clearing `PML4[0]` was perfectly fine. So why on earth would doing the exact same thing in the higher half be a problem? A table is a table, and an empty one is wasting a page.
>
> The answer has to do with the kernel window we parked in the last 2 GiB of the address space for no apparent reason. Both have the same cause, and both make sense the moment a second address space exists.

There's one more walker, `phys_of`, which answers "what physical address does `virt` map to?" for `vmm_get_physical`. It's a `walk`, plus the offset inside the page: `(PT_ENTRY_ADDR(*e) & ~(span - 1)) + (virt & (span - 1))`. Works the same for 4 KiB and 2 MiB pages, because `span` comes from the level the walk stopped at.

### Mapping Runs, and Huge Pages

Real allocations are rarely one page, and `map_run` maps a whole range. This is where 2 MiB pages come in:

```c
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
```

At every step, `map_run` checks whether it can take a 2 MiB step instead of a 4 KiB one. We only use one when the virtual address is 2 MiB aligned, the physical address is 2 MiB aligned, and there's at least 2 MiB left to map. If all three hold, one PD entry maps the next 2 MiB. Otherwise, we map a normal 4 KiB page.

We bother for the same reason the physmap used them. A 2 MiB page replaces an entire PT, so a 64 MiB allocation needs 32 PD entries instead of 32 PTs holding 16,384 entries. There's a second win we didn't care about back then. The TLB caches one translation per page, whatever its size. One TLB entry for 2 MiB instead of 512 of them means far fewer misses when a program sweeps through a big buffer.

If any step fails halfway (out of memory for a table, or an existing mapping in the way), the inner loop unmaps everything this call mapped so far. It's all or nothing, because a half-mapped range would be a nightmare to clean up for whoever called us.

The `huge` argument lets callers turn 2 MiB pages off completely (some of them will want to). More on who and why in a moment.

## Allocating and Freeing

We have bookkeeping and we have page table plumbing. Time to connect them.

### Flags, Translated

First, the function that turns `VM_FLAG_*` into real page table bits:

```c
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
```

`WRITE` and `USER` are one-to-one, while the other two need a story each.

`EXEC` is backwards, since the hardware bit is "no execute", `PAGE_NO_EXECUTE`, bit 63. We set it when the object is *not* executable. But that bit only means something if NX is switched on.

When it is off, bit 63 counts as a *reserved* bit, making the CPU page fault on any entry that has it set. Hence the `cpu_is_feature_enabled(CF_NX)` check. It reads a bit called `NXE` straight out of `IA32_EFER` (yes, our old friend, the MSR we flipped to enter long mode), and only sets NX if that bit is on.

>[!NOTE]
> Fun fact! For a long while, GatOS never actually turned NX on. `vmm_kernel_init` (the function that sets `NXE`, we'll see it at the end) asked a different function whether the CPU *supports* NX. That function reads from a table filled in by `cpu_init`, which runs *after* the VMM (Chapter 9). So the answer was always "no", `NXE` stayed off, and every page in the kernel was executable.
>
> Nothing crashed, and since the test kernel happened to call `cpu_init` first, the tests passed with flying colors too. The fix was asking the CPU directly. The test kernel now boots in the same order as the real one, too.

`DEVICE` reuses the PAT we set up for the framebuffer. Setting both `PAGE_PWT` and `PAGE_PCD` picks entry 3, which we deliberately left as uncacheable when we turned entry 1 into write-combining for the framebuffer. The PAT caution back then was about device registers. You write a command and read back a status, and both have to actually reach the device, in that order, right now, without any caching or batching.

### Placing an Object

Now for the heart of it. `place` creates a new object at a known address, backs it with memory, and maps it:

```c
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
```

**What's happening here:**

* `arg` is the odd one out. For a `FOREIGN` object, it's the physical address to map (the caller already knows where the device lives). For everything else, it's ignored.
* A `vm_object` comes out of the slab, magic and red zones already set by `vmm_alloc_vm_object`.
* Lazy objects stop right there, with no memory and no mapping, just the record. The page fault handler does the rest later.
* Owned memory comes from `pmm_alloc_exact`, the variant from last chapter that gives the rounding tail straight back. A 12 KiB request keeps exactly 12 KiB. Then it gets zeroed, **always**. The PMM hands out pages exactly as their last owner left them, and in a minute those pages could belong to a process. Handing userspace a page full of another process's secrets (or the kernel's) gets people writing papers about you.
* `map_run` maps it, with 2 MiB pages allowed for owned memory only. Device ranges can be tiny and oddly placed. They get the slow, safe 4 KiB path.
* Only once everything worked does the object go into the tree. If something failed halfway, nobody else can ever see a record for memory that isn't there.

There's a nice interaction with the buddy allocator hiding in here. `pmm_alloc_exact` allocates a full power-of-two block and only then trims the tail. Buddy blocks are aligned to their own size (the XOR trick relies on it), so any request of 2 MiB or more comes back from a block that's at least 2 MiB aligned. The physical side of `map_run`'s huge page check is basically free. The virtual side is `vmm_alloc`'s job:

```c
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
```

Round the length up to whole pages (the `len < length` check catches the round-up overflowing). For big owned allocations, ask the gap finder for a 2 MiB aligned address, and then, under the VMM's lock, find a gap and place the object there.

The lock is per VMM, same idea as the slab's per-cache locks. Two processes allocating at the same time don't wait on each other, but nobody can slip an object into the gap we just found before we've claimed it.

`vmm_alloc_at` skips the search. It checks the alignment, checks that the range is inside the window, checks `vma_overlaps`, and then calls `place`.

### Freeing

Freeing has to answer the question from the start of the chapter, namely whether the physical memory goes back to the PMM. `release` takes the answer as `owned`:

```c
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
```

We walk the range. Wherever something's mapped, unmap it, and if we own it, hand the page back, a whole 2 MiB at a time if it was a huge page. Wherever nothing's mapped (a lazy page nobody touched yet, for example), skip ahead to the end of whatever the walk found empty.

This is where the `*level` trick from `walk` helps, since an untouched 1 GiB lazy object whose PDPT entry was never even created takes one step instead of 262,144.

Freeing pages one by one (or 2 MiB by 2 MiB) is fine, even though the PMM handed them out as one block in `place`. The PMM doesn't remember who allocated what. All it checks is that the block being freed is aligned, in range, and not already free, thanks to the page map from last chapter.

`vmm_free` then strings it together:

```c
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
```

Find the object that starts at `addr`, validate it, release its range, take it out of the tree, and give the struct back to the slab.

The lookup can fail in two very different ways, with different error codes. No object at `addr` is `VMM_ERR_NOT_FOUND`, which is a caller bug (a double free, or an address in the *middle* of an object), but the VMM itself is fine. An object that's *there* but fails `vm_object_validate` is `VMM_ERR_INVALID`, a much worse result, because it means something scribbled over the VMM's own records.

Same idea as `take_buddy` in the PMM, where "the buddy isn't free" is perfectly normal and "the header is garbage" is corruption. `vmm_free_vm_object` wipes the magic and red zones first, so a stale pointer to a freed object fails validation instead of reading whatever the slab puts there next.

## Foreign Memory

We've been dancing around `VM_FLAG_FOREIGN` for a while. Let's look at who uses it.

The classic case is device registers. This is how GatOS maps the HPET, a timer chip we'll meet in Chapter 10, once ACPI tells us its physical address:

```c
if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_FOREIGN | VM_FLAG_DEVICE, (void*)phys_addr, &virt_addr) != VMM_OK) {
```

One page, in the kernel's address space, writable, not ours, uncached. `phys_addr` goes in through `arg`, and out comes a virtual address the timer code can read and write like a normal struct. When it's unmapped, the PMM never hears about it, and rightly so, because there's no RAM there to give back.

But `FOREIGN` is more general than "device". It means "this physical memory belongs to somebody else". Two other users in GatOS have nothing to do with hardware:

* **ACPI tables** (Chapter 10) are firmware data sitting in RAM. Usually the physmap covers them, but when they live above the end of RAM, `acpi_map_phys` maps them with `VM_FLAG_FOREIGN` and no other flags. Read-only, cacheable, and definitely not the PMM's.
* **Userspace code** (Chapter 15). In GatOS, the code that runs in ring 3 is linked into the kernel image itself. Each process maps those physical pages into its own lower half as `VM_FLAG_USER | VM_FLAG_EXEC | VM_FLAG_FOREIGN`. Kill the process, and its mappings go away, but the kernel's own pages stay put.

And now, a promise to keep. Last chapter, the PMM init loop passed every non-RAM region to `vmm_add_mmio`, and I said we'd cover what that does here. Here goes:

```c
void vmm_add_mmio(size_t bytes) {
    __atomic_add_fetch(&mmio_bytes, bytes, __ATOMIC_RELAXED);
}
```

`mmio_bytes` is just a counter. It tracks how much non-RAM memory the system knows about (firmware reserved regions at boot, plus every `FOREIGN` object while it's alive, as you saw in `place` and `vmm_free`), and `vmm_mmio_total` reports it to the dashboard we'll build in Chapter 14.

It's atomic because it's shared across every VMM in the system, and none of their locks cover it.

Since we've now seen every flag in action, here's who actually asks for what, straight from the GatOS source:

| Caller | Flags | Why |
| --- | --- | --- |
| Kernel heap arenas (Chapter 9) | `WRITE` | Plain kernel RAM. Not executable, not lazy. |
| User thread stacks (Chapter 14) | `USER \| WRITE \| LAZY` | 64 KiB promised, a page or two used. |
| HPET, LAPIC, I/O APIC, xHCI registers | `WRITE \| FOREIGN \| DEVICE` | Hardware. Not ours, never cached. |
| ACPI tables above RAM (Chapter 10) | `FOREIGN` | Firmware data. Read-only and not ours. |
| User code (Chapter 15) | `USER \| EXEC \| FOREIGN` | The kernel's own pages, lent to a process. |

`EXEC` shows up exactly once. With NX on, everything else in that table is data the CPU will refuse to run.

## Lazy Memory and the Page Fault Handler

Next is `VM_FLAG_LAZY`. Demand paging means pages only get real memory the first time they're touched, and `VM_FLAG_LAZY` is GatOS's version of it.

Thread stacks are the poster child for this. In GatOS, every user thread gets a 64 KiB stack (`USER_STACK_SIZE`). Most threads will never use more than a page or two of it. Backed eagerly, every thread would cost 64 KiB of RAM. Backed lazily, a stack only costs what the thread actually touches.

We already saw how a lazy object starts out: `place` creates the record and stops there, with no memory and no page table entries behind it. The interesting part is what happens when someone touches it.

The CPU walks the page tables, finds nothing, and raises a page fault (vector 14), like it would for any other unmapped address. The fault goes through the IDT, into the dispatcher, and lands in whatever is registered for vector 14. Exception handlers have a way out, though.

A handler may return `NULL` to say "not mine, I couldn't fix this", and the dispatcher carries on as if there was no handler. This is the rule I promised we would lean on. The VMM's page fault handler looks like this:

```c
static cpu_context_t* vmm_fault(cpu_context_t* ctx) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    return vmm_handle_fault(cr2, ctx->error_code) ? ctx : NULL;
}
```

Read the faulting address out of `cr2` (just like the panic screen does), and pass it to `vmm_handle_fault` with the error code. If it fixed the fault, return the context unchanged. The CPU then retries the exact instruction that faulted, which now finds a page and carries on, completely unaware anything happened.

If it couldn't fix it, `NULL`, and the dispatcher panics. (A fault in ring 3 gets handed to the process code instead, which is a later chapter's business.)

The real work is in `vmm_handle_fault`, which is its own public function so the tests can poke at it without having to cause real page faults:

```c
bool vmm_handle_fault(uintptr_t addr, uint64_t err) {
    // A page that is already there would fault again the moment we return
    if (err & PF_PRESENT) return false;

    // The active address space first, then the kernel's
    vmm_t* spaces[2] = { vmm_get_current(), vmm_kernel_get() };

    for (int i = 0; i < 2; i++) {
        if (!spaces[i]) continue;
        vm_object* obj = vmm_find_mapped_object(spaces[i], (void*)addr);
        if (!obj || !(obj->flags & VM_FLAG_LAZY)) continue;
        if ((err & PF_USER) && !(obj->flags & VM_FLAG_USER)) return false;

        uint64_t phys;
        if (pmm_alloc(PAGE_SIZE, &phys) != PMM_OK) return false;
        zero_page((void*)PHYSMAP_P2V(phys));

        vmm_status_t status = vmm_map_page(spaces[i], phys, (void*)align_down(addr, PAGE_SIZE), obj->flags);
        if (status == VMM_OK) return true;

        // Already mapped means another CPU backed it first, which is fine
        pmm_free(phys, PAGE_SIZE);
        return status == VMM_ERR_ALREADY_MAPPED;
    }

    return false;
}
```

**What's happening here:**

* First, which address space is it in? The fault could be in the current process's lower half, or in the kernel's higher half. We try the current VMM first, then the kernel's, and look for an object that contains `addr`. It is the floor lookup from earlier. This is why the tree was worth all that trouble.
* If there's no object, or the object isn't lazy, the fault isn't ours. We keep looking, then give up.
* Otherwise, grab one page from the PMM, zero it (same security reason as before), and map it with the object's own flags. We only ever map one page here, because a lazy object fills in one 4 KiB page per fault, in whatever order the program touches them. These pages come from all over physical memory, which is the "scattered frames, contiguous virtual range" trick from the start of the chapter, in practice.
* `vmm_map_page` passes `false` for huge pages, so a lazy page is always exactly 4 KiB.

The two checks at the top took me a bug to get right. Both come from the error code bits we decoded for the panic screen. GatOS names them now, in [`interrupts.h`](/src/arch/x86_64/cpu/interrupts.h):

```c
#define PF_PRESENT  (1 << 0)  // The page was mapped, so it's a protection fault
#define PF_WRITE    (1 << 1)
#define PF_USER     (1 << 2)  // Raised in ring 3
```

The first check is for `PF_PRESENT`. Picture a lazy object without `VM_FLAG_WRITE`. A program reads from it, and the page faults in, read-only, as it should. Then the program *writes* to it. This is a fault too, but a different kind, because the page is right there and only the write isn't allowed. The original handler didn't look at the error code at all.

The handler saw a lazy object, allocated a page, tried to map it, got `VMM_ERR_ALREADY_MAPPED` (the page is there, after all), treated that as "someone else backed it, fine", and returned. The CPU retried the write, which faulted again, forever. No crash and no panic, just one thread stuck in a loop that never gets anywhere. Any fault with `PF_PRESENT` set means the page exists. Allocating another page can't possibly fix that, so we bail.

The second check, `PF_USER`, stops ring 3 from touching kernel memory, even when it's lazy. Without it, a user program poking at a kernel lazy address would make the kernel allocate and map a page *for* it (in the kernel's address space, kernel-only), return, and then fault again on the now-present page.

That is the same loop as above, with the kernel spending real memory every time a user program feels curious. Now a ring 3 fault on an object without `VM_FLAG_USER` is refused before anything is allocated.

The last case is a race we can survive. If two threads touch the same lazy page at the same moment, both fault, both allocate a page, and only one of them wins the `vmm_map_page`. The loser gets `VMM_ERR_ALREADY_MAPPED`, frees its spare page, and returns `true`, because the page *is* there now, just not the one it brought. Either way, retrying the instruction works.

>[!NOTE]
> `vmm_find_mapped_object` takes the VMM's spinlock, which also turns interrupts off while it holds it. Inside a fault handler interrupts are already off (it's an interrupt gate), so that's harmless. It *would* deadlock if the fault came from code that already held that same VMM lock, but nothing inside the VMM ever touches a lazy page while holding it.

## Resizing and Protecting

Two smaller operations round out the API. Neither is very exciting, but each has one gotcha.

`vmm_resize` grows or shrinks an object in place. Growing checks that the new end doesn't run into the next object (`avl_next` again), then backs and maps the new part. A lazy object just gets a bigger `length`, nothing else, while shrinking releases the tail. The gotcha is 2 MiB pages. If the new end falls in the middle of one, we'd have to split it into 512 small pages first, and GatOS simply refuses instead:

```c
// Cutting through the middle of a 2MiB page would need it split, so that's refused
uint64_t* e = walk(vmm->public.pt_root, base + new_length, &level);
if (e && level == 2 && ((base + new_length) & (PAGE_2MB - 1))) {
    LOGF("[VMM ERROR] vmm_resize: Can't shrink into the middle of a 2MiB page\n");
    st = VMM_ERR_INVALID;
}
```

`vmm_protect` changes an object's flags, and rewrites the flags of every entry that's currently mapped, keeping the physical address and the `PAGE_HUGE` bit:

```c
*e = (*e & ADDR_MASK) | pt_flags | (*e & PAGE_HUGE);
invlpg((void*)virt);
```

The gotcha is the TLB, again. Making a page read-only in memory means nothing while the CPU still has the old, writable translation cached. Every rewritten entry gets an `invlpg`. Lazy pages that haven't faulted in yet need nothing at all, since they read the object's flags when they get mapped, and those are already the new ones.

## One Address Space Per Process

Up to now, we've mostly talked about the kernel VMM. But the whole point of `vmm_t` having its own `pt_root` was that there can be more than one. Processes are Chapter 14's job. For now, we only need what the VMM gives them: a fresh address space, a way to switch into it, and a clean way to destroy it.

```c
vmm_t* vmm_create(uintptr_t alloc_base, uintptr_t alloc_end) {
    // ... argument checks, PMM and slab must be online ...

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
    // ...

    if (kernel_vmm) {
        vmm_status_t status = vmm_copy_kmaps(pt_root);
        // ...
    }

    vmm->public.pt_root = pt_root;
    vmm->public.objects = NULL;
    vmm->public.alloc_base = alloc_base;
    vmm->public.alloc_end = alloc_end;
    // ...
    return &vmm->public;
}
```

A `vmm_ctx` from the slab, an empty tree, a lock, and a brand new, zeroed PML4. Then the one line that makes the whole higher half design actually work, `vmm_copy_kmaps`:

```c
// Entries 256-511 map 0xFFFF800000000000 and above
for (size_t i = 256; i < PAGE_ENTRIES; ++i) {
    dest_pml4[i] = src_pml4[i];
}
```

The upper half of a PML4 (entries 256 to 511) is the upper half of the address space, and we copy those 256 entries from the kernel's PML4 into the new one. Now this process sees the kernel image, the physmap and the rest of the higher half where the kernel expects them.

When it makes a syscall or takes an interrupt, the kernel code is right there, at the same addresses, with no switching needed. The lower half (entries 0 to 255) stays empty, ready for this process alone.

Notice what got copied, though: the PML4 *entries*, which are pointers to PDPTs, and not the PDPTs themselves. Every process's PML4 points at the very same PDPTs the kernel uses:

```
                 Kernel PML4    Process A PML4    Process B PML4
lower half [0]   (unused)       A's PDPT          B's PDPT        <- private
           ...
higher [256]     physmap PDPT   physmap PDPT      physmap PDPT    <- same table
half   ...
       [511]     KPDPT          KPDPT             KPDPT           <- same table
```

Each row is one PML4 slot. The lower half points somewhere different in every process. The higher half points at the exact same physical tables everywhere.

Three PML4s, but only one set of higher half tables underneath them. That picture is the answer to the trivia from the unmapping section, and to the kernel window question too.

This also explains why the kernel window lives in the last 2 GiB. Say the kernel VMM maps a new page after a hundred processes already exist. If that page lands under an existing PDPT, the change happens inside a table every process already shares, and all of them see it instantly.

But if it needed a *new* PML4 entry, only the kernel's own PML4 would get it. The hundred copies were taken before it existed. The kernel VMM's window is therefore the top 2 GiB, entirely under PML4 entry 511, the same entry the kernel image has always used. That entry always exists and was copied into every process, so any new table the VMM creates under it (PDs, PTs) is automatically shared too.

`unmap_one` stops at the PDPTs in the higher half because of the reverse problem. If unmapping freed a higher half PDPT and cleared its PML4 entry, only the kernel's PML4 would lose the pointer. Every process would keep pointing at a freed page.

The PMM would give that page to someone else, they would fill it with whatever they like, and every process would then read it as page tables. `floor = 1` prevents this. Higher half PDPTs are never freed, and the PML4 entries pointing at them never change.

Now for the property that makes all of this worth it. Say process A and process B both call `vmm_alloc` for a page, in their own VMMs. Both VMMs start out empty and both pick the lowest gap, so both get `0x400000`. A's PMM page is `0x2A3000`, B's is `0x5F1000`:

```
Process A: virtual 0x400000 -> A's PT -> physical 0x2A3000
Process B: virtual 0x400000 -> B's PT -> physical 0x5F1000
```

Same pointer value, two completely different pieces of RAM. While A's PML4 is in `cr3`, `0x400000` *is* `0x2A3000`. While B's is loaded, the very same address is `0x5F1000`. There's no way for A to even *name* B's memory, because every address A can produce goes through A's tables, and A's tables simply don't lead there. This is the private address space the higher half design promised, and it costs nothing more than a different value in `cr3`.

`vmm_switch` loads the new PML4 into `cr3` and records which VMM is active:

```c
void vmm_switch(vmm_t* vmm_pub) {
    vmm_ctx* vmm = vmm_get_instance(vmm_pub);
    if (!vmm) return;

    PML4_switch(vmm->public.pt_root);
    current_vmm = &vmm->public;
}
```

`PML4_switch` is the same `cr3` writer the physmap used. Loading `cr3` flushes the TLB by itself, taking the old process's translations with it. `current_vmm` remembers who's active, for things like the fault handler's "current VMM first" rule, and the scheduler will call this on every switch between threads of different processes.

`vmm_destroy` tears it all down in two passes. First, every object in the tree gets released, giving back owned memory and leaving foreign memory alone. Then, whatever page tables are left in the lower half get freed:

```c
// Whatever's left in the lower half was mapped without an object (vmm_map_page), so only the
// tables go. The higher half (entries 256-511) is the kernel's, shared by every process
uint64_t* pml4 = (uint64_t*)PHYSMAP_P2V(vmm->public.pt_root);
for (size_t i = 0; i < 256; ++i) {
    if (pml4[i] & PAGE_PRESENT) free_tables(PT_ENTRY_ADDR(pml4[i]), 1);
    pml4[i] = 0;
}

pmm_free(vmm->public.pt_root, PAGE_SIZE);
```

`free_tables` is a small recursive function that frees a table and every table below it, but never the pages they map. Those either belonged to an object (already released) or were mapped by hand with `vmm_map_page`, in which case the caller owns them.

This only covers the lower half, of course, since touching entries 256 to 511 here would free the kernel's tables out from under every other process. Finally, the PML4 itself goes, with the `vmm_ctx` going back to the slab.

## Bringing the Kernel VMM Online

We've built every piece, and all that's left is the switch that turns them on, `vmm_kernel_init`:

```c
vmm_status_t vmm_kernel_init(uintptr_t alloc_base, uintptr_t alloc_end) {
    if (kernel_vmm) return VMM_ERR_ALREADY_INIT;

    // ... PMM and slab must be online ...

    // cpu_init hasn't run yet, so cpu_has_feature would say no. Ask CPUID directly
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) write_msr(MSR_EFER, read_msr(MSR_EFER) | EFER_NXE);
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
    // ...

    irq_register(INT_PAGE_FAULT, vmm_fault);
    // ...
    return VMM_OK;
}
```

**What's happening here:**

* NX comes first, using the same `cpuid` check the long mode test did in assembly. We ask for the highest extended leaf, and if `0x80000001` exists, bit 20 of `EDX` says whether the CPU supports NX. If it does, set `NXE` in `IA32_EFER`. From this line on, `vmm_convert_vm_flags` starts setting `PAGE_NO_EXECUTE` on everything that isn't `VM_FLAG_EXEC`. This is the fix from the fun fact earlier.
* The kernel's own `vmm_ctx` comes straight from the PMM. A whole page for a struct of under a hundred bytes is the very waste last chapter spent a whole section complaining about. But there's exactly one of these, it lives forever, and the slab caches for `vmm_ctx` don't exist until a few lines further down. A page, once, is a fine price for not caring about the order. (Yes, the comment's reason is a bit grander than mine.)
* There are no new page tables, since the kernel VMM *adopts* the tables `build_physmap` made at boot. Everything that's already mapped (the kernel, the physmap) stays as it was. The VMM just doesn't have `vm_object`s for any of it. It never needs them, since those mappings never change and never get freed.
* The two slab caches, one for future `vmm_ctx`s, one for every `vm_object` in the system.
* The page fault handler gets registered, and from here on, lazy memory works.

>[!CAUTION]
> Be careful with that `pt_root` line, because it's deceptive. `KERNEL_V2P(getPML4())` looks like it does something clever. It doesn't. `getPML4` reads `cr3` and runs it through `KERNEL_P2V`, and `KERNEL_V2P` undoes that again. The result is just `cr3`.
>
> The round trip only works because the PML4 `build_physmap` made lives in the table pool, inside the kernel range. A process's PML4 comes from the PMM, somewhere past `KEND`, and `KERNEL_P2V` of *that* is an address nothing maps. This is why every other line in the VMM reaches page tables with `PHYSMAP_P2V(pt_root)`, and why this is the last place `getPML4` is safe to use.

Here's the call from `kernel_bootstrap()`:

```c
vmm_status_t vmm_status = vmm_kernel_init(KERNEL_P2V(kmem->kend) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
```

This is the window from earlier, from one page past the kernel up to the last page of the address space. Note that it's `kmem->kend` *after* the PMM's page map got carved out with `kend_grow`, so the window starts past everything the kernel has claimed so far.

>[!NOTE]
> Why `0xFFFFFFFFFFFFF000` and not `0xFFFFFFFFFFFFFFFF`? The window is half open, `[alloc_base, alloc_end)`. An end of `0` (which is what `0xFFFFFFFFFFFFFFFF + 1` wraps to) would break every comparison in the gap finder. We give up the last page of the address space. Not a big loss.

## Debugging and Integrity

Same as the PMM and the slab, the VMM comes with tools for when things smell funny:

* `vmm_dump` logs every object of a VMM to `debug.log`, in address order, with its range and flags.
* `vmm_stats` reports two numbers: how much memory the objects cover, and how much of it is mapped right now. The difference is lazy memory nobody has touched yet.
* `vmm_verify_integrity` walks the tree in order and checks every object: valid magic and red zones, page aligned base and length, inside the window, and not overlapping the object before it. The last check is the big one. Every invariant in this chapter falls apart the moment two objects overlap.

```c
if (prev) {
    uintptr_t prev_end = prev->public.base + prev->public.length;
    if (current->public.base < prev_end) {
        LOGF("[VMM VERIFY] Object %d overlaps with previous (0x%lx < 0x%lx)\n",
             count, current->public.base, prev_end);
        spinlock_release(&vmm->lock, lock_flags);
        return false;
    }
}
```

The test suite runs `vmm_verify_integrity` on the kernel VMM before every single VMM test, and skips the test if it fails. Once the tree is corrupt, no result from it means anything.

## Putting It All Together

This is the slice of `kernel_bootstrap()` that this chapter adds, with everything before it squashed:

```c
// PMM: physical pages (Chapter 7)
pmm_init(kmem->ram, kend_grow(mb, pmm_map_size(kmem->ram)));
pmm_exclude_range(0, kmem->kend);
// ... populate the free lists ...

// Slab: small objects, including every vm_object the VMM will ever make (Chapter 7)
slab_init();

// VMM: adopts the physmap's page tables, turns NX on, and owns everything
// from one page past the kernel to the top of the address space
vmm_status_t vmm_status = vmm_kernel_init(KERNEL_P2V(kmem->kend) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
if(vmm_status != VMM_OK) {
    QEMU_LOG("[VMM] Failed to initialize virtual memory manager", TOTAL_DBG);
    return false;
}
QEMU_LOG("Initialized kernel virtual memory manager", TOTAL_DBG);

// With the VMM online, we can use virtual addresses for everything from now on
```

Each step enables the next:

* The physmap (Chapter 5) lets the VMM read and write any page table at any level, with no special mappings.
* The PMM (Chapter 7) gives the VMM physical pages for new page tables and for backing memory, and `pmm_alloc_exact` gives it exactly as many as it asked for.
* The slab (Chapter 7) gives it `vm_object`s and `vmm_ctx`s without burning a page each.
* The AVL tree keeps every lookup `O(log n)`, even though our allocation pattern is the worst case for a plain tree.
* The interrupt dispatcher (Chapter 6) lets the VMM claim the page faults it can fix and pass on the ones it can't.
* And spinlocks (Chapter 6) keep two allocations from ever finding the same gap.

For the first time since Chapter 5, the kernel can put memory anywhere it wants. It can map a device's registers, uncached, at whatever address the hardware lives at, and it can hand out memory that's read-only, or not executable, or only exists once someone touches it. And it can build a whole new address space, with the kernel already inside it.

What it still can't do is the boring stuff. The VMM still hands out a whole page for a 40 byte request. Next chapter fixes that with the kernel heap, which carves VMM pages into whatever sizes you ask for.

Before the heap, though, the CPU wants a few things from us now that we can allocate memory. It needs a proper GDT built in C, plus the TSS with those IST stacks we kept dodging, and then there's `cpu_init`, the one that was supposed to come before the VMM all along. All of that is Chapter 9.
