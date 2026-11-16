# Chapter 9: The GDT, the CPU and the Kernel Heap

The previous chapter ended with the boss fight won. The VMM is online, it can map anything anywhere with whatever permissions we like, and it even handles its own page faults.

Along the way, this book has left a little trail of promises behind it. Chapter 6 said "the TSS comes later" every single time it mentioned the IST. Chapter 8 admitted that `cpu_init` was supposed to run before the VMM, and quietly never did. Chapter 7 promised that a 4 byte allocation would one day stop eating a whole page. None of these is as big as the VMM, but all three need memory to work with, and until now we had none to give them.

In this chapter, we pay all three back:

1. **The GDT, take two.** The GDT from Chapter 3 was the bare minimum needed to get into long mode. It has no segments for ring 3 and no TSS, while the TSS needs real memory for its stacks.
2. **Getting to know the CPU.** `cpu_init` asks the CPU what it can do, turns on a few protections, and sets up a small per-CPU data area through the `GS` register.
3. **The kernel heap.** `kmalloc` and `kfree`, for any size you like, carved out of pages the VMM hands over.

They run in that order in `kernel_bootstrap()`, right after `vmm_kernel_init`. By the end of this chapter, the memory subsystem from Chapter 7 is finally complete.

## Part 1: The GDT, Take Two

### What's Wrong With the Boot GDT?

Here is the GDT from Chapter 3 again, in all its glory:

```asm
gdt64:
	.quad 0 # null descriptor
gdt64_code_segment = . - gdt64
	.quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) # 64-bit code segment descriptor
```

A null descriptor and a single code segment were all long mode needed. Chapter 3 even set every data segment register to `0` and called it a day. In ring 0, long mode is perfectly happy with that.

Now think about what is coming. Chapter 2 told you the GDT is where privilege levels live: every segment has a DPL, and ring 3 code needs ring 3 segments. A user program has to run with a `CS` whose descriptor says "ring 3", and its stack segment has to say the same. Our GDT has no such descriptors. The day we try to drop into userspace with this table, the CPU will look for a ring 3 code segment, fail to find one, and throw a General Protection fault right in our face.

Then there is the TSS, which Chapter 6 has been dodging. It needs a GDT entry of its own, for which the boot GDT has no room. Worse, that entry has to hold the TSS's *address*, a 64-bit higher half pointer we can only know once the kernel is running. Writing that into `.rodata` by hand, in 32-bit assembly, is simply not happening.

Therefore, the GDT moves to C, in [`gdt.c`](/src/arch/x86_64/cpu/gdt.c), where we can build it properly.

### The New Layout

The new table looks like this:

| Index | Selector | Segment | DPL |
| --- | --- | --- | --- |
| 0 | `0x00` | Null | |
| 1 | `0x08` (`KERNEL_CS`) | Kernel code | 0 |
| 2 | `0x10` (`KERNEL_DS`) | Kernel data | 0 |
| 3 | `0x1B` (`USER_DS`) | User data | 3 |
| 4 | `0x23` (`USER_CS`) | User code | 3 |
| 5, 6 | `0x28` (`TSS_SEL`) | TSS | 0 |

We saw the selector format back in Chapter 2. The index sits in bits 3 and up, with the requested privilege level (RPL) in the bottom two bits, meaning a selector is essentially `index * 8` plus the RPL. Kernel code at index 1 is `1 * 8 = 0x08`. User data at index 3 is `3 * 8 = 0x18`, and because it is meant to be used from ring 3, we add an RPL of `3` to get `0x1B`. User code works the same way, giving `4 * 8 + 3 = 0x23`.

Two things about this table look odd at first.

**User data comes before user code.** Most tutorials put code first. GatOS cannot, because of the `syscall` instruction we will set up in Chapter 11. When a program returns from a syscall, the CPU does not read selectors from anywhere sensible. It takes one base value from a register and *computes* them, with user data at base + 8 and user code at base + 16. That arithmetic only works if data sits right before code. This is the order. It is not up for discussion.

The way *in* plays the same game with a second base value, putting kernel code at base and kernel data at base + 8. That one happens to match the order everybody uses anyway. Only the way out forces the user pair to flip.

**The TSS takes two slots.** We will see why in a moment.

### Building an Entry

A GDT entry has the same scattered layout you saw with the IDT in Chapter 6, and for the same reason: 30 years of backwards compatibility.

```c
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;
```

`gdt_set_entry` takes a base, a limit, an access byte and a flags nibble, and scatters them across those fields:

```c
static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    gdt.entries[index].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt.entries[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt.entries[index].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt.entries[index].access = access;
    gdt.entries[index].flags_limit_high = (uint8_t)(((limit >> 16) & 0x0F) | (flags << 4));
    gdt.entries[index].base_high = (uint8_t)((base >> 24) & 0xFF);
}
```

Here is the kernel code segment built with it:

```c
gdt_set_entry(1, 0, 0xFFFFFFFF, GDT_PRESENT | GDT_DPL_0 | GDT_SYSTEM | GDT_EXECUTABLE | GDT_READ_WRITE, GDT_LONG_MODE);
```

It is worth lining this up against Chapter 3's magic `.quad`, because the two describe exactly the same segment. The access byte is byte 5 of the entry, making it bits 40 to 47 of the whole 64-bit value. The flags nibble is the top half of byte 6, covering bits 52 to 55. Putting the two side by side:

| Chapter 3 bit | Where it lands | `gdt.c` name |
| --- | --- | --- |
| 43 | access bit 3 | `GDT_EXECUTABLE` |
| 44 | access bit 4 | `GDT_SYSTEM` (code or data, as opposed to a system descriptor) |
| 47 | access bit 7 | `GDT_PRESENT` |
| 53 | flags bit 1 | `GDT_LONG_MODE` |

They are the same four bits. The C version just adds `GDT_READ_WRITE` (bit 41, meaning the code may be read as well as executed) and a limit of all ones. In long mode the CPU ignores the base and limit of code and data segments anyway (Chapter 3 told you that), so those are only there to look tidy.

The other three segments are variations on the same theme. Kernel data drops `GDT_EXECUTABLE` and the long mode flag. User data and user code are copies of kernel data and kernel code with `GDT_DPL_3` instead of `GDT_DPL_0`. As far as segments are concerned, that one field is the entire difference between "kernel" and "user".

### The TSS

In Chapter 6, we dropped the task gate from the list of IDT gates because long mode does not do hardware task switching anymore. The TSS, or Task State Segment, comes from that same old machinery. In 32-bit mode, it held a whole saved task, with every register and every segment. In long mode, almost all of that is gone. What is left is a small table of **stack pointers**:

```c
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;
```

The whole thing is 104 bytes. Let's go through what the CPU uses each part for.

#### rsp0

Consider a simple scenario. A user program is running in ring 3, on its own stack somewhere in the lower half, when a timer interrupt fires. The CPU is about to push the interrupt frame from Chapter 6 (`ss`, `rsp`, `rflags`, `cs`, `rip`) and jump into the kernel.

The question is where that frame should go. Pushing it onto the user's stack would be a terrible idea, for two reasons:

* The program controls `rsp`. If it pointed it at a kernel address, the CPU would happily write the frame over kernel memory. It could also point it at nothing at all, and now we would fault while delivering an interrupt.
* Even with a perfectly good user stack, the kernel would then run its interrupt handler on memory the program can read. Every register the handler saves would be sitting there for the program to inspect after we return.

The CPU therefore does something else. Whenever an interrupt takes it from ring 3 to ring 0, it first loads `rsp` from `rsp0` in the TSS, and pushes the frame there instead, onto a stack the kernel picked, in kernel memory. (`rsp1` and `rsp2` are the same thing for rings 1 and 2, which nobody uses.)

Interrupts that arrive while we are *already* in ring 0 do not switch stacks at all. They push onto whatever kernel stack we happen to be on, as they have since Chapter 6.

#### ist

This is the IST from Chapter 6, finally. It holds seven more stack pointers, `ist[0]` to `ist[6]`. An IDT entry with IST index `n` makes the CPU switch to `ist[n - 1]` *every* time that vector fires, no matter which ring it came from. This is the "known good stack" Chapter 6 wanted for the double fault: even if the kernel stack is completely blown, the CPU has somewhere safe to push the frame.

It also explains why Chapter 6 had to leave every IST index at `0`. The IDT only holds the index, while the stacks themselves live here. Until this struct existed, an IST index pointed at nothing.

#### iopb_offset

This one is a bit of an odd duck. The port I/O instructions from Chapter 4, `inb` and `outb`, are privileged, but the TSS can carry a bitmap that lets ring 3 use specific ports anyway. `iopb_offset` says where in the TSS that bitmap starts. GatOS sets it to `sizeof(tss_t)`, past the end of the TSS. No bitmap means no ports for ring 3. A user program that tries `outb` gets a General Protection fault, which is what we want.

### The TSS Descriptor

The TSS needs a GDT entry for `ltr` (we will get to it) to find it by. Look at the base field of a normal entry, though. It adds up to 16 + 8 + 8 = 32 bits, while our TSS lives at a higher half address that needs all 64.

In long mode, a TSS descriptor is therefore 16 bytes long and takes up two GDT slots. The first slot is a normal entry holding the low 32 bits of the base, while the second holds the high 32 bits:

```c
static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
    gdt_set_entry(index, (uint32_t)base, limit, 0x89, 0x00);

    gdt_entry_t* extra = &gdt.entries[index + 1];
    uint32_t* extra_raw = (uint32_t*)extra;
    extra_raw[0] = (uint32_t)(base >> 32);
    extra_raw[1] = 0;
}
```

The access byte `0x89` means present (`0x80`), DPL 0, and type `9`, which the CPU reads as "available 64-bit TSS". Keep in mind that `GDT_SYSTEM` is *not* set here. A TSS is a *system* descriptor, a different kind of entry from code and data segments. That bit is how the CPU tells the two kinds apart. The limit is `sizeof(tss_t) - 1`, or 103.

This is also why `GDT_ENTRIES` is 7 and not 6.

### Stacks for the TSS

A table of stack pointers needs stacks to point at. This is the part that had to wait for the allocators:

```c
// 16kb for a kernel stack to save the user state and execute a kernel handler when necessary
uint64_t kstack_phys;
if (pmm_alloc(16384, &kstack_phys) == PMM_OK) {
    tss.rsp0 = PHYSMAP_P2V(kstack_phys + 16384);
    LOGF("[GDT] Primary kernel stack allocated at 0x%lx (virt)\n", tss.rsp0);
} else {
    LOGF("[GDT] ERROR: Failed to allocate kernel stack for TSS!\n");
}

// double fault stack, specific for critical exception
uint64_t df_stack_phys;
if (pmm_alloc(KERNEL_STACK_SIZE, &df_stack_phys) == PMM_OK) {
    tss.ist[0] = PHYSMAP_P2V(df_stack_phys + KERNEL_STACK_SIZE);
    LOGF("[GDT] Double Fault IST stack allocated at 0x%lx\n", tss.ist[0]);
}

// page fault stack, same as above
uint64_t pf_stack_phys;
if (pmm_alloc(KERNEL_STACK_SIZE, &pf_stack_phys) == PMM_OK) {
    tss.ist[1] = PHYSMAP_P2V(pf_stack_phys + KERNEL_STACK_SIZE);
    LOGF("[GDT] Page Fault IST stack allocated at 0x%lx\n", tss.ist[1]);
}
```

**What's happening here:**

* We allocate three 16 KiB stacks (`KERNEL_STACK_SIZE` is 16 KiB too), straight from the PMM.
* Careful with the `+ 16384`. Stacks on x86 grow *down*: a push decrements `rsp` first, and only then writes. The CPU therefore needs the address one past the *top* of the stack, not its start. If you hand it the start, the very first push writes below your allocation, into whatever happens to live there.
* We reach the stacks through the physmap, not through the VMM.

That last point is on purpose. These stacks only get used once something has already gone wrong, like a page fault, a double fault, or an interrupt arriving from a user program. A stack from `vmm_alloc` would be fine in theory, but a physmap address is as boring as it gets. It is always mapped and never lazy, meaning it can never page fault while the CPU is pushing a fault frame onto it. If there is one property you want from the stack your page fault handler runs on, it is that one.

>[!WARNING]
> The flip side is that these stacks have no guard pages. A guard page is an unmapped page placed right below a stack, so that an overflow page faults instead of quietly scribbling over whatever lives next in memory. Since physmap memory is mapped all the way through, nothing catches an overflow here. 16 KiB is plenty for a handler that only panics, but it is a sharp edge worth knowing about.

`rsp0` will not stay this way for long, either. Once there are threads (Chapter 14), every thread gets its own kernel stack. The scheduler then points `rsp0` at the current thread's stack on every switch, using `tss_set_rsp0`. This first stack is just what is there until then.

### Loading It

With the table and the TSS ready, we have two kinds of registers to update. `GDTR` has to point at the new table, while every segment register needs a selector from it:

```c
gdt_ptr_t ptr;
ptr.limit = sizeof(gdt) - 1;
ptr.base = (uintptr_t)&gdt;

/*
 * Note from the author: cs can't be set with a mov, so we far return. Push KERNEL_CS,
 * push the address of the next instruction (1f), and retfq pops both into cs and rip.
 */

__asm__ volatile (
    "lgdt %0\n\t"
    "mov %1, %%ds\n\t"
    "mov %1, %%es\n\t"
    "mov %1, %%fs\n\t"
    "mov %1, %%gs\n\t"
    "mov %1, %%ss\n\t"
    "push %2\n\t"
    "lea 1f(%%rip), %%rax\n\t"
    "push %%rax\n\t"
    "retfq\n\t"
    "1:\n\t"
    :
    : "m"(ptr), "r"((uint64_t)KERNEL_DS), "i"(KERNEL_CS)
    : "rax", "memory"
);

__asm__ volatile("ltr %%ax" :: "a"((uint16_t)TSS_SEL));
```

`lgdt` is the same instruction we used in Chapter 3, now pointing at the C table. The old table is still sitting in memory after this, but nothing points at it anymore. The data segment registers can all be loaded with a plain `mov`.

`CS` cannot, since there is no `mov cs, ...` on x86. The only way to change `CS` is to jump somewhere *with* a new segment, using a "far" jump, call or return. In Chapter 3, the far jump into 64-bit code did this for us. Here, the easiest trick is a far *return*. `retfq` pops two values off the stack, a return address into `rip` and a selector into `cs`. We push `KERNEL_CS`, push the address of the very next instruction (label `1:`), and "return" to it. Execution carries on exactly one line down, now with the new `CS` loaded. It is basically a return to nowhere.

Finally, `ltr` (load task register) points the CPU at the TSS by its selector, `0x28`. From now on, every ring 3 to ring 0 interrupt switches to `rsp0`.

`ltr` does something sneaky while it is at it, too: it *writes* to the GDT. It flips the descriptor's type from `9` (available) to `11` (busy). If we ran `ltr` a second time with the same selector, the CPU would refuse with a General Protection fault, because that TSS is already taken. This is another leftover from hardware task switching, where "busy" meant "this task is running right now". For us, it simply means `gdt_init` runs exactly once.

With the TSS loaded, the IST indices from Chapter 6 can finally be turned on:

```c
// The IST stacks only exist now, so this is the earliest the IDT may point at them
if (tss.ist[0]) idt_set_ist(INT_DOUBLE_FAULT, 1);
if (tss.ist[1]) idt_set_ist(INT_PAGE_FAULT, 2);
```

Index `1` is `ist[0]`, and index `2` is `ist[1]`. If either stack failed to allocate, its vector simply stays on the current stack, as before. A missing safety net is better than one pointing at address `0`.

>[!TIP]
> Here is a question worth sitting with. If IST stacks are so safe, why not put *every* exception on one?

The answer is that an IST stack has no idea whether it is already in use. Every time its vector fires, the CPU loads `rsp` from `ist[n]`, the very top of that stack, without asking any questions. Compare that with `rsp0`, which is only used when coming *in* from ring 3. An interrupt that arrives while the kernel is already running just pushes below whatever is on the current stack, like any nested function call. Nothing gets lost.

Now imagine a page fault *inside* the page fault handler. The CPU switches to `ist[1]` again and pushes the new frame at the top, right over the first fault's frame and saved registers. They are gone. When the handlers unwind, the first one returns using whatever the second one left behind.

The rule for an IST handler, then, is that it must never raise its own vector. For the double fault, that is easy, since its handler only panics. For the page fault, it is the reason `vmm_handle_fault` from Chapter 8 only ever touches the PMM, the slab and the page tables, all of which it reaches through the physmap. Nothing it touches is lazy, so nothing it touches can fault. If it is ever given lazy memory of its own, this is the bug you will get.

>[!CAUTION]
> Be careful with that `mov %1, %%gs`, because it does more than it looks like.
>
> In long mode, `GS` has a hidden 64-bit *base* address that the CPU adds to every `gs:` memory access. It is normally set through an MSR, and `cpu_init` is about to do just that. But loading a selector into `GS` with `mov` overwrites that hidden base with the base from the GDT entry, which is `0`.
>
> The order is therefore fixed: `gdt_init` first, `cpu_init` second. If you swap them, `cpu_init` sets up the per-CPU area, `gdt_init` silently zeroes the pointer to it, and the first `gs:[0]` reads address `0`. GatOS's own test kernel had them in the wrong order for a while, and nobody noticed until the first test that made a real syscall from ring 3.

## Part 2: Getting to Know the CPU

### Asking Questions with CPUID

We have used `cpuid` three times so far (in Chapter 3's assembly checks, for the PAT in Chapter 5, and for NX in Chapter 8) without ever properly sitting down with it. It is the CPU's self-description instruction. We put a "leaf" number in `EAX` (and sometimes a sub-leaf in `ECX`), run `cpuid`, and `EAX`, `EBX`, `ECX` and `EDX` come back filled with answers. Which answers we get depends entirely on the leaf.

`cpu_init` in [`cpu.c`](/src/arch/x86_64/cpu/cpu.c) walks through a handful of them:

| Leaf | What GatOS reads from it |
| --- | --- |
| `0` | The highest basic leaf, and the vendor string |
| `1` | Family, model, stepping, and the classic feature bits (SSE, AVX, PAE...) |
| `7` | Newer feature bits (AVX2, SMEP, SMAP) |
| `0xB` | Topology, how many logical processors there are |
| `0x80000000` | The highest *extended* leaf |
| `0x80000001` | Extended feature bits (NX, 64-bit support) |
| `0x80000002` to `0x80000004` | The brand string, the CPU's marketing name |

Leaf `0` has a fun quirk. The vendor string is 12 characters long, three registers' worth. You would expect them to come back in order. They do not:

```c
// 4b segments
cpuid(0, 0, &a, &b, &c, &d);
*((uint32_t*)&cpuinfo.vendor[0]) = b;
*((uint32_t*)&cpuinfo.vendor[4]) = d;
*((uint32_t*)&cpuinfo.vendor[8]) = c;
```

On Intel, `EBX` holds `"Genu"`, `EDX` holds `"ineI"` and `ECX` holds `"ntel"`. Stitching them together in B, D, C order gives `"GenuineIntel"`, and on AMD, the same dance spells `"AuthenticAMD"`. As for why that order, nobody knows. Every OS in the world has the same three lines in it anyway.

`EAX` from leaf `0` is the highest basic leaf this CPU supports. Anything above it returns garbage, which is why `cpu_init` remembers it (`max_basic`) and checks it before touching leaf `7` or `0xB`. The extended leaves (`0x8000xxxx`) work the same way, with their own maximum in leaf `0x80000000`. It is the exact check Chapter 3's long mode test did in assembly.

### Has It versus Is It On

Every feature bit `cpu_init` finds goes into a single 64-bit mask in `cpuinfo`:

```c
if (d & (1 << 26)) cpuinfo.features |= CF_SSE2;
if (c & (1 << 0)) cpuinfo.features |= CF_SSE3;
// ...
if (b & (1 << 5)) cpuinfo.features |= CF_AVX2;
if (b & (1 << 7)) cpuinfo.features |= CF_SMEP;
if (b & (1 << 20)) cpuinfo.features |= CF_SMAP;
```

From there, we can ask three different questions about any feature:

* `cpu_has_feature`: does the CPU *support* it? This is just a lookup in `cpuinfo.features`.
* `cpu_enable_feature`: turn it on, if supported. Each feature has its own switch somewhere in a control register or an MSR.
* `cpu_is_feature_enabled`: is it on *right now*? This one ignores `cpuinfo` entirely and reads the actual control register.

The difference between the first and the last question caused the NX bug in Chapter 8. `cpu_has_feature` reads a table that `cpu_init` fills, meaning anything that asks before `cpu_init` has run gets "no" for everything. `cpu_is_feature_enabled` reads the hardware and is always right. This is why `vmm_convert_vm_flags` uses it, and why `vmm_kernel_init` now skips both and asks `cpuid` directly.

>[!NOTE]
> Fun fact! While writing this chapter, I found two more of these bits were wrong. `CF_AVX2` existed (the dashboard even had a label for it), but nothing ever set it. Every CPU in the world "didn't have" AVX2. The core count was also off, reading the wrong register in one path and the wrong topology level in the other. Leaf `0xB` describes the CPU level by level (threads in a core, then cores in a package). Because the code only read level 0, a 4-core CPU with 2 threads per core reported "2". It now walks the levels until it finds the core level, and reports all 4 on QEMU with `-smp 4,cores=2,threads=2`.

### What `cpu_init` Turns On

With the inventory done, `cpu_init` switches a few things on:

```c
// SSE is enabled at boot time (it's a requirement for GatOS to work)
// but it's good to do it here too
if (cpu_has_feature(CF_SSE)) {
    cpu_enable_feature(CF_SSE);
}

if (cpu_has_feature(CF_AVX)) {
    cpu_enable_feature(CF_AVX);
}

if (cpu_has_feature(CF_SMEP)) {
    cpu_enable_feature(CF_SMEP);
}

if (cpu_has_feature(CF_SMAP)) {
    cpu_enable_feature(CF_SMAP);
}
```

**SSE** was already turned on by Chapter 3's `check_SSE`, using the same `CR0` and `CR4` bits. Doing it twice costs nothing.

**AVX** is SSE's bigger sibling, with 256-bit `ymm` registers instead of 128-bit `xmm` ones. Bigger registers mean more state for the OS to save and restore whenever it switches between programs. Because of that, the CPU will not run a single AVX instruction until the OS explicitly says "I know about these". That promise takes two steps. First, we set `CR4.OSXSAVE` (bit 18), which unlocks a special register called `XCR0`. Then, we set the bits in `XCR0` for each register set the OS promises to handle: x87, SSE and AVX. Keeping that promise takes more than two bits, as we will see right after SMEP and SMAP.

**SMEP and SMAP** are the interesting ones. Both revolve around the `PAGE_USER` bit from Chapters 5 and 8, and both protect the kernel from *itself*.

To see why that matters, consider how a kernel bug turns into an exploit. Say there is a corrupted function pointer in the kernel, pointing wherever an attacker wants. The classic trick is to point it at a page in their own user program, full of their own code. When the kernel calls through the pointer, attacker code runs in ring 0. There is also a sneakier variant, where a syscall takes a pointer from a user program and reads from it without checking. The program passes a pointer into its own memory, and changes what is there from another thread while the kernel is still reading it.

* **SMEP** (Supervisor Mode Execution Prevention, `CR4` bit 20) makes the CPU refuse to *execute* any `PAGE_USER` page while in ring 0. The first trick now page faults instead of running.
* **SMAP** (Supervisor Mode Access Prevention, `CR4` bit 21) makes it refuse to even *read or write* `PAGE_USER` pages from ring 0.

SMAP sounds like it breaks syscalls completely, since something like `read()` has to copy data into a user buffer. For that, there is an escape hatch: an `AC` flag that temporarily allows access, flipped by the `stac` and `clac` instructions:

```c
static inline void smap_allow(void) {
    if (cpu_is_feature_enabled(CF_SMAP))
        __asm__ volatile("stac" ::: "memory");
}

static inline void smap_deny(void) {
    if (cpu_is_feature_enabled(CF_SMAP))
        __asm__ volatile("clac" ::: "memory");
}
```

The kernel opens the hatch only for the few instructions that copy to or from user memory, after checking the buffer with `vmm_check_buffer` from Chapter 8, and closes it again right after. Every other user memory access from the kernel is a bug, and with SMAP on, it becomes a page fault instead of a silent read. We will see the copy helpers when we build syscalls in Chapter 11.

Of course, if a single flag switches SMAP off, a user program could try to set that flag itself. It can. In ring 3, `AC` is the "alignment check" flag, and any program is allowed to set it with `popf`. A program that sets it and then makes a syscall would have the entire syscall run with SMAP switched off. SMAP exists to stop that trick.

Every way into the kernel from ring 3 therefore has to clear `AC` on arrival. `syscall` comes with a mask of flags it clears on entry (we will set it up in Chapter 11), and `AC` is part of it. Interrupts do not touch `AC` at all, so the interrupt stub from Chapter 6 clears it by hand, right after the `swapgs` on the way in from ring 3:

```asm
test qword ptr [rsp + 24], 3
jz .no_swapgs
swapgs

// Ring 3 can set AC with popf, and AC=1 turns SMAP off. Interrupts don't clear it for us
pushfq
and qword ptr [rsp], ~0x40000
popfq
.no_swapgs:
```

Here, `pushfq` copies `RFLAGS` onto the stack, the `and` clears bit 18 in that copy, and `popfq` loads it back. The program's own `RFLAGS` is safe in the interrupt frame the CPU pushed earlier, and `iretq` hands it back untouched on the way out.

And yes, both of these were missing until I sat down to explain SMAP. The syscall mask only cleared two flags, and the stub did not clear anything at all.

### Keeping the AVX Promise

Back in Chapter 1, we banned kernel code from touching the floating point registers, because they belong to whatever program the kernel interrupted. That rule stops the kernel from *corrupting* them. Once there is more than one program, though, something also has to *swap* them, saving program A's registers when it stops running and loading program B's when it starts. That is the scheduler's job. We will build it in Chapter 14. What matters here is the instruction it uses to do the swapping.

For x87 and SSE, that instruction is `fxsave`. It dumps the whole floating point state into a 512 byte area in one go, and `fxrstor` loads it back. The 16 `xmm` registers take up 256 of those bytes.

Setting bit 2 of `XCR0` was the OS saying "I know about the 256-bit `ymm` registers, and I will keep them safe". The low half of each `ymm` register *is* the matching `xmm` register, which `fxsave` already covers. The top half, however, is not anywhere in `fxsave`'s 512 bytes. That layout was set in stone years before AVX existed, and nobody can grow it without breaking every OS that already sets aside exactly 512 bytes.

>[!NOTE]
> Fun fact! GatOS made that promise and then kept saving with `fxsave`. Say program A has a value in `ymm0`. The scheduler switches to B, which uses `ymm0` too, then back to A. A gets its low half back from its save area, but B's top half is still sitting in the register. A's number is now half its own and half somebody else's, and B has just leaked data into A. Nothing in GatOS or Appa is compiled with AVX yet, so it never actually happened, but it was waiting for the first program that did.

The fix is the instruction pair that came with AVX in the first place, `xsave` and `xrstor`. They work like `fxsave` and `fxrstor`, except that the save area can grow:

```
offset 0    [ legacy area, 512 bytes ]   same layout as fxsave: x87, SSE, xmm0-15
offset 512  [ XSAVE header, 64 bytes ]   which parts were saved
offset 576  [ AVX, 256 bytes         ]   top halves of ymm0-15
            = 832 bytes
```

Each part of the area is called a *component*. Components line up with the bits of `XCR0`: x87 is bit 0, SSE is bit 1 and AVX is bit 2. `xsave` takes a bitmask in `EDX:EAX` saying which components to save, before the CPU ANDs it with `XCR0`. Passing all ones therefore means "everything the OS turned on". The header then records which components actually got written, telling `xrstor` what to load back.

GatOS wraps both instruction pairs in two helpers in `cpu.c`. The scheduler never has to care which one is in use:

```c
void fpu_save(void* area)
{
    if (use_xsave)
        __asm__ volatile("xsave (%0)" :: "r"(area), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    else
        __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}
```

`fpu_restore` is the same thing with `xrstor` and `fxrstor`. `use_xsave` gets set by `cpu_enable_feature(CF_AVX)`, right after it writes `XCR0`, but only once the function has double checked the size:

```c
// With XCR0 set, CPUID 0xD reports how big an xsave of the enabled state is
uint32_t a, b, c, d;
cpuid(0x0D, 0, &a, &b, &c, &d);
if (b > FPU_AREA_SIZE) panic("FPU state doesn't fit FPU_AREA_SIZE");
use_xsave = true;
```

With x87, SSE and AVX turned on, `EBX` comes back as 832. Since that matches both the diagram and `FPU_AREA_SIZE` exactly, the check looks pointless. It is there for whoever turns on the next component, say AVX-512, which grows the area past 2.5 KiB. Thanks to this check, they will find out from a panic on the very first boot, instead of from threads quietly scribbling over each other's memory.

There is one more catch. `xsave` insists on a save area aligned to 64 bytes, and raises a General Protection fault otherwise (`fxsave` was happy with 16). Each thread keeps its area inside its thread struct, allocated with `kmalloc`. As we will see in Part 3, `kmalloc` only promises 16 byte alignment. The struct therefore carries a slightly oversized buffer, plus a pointer to the first 64 byte boundary inside it:

```c
uint8_t* fpu;
uint8_t fpu_raw[FPU_AREA_SIZE + FPU_AREA_ALIGN - 16] __attribute__((aligned(16)));
```

```c
thread->fpu = (uint8_t*)(((uintptr_t)thread->fpu_raw + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
```

>[!TIP]
> Why `+ 64 - 16`, and not a full `+ 64`? Think about where `fpu_raw` can start. It is 16 byte aligned, meaning its address modulo 64 is 0, 16, 32 or 48. The worst case is 16, which sits 48 bytes short of the next boundary. 48 bytes of padding is therefore always enough. A 49th byte would never get used.

A brand new thread's area is all zeros, apart from two control words whose reset values are not zero (one for x87 and one for SSE, both meaning "mask every floating point exception"). With `xsave`, an all-zero header means "every component is in its reset state", so the thread's first `xrstor` hands it a clean slate. Since `cpu_init` runs long before the first thread exists, every area in the system is always written and read by the same instruction. When the scheduler actually calls these helpers (lazily, only once a thread touches the FPU) is a story for Chapter 14.

>[!CAUTION]
> Even *asking* whether AVX is on was dangerous. For AVX, `cpu_is_feature_enabled` has to read `XCR0`, using an instruction called `xgetbv`. But `XCR0` only exists once `CR4.OSXSAVE` is set, and on a CPU where it is not, `xgetbv` is an invalid opcode. The question "is AVX on?" therefore crashed the kernel on the very CPUs where the answer was "no". It sat there unnoticed because nothing asked until the FPU tests for this chapter did. The fixed version checks `CR4` before it goes anywhere near `XCR0`:
>
> ```c
> case CF_AVX:
> case CF_AVX2:
>     // xgetbv is #UD until CR4.OSXSAVE is set, so that has to be checked first
>     cr4 = read_cr4();
>     if (!(cr4 & (1 << 18))) return false;
>     xcr0 = read_xcr0();
>     return (xcr0 & (1 << 0)) && (xcr0 & (1 << 1)) && (xcr0 & (1 << 2));
> ```

### The GS Base and `cpu_local`

The last thing `cpu_init` sets up is easy to miss:

```c
// GS_BASE -> cpu_local in ring 0, KERNEL_GS_BASE holds user GS on entry
write_msr(MSR_GS_BASE, (uint64_t)&cpu_local);
write_msr(MSR_KERNEL_GS_BASE, 0);
```

To see why it is needed, we have to jump ahead a little, to Chapter 11. A syscall is a user program asking the kernel to do something for it, using the `syscall` instruction. It is faster than an interrupt, and part of how it gets faster is by doing *less*. In particular, it does **not** switch stacks: interrupts use `rsp0`, but `syscall` ignores it. The first instruction of the kernel's syscall handler runs in ring 0, with `rsp` still pointing at the user's stack.

This is a problem, for every reason we just went through with `rsp0`. The handler needs to switch to a kernel stack before it touches the stack at all, and to do that, it has to load the kernel stack's address from *somewhere*. It cannot use any register, because they all still hold the user program's values. It cannot use the stack either, because the stack is the very thing we do not trust.

The way out is `GS`, with the hidden base from the caution earlier. Any instruction can read memory relative to that base with a `gs:` prefix, without needing a free register at all. If `GS` points at a little struct that holds the kernel stack's address, the handler can switch stacks in just two instructions:

```asm
mov gs:[8], rsp     // stash the user rsp in the struct
mov rsp, gs:[0]     // load the kernel stack from the struct
```

That little struct is `cpu_local`:

```c
// CPU-local data structure for GS base
typedef struct {
    uint64_t kernel_stack; // offset 0
    uint64_t user_stack;   // offset 8
} __attribute__((packed)) cpu_local_t;
```

It is `packed` because the assembly above has the offsets `0` and `8` hardcoded. The struct had better agree with it.

There is one complication. A user program may want `GS` for itself (C libraries often use it, or its twin `FS`, for thread-local storage). There are therefore two GS bases, both in MSRs. `MSR_GS_BASE` is the active one, while `MSR_KERNEL_GS_BASE` is a spare. The `swapgs` instruction from Chapter 6 swaps the two, giving us two states:

| | `MSR_GS_BASE` (active) | `MSR_KERNEL_GS_BASE` (spare) |
| --- | --- | --- |
| Running in ring 0 | `&cpu_local` | the user's GS |
| Running in ring 3 | the user's GS | `&cpu_local` |

Every entry into the kernel from ring 3 does a `swapgs`, and every exit back to ring 3 does another one. The `test` and `swapgs` at the top of Chapter 6's interrupt stub handle this. Since we are in ring 0 right now, `cpu_init` puts `cpu_local` in the active slot and `0` in the spare.

>[!NOTE]
> Why "CPU local"? GatOS runs on one core, with one `cpu_local`. A global variable would do just fine. On a multicore kernel, though, every core runs its own syscalls, on its own kernel stack, at the same time. Each core gets its own copy of this struct, plus its own `GS_BASE` pointing at it, while the handler code stays identical on every core. Pretty much every x86-64 kernel out there uses `GS` this way.

### Idling

`cpu_init` ends with `cpu_idle_init`, and it deserves a quick mention. When there is nothing to do, the CPU should sleep instead of spinning. `hlt` sleeps until the next interrupt. Most modern CPUs also have `monitor` and `mwait`, which can sleep *deeper* (in lower power states, called C-states) and can also wake up when a specific memory address gets written, on top of interrupts. `cpu_idle_init` checks for `mwait` in CPUID leaf 1, then picks the deepest C-state leaf 5 says is available. `cpu_idle_on` uses it, or falls back to `hlt` on CPUs that lack it. The idle thread in Chapter 14 is its main customer, and Chapter 10 will add one important condition to it.

## Part 3: The Kernel Heap

### Why Another Allocator?

Let's count what we have so far. The PMM hands out pages. The VMM hands out pages too, wherever we want them and with whatever flags we ask for. The slab hands out small objects, but only of sizes we registered up front, and only 16 kinds of them (`SLAB_MAX_CACHES`).

Now try to allocate a string whose length you only know at runtime. Or an array of `n` entries. Or a copy of a firmware table you just measured. None of those fit any of our allocators. The slab wants a fixed size per cache, while a page is either too big or too small.

What we want is the thing every C programmer already knows, `malloc(n)` for any `n`. This is the heap, the last layer of the memory subsystem from Chapter 7. In the kernel, it is called `kmalloc`. It lives in [`heap.c`](/src/kernel/memory/heap.c).

### The Core Idea: Arenas and Blocks

The idea is a classic, from the first volume of Donald Knuth's *The Art of Computer Programming*, back in 1968:

1. Ask the VMM for a big chunk of memory. GatOS calls that chunk an **arena**.
2. Treat the whole arena as one big free **block**.
3. When someone asks for `n` bytes, find a free block that is big enough, cut off the part you need, and leave the rest free.
4. When they give it back, mark the block free again. If its neighbors are free too, glue them together, so that free memory does not crumble into tiny useless pieces.

To make this work, every block carries a small header in front of the memory it hands out, plus a footer behind it:

```
arena:  [hdr| used data |ftr][hdr|     free       |ftr][hdr| used |ftr] ...
```

That header and footer pair is called a **boundary tag**, because the two sit on both boundaries of every block. Here they are:

```c
struct blk_hdr {
    uint32_t magic;
    uint32_t red_zone_pre;
    size_t size;
    size_t total_size;
    arena_t* arena;
    struct blk_hdr* next_free;
    struct blk_hdr* prev_free;
    uint32_t red_zone_post;
} __attribute__((aligned(16)));

typedef struct {
    uint32_t red_zone_pre;
    blk_hdr_t* header;
    uint32_t magic;
    uint32_t red_zone_post;
} __attribute__((aligned(16))) blk_foot_t;
```

**What's happening here:**

* `size` is the usable part of the block, meaning what the caller gets. `total_size` is the whole block, header and footer included. You will see both all over the code, and mixing them up is the classic heap bug.
* `magic` says whether the block is in use (`0xABADCAFE`) or free (`0xA110CA7E`). The footer repeats it.
* The red zones (`0x8BADF00D`) are the same idea as in the slab and the VMM. A write that runs off the end of one block smashes the next block's red zone, where we notice it.
* `next_free` and `prev_free` link free blocks into lists. Only free blocks use them.
* The footer's `header` points back at its own header. Keep that one in mind, because the footer exists for it.

With a 64 byte header and a 32 byte footer, every block costs 96 bytes of bookkeeping. On top of that, the smallest block is 32 bytes (`MIN_BLOCK_SIZE`), and every size is rounded up to a multiple of 16. This means `kmalloc(1)` costs 128 bytes. That is a lot of overhead for a single byte. We are not going to pretend otherwise. Small objects you allocate by the thousand belong in the slab. The heap is for everything the slab cannot do.

>[!NOTE]
> Why 16? It is the alignment C's `malloc` promises (`max_align_t` on x86-64), and SSE instructions also want it for 16 byte loads and stores. The arena starts on a page, the header is 64 bytes, and every size is a multiple of 16. As a result, every pointer `kmalloc` returns is 16 byte aligned without any extra effort.

### Walking Between Blocks

This is the part that makes boundary tags clever. Blocks do not need a list to find their neighbors, because their sizes are enough.

Going *forward* is easy, since the next block starts right where this one ends:

```c
uintptr_t next_addr = (uintptr_t)block + block->total_size;
```

Going *backward* is the problem. We know where our block starts, but not where the previous one starts, because we do not know how big it is. Its header is somewhere to our left, at a distance we cannot compute.

Its footer, though, is right next to us. The previous block's footer always ends exactly where our header begins. That footer knows where its own header is:

```c
static blk_hdr_t* prev_block(
    blk_hdr_t* block) {
    if (!block || !block->arena) return NULL;

    uintptr_t prev_footer_addr = (uintptr_t)block - sizeof(blk_foot_t);

    if (prev_footer_addr < block->arena->start) {
        return NULL;
    }

    blk_foot_t* prev_footer = (blk_foot_t*)prev_footer_addr;

    // quick sanity check on red zones in footer
    if (prev_footer->red_zone_pre != BLOCK_RED_ZONE ||
        prev_footer->red_zone_post != BLOCK_RED_ZONE) {
        return NULL;
    }

    return prev_footer->header;
}
```

We step back 32 bytes, read the footer, and follow its pointer. That is all there is to it. If stepping back took us past the start of the arena, we were the first block. There is nobody to our left.

Both directions are `O(1)`. This matters because freeing has to look at both neighbors, every single time.

### Arenas

An arena is one chunk of memory from the VMM, with a small struct describing it:

```c
static arena_t* arena_get(size_t size) {
    void* mem;
    if (slab_alloc(arena_cache, &mem) != SLAB_OK) {
        LOGF("[HEAP] Failed to allocate arena structure\n");
        return NULL;
    }

    arena_t* arena = (arena_t*)mem;
    size = align_up(size, PAGE_SIZE);

    void* region = NULL;
    vmm_status_t status = vmm_alloc(NULL, size, VM_FLAG_WRITE, NULL, &region);
    // ...
    arena->start = (uintptr_t)region;
    arena->end = arena->start + size;
    arena->size = size;
    return arena;
}
```

Here, the struct comes from a slab cache called `arena_t`, while the memory comes from the kernel VMM with just `VM_FLAG_WRITE`. This is the first row of the "who asks for what" table from Chapter 8: plain kernel RAM, writable, not executable, not lazy.

The struct lives in a separate cache instead of at the start of the arena itself. That way, the arena holds nothing but blocks, from its very first byte to its very last. Every walk you will see in this part can start at `arena->start` and expect a header there, without a special case for the first block. As a bonus, the arena's own bookkeeping sits on a different page, out of reach of a block that overflows.

`arena_add` then turns the fresh arena into one big free block. Let's put numbers on it with the smallest arena, `MIN_ARENA_SIZE`, which is 64 KiB:

* `total_size` = 65,536 bytes, the whole arena.
* `size` = 65,536 - 64 (header) - 32 (footer) = 65,440 bytes usable.

The header goes at the very start of the arena and the footer at the very end. The block itself goes onto a free list. The heap also keeps a list of all its arenas, to find them again later.

### Finding a Free Block

When `kmalloc` is called, we need a free block that is big enough. The naive way is to keep one list of all free blocks and scan it from the front until something fits. That costs `O(n)` in the number of free blocks. A busy heap has plenty of them.

GatOS keeps 16 lists instead, called **bins**, and each one only holds free blocks of a certain size range:

```c
static inline uint32_t bin_index(size_t size) {
    if (size < HEAP_MIN_ALIGN) size = HEAP_MIN_ALIGN;
    uint32_t i = (uint32_t)(63 - __builtin_clzll((uint64_t)(size >> 4)));
    return i < HEAP_BIN_COUNT ? i : HEAP_BIN_COUNT - 1;
}
```

`__builtin_clzll` counts leading zero bits. `63 - clz(x)` is then the position of the highest set bit in `x`, also known as `floor(log2(x))`. Applied to `size / 16`, that gives us:

| Bin | Block sizes |
| --- | --- |
| 0 | 16 to 31 |
| 1 | 32 to 63 |
| 2 | 64 to 127 |
| ... | doubling each time |
| 11 | 32,768 to 65,535 |
| 15 | 524,288 and up |

These are powers of two again, just like the buddy orders in Chapter 7. A fresh 64 KiB arena's 65,440 byte block lands in bin 11.

The search uses the bins like this:

```c
static blk_hdr_t* find_free_block(heap_t* heap, size_t size) {
    if (!heap) return NULL;

    for (uint32_t bin = bin_index(size); bin < HEAP_BIN_COUNT; bin++) {
        blk_hdr_t* cur = heap->bins[bin];

        while (cur) {
            if (!blk_ok(cur)) {
                LOGF("[HEAP ERROR] Corrupted block in free list\n");
                return NULL;
            }

            if (cur->size >= size) {
                return cur;
            }

            cur = cur->next_free;
        }
    }

    return NULL;
}
```

We start at the bin our request belongs to. A block in that same bin might still be too small (a 100 byte request and a 70 byte block both live in bin 2), so we check each one. The moment we move up to a higher bin, however, every block in it is bigger than anything in ours. The first one we see fits. In practice, the search is a couple of pointer reads.

The last bin is the exception. It has no upper limit and holds everything from 512 KiB up, meaning it really does need the size check. Huge allocations are rare enough that nobody minds.

Notice also that the search takes the *first* block that fits, without trying to find the best one. Combined with how `freelist_insert` always pushes onto the front of a list:

```c
block->prev_free = NULL;
block->next_free = heap->bins[bin];
// ...
heap->bins[bin] = block;
```

the first block the search sees is the one that was freed most recently. This is on purpose. Memory you just freed is likely still in the CPU's cache, which makes handing it straight back out cheaper than it looks. It also means two `kmalloc`s in a row do not have to land next to each other, something that tripped me up while writing a test for this chapter.

### Splitting

Once we have found a block, it is probably bigger than we need. Handing out a whole 65 KB block for a 100 byte request would be silly, so we cut it in two:

```c
static void split_block(heap_t* heap, blk_hdr_t* block, size_t size) {
    if (!block || !heap) return;

    size_t remaining = block->size - size;
    size_t overhead = sizeof(blk_hdr_t) + sizeof(blk_foot_t);

    if (remaining < MIN_BLOCK_SIZE + overhead) {
        return;
    }
    // ...
    // shrink current block to requested size
    block->size = size;
    block->total_size =
        sizeof(blk_hdr_t) + size + sizeof(blk_foot_t);
    // ... refresh the footer ...

    // create a new block at the end to represent the leftover free portion
    blk_hdr_t* new_block = (blk_hdr_t*)((uint8_t*)block + block->total_size);
    // ... header, footer, onto a free list ...
}
```

Let's trace `kmalloc(100)` on our fresh 64 KiB arena:

1. `100` rounds up to a multiple of 16, giving `112`.
2. `bin_index(112)` is bin 2. Bins 2 to 10 are empty, and bin 11 has our 65,440 byte block. Found it.
3. `remaining = 65440 - 112 = 65328`, far more than the threshold, so we split.
4. Our block shrinks to `size = 112`, with `total_size = 64 + 112 + 32 = 208`.
5. A new free block starts right after it, at offset 208. Its `total_size` is `65328`, leaving a usable `size` of `65328 - 96 = 65232`.

As a check, `208 + 65328 = 65536`. That is the whole arena, with nothing lost.

The threshold, `MIN_BLOCK_SIZE + overhead` (128 bytes), stops us from cutting off slivers nobody could ever use. If the leftover cannot even hold a header, a footer and the smallest possible block, it stays attached to the block we hand out, as a few bytes of slack.

After splitting, `heap_alloc` takes the block off its free list, flips its magic to "used", zeroes the user part (every `kmalloc` comes back zeroed), and returns the pointer right after the header.

### Freeing and Coalescing

`kfree` goes the other way. From the pointer, it steps back 64 bytes to the header, checks it with `blk_ok` (magic, red zones, and a footer that agrees with the header and points back at it), and makes sure the block is actually in use:

```c
if (block->magic != BLOCK_MAGIC_USED) {
    LOGF("[HEAP ERROR] Double free or invalid free at %p (magic: 0x%x)\n", ptr, block->magic);
    return;
}
```

A double free sees the "free" magic where it expects "used", the same trick the slab uses.

The block then goes back on a free list, and `coalesce_blocks` tries to merge it with its neighbors:

```c
blk_hdr_t* next = next_block(block);
if (next && next->magic == BLOCK_MAGIC_FREE && blk_ok(next)) {
    freelist_remove(heap, block);
    freelist_remove(heap, next);

    stat_absorb(heap, block, next);

    block->size += next->total_size;
    block->total_size += next->total_size;
    // ... rewrite the footer, which is now next's old footer ...

    freelist_insert(heap, block);

    return coalesce_blocks(heap, block);
}
// ... then the same with prev_block, where prev absorbs us ...
```

When two free blocks merge, the left one swallows the right one whole. Its `size` grows by the right one's `total_size`, since the right block's header and footer stop being bookkeeping and become usable space. `stat_absorb` tells the heap's running totals about this: 96 bytes that used to be overhead are now free memory. (Those totals come back at the end of this chapter, in `heap_check`.) Finally, the function recurses, in case the newly merged block has another free neighbor.

Let's trace it. Three `kmalloc(100)`s in a row on our arena give us `A`, `B` and `C`, 208 bytes each, followed by the rest of the arena, free:

```
[A used][B used][C used][rest free.....................]
```

1. **Free `B`.** Its next block is `C` and its previous block is `A`, both used. No merging happens, and `B` sits alone on a free list.
2. **Free `A`.** Its next block is `B`. Since `B` is free, `A` swallows it. Recursing, the block after `A+B` is `C` (used), and there is nothing before `A`. Done.

   ```
   [A+B free.....][C used][rest free.....................]
   ```

3. **Free `C`.** Its next block is the free rest of the arena, and `C` swallows it. Recursing, there is nothing after it (we hit the end of the arena), but the block before it is `A+B`, also free. `A+B` swallows `C+rest`. One more recursion finds nothing on either side.

   ```
   [one free block, the whole arena.......................]
   ```

Three frees, and we are back to exactly the block `arena_add` made. That invariant, "no two free blocks are ever next to each other", is what keeps the heap from crumbling.

>[!NOTE]
> Fun fact! That invariant had a hole, and I found it while writing this section. `krealloc` can *shrink* a block in place, by splitting off its tail as a new free block. That tail can land right before a block that is already free, and `split_block` never merged the two. Two free neighbors would then sit side by side, and since the arena never looked completely free, it could never be returned to the VMM. The fix is one call: when a used block gets split, the new free tail goes through `coalesce_blocks` too. There is now a test for it as well. It shrinks the same block twice (putting the second tail right before the first) and checks that the two become one.

### Safe Unlinking

Every free list operation goes through `freelist_remove`. It starts with a paranoid little check:

```c
if (block->prev_free && block->prev_free->next_free != block) {
    panic("heap: safe-unlink violation (prev->next != block)");
}
if (block->next_free && block->next_free->prev_free != block) {
    panic("heap: safe-unlink violation (next->prev != block)");
}
```

In plain words: "my neighbors had better point back at me". In a healthy doubly-linked list, this is always true. Why check it at all, then, and why *panic* instead of just logging like everywhere else?

The answer is in what unlinking does. Removing a block writes `block->next_free` into `block->prev_free->next_free`. That is a write of an address we got from the block, *to* an address we also got from the block. If an overflow from the block next door has overwritten those two pointers, the attacker gets to pick both, and unlinking will write any value they like, anywhere they like. This is one of the oldest heap exploits there is, written up in Phrack back in 2001. glibc grew the exact same check a few years later because of it.

With the check in place, the forged pointers almost certainly do not point back at the block. We stop before the write happens. Corruption in the heap's own links means somebody has already written where they should not have. There is no safe way to continue from there, hence the panic.

### Growing and Shrinking

If no bin has a big enough block, `heap_alloc` makes room:

```c
if (!block) {
    size_t needed = size + sizeof(blk_hdr_t) + sizeof(blk_foot_t);
    if (!arena_add(heap, needed > MIN_ARENA_SIZE ? needed : MIN_ARENA_SIZE)) {
        LOGF("[HEAP] Failed to create an arena for %zu bytes\n", needed);
        return NULL;
    }
    block = find_free_block(heap, size);
    if (!block) return NULL;
}
```

Here, we add a new arena that is either 64 KiB or exactly big enough for this one request, whichever is larger. A 1 MiB `kmalloc` gets a 1 MiB (plus a page) arena of its own. Since Chapter 8's `vmm_alloc` aligns anything of 2 MiB or more to a 2 MiB boundary, a big enough arena even gets huge pages for free.

The reverse happens on free. After coalescing, `try_shrink_arena` checks whether the block's arena is now completely empty, and gives it back to the VMM if a few conditions hold:

* It is not the last arena. Giving back the last one would just mean making a new one on the next `kmalloc`.
* Nothing in it is allocated.
* The heap as a whole has at least 4 times as much free memory as allocated memory (`HEAP_SHRINK_THRESHOLD`). If it does not, we are probably in the middle of a busy stretch. The memory is about to get used again.

This is the same reasoning as the slab keeping one empty slab around in Chapter 7. Handing memory back and forth with the layer below costs real work. We only do it when it is clearly worth it.

### Reallocating

`krealloc` tries three things, cheapest first:

1. **The block is already big enough.** We shrink it in place (that is the split from the fun fact above) and return the same pointer.
2. **The next block is free and the two together are big enough.** We swallow the neighbor, split off whatever is left over, and return the same pointer, without copying anything.
3. **Otherwise**, we `heap_alloc` a new block, copy the data across, free the old one, and return the new pointer.

The grown part is zeroed in every case, meaning a `krealloc` behaves like a `kmalloc` for the new bytes. The two edge cases follow C's `realloc`: `krealloc(NULL, n)` is just `kmalloc(n)`, and `krealloc(p, 0)` frees `p` and returns `NULL`.

### The Public API

Finally, here are the functions the rest of the kernel actually calls:

```c
void* kmalloc(size_t size) {
    if (!kheap) panic("kmalloc before the kernel heap is up");
    if (size == 0) return NULL;

    bool flags = spinlock_acquire(&kheap_lock);
    void* result = heap_alloc(kheap, size, true);
    spinlock_release(&kheap_lock, flags);

    if (!result) panicf("[HEAP] kmalloc(%zu) failed", size);
    return result;
}
```

Two decisions in there are worth a closer look.

**`kmalloc` never returns `NULL` for a real request.** (`kmalloc(0)` is not a real request, and gets `NULL`.) If it cannot deliver, it panics. That sounds harsh, but think about where `kmalloc` is called from: deep inside drivers, the TTY layer, process creation. Almost none of those could do anything sensible with a `NULL`, except crash in a more confusing place later on. One loud panic, right where the memory ran out, is a lot easier to debug. `kcalloc` and `krealloc` do return `NULL`, for the few callers that really can recover (and `kcalloc` checks `nmemb * size` for overflow first, so that `kcalloc(2^62, 8)` does not quietly become a tiny allocation).

**One lock for the whole heap.** `kheap_lock` is a plain spinlock from Chapter 6, and it also turns interrupts off while it is held. This makes `kmalloc` safe to call from an interrupt handler. A single core can never deadlock on it.

Notice also where the heap *itself* lives:

```c
static heap_t kheap_s;
static heap_t* kheap = NULL;
```

It sits in BSS, just like the slab's cache table in Chapter 7. There is one kernel heap and it lives forever, leaving no bootstrapping problem to solve. `heap_kernel_init` creates the `arena_t` slab cache, makes the first 64 KiB arena, and sets `kheap`:

```c
heap_status_t heap_kernel_init(void) {
    if (kheap) return HEAP_ERR_ALREADY_INIT;

    if (!vmm_kernel_get()) {
        LOGF("[HEAP] The kernel VMM must be online first\n");
        return HEAP_ERR_NOT_INIT;
    }

    arena_cache = slab_cache_create("arena_t", sizeof(arena_t), _Alignof(arena_t));
    // ...
    kheap_s.magic = HEAP_MAGIC;

    // create the initial arena to get things rolling
    arena_t* initial_arena = arena_add(&kheap_s, MIN_ARENA_SIZE);
    // ...
    kheap = &kheap_s;
    return HEAP_OK;
}
```

>[!NOTE]
> Userspace gets its own heap, a separate implementation in `ulibc` with `malloc` and `free`, built on the same boundary tag idea. Instead of calling the VMM directly, it gets its arenas from the kernel through a syscall. We will meet it in Chapter 15.

### Checking the Heap

`heap_check` is the heap's equivalent of `vmm_verify_integrity`. It walks every arena from start to end, block by block, using nothing but `total_size`:

```c
while (current_addr < arena->end) {
    blk_hdr_t* block = (blk_hdr_t*)current_addr;

    if (!blk_ok(block)) {
        // ...
        return HEAP_ERR_CORRUPTED;
    }
    // ... count free and used bytes ...
    current_addr += block->total_size;
}
```

Every block has to pass `blk_ok`. On top of that, the free and used totals it counts have to match the running statistics the heap keeps, per arena and overall. If a single `total_size` is off by even one byte, the walk lands in the middle of some block's data, `blk_ok` fails on garbage, and we know about it. It is a surprisingly strong check for twenty lines of code. The heap tests run it after the trickier operations.

## Putting It All Together

Here is the slice of `kernel_bootstrap()` this chapter covers, picking up right after the VMM:

```c
// VMM (Chapter 8)
vmm_status_t vmm_status = vmm_kernel_init(KERNEL_P2V(kmem->kend) + PAGE_SIZE, 0xFFFFFFFFFFFFF000);
// ...

// With the VMM online, we can use virtual addresses for everything from now on
gdt_init();
cpu_init();
QEMU_LOG("Parsed CPU information and configured GS base", TOTAL_DBG);

// kmalloc after heap init is available
heap_status_t heap_status = heap_kernel_init();

if(heap_status != HEAP_OK) {
    QEMU_LOG("[HEAP] Failed to initialize kernel heap", TOTAL_DBG);
    return false;
}
QEMU_LOG("Initialized kernel heap", TOTAL_DBG);
```

Each step enables the next. The PMM from Chapter 7 gives `gdt_init` the memory for the TSS stacks. The physmap from Chapter 5 gives it addresses for them that can never fault. `gdt_init` in turn gives us ring 3 segments, a TSS with `rsp0`, and real IST stacks for the IDT entries from Chapter 6 to point at. Because it also reloads `GS` and wipes its base, `cpu_init` has to come after it to set that base to `cpu_local`.

`cpu_init` learns what the CPU can do and turns on SMEP and SMAP. From then on, any bug that makes the kernel touch user memory becomes a page fault, while the interrupt stub and the syscall mask make sure ring 3 cannot switch SMAP back off with `AC`. If the CPU has AVX, `cpu_init` also switches the FPU helpers to `xsave`, allowing the scheduler in Chapter 14 to keep every program's `ymm` registers intact.

Finally, the VMM from Chapter 8 and the slab from Chapter 7 give the heap its arenas and their structs. The heap, in turn, gives the rest of the kernel `kmalloc`. With that, the memory subsystem from Chapter 7 is complete: PMM, slab, VMM and heap, in that order, each one standing on the one before it.

The CPU is ready too, but it is still deaf. Since Chapter 6, the old PIC has been disabled and interrupts have been off. The only thing that can interrupt the kernel is the kernel's own mistakes, which means no timer, no keyboard, and nothing from the outside world. In the next chapter, we go find the hardware that delivers those interrupts. We will dig through the firmware's ACPI tables, set up the APIC, and get a timer to tick. Strap in, there are a lot of acronyms ahead.
