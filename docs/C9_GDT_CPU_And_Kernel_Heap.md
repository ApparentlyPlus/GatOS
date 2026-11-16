# Chapter 9: The GDT, the CPU and the Kernel Heap

We survived the boss fight. The VMM is up, it can map anything anywhere, and it even handles its own page faults.

Now look back at the trail of promises this book has left behind. Chapter 6 kept saying "the TSS comes later" every time it mentioned the IST. Chapter 8 told you, with a straight face, that `cpu_init` was supposed to run before the VMM and quietly never did. And Chapter 7 promised that a 4 byte allocation would one day stop eating a whole page.

This chapter pays all three back. None of them is as big as the VMM, but each one needs memory, and until now we didn't have any to give them:

1. **The GDT, take two.** The GDT from Chapter 3 was the bare minimum to get into long mode. It has no segments for ring 3 and no TSS, and the TSS needs real memory for its stacks.
2. **Getting to know the CPU.** `cpu_init` asks the CPU what it can do, turns on a few protections, and sets up a small per-CPU data area through the `GS` register.
3. **The kernel heap.** `kmalloc` and `kfree`. Any size you like, carved out of pages the VMM hands over.

They come in that order in `kernel_bootstrap()`, right after `vmm_kernel_init`. By the end of this chapter, the memory subsystem from Chapter 7 is finally complete.

## Part 1: The GDT, Take Two

### What's Wrong With the Boot GDT?

Remember the GDT from Chapter 3? Here it is again, in all its glory:

```asm
gdt64:
	.quad 0 # null descriptor
gdt64_code_segment = . - gdt64
	.quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) # 64-bit code segment descriptor
```

A null descriptor and one code segment. That was all long mode needed, and Chapter 3 even set every data segment register to `0` and called it a day. In ring 0, long mode is fine with that.

Now think about what's coming. Chapter 2 told you the GDT is where privilege levels live: every segment has a DPL, and ring 3 code needs ring 3 segments. A user program has to run with a `CS` whose descriptor says "ring 3", and its stack segment has to say the same. Our GDT has no such descriptors. The day we try to drop into userspace with this table, the CPU looks for a ring 3 code segment, doesn't find one, and throws a General Protection fault right in our face.

Then there's the TSS, which Chapter 6 has been dodging. It needs a GDT entry too, and the boot GDT has no room for one. Worse, that entry has to hold the TSS's *address*, which is a 64-bit higher half pointer we can only know once the kernel is running. Writing that in `.rodata` by hand, in 32-bit assembly, is not happening.

So the GDT moves to C, in [`gdt.c`](/src/arch/x86_64/cpu/gdt.c), where we can build it properly.

### The New Layout

Here's what the new table holds:

| Index | Selector | Segment | DPL |
| --- | --- | --- | --- |
| 0 | `0x00` | Null | |
| 1 | `0x08` (`KERNEL_CS`) | Kernel code | 0 |
| 2 | `0x10` (`KERNEL_DS`) | Kernel data | 0 |
| 3 | `0x1B` (`USER_DS`) | User data | 3 |
| 4 | `0x23` (`USER_CS`) | User code | 3 |
| 5, 6 | `0x28` (`TSS_SEL`) | TSS | 0 |

Remember the selector format from Chapter 2? The index sits in bits 3 and up, and the bottom two bits are the requested privilege level (RPL). So a selector is just `index * 8`, plus the RPL. Kernel code at index 1 is `1 * 8 = 0x08`. User data at index 3 is `3 * 8 = 0x18`, and since it's meant to be used from ring 3, we add an RPL of `3` and get `0x1B`. Same for user code, `4 * 8 + 3 = 0x23`.

Two things look odd about this table.

**User data comes before user code.** Most tutorials put code first. GatOS can't, because of the `syscall` instruction we'll set up in Chapter 11. When a program returns from a syscall, the CPU doesn't read selectors from anywhere sensible. It takes one base value from a register and *computes* them: user data is base + 8, user code is base + 16. That arithmetic only works if data sits right before code. So that's the order, and it's not up for discussion.

The way *in* plays the same game with a second base value: kernel code is base, kernel data is base + 8. That one happens to match the order everybody uses anyway. Only the way out forces the user pair to flip.

**The TSS takes two slots.** We'll see why in a moment.

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

`gdt_set_entry` scatters a base, a limit, an access byte and a flags nibble into those fields:

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

And here's the kernel code segment:

```c
gdt_set_entry(1, 0, 0xFFFFFFFF, GDT_PRESENT | GDT_DPL_0 | GDT_SYSTEM | GDT_EXECUTABLE | GDT_READ_WRITE, GDT_LONG_MODE);
```

Let's line this up against Chapter 3's magic `.quad`, because they describe the exact same segment. The access byte is byte 5 of the entry, which makes it bits 40 to 47 of the whole 64-bit value. The flags nibble is the top half of byte 6, bits 52 to 55. So:

| Chapter 3 bit | Where it lands | `gdt.c` name |
| --- | --- | --- |
| 43 | access bit 3 | `GDT_EXECUTABLE` |
| 44 | access bit 4 | `GDT_SYSTEM` (code or data, as opposed to a system descriptor) |
| 47 | access bit 7 | `GDT_PRESENT` |
| 53 | flags bit 1 | `GDT_LONG_MODE` |

Same four bits. The C version just adds `GDT_READ_WRITE` (bit 41, code may be read as well as executed) and a limit of all ones. In long mode, the CPU ignores the base and limit of code and data segments anyway (Chapter 3 told you that), so those are just there to look tidy.

The other three are variations on the same theme. Kernel data drops `GDT_EXECUTABLE` and the long mode flag. User data and user code are copies of kernel data and kernel code with `GDT_DPL_3`. That one field is the entire difference between "kernel" and "user", as far as segments are concerned.

### The TSS

In Chapter 6, we cut the task gate from the list of IDT gates because long mode doesn't do hardware task switching anymore. The TSS, the Task State Segment, comes from the same old machinery. In 32-bit mode it held a whole saved task: every register, every segment. In long mode almost all of that is gone. What's left is a small table of **stack pointers**:

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

104 bytes. Let's go through what the CPU uses them for.

#### rsp0

Picture a user program running in ring 3. It's using its own stack, somewhere in the lower half. A timer interrupt fires. The CPU is about to push the interrupt frame from Chapter 6 (`ss`, `rsp`, `rflags`, `cs`, `rip`) and jump into the kernel.

Push it where? The user's stack? Think about that for a second.

* The program controls `rsp`. It could point it at a kernel address, and the CPU would happily write the frame over kernel memory. Or at nothing at all, and now we fault while delivering an interrupt.
* Even with a perfectly good user stack, the kernel would then run its interrupt handler on memory the program can read. Every register the handler saves would be sitting there for the program to look at after we return.

So the CPU doesn't. Whenever an interrupt takes it from ring 3 to ring 0, it first loads `rsp` from `rsp0` in the TSS, and pushes the frame there. That's a stack the kernel picked, in kernel memory. (`rsp1` and `rsp2` are the same thing for rings 1 and 2, which nobody uses.)

Interrupts that come in while we're *already* in ring 0 don't switch stacks at all. They just push onto whatever kernel stack we're on, which is exactly what has been happening since Chapter 6.

#### ist

The IST from Chapter 6, finally. Seven more stack pointers, `ist[0]` to `ist[6]`, and an IDT entry with IST index `n` makes the CPU switch to `ist[n - 1]`, *always*, no matter which ring it came from. That's the "known good stack" for the double fault: even if the kernel stack is completely blown, the CPU has somewhere safe to push the frame.

Remember why Chapter 6 had to leave every IST index at `0`? The IDT only holds the index. The stacks live here, and until this struct existed, an IST index pointed at nothing.

#### iopb_offset

This one's an odd duck. Remember `inb` and `outb` from Chapter 4? Those port I/O instructions are privileged, and the TSS can carry a bitmap that lets ring 3 use specific ports anyway. `iopb_offset` says where in the TSS that bitmap starts. GatOS sets it to `sizeof(tss_t)`, which is past the end of the TSS. No bitmap, no ports for ring 3. A user program that tries `outb` gets a General Protection fault, which is exactly what we want.

### The TSS Descriptor

The TSS needs a GDT entry, so `ltr` (we'll get to it) can find it. But look at the base field of a normal entry: 16 + 8 + 8 = 32 bits. Our TSS lives at a higher half address, which needs 64.

So in long mode, a TSS descriptor is 16 bytes, two GDT slots. The first slot is a normal entry with the low 32 bits of the base, and the second holds the high 32 bits:

```c
static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
    gdt_set_entry(index, (uint32_t)base, limit, 0x89, 0x00);

    gdt_entry_t* extra = &gdt.entries[index + 1];
    uint32_t* extra_raw = (uint32_t*)extra;
    extra_raw[0] = (uint32_t)(base >> 32);
    extra_raw[1] = 0;
}
```

The access byte `0x89` is present (`0x80`), DPL 0, and type `9`, which the CPU reads as "available 64-bit TSS". Notice that `GDT_SYSTEM` is *not* set. A TSS is a *system* descriptor, a different kind of entry from code and data segments, and that bit is how the CPU tells the two kinds apart. The limit is `sizeof(tss_t) - 1`, so 103.

That's also why `GDT_ENTRIES` is 7 and not 6.

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

* Three 16 KiB stacks (`KERNEL_STACK_SIZE` is 16 KiB too), straight from the PMM.
* Notice the `+ 16384`. Stacks on x86 grow *down*: a push decrements `rsp` first, then writes. So the CPU needs the address one past the *top* of the stack, not its start. Hand it the start, and the very first push writes below your allocation, into whatever lives there.
* The stacks are reached through the physmap, not through the VMM.

That last point is on purpose. These stacks are used in exactly the situations where things have gone wrong: a page fault, a double fault, an interrupt arriving from a user program. A stack from `vmm_alloc` would be fine in theory, but a physmap address is as boring as it gets. It's always mapped, it's never lazy, and it can never itself page fault while the CPU is pushing a fault frame onto it. That's the property you want from the stack your page fault handler runs on.

>[!WARNING]
> The flip side: these stacks have no guard pages. A guard page is an unmapped page right below a stack, so an overflow page faults instead of quietly scribbling over whatever is next in memory. Physmap memory is mapped all the way through, so there's nothing to catch an overflow here. 16 KiB is plenty for a handler that only panics, but it's a sharp edge worth knowing about.

`rsp0` won't stay this way for long, either. Once there are threads (Chapter 14), every thread gets its own kernel stack, and the scheduler points `rsp0` at the current thread's stack on every switch with `tss_set_rsp0`. This first stack is just what's there until then.

### Loading It

Two registers to update: `GDTR` to the new table, and every segment register to a selector in it.

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

`lgdt` is the same instruction as in Chapter 3, now pointing at the C table. The old table is still sitting in memory after this, but nothing points at it anymore. The data segment registers can all be loaded with a plain `mov`.

`CS` can't. There is no `mov cs, ...` on x86. The only way to change `CS` is to jump somewhere *with* a new segment, a "far" jump, call or return. In Chapter 3, the far jump into 64-bit code did it for us. Here, the easiest trick is a far *return*. `retfq` pops two values off the stack, a return address into `rip` and a selector into `cs`. So we push `KERNEL_CS`, push the address of the very next instruction (label `1:`), and "return" to it. Execution carries on exactly one line down, with the new `CS` loaded. A return to nowhere, basically.

Finally, `ltr` (load task register) points the CPU at the TSS, by its selector `0x28`. From now on, every ring 3 to ring 0 interrupt switches to `rsp0`.

`ltr` does something sneaky while it's at it: it *writes* to the GDT. It flips the descriptor's type from `9` (available) to `11` (busy). Run `ltr` a second time with the same selector and the CPU refuses with a General Protection fault, because that TSS is already taken. It's another leftover from hardware task switching, where "busy" meant "this task is running right now". For us it just means `gdt_init` runs exactly once, and never again.

And the IST indices from Chapter 6 can finally be turned on:

```c
// The IST stacks only exist now, so this is the earliest the IDT may point at them
if (tss.ist[0]) idt_set_ist(INT_DOUBLE_FAULT, 1);
if (tss.ist[1]) idt_set_ist(INT_PAGE_FAULT, 2);
```

Index `1` is `ist[0]`, index `2` is `ist[1]`. If either stack failed to allocate, its vector simply stays on the current stack, like before. A missing safety net is better than one pointing at address `0`.

>[!TIP]
> Quick puzzle before we move on. If IST stacks are so safe, why not put *every* exception on one?

Because an IST stack has no idea whether it's already in use. Every time its vector fires, the CPU loads `rsp` from `ist[n]`, the very top of that stack, no questions asked. Compare that with `rsp0`, which is only used when coming *in* from ring 3. An interrupt that arrives while the kernel is already running just pushes below whatever is on the current stack, like any nested function call, and nothing gets lost.

Now picture a page fault *inside* the page fault handler. The CPU switches to `ist[1]` again and pushes the new frame at the top, which is exactly where the first fault's frame and saved registers are sitting. They're gone. When the handlers unwind, the first one returns using whatever the second one left behind.

So the rule for an IST handler is that it must never raise its own vector. For the double fault, that's easy, since its handler only panics. For the page fault, it's why `vmm_handle_fault` from Chapter 8 only ever touches the PMM, the slab and the page tables, all of which it reaches through the physmap. Nothing it touches is lazy, so nothing it touches can fault. Give it lazy memory of its own one day, and this is the bug you'll get.

>[!CAUTION]
> Be careful with that `mov %1, %%gs`, because it does more than it looks like.
>
> In long mode, `GS` has a hidden 64-bit *base* address that the CPU adds to every `gs:` memory access. It's normally set through an MSR, and we're about to do exactly that in `cpu_init`. But loading a selector into `GS` with `mov` overwrites that hidden base with the base from the GDT entry, which is `0`.
>
> So the order is fixed: `gdt_init` first, `cpu_init` second. Swap them, and `cpu_init` sets up the per-CPU area, then `gdt_init` silently zeroes the pointer to it, and the first `gs:[0]` reads address `0`. GatOS's own test kernel had them in the wrong order for a while, and nobody noticed until the first test that made a real syscall from ring 3.

## Part 2: Getting to Know the CPU

### Asking Questions with CPUID

We've used `cpuid` three times so far without ever properly sitting down with it: in Chapter 3's assembly checks, for the PAT in Chapter 5, and for NX in Chapter 8. It's the CPU's self-description instruction. Put a "leaf" number in `EAX` (and sometimes a sub-leaf in `ECX`), run `cpuid`, and `EAX`, `EBX`, `ECX` and `EDX` come back filled with answers. Which answers depends entirely on the leaf.

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

Leaf `0` has a fun quirk. The vendor string is 12 characters, three registers' worth, and you'd expect them in order. They're not:

```c
// 4b segments
cpuid(0, 0, &a, &b, &c, &d);
*((uint32_t*)&cpuinfo.vendor[0]) = b;
*((uint32_t*)&cpuinfo.vendor[4]) = d;
*((uint32_t*)&cpuinfo.vendor[8]) = c;
```

On Intel, `EBX` holds `"Genu"`, `EDX` holds `"ineI"`, and `ECX` holds `"ntel"`. B, D, C. Stitch them together and you get `"GenuineIntel"`. On AMD, the same dance spells `"AuthenticAMD"`. Why that order? Nobody knows, nobody will ever know, and every OS in the world has the same three lines in it.

`EAX` from leaf `0` is the highest basic leaf this CPU supports. Ask for anything above it and you get garbage, so `cpu_init` remembers it (`max_basic`) and checks before touching leaf `7` or `0xB`. The extended leaves (`0x8000xxxx`) work the same way, with their own maximum in leaf `0x80000000`. It's the exact check Chapter 3's long mode test did in assembly.

### Has It versus Is It On

Every feature bit `cpu_init` finds goes into one 64-bit mask in `cpuinfo`:

```c
if (d & (1 << 26)) cpuinfo.features |= CF_SSE2;
if (c & (1 << 0)) cpuinfo.features |= CF_SSE3;
// ...
if (b & (1 << 5)) cpuinfo.features |= CF_AVX2;
if (b & (1 << 7)) cpuinfo.features |= CF_SMEP;
if (b & (1 << 20)) cpuinfo.features |= CF_SMAP;
```

And then there are three questions you can ask about any feature:

* `cpu_has_feature`: does the CPU *support* it? That's just a lookup in `cpuinfo.features`.
* `cpu_enable_feature`: turn it on, if supported. Each feature has its own switch somewhere in a control register or an MSR.
* `cpu_is_feature_enabled`: is it on *right now*? This one ignores `cpuinfo` and reads the actual control register.

The difference between the first and the last one is exactly the NX bug from Chapter 8. `cpu_has_feature` reads a table that `cpu_init` fills, so anything that asks before `cpu_init` runs gets "no" for everything. `cpu_is_feature_enabled` reads the hardware and is always right. That's why `vmm_convert_vm_flags` uses it, and why `vmm_kernel_init` now asks `cpuid` directly instead of either.

>[!NOTE]
> Fun fact! While writing this chapter, two more of these bits were wrong. `CF_AVX2` existed, the dashboard even had a label for it, but nothing ever set it, so every CPU in the world "didn't have" AVX2. And the core count read the wrong register in one path and the wrong topology level in the other. Leaf `0xB` describes the CPU level by level (threads in a core, then cores in a package), and the code only read level 0, so a 4-core CPU with 2 threads per core reported "2". It now walks the levels until it finds the core level, and reports all 4 of them on QEMU with `-smp 4,cores=2,threads=2`.

### What cpu_init Turns On

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

**SSE** we already turned on in Chapter 3's `check_SSE`, same `CR0` and `CR4` bits. Doing it twice costs nothing.

**AVX** is SSE's bigger sibling, with 256-bit `ymm` registers instead of 128-bit `xmm` ones. Bigger registers are more state for the OS to save and restore whenever it switches between programs, so the CPU won't run a single AVX instruction until the OS explicitly says "I know about these". That promise is two steps. Set `CR4.OSXSAVE` (bit 18), which unlocks a special register called `XCR0`. Then set the bits in `XCR0` for each register set the OS promises to handle: x87, SSE, and AVX. Keeping that promise takes more than two bits, and we'll get to it right after SMEP and SMAP.

**SMEP and SMAP** are the interesting ones. Both are about the `PAGE_USER` bit from Chapters 5 and 8, and both protect the kernel from *itself*.

Think about how a kernel bug turns into an exploit. Say there's a corrupted function pointer in the kernel, and an attacker controls what it points to. The classic trick is to point it at a page in their own user program, full of their own code. The kernel calls through the pointer, and now attacker code runs in ring 0. Game over. Or, a sneakier variant: a syscall takes a pointer from a user program and reads from it without checking. The program passes a pointer into its own memory, which it can change from another thread while the kernel is reading it.

* **SMEP** (Supervisor Mode Execution Prevention, `CR4` bit 20) makes the CPU refuse to *execute* any `PAGE_USER` page while in ring 0. The first trick now page faults instead of running.
* **SMAP** (Supervisor Mode Access Prevention, `CR4` bit 21) makes it refuse to even *read or write* `PAGE_USER` pages from ring 0.

SMAP sounds like it breaks syscalls completely. How does `read()` copy anything into a user buffer, then? There's an escape hatch, an `AC` flag that temporarily allows access, flipped by the `stac` and `clac` instructions:

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

The kernel opens the hatch for exactly the few instructions that copy to or from user memory, after checking the buffer with `vmm_check_buffer` from Chapter 8, and closes it right after. Every other user memory access from the kernel is a bug, and now it's a page fault instead of a silent read. We'll see the copy helpers when we build syscalls in Chapter 11.

Hold on, though. If one flag switches SMAP off, can't a user program just set it? It can. In ring 3, `AC` is the "alignment check" flag, and any program is allowed to set it with `popf`. Set it, make a syscall, and the kernel runs the entire syscall with SMAP switched off. That's exactly the trick SMAP exists to stop.

So every way into the kernel from ring 3 has to clear `AC` on arrival. `syscall` comes with a mask of flags it clears on entry, which we'll set up in Chapter 11, and `AC` is in it. Interrupts don't touch `AC` at all, so the interrupt stub from Chapter 6 clears it by hand, right after the `swapgs` on the way in from ring 3:

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

`pushfq` copies `RFLAGS` onto the stack, the `and` clears bit 18 in that copy, and `popfq` loads it back. The program's own `RFLAGS` is safe in the interrupt frame the CPU pushed, so `iretq` hands it back untouched on the way out.

And yes, both of these were missing until I sat down to explain SMAP. The syscall mask only cleared two flags, and the stub didn't clear anything.

### Keeping the AVX Promise

Remember Chapter 1's rule? Kernel code never touches the floating point registers, because they belong to whatever program the kernel interrupted. That stops the kernel from *corrupting* them. But once there's more than one program, something also has to *swap* them: save program A's registers when it stops running, and load program B's when it starts. That's the scheduler's job, and we'll build it in Chapter 14. What matters here is the instruction it uses.

For x87 and SSE, that instruction is `fxsave`. It dumps the whole floating point state into a 512 byte area in one go, and `fxrstor` loads it back. The 16 `xmm` registers take 256 of those bytes.

Now think about what setting bit 2 of `XCR0` said: "I know about the 256-bit `ymm` registers, and I'll keep them safe." The low half of each `ymm` register *is* the matching `xmm` register, so `fxsave` covers that half. The top half isn't anywhere in `fxsave`'s 512 bytes. That layout was set in stone years before AVX existed, and nobody can grow it without breaking every OS that already sets aside exactly 512 bytes.

>[!NOTE]
> Fun fact! GatOS made that promise and then kept saving with `fxsave`. Picture program A with a value in `ymm0`. The scheduler switches to B, which uses `ymm0` too, then back to A. A gets its low half back from its save area, but B's top half is still sitting in the register. A's number is now half its own and half somebody else's, and B just leaked data into A. Nothing in GatOS or Appa is compiled with AVX yet, so it never actually happened, but it was waiting for the first program that did.

The fix is the instruction pair that came with this mess, `xsave` and `xrstor`. Same idea as `fxsave`, but the save area can grow:

```
offset 0    [ legacy area, 512 bytes ]   same layout as fxsave: x87, SSE, xmm0-15
offset 512  [ XSAVE header, 64 bytes ]   which parts were saved
offset 576  [ AVX, 256 bytes         ]   top halves of ymm0-15
            = 832 bytes
```

Each part is called a *component*, and they line up with the bits of `XCR0`: x87 is bit 0, SSE is bit 1, AVX is bit 2. `xsave` takes a bitmask in `EDX:EAX` saying which components to save, and the CPU ANDs it with `XCR0`, so passing all ones means "everything the OS turned on". The header records which components actually got written, so `xrstor` knows what to load back.

GatOS wraps both pairs in two helpers in `cpu.c`, so the scheduler doesn't have to care which one is in use:

```c
void fpu_save(void* area)
{
    if (use_xsave)
        __asm__ volatile("xsave (%0)" :: "r"(area), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    else
        __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}
```

`fpu_restore` is the same thing with `xrstor` and `fxrstor`. `use_xsave` gets set by `cpu_enable_feature(CF_AVX)`, right after it writes `XCR0`, and it double checks the size first:

```c
// With XCR0 set, CPUID 0xD reports how big an xsave of the enabled state is
uint32_t a, b, c, d;
cpuid(0x0D, 0, &a, &b, &c, &d);
if (b > FPU_AREA_SIZE) panic("FPU state doesn't fit FPU_AREA_SIZE");
use_xsave = true;
```

With x87, SSE and AVX on, `EBX` comes back as 832, the exact number from the diagram, and `FPU_AREA_SIZE` is 832. So why ask at all? Because whoever turns on the next component, say AVX-512, grows the area past 2.5 KiB. With this check, they find out from a panic on the very first boot, not from threads quietly scribbling over each other's memory.

There's one more catch. `xsave` insists on a save area aligned to 64 bytes, and raises a General Protection fault otherwise (`fxsave` was happy with 16). Each thread keeps its area inside its thread struct, which comes from `kmalloc`. And as we'll see in Part 3, `kmalloc` only promises 16 byte alignment. So the struct carries a slightly oversized buffer, plus a pointer to the first 64 byte boundary inside it:

```c
uint8_t* fpu;
uint8_t fpu_raw[FPU_AREA_SIZE + FPU_AREA_ALIGN - 16] __attribute__((aligned(16)));
```

```c
thread->fpu = (uint8_t*)(((uintptr_t)thread->fpu_raw + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
```

>[!TIP]
> Why `+ 64 - 16`, and not a full `+ 64`? Think about where `fpu_raw` can start. It's 16 byte aligned, so its address modulo 64 is 0, 16, 32 or 48. The worst case is 16, which sits 48 bytes short of the next boundary. So 48 bytes of padding is always enough, and a 49th byte would never get used.

A brand new thread's area is all zeros, apart from two control words whose reset values aren't zero (one for x87 and one for SSE, both meaning "mask every floating point exception"). With `xsave`, an all-zero header means "every component is in its reset state", so the thread's first `xrstor` hands it a clean slate. And since `cpu_init` runs long before the first thread exists, every area in the system is always written and read by the same instruction. When the scheduler actually calls these helpers (lazily, only once a thread touches the FPU) is a story for Chapter 14.

>[!CAUTION]
> Even *asking* whether AVX is on was dangerous. For AVX, `cpu_is_feature_enabled` has to read `XCR0`, with an instruction called `xgetbv`. But `XCR0` only exists once `CR4.OSXSAVE` is set, and on a CPU where it isn't, `xgetbv` is an invalid opcode. So the question "is AVX on?" crashed the kernel on exactly the CPUs where the answer was "no". It sat there unnoticed because nothing asked until the FPU tests for this chapter did. The fixed version checks `CR4` before it goes anywhere near `XCR0`:
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

### The GS Base and cpu_local

The last thing `cpu_init` sets up is easy to miss:

```c
// GS_BASE -> cpu_local in ring 0, KERNEL_GS_BASE holds user GS on entry
write_msr(MSR_GS_BASE, (uint64_t)&cpu_local);
write_msr(MSR_KERNEL_GS_BASE, 0);
```

To see why, we have to jump ahead a little, to Chapter 11. A syscall is a user program asking the kernel to do something, using the `syscall` instruction. It's faster than an interrupt, and part of how it gets faster is by doing *less*. In particular, it does **not** switch stacks. Remember `rsp0`? Interrupts use it, `syscall` ignores it. The first instruction of the kernel's syscall handler runs in ring 0, with `rsp` still pointing at the user's stack.

That's a problem, for every reason we just went through for `rsp0`. The handler needs to switch to a kernel stack before it touches the stack at all. To do that, it has to load the kernel stack's address from *somewhere*. But it can't use any register, because they all still hold the user program's values. And it can't use the stack, because that's the thing we don't trust.

The way out is `GS`. Remember that hidden base from the caution earlier? Any instruction can read memory relative to it with a `gs:` prefix, and doing so needs no free register at all. So if `GS` points at a little struct that holds the kernel stack's address, the handler can switch stacks in two instructions:

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

`packed`, because the assembly above has the offsets `0` and `8` hardcoded, and the struct had better agree.

There's one complication. A user program may want `GS` for itself (C libraries often use it, or its twin `FS`, for thread-local storage). So there are actually two GS bases in MSRs: `MSR_GS_BASE`, the one that's active, and `MSR_KERNEL_GS_BASE`, a spare. The `swapgs` instruction from Chapter 6 swaps the two. That gives us two states:

| | `MSR_GS_BASE` (active) | `MSR_KERNEL_GS_BASE` (spare) |
| --- | --- | --- |
| Running in ring 0 | `&cpu_local` | the user's GS |
| Running in ring 3 | the user's GS | `&cpu_local` |

Every entry into the kernel from ring 3 does a `swapgs`, and every exit back does another one. That's exactly what the `test` and `swapgs` at the top of Chapter 6's interrupt stub are for. Right now we're in ring 0, so `cpu_init` puts `cpu_local` in the active slot and `0` in the spare.

>[!NOTE]
> Why "CPU local"? GatOS runs on one core, so there's one `cpu_local`, and a global variable would do. On a multicore kernel, though, every core runs its own syscalls, on its own kernel stack, at the same time. Each core gets its own copy of this struct, and its own `GS_BASE` pointing at it. The handler code stays identical on every core. That's what `GS` is for in pretty much every x86-64 kernel out there.

### Idling

`cpu_init` ends with `cpu_idle_init`, which deserves a quick mention. When there's nothing to do, the CPU should sleep, not spin. `hlt` sleeps until the next interrupt. Most modern CPUs also have `monitor` and `mwait`, which can sleep *deeper* (lower power states, called C-states), and can also wake up when a specific memory address gets written, on top of interrupts. `cpu_idle_init` checks for `mwait` in CPUID leaf 1, then picks the deepest C-state leaf 5 says is available. `cpu_idle_on` uses it, or falls back to `hlt` on CPUs that don't have it. The idle thread in Chapter 14 is the main customer.

## Part 3: The Kernel Heap

### Why Another Allocator?

Let's count what we have. The PMM hands out pages. The VMM hands out pages, wherever we want them, with whatever flags. The slab hands out small objects, but only of sizes we registered up front, and only 16 kinds of them (`SLAB_MAX_CACHES`).

Now try to allocate a string whose length you only know at runtime. Or an array of `n` entries. Or a copy of a firmware table you just measured. None of those fit any of our allocators: the slab wants a fixed size per cache, and a page is either too big or too small.

What we want is the thing every C programmer already knows. `malloc(n)`, any `n`. That's the heap, the last layer of the memory subsystem from Chapter 7. In the kernel it's called `kmalloc`, and it lives in [`heap.c`](/src/kernel/memory/heap.c).

### The Plan: Arenas and Blocks

The idea is a classic, from the first volume of Donald Knuth's *The Art of Computer Programming*, back in 1968:

1. Ask the VMM for a big chunk of memory. GatOS calls that chunk an **arena**.
2. Treat the whole arena as one big free **block**.
3. When someone asks for `n` bytes, find a free block that's big enough, cut off the part you need, and leave the rest free.
4. When they give it back, mark the block free again, and glue it to its neighbors if those are free too, so free memory doesn't crumble into tiny useless pieces.

Every block carries a little header in front of the memory it hands out, and a footer behind it:

```
arena:  [hdr| used data |ftr][hdr|     free       |ftr][hdr| used |ftr] ...
```

That header and footer pair is called a **boundary tag**, because they sit on both boundaries of every block. Here they are:

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

* `size` is the usable part, what the caller gets. `total_size` is the whole block, header and footer included. You'll see both all over the code, and mixing them up is the classic heap bug.
* `magic` says whether the block is in use (`0xABADCAFE`) or free (`0xA110CA7E`). The footer repeats it.
* The red zones (`0x8BADF00D`) are the same idea as in the slab and the VMM. A write that runs off the end of one block smashes the next block's red zone, and we notice.
* `next_free` and `prev_free` link free blocks into lists. Only free blocks use them.
* The footer's `header` points back at its own header. Hold on to that one, it's the whole reason the footer exists.

The header is 64 bytes and the footer is 32, so every block costs 96 bytes of bookkeeping. On top of that, the smallest block is 32 bytes (`MIN_BLOCK_SIZE`), and every size is rounded up to a multiple of 16. So `kmalloc(1)` costs 128 bytes. That's a lot of overhead for one byte, and it's honest. Small objects you allocate by the thousand belong in the slab. The heap is for everything the slab can't do.

>[!NOTE]
> Why 16? It's the alignment C's `malloc` promises (`max_align_t` on x86-64), and it's what SSE instructions want for 16 byte loads and stores. Since the arena starts on a page, the header is 64 bytes, and every size is a multiple of 16, every pointer `kmalloc` returns is 16 byte aligned without any extra effort.

### Walking Between Blocks

Here's the part that makes boundary tags clever. Blocks don't need a list to find their neighbors. Their sizes are enough.

Going *forward* is easy. The next block starts right where this one ends:

```c
uintptr_t next_addr = (uintptr_t)block + block->total_size;
```

Going *backward* is the problem. We know where our block starts, but not where the previous one starts, because we don't know how big it is. Its header is somewhere to our left, at a distance we can't compute.

Its footer, though, is right next to us. The previous block's footer always ends exactly where our header begins. And that footer knows where its header is:

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

Step back 32 bytes, read the footer, follow its pointer. That's the whole trick. If we've stepped back past the start of the arena, we were the first block and there's nobody to our left.

Both directions are `O(1)`. That matters because freeing has to look at both neighbors, every time.

### Arenas

An arena is one chunk from the VMM, with a small struct describing it:

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

The struct comes from a slab cache called `arena_t`, and the memory from the kernel VMM with just `VM_FLAG_WRITE`. That's the first row of the "who asks for what" table from Chapter 8: plain kernel RAM, writable, not executable, not lazy.

Why keep the struct in a separate cache, and not at the start of the arena itself? Because this way, the arena holds nothing but blocks, from its very first byte to its very last. Every walk you'll see in this part can start at `arena->start` and expect a header there, no special case for the first block. And the arena's own bookkeeping sits on a different page, out of reach of a block that overflows.

`arena_add` then turns the fresh arena into one big free block. Let's put numbers on it with the smallest arena, `MIN_ARENA_SIZE`, 64 KiB:

* `total_size` = 65,536 bytes, the whole arena.
* `size` = 65,536 - 64 (header) - 32 (footer) = 65,440 bytes usable.

The header goes at the very start of the arena, the footer at the very end, and the block goes onto a free list. The heap keeps a list of all its arenas too, so it can find them again.

### Finding a Free Block

When `kmalloc` is called, we need a free block that's big enough. The naive way is one list of all free blocks, scanned from the front until something fits. That's `O(n)` in the number of free blocks, and a busy heap has plenty of them.

GatOS keeps 16 lists instead, called **bins**, and each one only holds free blocks of a certain size range:

```c
static inline uint32_t bin_index(size_t size) {
    if (size < HEAP_MIN_ALIGN) size = HEAP_MIN_ALIGN;
    uint32_t i = (uint32_t)(63 - __builtin_clzll((uint64_t)(size >> 4)));
    return i < HEAP_BIN_COUNT ? i : HEAP_BIN_COUNT - 1;
}
```

`__builtin_clzll` counts leading zero bits, so `63 - clz(x)` is the position of the highest set bit in `x`, which is `floor(log2(x))`. Applied to `size / 16`, that gives:

| Bin | Block sizes |
| --- | --- |
| 0 | 16 to 31 |
| 1 | 32 to 63 |
| 2 | 64 to 127 |
| ... | doubling each time |
| 11 | 32,768 to 65,535 |
| 15 | 524,288 and up |

Powers of two again, like the buddy orders in Chapter 7. A fresh 64 KiB arena's 65,440 byte block lands in bin 11.

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

Start at the bin our request belongs to. A block in that same bin might still be too small (a 100 byte request and a 70 byte block both live in bin 2), so we check each one. But the moment we move up to a higher bin, every block in it is bigger than anything in ours, so the first one we see fits. In practice, the search is a couple of pointer reads.

The last bin is the exception. It has no upper limit, so it holds everything from 512 KiB up, and it really does need the size check. Huge allocations are rare enough that nobody minds.

Notice it takes the *first* block that fits, not the best one. And `freelist_insert` always pushes onto the front of a list:

```c
block->prev_free = NULL;
block->next_free = heap->bins[bin];
// ...
heap->bins[bin] = block;
```

So the first block the search sees is the one that was freed most recently. That's on purpose. Memory you just freed is likely still in the CPU's cache, so handing it straight back out is cheaper than it looks. It also means two `kmalloc`s in a row don't have to land next to each other, which tripped me up while writing a test for this chapter.

### Splitting

We found a block, but it's probably bigger than we need. Handing out a whole 65 KB block for a 100 byte request would be silly, so we cut it in two:

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

1. `100` rounds up to a multiple of 16: `112`.
2. `bin_index(112)` is bin 2. Bins 2 to 10 are empty. Bin 11 has our 65,440 byte block. Found it.
3. `remaining = 65440 - 112 = 65328`. That's way more than the threshold, so we split.
4. Our block shrinks to `size = 112`, `total_size = 64 + 112 + 32 = 208`.
5. A new free block starts right after it, at offset 208. Its `total_size` is `65328`, and its usable `size` is `65328 - 96 = 65232`.

Check: `208 + 65328 = 65536`. The whole arena, nothing lost.

The threshold, `MIN_BLOCK_SIZE + overhead` (128 bytes), stops us from cutting off slivers nobody could use. If the leftover can't even hold a header, a footer and the smallest possible block, it stays attached to the block we hand out, as a few bytes of slack.

After splitting, `heap_alloc` takes the block off its free list, flips its magic to "used", zeroes the user part (every `kmalloc` comes back zeroed), and returns the pointer right after the header.

### Freeing and Coalescing

`kfree` goes the other way. From the pointer, step back 64 bytes to the header, check it with `blk_ok` (magic, red zones, and a footer that agrees with the header and points back at it), and make sure it's actually in use:

```c
if (block->magic != BLOCK_MAGIC_USED) {
    LOGF("[HEAP ERROR] Double free or invalid free at %p (magic: 0x%x)\n", ptr, block->magic);
    return;
}
```

A double free sees the "free" magic where it expects "used". Same trick as the slab.

Then the block goes back on a free list, and `coalesce_blocks` tries to merge it with its neighbors:

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

When two free blocks merge, the left one swallows the right one whole. Its `size` grows by the right one's `total_size`, because the right block's header and footer stop being bookkeeping and become usable space. `stat_absorb` tells the heap's running totals about that: 96 bytes that used to be overhead are now free memory. (Those totals come back at the end of this chapter, in `heap_check`.) Then it recurses, in case the newly merged block has another free neighbor.

Let's trace it. Three `kmalloc(100)`s in a row on our arena give us `A`, `B` and `C`, 208 bytes each, followed by the rest of the arena, free:

```
[A used][B used][C used][rest free.....................]
```

1. **Free `B`.** Next is `C`, used. Previous is `A`, used. No merging. `B` sits alone on a free list.
2. **Free `A`.** Next is `B`, free! `A` swallows `B`. Recurse: the next block after `A+B` is `C`, used. There's nothing before `A`. Done.

   ```
   [A+B free.....][C used][rest free.....................]
   ```

3. **Free `C`.** Next is the rest of the arena, free. `C` swallows it. Recurse: nothing after it (end of the arena). Previous is `A+B`, free! `A+B` swallows `C+rest`. Recurse once more: nothing on either side.

   ```
   [one free block, the whole arena.......................]
   ```

Three frees, and we're back to exactly the block `arena_add` made. That invariant, "no two free blocks are ever next to each other", is what keeps the heap from crumbling.

>[!NOTE]
> Fun fact! That invariant had a hole, and I found it while writing this section. `krealloc` can *shrink* a block in place, by splitting off its tail as a new free block. That tail can land right before a block that's already free, and `split_block` never merged them. Two free neighbors, side by side, and the arena could never be returned to the VMM because it never looked completely free. The fix is one call: when a used block gets split, the new free tail goes through `coalesce_blocks` too. And there's now a test that shrinks the same block twice, so the second tail lands right before the first, and checks that they become one.

### Safe Unlinking

Every free list operation goes through `freelist_remove`, and it starts with a paranoid little check:

```c
if (block->prev_free && block->prev_free->next_free != block) {
    panic("heap: safe-unlink violation (prev->next != block)");
}
if (block->next_free && block->next_free->prev_free != block) {
    panic("heap: safe-unlink violation (next->prev != block)");
}
```

"My neighbors had better point back at me." In a healthy doubly-linked list, that's always true. Why check it, then, and why *panic* instead of just logging like everywhere else?

Because of what unlinking does. Removing a block writes `block->next_free` into `block->prev_free->next_free`. That's a write of an address we got from the block, *to* an address we got from the block. If an overflow from the block next door has overwritten those two pointers, the attacker picks both, and unlinking writes any value they like anywhere they like. That's not a theory. It's one of the oldest heap exploits there is, written up in Phrack back in 2001, and it's why glibc grew the exact same check a few years later.

With the check in place, the forged pointers almost certainly don't point back at the block, and we stop before the write happens. Corruption in the heap's own links means somebody already wrote where they shouldn't have, so there's no safe way to continue. Hence the panic.

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

A new arena, either 64 KiB or exactly big enough for this one request, whichever is larger. A 1 MiB `kmalloc` gets a 1 MiB (plus a page) arena of its own. And since Chapter 8's `vmm_alloc` aligns anything of 2 MiB or more to a 2 MiB boundary, a big enough arena even gets huge pages for free.

The reverse happens on free. After coalescing, `try_shrink_arena` checks whether the block's arena is now completely empty, and gives it back to the VMM if a few conditions hold:

* It's not the last arena. Giving back the last one would just mean making a new one on the next `kmalloc`.
* Nothing in it is allocated.
* The heap as a whole has at least 4 times as much free memory as allocated memory (`HEAP_SHRINK_THRESHOLD`). Otherwise we're probably in the middle of a busy stretch, and the memory is about to get used again.

Same reasoning as the slab keeping one empty slab around in Chapter 7. Handing memory back and forth with the layer below costs real work, so we only do it when it's clearly worth it.

### Reallocating

`krealloc` tries three things, cheapest first:

1. **The block is already big enough.** Shrink it in place (that's the split from the fun fact above), return the same pointer.
2. **The next block is free and the two together are big enough.** Swallow the neighbor, split off whatever's left over, return the same pointer. No copying.
3. **Otherwise**, `heap_alloc` a new block, copy the data across, free the old one, return the new pointer.

The grown part is zeroed in every case, so a `krealloc` behaves like a `kmalloc` for the new bytes.

The two edge cases follow C's `realloc`. `krealloc(NULL, n)` is just `kmalloc(n)`, and `krealloc(p, 0)` frees `p` and returns `NULL`.

### The Public API

Finally, the functions the rest of the kernel actually calls:

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

**`kmalloc` never returns `NULL` for a real request.** (`kmalloc(0)` isn't one, and gets `NULL`.) If it can't deliver, it panics. That sounds harsh, but think about where `kmalloc` is called from: deep inside drivers, the TTY layer, process creation. Almost none of those could do anything sensible with a `NULL` except crash in a more confusing place later. One loud panic, right where the memory ran out, is a lot easier to debug. `kcalloc` and `krealloc` do return `NULL`, for the few callers that really can recover (and `kcalloc` checks `nmemb * size` for overflow first, so `kcalloc(2^62, 8)` doesn't quietly become a tiny allocation).

**One lock for the whole heap.** `kheap_lock` is a plain spinlock from Chapter 6, which also turns interrupts off while it's held. So `kmalloc` is safe to call from an interrupt handler, and a single core can never deadlock on it.

And notice where the heap *itself* lives:

```c
static heap_t kheap_s;
static heap_t* kheap = NULL;
```

In BSS, like the slab's cache table in Chapter 7. There's one kernel heap, it lives forever, so there's no bootstrapping problem to solve. `heap_kernel_init` creates the `arena_t` slab cache, makes the first 64 KiB arena, and sets `kheap`:

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
> Userspace gets its own heap, a separate implementation in `ulibc` with `malloc` and `free`, built on the same boundary tag idea. It gets its arenas from the kernel through a syscall instead of calling the VMM directly. We'll meet it in Chapter 15.

### Checking the Heap

`heap_check` is the heap's `vmm_verify_integrity`. It walks every arena from start to end, block by block, using nothing but `total_size`:

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

Every block has to pass `blk_ok`. Then the free and used totals it counted have to match the running statistics the heap keeps, per arena and overall. If a single `total_size` is off by one byte, the walk lands in the middle of some block's data, `blk_ok` fails on garbage, and we know. It's a surprisingly strong check for twenty lines of code, and the heap tests run it after the trickier operations.

## Putting It All Together

Here's the slice of `kernel_bootstrap()` this chapter covers, picking up right after the VMM:

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

Each step enables the next:

* The PMM (Chapter 7) gives `gdt_init` the memory for the TSS stacks, and the physmap (Chapter 5) gives it addresses for them that can never fault.
* `gdt_init` gives us ring 3 segments, a TSS with `rsp0`, and real IST stacks, so the IDT entries from Chapter 6 finally point somewhere.
* `gdt_init` also reloads `GS`, which wipes its base, so `cpu_init` has to come after it to set the base to `cpu_local`.
* `cpu_init` learns what the CPU can do, and turns on SMEP and SMAP, so every bug that makes the kernel touch user memory becomes a page fault. The interrupt stub and the syscall mask make sure ring 3 can't switch SMAP back off with `AC`.
* If the CPU has AVX, `cpu_init` also switches the FPU helpers to `xsave`, so the scheduler in Chapter 14 can keep every program's `ymm` registers intact.
* The VMM (Chapter 8) and the slab (Chapter 7) give the heap its arenas and their structs.
* And the heap gives the rest of the kernel `kmalloc`.

With that, the memory subsystem from Chapter 7 is complete. PMM, slab, VMM, heap, in that order, each one standing on the one before.

The CPU is ready too. But it's still deaf. Since Chapter 6, the old PIC has been disabled, interrupts have been off, and the only thing that can interrupt the kernel is the kernel's own mistakes. No timer, no keyboard, nothing from the outside world. Next chapter, we go find the hardware that delivers those interrupts, which means digging through the firmware's ACPI tables, setting up the APIC, and getting a timer to tick. Strap in, there's a lot of acronyms ahead.
