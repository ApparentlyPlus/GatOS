# Chapter 11: System Calls and Input

You survived the acronyms, congrats! This is the chapter where GatOS starts to feel like a computer you can actually *use*, because by the end of it, you can type on it.

A program running in ring 3 is, by design, almost completely powerless. It cannot touch kernel memory, it cannot talk to devices, and it cannot even turn interrupts off. If it wants to print "Hello", someone with more privilege has to do it on its behalf. Right now, nobody is listening.

The first half of this chapter gives it a way to ask. We set up the `syscall` instruction, and look at everything the kernel has to check before it believes a word a program says.

The second half is the keyboard. We bring up the ancient PS/2 controller, decode the bytes a keyboard sends, and build a small hub that decides where each keypress ends up.

>[!NOTE]
>The code for this chapter lives in [`syscall.c`](/src/kernel/sys/syscall.c), [`syscall_entry.S`](/src/arch/x86_64/cpu/syscall_entry.S), [`i8042.c`](/src/kernel/drivers/i8042.c), [`keyboard.c`](/src/kernel/drivers/keyboard.c) and [`input.c`](/src/kernel/drivers/input.c), along with their headers.

## What is a System Call?

Back in Chapter 9, we split the world into kernel and user. User code runs in ring 3, its pages are marked as user pages, and everything else is off limits. A user program that tries to read a kernel address gets a page fault. A user program that tries `outb` gets a General Protection fault. That is the whole point of the split: a buggy (or malicious) program can only ever break itself.

But a program that can only break itself cannot do much else either. Printing to the screen means writing to the framebuffer (kernel memory). Reading the keyboard means talking to a device. Getting more memory means editing page tables. Every useful thing a program wants to do requires privilege it does not have. That is intentional.

A **system call** (or syscall) is the controlled doorway through those walls. The program does not get privilege. Instead, it asks the kernel to perform one specific, well defined operation. Whether it happens is the kernel's call.

When you call `printf("Hello")` in a C program, this is roughly what happens:

```
printf("Hello")                       (ring 3, ulibc)
  └─> formats the string into a buffer
      └─> write(buffer, 5)            (ring 3, ulibc)
          └─> syscall instruction     (ring 3 -> ring 0)
              └─> syscall_entry       (ring 0, assembly)
                  └─> syscall_dispatcher -> SYS_WRITE -> tty_write
                      └─> back to ring 3, rax = 5
```

The interesting part is the arrow in the middle. Somehow, the CPU has to go from running user code to running kernel code, at a place the kernel chose (never the program), and then back again. Everything before that arrow is ordinary userspace C, and everything after it is ordinary kernel C. This chapter is mostly about the arrow.

>[!IMPORTANT]
>A syscall is not a function call, even though C libraries go to great lengths to make it look like one. The program cannot pick *where* in the kernel it lands. It can only pick *which* operation it wants, by putting a number in a register. The kernel looks that number up in its own table. What happens next is none of the program's business. This is what makes the doorway safe: there is exactly one way in, and the kernel is standing right behind it.

## Why Not Just Use an Interrupt?

We already have a mechanism that jumps from ring 3 into kernel code at a location the kernel chose. It is called an interrupt.

In fact, that is how syscalls were done for a long time. Linux on 32-bit x86 used `int 0x80`: the program puts the syscall number in `eax`, executes `int 0x80`, and the CPU treats it like any other interrupt with vector `0x80`.

The IDT entry for that vector gets a DPL of 3, allowing ring 3 to trigger it. Every other vector stays at DPL 0 (a user `int 14` gets a General Protection fault instead of a fake page fault).

It works. The problem is that interrupts are expensive, because they were designed to handle *anything*. On every `int`, the CPU has to:

1. Look up the vector in the IDT, and read the 16 byte gate descriptor
2. Check the gate's DPL against the current privilege level
3. Load the kernel code segment and check it too
4. Load `rsp0` from the TSS, since we are changing rings
5. Push `ss`, `rsp`, `rflags`, `cs` and `rip` onto the new stack
6. Jump to the handler

And `iretq` does most of that in reverse, popping all five values and re-checking the segments on the way out. For a hardware interrupt that happens a few hundred times a second, nobody cares. For a syscall that a busy program might make hundreds of thousands of times a second, all of that checking adds up quickly.

AMD's answer, when they designed x86-64, was the `syscall` and `sysret` pair. The idea is that the kernel sets everything up *once*, ahead of time, in a handful of MSRs. At call time, the CPU has nothing left to look up. When a program executes `syscall`, the CPU:

1. Saves the user's `rip` (the instruction right after `syscall`) into `rcx`
2. Saves the user's `rflags` into `r11`
3. Clears whichever bits of `rflags` the kernel asked it to
4. Loads `cs` and `ss` with fixed, precomputed values, without reading the GDT
5. Jumps to the address the kernel stored in advance

No IDT, no TSS and no memory accesses at all. `sysret` is the mirror image: `rip` comes back from `rcx`, `rflags` from `r11`, and the user segments are, once again, precomputed.

Note what is missing from that list: the stack. `syscall` does not switch stacks and it does not push anything. When the first instruction of our handler runs, it is in ring 0, but `rsp` still points at the user's stack (memory the user program fully controls). Chapter 9 already explained the consequence, along with the `gs` trick we use to deal with it. We are about to see the full picture.

>[!NOTE]
>Fun fact! Intel had its own pair, `sysenter` and `sysexit`. It came first (with the Pentium II) and worked differently. In 64-bit mode, Intel CPUs support AMD's `syscall` as well, and that's what every 64-bit kernel uses. On AMD CPUs, `sysenter` doesn't even work in 64-bit mode.

## Setting Up `syscall`

Before the first `syscall` can work, the kernel has to fill in those precomputed values. They all live in MSRs (model specific registers, read with `rdmsr` and written with `wrmsr`), and `syscall_init` sets all four of them up in one go.

The first one turns the instruction on:

```c
uint64_t efer = read_msr(MSR_EFER);
efer |= EFER_SCE;
write_msr(MSR_EFER, efer);
```

`EFER` is the same Extended Feature Enable Register we used to turn on long mode, and again for the NX bit. Bit 0 is `SCE`, System Call Extensions. Until it is set, `syscall` is an invalid opcode, and a program that tries it gets a `#UD`.

### The Segments: STAR

Next, the CPU needs to know which segments to load on the way in and on the way out. Both answers live in one MSR, `STAR`:

```
 63          48 47          32 31                       0
┌──────────────┬──────────────┬──────────────────────────┐
│  SYSRET base │ SYSCALL base │   (unused in 64-bit)     │
└──────────────┴──────────────┴──────────────────────────┘
```

The CPU does not store four separate selectors. It stores two *base* values and computes the rest with fixed offsets:

- `syscall` loads `cs = SYSCALL base` and `ss = SYSCALL base + 8`
- `sysret` (64-bit) loads `cs = SYSRET base + 16` and `ss = SYSRET base + 8`

Back then, we settled on this GDT order.

```
0x08  Kernel Code
0x10  Kernel Data
0x18  User Data  (DPL 3)
0x20  User Code  (DPL 3)
```

For the way in, we set the base to `0x08`. Then `cs = 0x08` (kernel code) and `ss = 0x08 + 8 = 0x10` (kernel data). This is the order every kernel uses anyway.

The way out is where it gets interesting. User data has to sit at base + 8 and user code at base + 16. This is why Chapter 9 put user data *before* user code. If we pick a base of `0x10`, we get `ss = 0x18` and `cs = 0x20`, both pointing at the right descriptors.

But a selector also carries a requested privilege level in its bottom two bits, and for ring 3 that needs to be `3`. On paper, `sysret` takes care of it by forcing those two bits to `3` itself.

In practice, the comment I left in `syscall_init` tells a different story:

```c
// We set the user base to 0x13 (= 0x10 | RPL3) so that SYSRETQ computes:
//   CS = 0x13 + 16 = 0x23 = USER_CS
//   SS = 0x13 + 8  = 0x1B = USER_DS
//
// This avoids reliance on the CPU forcing SS.RPL=3, which certain Intel
// microarchitectures fail to do (producing SS=0x18 instead of 0x1B)
// Author's Note: Oooooof dude, this was a pain to figure out, thanks google

uint64_t star = ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32);
write_msr(MSR_STAR, star);
```

Instead of trusting the CPU to add the RPL, we bake it into the base. `0x13` is `0x10` with the bottom two bits set, and since we only ever *add* multiples of 8 to it, those two bits survive the arithmetic: `0x13 + 8 = 0x1B` and `0x13 + 16 = 0x23`.

Both are user selectors with RPL 3, whether the CPU remembers to force it or not. On CPUs that do behave, forcing `3` on top of an existing `3` changes nothing. The trick is free.

>[!NOTE]
>An `ss` of `0x18` instead of `0x1B` is a nasty one, because nothing breaks right away. In 64-bit mode, the stack segment is mostly ignored. The program keeps running normally until the next interrupt, when the CPU pushes that `ss` into the iret frame.
>
>The `iretq` at the end then tries to load `0x18` into `ss` while returning to ring 3, sees an RPL that does not match, and raises a General Protection fault. The crash ends up in the interrupt path, nowhere near the actual bug.

The third MSR holds the entry point:

```c
write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
```

`LSTAR` (Long mode STAR) is simply the address `syscall` jumps to. Every syscall from every program lands on `syscall_entry`. We will dissect it shortly. A program cannot pick another entry point, because the only thing it controls is the register contents at the moment it executes `syscall`.

### The Flag Mask: FMASK

The last MSR decides which bits of `rflags` get cleared on entry. Whatever bit is set in `FMASK`, the CPU clears in `rflags` right after saving the old value into `r11`:

```c
write_msr(MSR_FMASK, 0x100 | 0x200 | 0x400 | 0x4000 | 0x40000);
```

Every one of these bits is something the user program can set (with `popfq`) right before executing `syscall`, and every one of them would hurt us if it survived into the kernel:

| Bit | Flag | Why we clear it |
| --- | --- | --- |
| 8 | `TF` (trap) | Single steps every instruction. The very first one runs while `rsp` still points at the user stack |
| 9 | `IF` (interrupts) | An interrupt before we switch stacks would push its frame onto the user stack |
| 10 | `DF` (direction) | Makes string instructions run *backwards*. Compiled code assumes it is clear, so our `kmemcpy` would copy the wrong way |
| 14 | `NT` (nested task) | A leftover from hardware task switching that makes `iretq` fault |
| 18 | `AC` (alignment check) | In ring 0, this is the SMAP override. Leaving it set would switch SMAP off for the whole syscall |

`IF` deserves a second look. Because it is in the mask, **the kernel runs every syscall with interrupts disabled**, from the first instruction of the stub to the `sysretq` at the end, and nothing in GatOS turns them back on in between.

Keeping them off keeps the stub simple, since no interrupt can ever arrive while `rsp` is in a weird state. The cost is that a long syscall delays the timer. Blocking still works, as we will see when we get to the dispatcher.

>[!NOTE]
>`TF`, `NT` and `AC` were added to this mask while writing Chapter 9. Before that, it only cleared `IF` and `DF`, and a user program could set `AC`, make a syscall, and have the kernel touch user memory with SMAP switched off for the whole duration.

## The Entry Stub

Let's take stock of what the CPU hands us on the first instruction of `syscall_entry`:

- We are in ring 0, running kernel code
- `rsp` still points at the user's stack
- `rcx` holds the user's `rip`, and `r11` holds the user's `rflags`
- Every other register holds whatever the user program left in it (including the syscall number in `rax` and the arguments)
- `gs` still holds the *user's* GS base
- Interrupts are off, courtesy of `FMASK`

The stub has three jobs. It has to get onto a kernel stack, save the user's state in a form the rest of the kernel understands, and hand control to C. Then, on the way back, it has to undo all of that without leaving a single hole a program could crawl through. It is about a hundred lines of assembly, and we will go through all of it.

Getting onto a kernel stack takes three instructions:

```asm
syscall_entry:
    swapgs

    mov gs:[8], rsp
    mov rsp, gs:[0]
```

We have seen these three lines before, when we built `cpu_local`, so this is only a quick reminder. `swapgs` swaps the user's GS base with the kernel's, making `gs` point at `cpu_local`.

We then park the user's `rsp` at offset 8 of that struct, and load the current thread's kernel stack from offset 0. The scheduler updates offset 0 on every context switch (along with `rsp0` in the TSS), meaning `gs:[0]` always holds the top of the running thread's kernel stack.

From the fourth instruction on, we are on a stack we trust.

### Building a CPU Context

Now we need to save the user's registers. We could push them in whatever order we like, but there is a much better option. Since Chapter 6, every interrupt in GatOS builds a `cpu_context_t` on the stack:

```c
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector_number;
    uint64_t error_code;
    uint64_t iret_rip;
    uint64_t iret_cs;
    uint64_t iret_flags;
    uint64_t iret_rsp;
    uint64_t iret_ss;
} cpu_context_t;
```

If the syscall stub builds the *same* structure, then the rest of the kernel never has to care how a thread entered it. This matters more than you'd think. A thread can block in the middle of a syscall (waiting for a keypress, say).

When it does, the scheduler switches away from it. To resume it later, the scheduler has to find its saved state, and it would be a real pain if that state had two possible layouts depending on how the thread came in.

The bottom of a `cpu_context_t` is the iret frame. On an interrupt, the CPU pushes it by itself. `syscall` pushes nothing, meaning we have to fake it:

```asm
push 0x1b               // iret_ss    = USER_DS (RPL=3)
push qword ptr gs:[8]   // iret_rsp   = user RSP
push r11                // iret_flags = user RFLAGS
push 0x23               // iret_cs    = USER_CS (RPL=3)
push rcx                // iret_rip   = user RIP
```

These are the five values an interrupt from ring 3 would have pushed, in the same order. The segments are constants (we know where the program came from), `rflags` and `rip` come from the two registers `syscall` stashed them in, and the user `rsp` comes back out of `gs:[8]`.

That last one is worth a moment. Why copy the user `rsp` onto the stack, when it is already sitting safely in `cpu_local`? Because `cpu_local` is not safe at all once the thread blocks. There is only one `cpu_local` (one per core, on a multicore kernel).

The very next thread to make a syscall overwrites `gs:[8]` with *its* user `rsp`. By the time our thread wakes up, the slot belongs to someone else. The copy on the kernel stack, on the other hand, belongs to this thread and nobody else.

Next come the two interrupt metadata fields:

```asm
push 0                  // error_code
push 0x80               // vector_number = 0x80 (I guess this is a sentinel?)
```

A syscall has no error code. It gets a zero. The vector number is a fake as well. Nothing in GatOS actually checks for `0x80`, but it makes a syscall frame easy to recognize in a debugger or a crash dump.

>[!NOTE]
>Fun fact! `0x80` is a nod to `int 0x80`, the old Linux syscall vector from earlier in this chapter. To this day, 64-bit Linux still supports `int 0x80` for running old 32-bit programs.

Finally, the general purpose registers, in the same order Chapter 6's interrupt stub pushes them:

```asm
push rax
push rbx
push rcx
// ... rdx, rsi, rdi, rbp, r8 - r14 ...
push r15
```

At this point, `rsp` points at a complete, valid `cpu_context_t`. In total, we pushed 5 values for the iret frame, 2 for the metadata and 15 registers, for 22 values, or 176 bytes. Keep that number in mind for the next section.

>[!IMPORTANT]
>Notice that `rcx` and `r11` get saved twice: once inside the iret frame (as `iret_rip` and `iret_flags`) and once as plain registers. The plain copies are useless, since `syscall` already overwrote the user's real `rcx` and `r11` before we got here. They are pushed anyway, only so the layout matches `cpu_context_t` field for field. This is also why every syscall ABI on x86-64 (GatOS's included) says that `rcx` and `r11` are destroyed by a syscall. They really are, by the CPU itself.

### Calling into C

The dispatcher takes a pointer to the context, which is simply the current `rsp`:

```asm
mov rdi, rsp

mov rbx, rsp
and rsp, -16
call syscall_dispatcher
mov rsp, rbx
```

`rdi` is the first argument register in the System V ABI, meaning the dispatcher receives `regs` pointing straight at the stack. Any change it makes to `regs->rax` (the return value) or to the iret frame ends up in the registers the program gets back.

The `and rsp, -16` is there because of an ABI rule we have not needed until now. System V requires `rsp` to be a multiple of 16 right before every `call`. The `call` itself then pushes an 8 byte return address. A function therefore always starts with `rsp` 8 bytes off a 16 byte boundary. The compiler counts on that when it places local variables.

`and rsp, -16` clears the bottom four bits, rounding `rsp` down to the nearest multiple of 16. We save the original in `rbx` first so we can put it back after the call. `rbx` is callee saved (the dispatcher must return it unchanged). Its user value is already safe in the context, making it free to use.

This used to look different:

```asm
// BROKEN
// SysV ABI needs rsp ≡ 8 (mod 16) before calling, so adjust
sub rsp, 8
call syscall_dispatcher
add rsp, 8
```

The comment got the rule backwards. Remember that 176 bytes from earlier? The kernel stacks come from `kmalloc` (16 byte aligned) and are 16 KiB long. The top of every kernel stack is therefore a multiple of 16. 176 is `11 * 16`, and after all 22 pushes, `rsp` is *still* a multiple of 16.

It was already correct for the `call`. The `sub rsp, 8` then knocked it off by 8, and every syscall in GatOS ran its C code on a misaligned stack.

It never crashed. The instructions that actually fault on a misaligned stack are SSE loads and stores like `movaps`, and the kernel is compiled without SSE (`-mno-sse`, which keeps it away from the FPU registers entirely, a topic for Chapter 14).

It was still a bug waiting for someone to turn SSE back on, or for a compiler upgrade to rely on the alignment in some new way. It was found while writing this section, and a test now records the alignment of the dispatcher's frame on every syscall.

## Two Ways Out

When the dispatcher returns, we have to get back to ring 3. There are two instructions that can do it: `sysretq`, which is fast, and `iretq`, which is safe. GatOS uses both, and the choice is made right away:

```asm
mov rcx, [rsp + CTX_IRET_RIP]
mov rax, rcx
shl rax, 16
sar rax, 16
cmp rax, rcx
jne .slow_exit
```

Before we can understand what this checks, we need to talk about one of the more famous CPU bugs of the last decades.

### The SYSRET Problem

x86-64 addresses are 64 bits wide, but only 48 of them are used. Bits 48 to 63 must be copies of bit 47, and an address that follows this rule is called **canonical**. Everything from `0x0000000000000000` to `0x00007FFFFFFFFFFF` is canonical (the lower half, where user programs live), and so is everything from `0xFFFF800000000000` up (the higher half, where the kernel lives). The huge hole in between is not.

Now, consider a user program that maps a page at the very top of the lower half, and places a `syscall` instruction in its last two bytes, at `0x00007FFFFFFFFFFE`. The CPU executes it and saves the address of the *next* instruction in `rcx`: `0x0000800000000000`. That address is not canonical. It is the first address of the hole.

The syscall runs. Eventually, the kernel reaches `sysretq` with that value in `rcx`. On AMD CPUs, `sysretq` switches to ring 3 first and *then* notices the bad `rip`. The program gets a General Protection fault in ring 3, and only the program suffers.

On Intel CPUs, the check happens *before* the switch, meaning the General Protection fault fires in **ring 0**. By then, the stub has already restored the user's registers, loaded the user's `rsp` and executed `swapgs`. The kernel's fault handler starts running on a stack the user chose, with the user's GS base active.

That is a full privilege escalation. And it was very real. Published in 2012 as CVE-2012-0217, it affected FreeBSD, NetBSD, Xen and Windows, among others. Linux had dodged it years earlier, by doing more or less what GatOS does.

The fix is to never hand `sysretq` a non canonical address. The four instructions above check it: shifting left by 16 throws away the top 16 bits, and an arithmetic shift right by 16 fills them back in with copies of bit 47. If the result is the same as the original, the address was already canonical. For our program:

```
rcx            = 0x0000800000000000
shl rax, 16    = 0x8000000000000000
sar rax, 16    = 0xFFFF800000000000   (bit 47 was 1, so it is copied up)
cmp rax, rcx   -> not equal, jump to .slow_exit
```

Note that this check runs *before* the GPRs are restored, while every register is still kernel scratch. It used to run after, using `r10` as its scratch register, after `r10` had already been restored to the user's value. The result was that every single syscall returned with `r10` overwritten by the user's `rip`.

Optimized builds keep values in `r10` all the time. One of the demo programs eventually started dying with "invalid buffer" errors that made no sense, in release builds only. The culprit turned out to be a few instructions in the wrong order.

### The Fast Path

If the address is canonical, we leave through `sysretq`:

```asm
RESTORE_GPRS
add rsp, 16             // skip vector_number and error_code

mov rcx, [rsp]          // iret_rip
mov r11, [rsp + 16]     // iret_flags
mov rsp, [rsp + 24]     // iret_rsp
swapgs
sysretq
```

`RESTORE_GPRS` pops the 15 registers back (the mirror of the pushes, with `rax` coming back holding the return value the dispatcher wrote into `regs->rax`). After skipping the two metadata fields, `rsp` points at the iret frame. `sysretq` expects the return address in `rcx` and the flags in `r11`. We load both from the frame, not from the GPR pops we just did. If the dispatcher (or the scheduler) changed the iret frame, the frame is the truth.

The last two moves are the scary part. Once `mov rsp, [rsp + 24]` runs, we are in ring 0 with a user stack, and after `swapgs`, with a user GS base too. That is the same state we started in, and it is fine for the same reason. Interrupts are still off from `FMASK`. Nothing can sneak in during those two instructions.

>[!NOTE]
>"Nothing" is not completely true. A non-maskable interrupt (NMI) ignores `IF` by definition, and one that lands in that two instruction window would run its handler with the user's GS base. In GatOS, an NMI means something is badly broken (it is a fatal error). We can live with that. Kernels that use NMIs for things like profiling have to work quite a bit harder here.

The slow path restores the registers the same way and leaves through `iretq`:

```asm
.slow_exit:
    RESTORE_GPRS
    add rsp, 16
    swapgs
    iretq
```

`iretq` does not care about canonical `rip` values any more than `sysretq` does, but it handles them correctly on every CPU. It switches to ring 3 using the full iret frame, and then the CPU faults trying to fetch from a non canonical `rip`, in ring 3, where the fault belongs. The program crashes. Only a malicious (or extremely unlucky) program ever takes this path, so its cost doesn't matter.

## The Syscall ABI

Before looking at the C side, we need to agree on which register carries what. This agreement is the **syscall ABI**, and both sides (the kernel and `ulibc`) have to follow it to the letter:

| Register | On the way in | On the way out |
| --- | --- | --- |
| `rax` | syscall number | return value |
| `rdi` | argument 1 | preserved |
| `rsi` | argument 2 | preserved |
| `rdx` | argument 3 | preserved |
| `rcx`, `r11` | (anything) | destroyed by the CPU |
| everything else | (anything) | preserved |

The numbers themselves are plain `#define`s in [`syscall.h`](/src/kernel/sys/syscall.h), duplicated in `ulibc`'s [`syscalls.h`](/src/ulibc/syscalls.h) (`SYS_EXIT` is 1, `SYS_WRITE` is 2, and so on). On the user side, every syscall goes through a tiny inline wrapper:

```c
userspace static inline uint64_t sc1(uint64_t num, uint64_t arg1) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

`"a"(num)` puts the number in `rax`, `"D"(arg1)` puts the argument in `rdi`, and `"=a"(ret)` reads the result back out of `rax`. The clobber list is the important part. It tells the compiler that `rcx` and `r11` will not survive the `syscall`, and that memory may have changed (the kernel might have written into a buffer we passed it).

The compiler may assume that everything not listed survives untouched. The `r10` bug from the previous section broke that assumption. There are `sc0` through `sc3` variants for zero to three arguments, and functions like `write` or `mmap` in `ulibc` are thin wrappers around them.

>[!NOTE]
>Linux uses the same registers, plus `r10`, `r8` and `r9` for arguments 4 to 6. The odd one out is `r10`, which replaces the `rcx` a normal function call would use for argument 4. You already know why: `syscall` destroys `rcx`. GatOS never needs more than three arguments. It stops at `rdx`.

## The Dispatcher

On the C side, `syscall_dispatcher` receives the `cpu_context_t` the stub just built, and is essentially one big `switch`:

```c
void syscall_dispatcher(cpu_context_t* regs) {
    thread_t* current = sched_current();
    if (!current) return;

    uint64_t syscall_num = regs->rax;

    switch (syscall_num) {
        case SYS_EXIT:
            sched_exit();
            break;

        // ...

        default:
            LOGF("[SYSCALL] Unknown syscall: %lu from thread '%s' (PID %u)\n", ...);
            sched_exit();
            break;
    }
}
```

The arguments are read straight out of the saved registers (`regs->rdi`, `regs->rsi`, `regs->rdx`). Results go into `regs->rax`, and the stub pops that into the real `rax` on the way out. A syscall number the kernel does not know is not treated as an error code. The thread that made it gets killed on the spot, since a program sending garbage syscall numbers is broken beyond saving.

Remember that interrupts are off for the whole syscall. So how does a syscall like `SYS_SLEEP_MS` ever give the CPU to someone else? It calls `sched_sleep`, which ends up in `sched_yield`, which is just this:

```c
__asm__ volatile("int $32");
```

That is a *software* interrupt to vector 32, the timer's vector. `int` works regardless of `IF`. The scheduler runs, saves our thread's context like it would on a real timer tick, and switches to another thread.

When our thread is eventually picked again, it resumes right after the `int`, back inside the syscall, with interrupts still off. Chapter 14 covers the scheduler side of this in full. For now, it is enough to know that a syscall can block without ever enabling interrupts.

## Trusting Nothing

Most syscalls take a pointer. `write` takes a buffer to print, `read` takes a buffer to fill. And a pointer from a user program is the single most dangerous thing a kernel ever receives. Why?

Consider the most naive `SYS_WRITE` possible:

```c
// BROKEN
case SYS_WRITE: {
    const char* buf = (const char*)regs->rdi;
    size_t len = (size_t)regs->rsi;
    tty_write(current->process->tty, buf, len);
    regs->rax = len;
    break;
}
```

Now imagine a program that calls `write` with `buf = 0xFFFFFFFF80000000` (the start of the kernel image) and `len = 65536`. The kernel reads its own memory (it is allowed to) and prints it on the program's screen.

Kernel code, kernel data, the contents of other processes' pages through the physmap, whatever the attacker likes. The `read` direction is even worse: a program could ask the kernel to write keyboard input *into* kernel memory, over a function pointer of its choosing.

This attack has a name. The kernel is a **confused deputy**: it holds a privilege (access to all of memory) that it uses on behalf of someone who does not hold it, without checking whether that someone should be allowed to.

We have already built both defenses. `vmm_check_buffer` looks a range up in a process's VMM and checks that every byte of it is mapped with the flags we ask for, while SMAP makes the CPU fault whenever the kernel touches a user page without explicitly opening the hatch. `copy_user` puts the two together:

```c
static bool copy_user(vmm_t* vmm, void* dst, const void* src, size_t n, bool to_user) {
    uintptr_t user = (uintptr_t)(to_user ? dst : src);
    size_t need = VM_FLAG_USER | (to_user ? VM_FLAG_WRITE : 0);

    for (size_t done = 0; done < n; ) {
        size_t c = n - done < 1024 ? n - done : 1024;
        bool ints = intr_save();
        if (!vmm_check_buffer(vmm, (void*)(user + done), c, need)) {
            intr_restore(ints);
            return false;
        }
        smap_allow();
        kmemcpy((uint8_t*)dst + done, (const uint8_t*)src + done, c);
        smap_deny();
        intr_restore(ints);
        done += c;
    }
    return true;
}
```

The `to_user` flag picks the direction. Exactly one side of the copy is a user pointer. That side is the one we check. Copying *from* a user buffer only requires it to be a user mapping, while copying *into* one also requires it to be writable (otherwise a program could have the kernel scribble over its own read-only code, something the CPU would never let the program do itself).

The copy itself is wrapped in `smap_allow` and `smap_deny`, the `stac` and `clac` pair. This is the only place in the syscall path where the hatch is open, and only for one `kmemcpy` at a time.

The loop is the interesting part. Why copy in 1 KiB chunks, re-checking each one, instead of checking the whole buffer once and copying it in one go?

Because a check is only as good as the moment it is made. If the kernel checks a buffer and then does anything at all before using it, the answer may already be stale. Another thread of the same process could `munmap` the buffer in between.

The kernel would then copy from (or into) memory that is no longer the program's. Even worse, the freed pages could have already been handed to someone else. This is a classic bug class, called **time of check to time of use** (TOCTOU).

`copy_user` closes it by making the check and the use atomic. Each chunk is checked and copied with interrupts off. Nothing can run between the check and the copy. The chunking keeps every stretch with interrupts off short. In GatOS today, syscalls already run with interrupts off, and `intr_save` usually finds nothing to do. `copy_user` does not rely on that, though. It stays correct no matter who calls it, or when.

>[!NOTE]
>Linux does this differently. Its `copy_from_user` does not check page tables at all. It checks that the pointer is in the user half of the address space, then simply attempts the copy, and if it page faults, the fault handler recognizes the faulting instruction (through a special table of "allowed to fault" addresses) and makes `copy_from_user` return an error.

Linux's way is faster, but it takes a lot more machinery than we need.

## The Syscalls

With `copy_user` in hand, each syscall is mostly bookkeeping. We will go through the important ones.

### SYS_WRITE

```c
case SYS_WRITE: {
    const char* buf = (const char*)regs->rdi;
    size_t len = (size_t)regs->rsi;
    tty_t* tty = current->process ? current->process->tty : NULL;
    if (!buf || len == 0 || !current->process) {
        regs->rax = (uint64_t)-1;
        break;
    }
    if (len > 65536) len = 65536;

    char kbuf[4096];
    bool held = tty_lock_output(tty);
    for (size_t done = 0; done < len; ) {
        size_t block = len - done < sizeof(kbuf) ? len - done : sizeof(kbuf);
        if (!copy_user(current->process->vmm, kbuf, buf + done, block, false)) {
            tty_unlock_output(tty, held);
            LOGF("[SYSCALL] SYS_WRITE: Invalid buffer pointer ...");
            sched_exit();
        }
        if (tty) tty_write(tty, kbuf, block);
        done += block;
    }
    tty_unlock_output(tty, held);
    regs->rax = (uint64_t)len;
    break;
}
```

We first reject the obviously wrong cases (a null buffer, zero bytes, or a thread with no process behind it) with a return value of `-1`, and cap a single write at 64 KiB. A program that wants to print more simply calls `write` again, and no single write can keep the kernel busy for long.

The data then goes through a 4 KiB buffer on the kernel stack, one block at a time. Each block is copied in with `copy_user` and handed to the TTY. The kernel never prints straight out of user memory. SMAP is only ever open for one chunk of `copy_user` at a time.

`tty_lock_output` makes the process the owner of its TTY's output for the whole write. Without it, two threads printing at once could interleave their 4 KiB blocks, and a long line from one thread would come out with the other's text in the middle of it. Chapter 13 covers how the ownership works.

If `copy_user` fails, the program passed us a bad pointer. We let go of the TTY and kill the thread. There is no error code to return here, since a program that passes garbage to `write` is not going to handle an error from `write` gracefully either.

### SYS_READ

```c
case SYS_READ: {
    char* buf = (char*)regs->rdi;
    size_t count = (size_t)regs->rsi;
    // ... same null checks as SYS_WRITE ...

    if (count > 4096) count = 4096;
    if (!vmm_check_buffer(current->process->vmm, buf, count, VM_FLAG_USER | VM_FLAG_WRITE)) {
        regs->rax = (uint64_t)-1;
        break;
    }

    char kbuf[4096];
    size_t n = tty_read(tty, kbuf, count);
    if (!copy_user(current->process->vmm, buf, kbuf, n, true)) sched_exit();
    regs->rax = (uint64_t)n;
    break;
}
```

The mirror image, with one twist. `tty_read` can block for a very long time (until the user presses Enter). Before waiting, we check the buffer once up front with `vmm_check_buffer`. A program passing a bad buffer gets `-1` immediately, instead of sitting through a whole line of typing before being killed.

That early check does not replace the one in `copy_user`. While we sat in `tty_read`, the program had plenty of time to unmap the buffer. `copy_user` checks it again right before copying. This is the TOCTOU problem from the previous section in its purest form. If the second check fails, the thread is killed, taking the line it read along with it.

### SYS_MMAP and SYS_MUNMAP

```c
case SYS_MMAP: {
    void* addr = (void*)regs->rdi;
    size_t length = (size_t)regs->rsi;
    size_t vm_flags = (size_t)regs->rdx;

    size_t user_allowed_flags = VM_FLAG_WRITE | VM_FLAG_EXEC | VM_FLAG_LAZY;
    vm_flags &= user_allowed_flags;

    if ((vm_flags & VM_FLAG_WRITE) && (vm_flags & VM_FLAG_EXEC)) {
        regs->rax = (uint64_t)-1;
        break;
    }
    // ... vmm_alloc_at if addr was given, vmm_alloc otherwise, always with VM_FLAG_USER ...
}
```

This is how userspace gets memory. The `ulibc` heap gets all of its arenas through it. The flags are the usual VMM flags, but the program only gets to pick from three of them.

`VM_FLAG_FOREIGN` (map someone else's physical memory) and `VM_FLAG_DEVICE` (uncacheable device memory) are stripped, since either one in user hands would let a program map any physical address it likes. `VM_FLAG_USER` is not taken from the program at all. It is added by the kernel, every time.

The second check rejects mappings that are both writable and executable. This rule is called **W^X** (write XOR execute). The NX bit is what lets the CPU enforce it. If a program could map memory that is both, then any bug that lets an attacker write data into it (say, an overflowing buffer) immediately becomes a bug that lets the attacker run code. With W^X, memory is either data or code, never both.

When `addr` is given, `vmm_alloc_at` checks that the whole range fits in the process's allocatable space, and that space never includes the kernel half. A program asking for a mapping at a kernel address simply gets `-1`.

`SYS_MUNMAP` hands the address to `vmm_free` and returns `0` on success, or `-1` if there was nothing mapped there. It used to return `0` no matter what, which is a small lie, but a lie nonetheless. The same pass also fixed `SYS_YIELD` and `SYS_SLEEP_MS`. Neither set `regs->rax` at all, and the program got back whatever was in `rax` when it entered: the syscall number itself.

### SYS_SET_FS_BASE

```c
case SYS_SET_FS_BASE: {
    uint64_t base = regs->rdi;
    if (base >= 0x0000800000000000ULL) {
        regs->rax = (uint64_t)-1;
        break;
    }
    current->fs_base = base;
    write_msr(MSR_FS_BASE, base);
    regs->rax = 0;
    break;
}
```

`FS` is the twin of `GS`, a segment with a hidden 64-bit base that any instruction can use with an `fs:` prefix. User programs use it for **thread local storage**: every thread gets its own small block of variables, with `fs` pointing at the current thread's block. The same `fs:[16]` then reads a different variable in every thread. The scheduler restores `fs_base` on every context switch. This syscall is how a program sets it in the first place.

The check refuses anything at or above `0x0000800000000000`. That covers two problems at once. Kernel addresses would be harmless today (a ring 3 access through them just page faults), but there is no reason for the kernel to accept one. Non canonical addresses are the real problem, because they make `wrmsr` raise a General Protection fault. That fault would happen in ring 0, which is not where we want a user program's mistakes to end up.

The remaining syscalls are short. `SYS_EXIT` kills the current thread. `SYS_TIME_NS` returns the uptime in nanoseconds, straight from the calibrated TSC. `SYS_TTY_CTRL` is a small grab bag of terminal commands (clear the screen, get its size, switch between line and raw input), which will make sense once Chapter 13 introduces TTYs properly.

`SYS_POWEROFF` and `SYS_REBOOT` do what they say, and they do it for anyone who asks. There is no notion of users or permissions in GatOS. Any program can turn the machine off. It's a hobby kernel, after all.

## The PS/2 Controller

Programs can talk to the kernel now. The other half of this chapter is about the user talking to the programs. That starts with the keyboard.

Almost every keyboard you can buy today is a USB keyboard, and Chapter 12 will deal with those properly. USB, however, is an enormous amount of work (a PCI driver, a host controller driver, a whole protocol stack) before the first keypress arrives. Luckily, there is a much older and much simpler way in, and practically every PC still supports it: the **PS/2 controller**.

The original IBM PC/AT (1984) had a small Intel 8042 microcontroller on its motherboard, whose job was to talk to the keyboard over a serial line and hand the CPU one byte at a time.

That chip is long gone, but its interface lives on. Modern chipsets emulate it, and when there is no PS/2 keyboard plugged in at all, the firmware usually translates USB keyboard input into fake PS/2 traffic (a feature called "USB legacy support"). QEMU emulates a PS/2 keyboard by default. The controller is often still called "the 8042" or "i8042" because of all this, which is where [`i8042.c`](/src/kernel/drivers/i8042.c) gets its name.

The controller has two ports, one for a keyboard and one for a mouse (the second port came later, with IBM's PS/2 line, hence the name). From the CPU's side, the whole thing is just two I/O ports:

- `0x60` is the **data port**. Reading it gives us a byte from the controller (or from a device behind it), and writing it sends a byte the other way.
- `0x64` is the **status register** when read, and the **command register** when written. Same port number, two completely different registers, depending on the direction.

The status register at `0x64` is the one we will check constantly. Its most useful bits are:

```
Bit 0  Output buffer full   A byte is waiting for us at 0x60
Bit 1  Input buffer full    The controller has not taken our last byte yet
Bit 5  Aux output           The waiting byte came from port 2 (the mouse)
```

>[!IMPORTANT]
>The names "input" and "output" are from the *controller's* point of view. From ours, they read backwards. The **output** buffer holds bytes the controller *outputs* to us, meaning "output full" is "there is something to read". The **input** buffer holds bytes we *input* to it, and "input full" is "still busy with our last byte, wait before writing".

Everything the driver does is built on two waits:

```c
static bool wait_status(uint8_t mask, uint8_t want, uint32_t us) {
    for (uint32_t i = 0; i < us / 100; i++) {
        if ((inb(PS2_STATUS_PORT) & mask) == want) {
            return true;
        }
        sleep_us(100);
    }
    return false;
}

bool i8042_wait_read(void) {
    return wait_status(PS2_STATUS_OUTPUT_FULL, PS2_STATUS_OUTPUT_FULL, I8042_TIMEOUT_US);
}

bool i8042_wait_write(void) {
    return wait_status(PS2_STATUS_INPUT_FULL, 0, I8042_TIMEOUT_US);
}
```

`wait_status` polls the status register every 100 microseconds until the bits under `mask` look like `want`, or until it runs out of time. Reading waits for "output full" to be set, writing waits for "input full" to be clear. The default timeout, `I8042_TIMEOUT_US`, is 100 ms. For this chip, that is an eternity. A controller that has not answered by then is not going to.

The timeout matters more than it seems. Some machines have no PS/2 controller at all, and reading a status port with nothing behind it usually returns `0xFF`, with every bit set. Without a timeout, `i8042_wait_write` would spin forever waiting for "input full" to clear. The boot would hang right there.

On top of the waits sit three tiny helpers: `i8042_write_command` waits and writes to `0x64`, `i8042_write_data` waits and writes to `0x60`, and `i8042_read_data` waits and reads from `0x60` (returning `0` if nothing ever arrives).

### Bringing It Up

`i8042_init` follows the classic initialization sequence. We will go through it in order.

```c
i8042_write_command(PS2_CMD_DISABLE_PORT1);
i8042_write_command(PS2_CMD_DISABLE_PORT2);
i8042_flush();
```

First, we shut both ports off (commands `0xAD` and `0xA7`). We don't want any device talking to us while we reconfigure the controller. Then we throw away anything already sitting in the output buffer, like a keypress from while GRUB was showing its menu. `i8042_flush` simply reads `0x60` until "output full" clears.

```c
i8042_write_command(PS2_CMD_READ_CONFIG);
uint8_t config = i8042_read_data();

config |= PS2_CFG_PORT1_INT | PS2_CFG_PORT1_TRANS | PS2_CFG_PORT2_CLOCK;
config &= ~(PS2_CFG_PORT1_CLOCK | PS2_CFG_PORT2_INT);

i8042_write_command(PS2_CMD_WRITE_CONFIG);
i8042_write_data(config);
```

Next comes the **configuration byte**, which holds the controller's settings. Command `0x20` reads it, command `0x60` writes it. We read it, change only the bits we care about, and write it back:

| Bit | Meaning | We set it to |
| --- | --- | --- |
| 0 | Port 1 raises an interrupt (IRQ 1) when a byte arrives | 1 |
| 1 | Port 2 raises an interrupt (IRQ 12) when a byte arrives | 0 |
| 4 | Port 1 clock **disabled** | 0 |
| 5 | Port 2 clock **disabled** | 1 |
| 6 | Port 1 translation | 1 |

Bits 4 and 5 are inverted. A *set* bit turns the port's clock off, and a device without a clock cannot send anything. We clear bit 4 (so the keyboard can talk) and set bit 5 (so the mouse cannot).

Translation (bit 6) is another piece of history. We will get to it in the next section, when we look at what the keyboard actually sends.

>[!NOTE]
>This byte used to be wrong. The old code cleared *both* clock bits, turning the mouse port's clock **on**. It also left the port 2 interrupt bit at whatever the firmware had set it to. On a machine with a real PS/2 mouse that had been told to report movement, mouse bytes would start arriving in the same output buffer as the keyboard's.
>
>Either the keyboard handler would read them as keypresses, or (with nobody listening on IRQ 12) the first mouse byte would sit in the buffer forever and block the keyboard behind it. QEMU's mouse stays quiet until someone enables it. That's why this never showed up.

```c
i8042_write_command(PS2_CMD_TEST_CONTROLLER);
uint8_t self_test = i8042_read_data();
if (self_test != 0x55) {
    LOGF("[PS2 ERROR] Controller self-test failed (0x%x).\n", self_test);
    return false;
}

i8042_write_command(PS2_CMD_WRITE_CONFIG);
i8042_write_data(config);
```

Command `0xAA` asks the controller to test itself, and a healthy one answers `0x55`. If it doesn't, we give up on PS/2 and boot without a keyboard (Chapter 12's USB keyboard may still save us).

The config byte is then written a *second* time. Some controllers reset themselves as part of the self-test, which quietly throws away the configuration we wrote a moment ago. Writing it again costs one command and makes sure the controller is in the state we chose. The old code skipped this, and nothing in QEMU cares, but real hardware does.

```c
i8042_write_command(PS2_CMD_ENABLE_PORT1);

i8042_write_data(0xFF);
uint8_t ack = i8042_read_data();
uint8_t bat = wait_status(PS2_STATUS_OUTPUT_FULL, PS2_STATUS_OUTPUT_FULL, I8042_BAT_US) ? inb(PS2_DATA_PORT) : 0;
if (ack != 0xFA || bat != 0xAA) {
    LOGF("[PS2] Keyboard reset answered 0x%x 0x%x (want 0xFA 0xAA).\n", ack, bat);
}

i8042_write_data(0xF4);
i8042_read_data(); // ACK
```

With the controller ready, we turn port 1 back on (`0xAE`) and start talking to the keyboard itself. Writing to `0x60` with no command in front of it sends the byte straight through to the device on port 1.

`0xFF` tells the keyboard to reset. It answers in two steps: `0xFA` (ACK, "I got your command"), then, after running its own self-test, `0xAA` for success (or `0xFC` for failure). That self-test is called **BAT**, the Basic Assurance Test.

The second wait has its own timeout, `I8042_BAT_US`. A full second. A real keyboard can take 500 to 750 ms to finish BAT, while the old code waited the usual 100 ms. On a real keyboard, the read timed out and the `0xAA` arrived later, where the next step would read it as the ACK for `0xF4`. QEMU's keyboard answers instantly. Only real hardware ever noticed.

Finally, `0xF4` tells the keyboard to start sending keypresses ("enable scanning"). We eat its ACK.

## Scancodes

So what does a keyboard actually send when you press a key?

Not characters. A keyboard has no idea what a letter is. It sends a **scancode**, a number that identifies a physical key. One goes out when the key goes down, and another when it comes back up. The down one is the **make code** and the up one is the **break code**. Turning "the third key in the second row went down while the left shift key was held" into the character `E` is entirely the kernel's job.

There are three different scancode sets, for historical reasons. Set 1 is from the original IBM PC/XT, set 2 came with the PC/AT, and set 3 was barely ever used. Modern keyboards speak set 2 by default. However, a lot of old software was written for set 1. To keep it working, the 8042 learned to **translate** set 2 into set 1 on the fly.

That is bit 6 of the config byte, the one we turned on. Set 1 also happens to be the easier set to decode, and if the controller offers to do the translating for us, why say no?

In set 1, the rules are simple:

- Every key has a one byte make code, below `0x80` (the `A` key is `0x1E`)
- Its break code is the same byte with bit 7 set (`A` released is `0x9E`)

Typing a capital `A` with the left shift key looks like this on the wire:

```
0x2A   Left Shift pressed
0x1E   A pressed
0x9E   A released    (0x1E | 0x80)
0xAA   Left Shift released    (0x2A | 0x80)
```

Four bytes, four interrupts. Of these, only the second one produces a character, and only because the first one told us shift was down.

### Extended Keys

The original PC keyboard had 83 keys. When IBM's Enhanced keyboard added the arrow keys, a second Ctrl and Alt, the keypad's Enter and friends, there were not enough single byte codes left for all of them. Many of the new keys also duplicated old ones. The solution was a prefix: an extended key sends `0xE0` first, then a code that often matches an existing key.

```
0x1D        Left Ctrl pressed
0xE0 0x1D   Right Ctrl pressed
0xE0 0x9D   Right Ctrl released

0x48        Keypad 8 pressed
0xE0 0x48   Up arrow pressed
```

The Up arrow shares its code with keypad 8 (look at a full size keyboard, and you'll find an up arrow printed on it). Only the prefix tells them apart.

Then there is Pause. Pause is the odd one out, because it does not send a make and a break code at all. When you press it, the keyboard sends all of this at once, and nothing when you let go:

```
0xE1 0x1D 0x45 0xE1 0x9D 0xC5
```

It has its own prefix, `0xE1`. The bytes after it look *exactly* like Ctrl pressed (`0x1D`), NumLock pressed (`0x45`), Ctrl released (`0x9D`) and NumLock released (`0xC5`).

The rest of the kernel does not deal in scancodes. It deals in `keycode_t`, an enum of every key we know about:

```c
typedef enum {
    KEY_UNKNOWN = 0,
    KEY_ESC,
    KEY_1, KEY_2, /* ... */
    KEY_F11 = 0x57, KEY_F12 = 0x58,

    // Extended keys
    KEY_KPENTER = 0x9C,
    KEY_RIGHT_CTRL = 0x9D,
    KEY_UP = 0xC8,
    // ...
} keycode_t;
```

The values are not arbitrary. For a normal key, the keycode is its set 1 make code: `KEY_ESC` is 1, because Escape is scancode `0x01`.

For an extended key, the keycode is its make code with bit 7 set: Right Ctrl is `0xE0 0x1D`, so `KEY_RIGHT_CTRL` is `0x1D | 0x80 = 0x9D`, and the Up arrow is `0x48 | 0x80 = 0xC8`. Bit 7 can never be confused with a break code here, since by the time a keycode exists, the break bit has already been stripped off.

The non extended keys go through a lookup table, `scancode_set1`, indexed by make code. With the values lined up like this, the table is mostly the identity, but it keeps the door open for keys we do not want to report (`KEY_UNKNOWN` entries).

## Decoding Keypresses

All of the decoding happens in `keyboard_feed`, which takes one byte at a time. It has to remember what it saw before (a pending `0xE0`, or which modifiers are held), which makes it a small state machine:

```c
bool keyboard_feed(uint8_t scancode, key_event_t* out) {
    if (e1_skip) {
        e1_skip--;
        return false;
    }

    if (scancode == 0xE1) {
        e1_skip = 5;
        return false;
    }

    if (scancode == 0xE0) {
        extended = true;
        return false;
    }

    bool pressed = !(scancode & 0x80);
    uint8_t code = scancode & 0x7F;

    keycode_t key = KEY_UNKNOWN;
    if (extended) {
        key = (keycode_t)(code | 0x80);
        extended = false;
    } else if (code < sizeof(scancode_set1)/sizeof(scancode_set1[0])) {
        key = scancode_set1[code];
    }

    if (key == KEY_UNKNOWN) return false;
    // ... modifiers and locks ...

    *out = (key_event_t){ .keycode = key, .pressed = pressed, .modifiers = modifiers, .locks = locks };
    return true;
}
```

It returns `true` only when a byte completes a key event, and fills in `out`. Prefix bytes return `false` and just change the state.

We start with the prefixes. `0xE1` (Pause) arms `e1_skip`. The five bytes after it get swallowed without a trace. `0xE0` sets `extended`, marking the *next* byte as an extended key.

For every other byte, bit 7 says whether the key went up or down, and the low 7 bits identify it. If the previous byte was `0xE0`, we form the extended keycode and clear the flag. Otherwise, we look the code up in the table (checking the bounds first, since set 1 has some codes beyond the end of our table).

>[!NOTE]
>Pause used to toggle NumLock. Before `e1_skip` existed, `0xE1` fell through the table as an unknown key and was ignored, but the five bytes after it were not. `0x1D` was decoded as Left Ctrl being pressed, `0x45` as NumLock being pressed (toggling NumLock and its LED), and then both were "released".
>
>Every press of Pause flipped NumLock and sent a stray Ctrl. It went unnoticed for so long because GatOS does not use NumLock for anything yet.

### Modifiers and Locks

Some keys do not produce characters, but change what *other* keys produce. GatOS tracks two kinds:

```c
switch (key) {
    case KEY_LEFT_SHIFT: pressed ? (modifiers |= MOD_LSHIFT) : (modifiers &= ~MOD_LSHIFT); break;
    case KEY_RIGHT_SHIFT: pressed ? (modifiers |= MOD_RSHIFT) : (modifiers &= ~MOD_RSHIFT); break;
    // ... the same for both Ctrls and both Alts ...

    case KEY_CAPSLOCK: lock_key(LOCK_CAPS, pressed); break;
    case KEY_NUMLOCK: lock_key(LOCK_NUM, pressed); break;
    case KEY_SCROLLLOCK: lock_key(LOCK_SCROLL, pressed); break;

    default: break;
}
```

**Modifiers** (Shift, Ctrl, Alt) are held: the bit is set while the key is down, and cleared when it comes back up. Left and right get separate bits, `MOD_LSHIFT` and `MOD_RSHIFT`, because some software cares which one you pressed. Code that does not care uses the combined masks, like `MOD_SHIFT = MOD_LSHIFT | MOD_RSHIFT`.

**Locks** (Caps, Num, Scroll Lock) toggle instead. Each press flips the bit, and releases are ignored. Or rather, each *first* press, which is what `lock_key` is for:

```c
static void lock_key(uint8_t bit, bool pressed) {
    if (pressed && !(lock_held & bit)) locks ^= bit;
    lock_held = pressed ? (lock_held | bit) : (lock_held & ~bit);
}
```

When you hold a key down, the keyboard repeats its make code ten or twenty times a second (that is how holding `a` types `aaaaaa`). There is no break code in between, just the same make code over and over. For a letter, that is what we want.

For Caps Lock, every repeat would flip the lock again, and holding it for a second would leave it in a random state. `lock_held` remembers which lock keys are currently down, and only a make code for a key that was *up* toggles anything.

The first version of the driver did not have this, and every repeat toggled the lock (along with an LED update each time). I had initially written it off as normal behavior. It is not, as a quick test on any other OS will show you.

Every event carries a *snapshot* of both at the moment it happened. Whoever receives the event never has to ask the keyboard driver what the state was. That will matter in Chapter 12, when a second keyboard (USB) starts producing events of its own.

### From Keycodes to Characters

Turning an event into a character is the job of `keycode_to_ascii`, in [`keyboard.h`](/src/kernel/drivers/keyboard.h):

```c
static inline char keycode_to_ascii(key_event_t event) {
    if (event.keycode > KEY_CAPSLOCK) return 0;

    bool shift = (event.modifiers & MOD_SHIFT) != 0;
    bool caps = (event.locks & LOCK_CAPS) != 0;

    bool upper = shift;
    if (event.keycode >= KEY_Q && event.keycode <= KEY_P) upper ^= caps;
    if (event.keycode >= KEY_A && event.keycode <= KEY_L) upper ^= caps;
    if (event.keycode >= KEY_Z && event.keycode <= KEY_M) upper ^= caps;

    return upper ? layout_us_qwerty_shift[event.keycode] : layout_us_qwerty[event.keycode];
}
```

There are two tables, one for each "level" of the US QWERTY layout (`1` and `!`, `q` and `Q`, and so on), indexed by keycode. Keys that do not produce characters have `0` in both, and anything past Caps Lock (function keys, the keypad, extended keys) returns `0` right away.

The interesting bit is how Shift and Caps Lock combine. Shift alone picks the upper level for every key. Caps Lock only affects letters. It *flips* the choice rather than forcing it. The three ranges are the three rows of letters on the keyboard (`Q` to `P`, `A` to `L`, `Z` to `M`), which are contiguous in the keycode enum. For the `A` key:

```
Shift   Caps Lock   upper                  result
no      off         false                  a
yes     off         true                   A
no      on          false ^ true = true    A
yes     on          true ^ true  = false   a
```

The last line is what every OS does. With Caps Lock on, Shift gives you lowercase letters. For a digit, the XOR never runs. Shift + `1` is `!` regardless of Caps Lock.

>[!NOTE]
>Tab and Escape map to `0` in both tables. For now, they never reach a program as characters. The input hub below uses them for hotkeys, and nothing in GatOS has needed them as text yet.

## The LEDs

When a lock key toggles, the light on the keyboard should follow. The LEDs are controlled by the kernel, not the keyboard, and setting them takes a small handshake:

```c
static void update_leds(void) {
    i8042_write_data(0xED);
    if (i8042_read_data() == 0xFA) {
        i8042_write_data(keyboard_led_byte(locks));
        i8042_read_data(); // consume ACK for the LED state byte
    }
}
```

We send the "set LEDs" command, `0xED`, and wait for the ACK (`0xFA`). Only then do we send the byte with the new LED state, and the keyboard ACKs that one too. If the first answer is anything other than an ACK (a keyboard can answer `0xFE`, "resend"), we skip the update. The lights will catch up on the next toggle.

The LED byte has its own bit order:

```c
uint8_t keyboard_led_byte(uint8_t l) {
    return ((l & LOCK_SCROLL) ? 1 : 0) | ((l & LOCK_NUM) ? 2 : 0) | ((l & LOCK_CAPS) ? 4 : 0);
}
```

The keyboard wants Scroll Lock in bit 0, Num Lock in bit 1 and Caps Lock in bit 2. Our `LOCK_` flags happen to be defined in the opposite order (`LOCK_CAPS` is bit 0). They have to be shuffled.

They were not, until recently. The old code sent `locks & 0x07` as is, and pressing Caps Lock lit up the Scroll Lock light (and the other way around). QEMU has no LEDs to look at. Nobody noticed.

## The Keyboard Interrupt

When the keyboard sends a byte, the controller puts it in the output buffer and raises IRQ 1. Chapter 10 set up the I/O APIC to route IRQ 1 to vector 33, and this is the handler that runs there:

```c
cpu_context_t* keyboard_handler(cpu_context_t* ctx) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) return ctx;

    uint8_t scancode = inb(PS2_DATA_PORT);

    if (status & PS2_STATUS_AUX_OUTPUT) return ctx;

    uint8_t before = locks;
    key_event_t ev;
    if (!keyboard_feed(scancode, &ev)) return ctx;

    if (locks != before) update_leds();
    input_handle_key(ev);

    return ctx;
}
```

We read the status first. If there is nothing to read, the interrupt was spurious, and we leave. Otherwise, we read the byte, and *then* check whether it came from port 2. A mouse byte still has to be read (to empty the buffer), but it is thrown away instead of being decoded as a keypress.

The byte then goes through `keyboard_feed`. If it completed a key event, we update the LEDs when a lock changed, and hand the event to the input hub.

Splitting `keyboard_feed` out of the handler also means the decoding logic does not touch any hardware. The tests can feed it made up scancodes directly. The Pause sequence, the extended keys, the Shift and Caps Lock table above and the LED bit order all have tests in [`test_input.c`](/src/tests/test_input.c) that do exactly that.

### The Drain

`keyboard_init` has one more piece, with a comment explaining it:

```c
void keyboard_init(void) {
    if (i8042_init()) {
        update_leds();

        /*
            Author's Note:

            Wait for any in flight PS/2 responses to arrive, then drain.
            Without this delay, an unread byte can hold IRQ1 high and
            prevent the IOAPIC from ever seeing an edge transition.

            Ask me how I know.
        */

        sleep_us(2000);
        while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            inb(PS2_DATA_PORT);
        }
    }
}
```

ISA interrupts are **edge triggered**: the I/O APIC raises an interrupt when the line *goes* from low to high, not while it *is* high. The PS/2 controller drives IRQ 1 high whenever a byte is waiting in its output buffer, and drops it once we read the byte.

Now suppose a late ACK from `update_leds` (or the keyboard's reset) lands in the output buffer *before* IRQ 1 is unmasked. The line goes high while the I/O APIC is not listening, and stays high, because nobody reads the byte.

When the pin is finally unmasked, there is no edge. The line is just sitting there, high. And since the byte is never read, the line never drops. The next keypress cannot cause an edge either. The keyboard is dead before the first key is ever pressed.

The fix is to wait 2 ms for any stragglers, then read until the buffer is empty. The line drops, and the first real keypress gets a clean edge.

## The Input Hub

The keyboard driver produces `key_event_t`s. Somebody has to decide where they go, and that somebody is [`input.c`](/src/kernel/drivers/input.c).

It might seem like overkill to give this its own file, since the keyboard driver could just call the TTY directly. But GatOS has two keyboard drivers. In Chapter 12, the USB keyboard driver will produce `key_event_t`s too, and both drivers call the same function, `input_handle_key`. The hub does not know or care which keyboard an event came from, and neither driver knows anything about TTYs, hotkeys or what is on the screen.

```c
void input_handle_key(key_event_t event) {
    if (!event.pressed) return;
    // ... hotkeys ...
    // ... route to the active TTY ...
}
```

The first thing it does is throw releases away. Nothing in GatOS reacts to a key going *up* yet.

### Hotkeys

Before an event reaches any program, the hub checks it against a few global hotkeys:

```c
if ((event.modifiers & MOD_CTRL) &&
    (event.modifiers & MOD_SHIFT) &&
    event.keycode == KEY_ESC) {
    dash_toggle();
    return;
}

if ((event.modifiers & MOD_ALT) && event.keycode == KEY_TAB) {
    tty_cycle();
    return;
}

if ((event.modifiers & MOD_ALT) && event.keycode == KEY_F4) {
    if (active_tty && active_tty != kernel_tty && !dash_active()) {
        tty_destroy(active_tty);
    }
    return;
}
```

This is where the modifier snapshot comes in. Each check tests bits on the event itself, without asking the driver what was held at the time:

- **Ctrl + Shift + Esc** toggles the system dashboard, a live view of processes and resources (Chapter 14)
- **Alt + Tab** switches to the next TTY, each running program's terminal (Chapter 13)
- **Alt + F4** closes the current TTY, killing the program attached to it, unless it is the kernel's own TTY or the dashboard

A hotkey is consumed. It returns early. The program in the foreground never sees the keypress.

### Routing

Everything else goes to the active TTY, the terminal currently on screen:

```c
if (active_tty) {
    char c = keycode_to_ascii(event);
    char arrow = arrow_final(event.keycode);
    if (c) {
        tty_input(active_tty, c);
    } else if (event.keycode == KEY_BACKSPACE) {
        tty_input(active_tty, '\b');
    } else if (arrow && active_tty->ldisc.raw) {
        tty_input(active_tty, '\x1b');
        tty_input(active_tty, '[');
        tty_input(active_tty, arrow);
    }
}
```

Printable characters go through `keycode_to_ascii` and into the TTY's input buffer, one at a time. Backspace becomes `'\b'`, which the TTY's line editing turns into "delete the last character" (Chapter 13).

The arrow keys are a bit special. There is no ASCII character for "up". For decades, terminals have sent a short **escape sequence** instead: the Escape character (`0x1B`), then `[`, then a letter (`A` for up, `B` down, `C` right, `D` left). These are the VT100 conventions, from DEC's terminals of the late 1970s, and every terminal emulator today still speaks them. `arrow_final` picks the letter.

They are only sent when the TTY is in **raw mode**, which programs like games and editors switch on to get every key as it is pressed. In the normal line mode (type a line, press Enter), arrow keys would just dump strange characters into the line. They are left out.

## Putting It All Together

Here is the part of `bootstrap.c` this chapter covers:

```c
// Timers before scheduler
timer_init();
// ...

// Syscalls before userspace
syscall_init();

// TTYs and output finally online
tty_t* k_tty = tty_create();
// ...

// Input drivers and subsystems
input_init();

// Keyboard and routing
keyboard_init();
irq_register(INT_FIRST_INTERRUPT + 1, (irq_handler_t)keyboard_handler);
ioapic_unmask(1);

// PCI and USB for external keyboards
pci_init();
// ...

// Let the good times roll
intr_on();
```

`syscall_init` only writes four MSRs. It could run almost anywhere. It just has to run before the first user program, which Chapter 15 starts much later. The stub it points `LSTAR` at depends on the GDT order and on `cpu_local`, and on a kernel stack the scheduler will keep up to date.

The keyboard comes up after the timers, because the PS/2 waits sleep with `sleep_us`, and that needs the calibrated clock. `keyboard_init` brings up the controller, resets the keyboard and drains the buffer. Only then do we register the handler on vector 33 and unmask I/O APIC pin 1, in that order, since the first interrupt has to find a handler waiting for it.

`apic_init` had already pointed pin 1 at vector 33, with any polarity and trigger overrides the MADT asked for. (Bootstrap used to route the pin a second time here, with the override flags dropped. That line is gone.)

Interrupts are still off globally at this point. They only come on with `intr_on()` at the very end of bootstrap, after the scheduler is ready. That is why the drain matters: any byte that shows up before the pin is unmasked would otherwise hold the line high forever.

Each step builds on the one before. Chapter 9's GDT order makes `STAR`'s arithmetic work, and its `cpu_local` gives the stub a kernel stack. `vmm_check_buffer` and SMAP let `copy_user` take user pointers without trusting them. Chapter 10's calibrated timer gives the PS/2 driver its timeouts, and its I/O APIC routes the keyboard's interrupt to our handler. Finally, the input hub gives every keypress a destination, whichever keyboard it came from.

At this point, a program can ask the kernel to print text, give it memory and read a line of input, and the user can type that line on a PS/2 keyboard. In the next chapter, we go after the keyboard you are most likely using right now: the USB one. That means PCI, a host controller called xHCI, and a lot more than four MSRs.
