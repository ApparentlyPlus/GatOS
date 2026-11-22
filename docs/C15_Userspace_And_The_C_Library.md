# Chapter 15: Userspace and the C Library

Here is a question that sounds silly until you think about it. When a C program calls `printf("%d\n", 42)`, what actually happens?

Not a system call, at least not right away. `printf` is just a function, written in C, living in a library that got linked into the program. It walks the format string and turns `42` into the characters `'4'` and `'2'`. Only at the very end, once it has a finished string, does it ask the kernel to put that string somewhere. The kernel never sees a format string. All it sees is "write these three bytes".

That library is the **C standard library** (libc for short), the layer every program stands on. `printf`, `malloc`, `strlen`, `sin`, all of them live there. On Linux, it is usually glibc. On GatOS, it is a small one of our own, called `ulibc`. This chapter is about it, and about the programs that use it.

We have actually been running those programs for a while. The demo that asks for your name and the donut spinning in its own TTY are both ring 3 code built on `ulibc`. By the end of this chapter, you will know every layer one frame of that donut passes through, from a `sin` call all the way down to the pixels on the screen.

And as usual, explaining it turned up bugs. Quite a few of them, this time, mostly hiding in the math library.

>[!NOTE]
>The code for this chapter lives in [`ulibc/`](/src/ulibc/), [`uproc.c`](/src/kernel/uproc.c), [`umain.c`](/src/kernel/umain.c), [`power.c`](/src/kernel/sys/power.c) and the linker script, [`linker.ld`](/targets/x86_64/linker.ld).

## Programs Inside the Kernel

Before we look at any code, let's see where a user program comes from in the first place. GatOS does this very differently from most operating systems, and the reason why has more to do with Pawstack than with GatOS itself.

### How Programs Usually Get Loaded

On an operating system like Linux, a program is a file. When you type `ls` into a shell, the shell asks the kernel to run `/usr/bin/ls` (through the `execve` system call), and the kernel takes it from there.

That file is in a format called **ELF**. Besides the machine code, it carries a small table that says which piece goes where: "map these bytes at `0x400000` as code, map these at `0x600000` as data, then start running here". The kernel reads that table, builds a fresh address space, maps every piece where the table says and jumps to the start address in ring 3.

The C library is usually not even in that file. Most programs are *dynamically linked*, meaning the file only records "I need `libc.so.6`". Before `main` runs, a small helper called the **dynamic linker** finds that library on disk, maps it into the new address space and points every call to `printf` at the real thing:

```
   shell                  kernel                          the new process
 ┌───────┐   execve    ┌────────────────────────┐      ┌────────────────────────┐
 │  ls   │ ──────────► │ read /usr/bin/ls       │      │ 0x400000   ls code     │
 └───────┘             │ parse its ELF headers  │ ───► │ 0x600000   ls data     │
                       │ map every segment      │      │ 0x7f...    libc.so.6   │
                       │ jump to the entry      │      │ 0x7ff...   the stack   │
                       └────────────────────────┘      └────────────────────────┘
```

That is a lot of machinery. We would need a disk driver, a file system, an ELF loader and (for shared libraries) a dynamic linker. GatOS has none of them yet.

### Why GatOS Bakes Them In

GatOS takes a shortcut. Instead of loading programs from a disk while it runs, it has them built right into the kernel image, before it ever boots. Think of an old game console cartridge: the console never installs anything, since the game was on the chip from the start.

This is not just laziness, though. GatOS is the bottom layer of **Pawstack**, the toolchain this whole project is building. In Pawstack, programs are written in a higher level language and compiled together with the kernel into one bootable ISO. Every program GatOS will ever run is already known when the image is built. We will look at how Pawstack fits together in the last chapter. For now, all that matters is what ends up in the GatOS source tree.

What ends up there is two C files:

* [`uproc.c`](/src/kernel/uproc.c) holds the programs themselves, every function that should run in ring 3.
* [`umain.c`](/src/kernel/umain.c) holds a single kernel function, `uapps`, that sets those programs up at boot.

`uapps` is the part that replaces `execve`. Where Linux would read a file and build a process from it, GatOS calls `process_create` and `thread_create` (from the last chapter) with functions that are already sitting in memory:

```c
void uapps(void) {
    process_t* proc = process_create("demo", NULL);
    sched_add(thread_create(proc, "thread_a", demo_threadA, NULL, true, 0));
    ...
}
```

The two files in GatOS are written by hand, as demos. In a full Pawstack build, the compiler generates both of them and replaces GatOS's versions. Either way, the kernel sees the same shape.

With nothing to load at runtime, we do not need a disk driver, a file system or an ELF loader (at least not yet). Microcontroller firmware works the same way, and it keeps the kernel small. It does leave us with a puzzle, though.

### The Linking Problem

We put the kernel in the higher half, at addresses like `0xFFFFFFFF80100000`. A user program has to live in the lower half, since ring 3 cannot touch anything above it (those pages are mapped supervisor-only, Chapter 8).

So we have to *link* the user code to run at a low address, like `0x400000`, while still *storing* it inside the kernel image (which GRUB loads near the start of physical memory).

### Two Addresses for Everything

The linker has a concept made for exactly this situation. Every section has two addresses:

* The **VMA** (virtual memory address) is the address the code will run at. Every jump, every call and every pointer to a global variable in the code is computed for this address.
* The **LMA** (load memory address) is where the bytes are stored in the file. The bootloader places them there, physically.

For most of the kernel, the two are related by a fixed offset (`KERNEL_VIRTUAL_BASE`, from the higher half chapter). For the user sections, we deliberately pull them apart:

```ld
/* Switch VMA to userspace */
. = 0x400000;

.user_text : AT(ALIGN(KERNEL_LMA_MARK, 4096)) ALIGN(4K)
{
    USER_TEXT_LOAD_ADDR = LOADADDR(.user_text);
    USER_TEXT_START = .;
    *(.user_text .user_text.*)
    *ulibc*(.text .text.*)
    *uproc*(.text .text.*)
    USER_TEXT_END = .;
}
```

**What is happening here:**
* `. = 0x400000` sets the VMA counter. From here on, the linker computes every address as if this code runs at `0x400000`.
* `AT(...)` sets the LMA separately, right after the boot code, rounded up to a page. Physically, the user code sits next to the rest of the kernel.
* Inside the braces, `*ulibc*(.text .text.*)` grabs the code of every object file whose path contains `ulibc`. `*uproc*` does the same for the demo programs. These files are ordinary C, with no special markings. The linker script alone decides they are user code.
* `USER_TEXT_LOAD_ADDR`, `USER_TEXT_START` and `USER_TEXT_END` are symbols the kernel can read, telling it where the section lives physically and where it should appear virtually.

The same pattern repeats for `.user_rodata`, `.user_data` and `.user_bss`. After the last one, we move the VMA counter back to the higher half, and the rest of the kernel continues as before:

```
             stored at (LMA, physical)          runs at (VMA, virtual)
            ┌─────────────────────────┐
  0x100000  │ .boot                   │
            ├─────────────────────────┤
  0x101000  │ .user_text              │ ───►  0x400000            ┐
  0x108000  │ .user_rodata            │ ───►  0x407000            │ the lower half,
  0x109000  │ .user_data              │ ───►  0x408000            │ in every process
  0x10a000  │ .user_bss               │ ───►  0x409000            ┘
            ├─────────────────────────┤
  0x10b000  │ .text, .rodata, ...     │ ───►  0xFFFFFFFF8010b000  ← the kernel, as usual
            └─────────────────────────┘
```

These are the real addresses from a current build. The kernel's `.text` picks up physically right where the user sections end, at `0x10b000`, and goes back to its usual offset from there.

This means the user code is never mapped in the kernel's own address space. Nothing in the kernel can call `printf` from `ulibc` (and nothing needs to). The user code only becomes runnable when `process_create` builds a process. We map those physical pages at their link addresses in that process's page tables, as we saw in the last chapter.

>[!NOTE]
>There is a small trick hiding in the compiler flags. We compile every file in GatOS, user code included, with `-mcmodel=kernel`. It assumes all addresses fit in a sign-extended 32-bit number, which is meant for the top 2 GiB of the address space. It works just as well for the bottom 2 GiB, and `0x400000` is comfortably inside it.

### A Kernel File Pointing at User Code

Let's go back to the two files. They look like they belong together, but the linker puts them on opposite sides of the address space. `uproc.c` matches the `*uproc*` pattern, so it lands in the user sections at `0x400000`. `umain.c` does not, so it becomes ordinary kernel code in the higher half.

Both choices are forced. The programs in `uproc.c` run in ring 3, so they have to sit on user pages. `uapps` runs at boot, before any process even exists, and it calls `process_create`, `thread_create` and `sched_add`. Those live on supervisor-only pages, where ring 3 could never reach them.

That leaves the kernel with a strange job. `uapps` has to hand `thread_create` a pointer to `demo_threadA`, a function that is not mapped anywhere the kernel can see. Here is that call, disassembled from a real build:

```
ffffffff8010d80b <uapps>:
  ...
  mov    $0x4067f2,%rdx            ; entry = demo_threadA
  call   ffffffff8012cb6f <thread_create>
```

Code at `0xffffffff8010d80b` passes `0x4067f2` as an argument. In the kernel's own page tables, nothing is mapped at `0x4067f2`. If `uapps` tried to call it, it would page fault on the spot.

The trick is that a virtual address does not mean anything on its own. It only means something *relative to a set of page tables*. `0x4067f2` is a bit like a room number. On its own, it does not tell you which building to look in. In the kernel's page tables, that room does not exist. In the demo process's page tables, it is exactly where `demo_threadA` lives.

So the kernel never uses the address. It only writes it down. `thread_create` does not jump to `entry`, it stores it in the new thread's saved registers, for whoever runs that thread later:

```c
uintptr_t offset_entry = (uintptr_t)entry - (uintptr_t)&USER_TEXT_START;
thread->context.rdi = USER_CODE_VIRT_ADDR + offset_entry;
```

Both constants are `0x400000`, so this works out to `entry` itself. (The subtraction is there so the two could differ one day, if a process ever placed its code somewhere else.) The saved `rip` gets `userspace_start` the same way.

Here is the whole life of that one number:

```
  at boot                            later, the first time the thread runs
  (kernel page tables)               (the demo process's page tables)

  uapps: "entry is 0x4067f2"         scheduler loads the process's cr3
         │                                  │
         ▼                                  ▼
  thread_create writes it            0x4067f2 is now mapped, onto the
  into context.rdi                   physical copy of demo_threadA
         │                                  │
         ▼                                  ▼
  0x4067f2 is not mapped,            iretq into ring 3, userspace_start
  but nobody touches it              calls entry, and demo_threadA runs
```

The address is written in one address space, and only used in another. That is completely fine, as long as nothing in between ever tries to follow it.

The CPU helps make sure of that. Even after the process's page tables are loaded, the kernel still cannot call `demo_threadA` itself. GatOS turns on **SMEP**, a CPU feature that faults whenever ring 0 tries to execute a user page. A kernel bug that wanders into user code gets caught right away, instead of running whatever a program put there.

>[!NOTE]
>This split is also why user files are compiled without LTO, while the kernel uses it to drop dead code. With LTO, GCC merges every file into one big program at link time, and hands the linker new temporary object files with names of its own. `*uproc*` matches none of them. I tried this on a two-file test: with LTO, the user section came out empty, and the "user" function ended up in the kernel's `.text`.

### The First Instruction in Ring 3

When a user thread runs for the first time, it lands in `userspace_start`, a tiny function that lives in `.user_text` even though it is written in `process.c`:

```c
userspace void userspace_start(void (*entry)(void*), void* arg) {
    if (entry) {
        entry(arg);
    }

    // call SYS_EXIT
    __asm__ volatile (
        "mov $1, %rax \n"
        "syscall \n"
    );

    while(1); // Should never reach here, but just in case
}
```

The `userspace` macro is `__attribute__((section(".user_text")))`. We use it to move a single function into the user section. `userspace_start` calls the thread's real entry point and, once that returns, makes the `SYS_EXIT` system call (number 1). Without it, a thread function that simply returned would return into whatever garbage sits at the top of its stack.

## Talking to the Kernel

Every library function that needs the kernel eventually goes through one of a handful of tiny wrappers in [`syscalls.h`](/src/ulibc/syscalls.h). Here is the one for system calls with two arguments:

```c
userspace static inline uint64_t sc2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}
```

This is GCC's extended inline assembly. It reads a bit like a contract with the compiler. The `syscall` instruction itself is the easy part. The interesting part is everything after the colons:

* `"=a"(ret)` says the result comes back in `rax`.
* `"a"(num), "D"(arg1), "S"(arg2)` puts the system call number in `rax`, the first argument in `rdi` and the second in `rsi`. These are exactly the registers GatOS's syscall ABI uses, from Chapter 11.
* `"rcx", "r11"` tells the compiler that those two registers will be destroyed. The CPU itself overwrites them, since `syscall` saves the return address in `rcx` and the flags in `r11`. If we did not say so, the compiler might keep a variable in `rcx` across the call, only to find it replaced by a return address.
* `"memory"` tells the compiler that the kernel may read or write any memory. Without it, the compiler could keep a buffer's contents in registers and never actually store them before a `write` system call, or assume a buffer filled by `read` still holds its old contents.

On top of `sc0` to `sc3` sit friendly wrappers like this one:

```c
userspace static inline void syscall_write(const char* buf, size_t len) {
    sc2(SYS_WRITE, (uint64_t)buf, (uint64_t)len);
}
```

Here is everything that happens in one `syscall_write("hi", 2)`:

1. The wrapper loads `rax = 2` (`SYS_WRITE`), `rdi` = the address of `"hi"` and `rsi = 2`, then runs `syscall`.
2. The CPU switches to ring 0 and jumps to the syscall entry stub. The stub swaps to the thread's kernel stack (the one the scheduler set up).
3. The dispatcher checks that the buffer really belongs to the program, copies it into the kernel in chunks and calls `tty_write` on the process's own TTY.
4. The TTY takes its output lock. The console turns the two bytes into two cells and two glyphs.
5. The stub returns to ring 3 with `sysretq`, right after the `syscall` instruction, with the result in `rax`.

Every byte a GatOS program ever prints takes that path. Which is exactly why `printf` tries very hard to take it as rarely as possible.

## printf and scanf

Writing a `printf` from scratch is a rite of passage. It is also a few hundred lines of fiddly edge cases (field widths, precision, `%-08.3f` and so on). GatOS uses Marco Paland's excellent [printf](https://github.com/mpaland/printf), the same one the kernel's `kprintf` has used since Chapter 4. It formats into a callback, one character at a time, without ever needing `malloc`.

The part GatOS adds is what that callback does with the characters. The obvious version would make one `SYS_WRITE` per character. That would be terrible. Every system call costs a trip into the kernel and back, plus a lock, plus a console update. Printing a 50 character line would cost 50 of them.

So `printf` collects its output on the stack first:

```c
typedef struct {
  char buf[256];
  size_t n;
} uout_buf_t;

static void _uout_flush(uout_buf_t* b) {
  if (b->n) syscall_write(b->buf, b->n);
  b->n = 0;
}

static void _uout_append(char character, void* arg) {
  uout_buf_t* b = (uout_buf_t*)arg;
  b->buf[b->n++] = character;
  if (b->n == sizeof(b->buf)) _uout_flush(b);
}
```

Characters pile up in a 256 byte buffer. They only go to the kernel when the buffer is full or `printf` is done. A typical line now costs a single system call, while a very long one costs one every 256 bytes. Since this buffer lives on the stack of whoever called `printf`, two threads printing at the same time each have their own, with no locking needed.

### Reading Input

Going the other way, `scanf` needs a similar trick. Reading one character at a time from the kernel would also be one system call per key. Instead, `u_getchar` asks for up to 256 bytes at once:

```c
if (rhead == rtail) {
    ulock_release(&rlock);
    int64_t n = syscall_read(rbuf, sizeof(rbuf));
    ulock_acquire(&rlock);
    // ...
    rhead = 0;
    rtail = (size_t)n;
}
// ...
int ch = (unsigned char)rbuf[rhead++];
```

Since the TTY's line discipline hands over a whole line at once (in canonical mode), a single `SYS_READ` usually returns an entire line. `scanf` then picks through it character by character, without bothering the kernel again.

Notice that the lock is released *before* the read. `SYS_READ` can block for as long as the user takes to type. Holding a lock while blocked would freeze every other thread that wanted to read.

`scanf` itself is a small format parser that supports what the demos need (`%d`, `%s`, `%c` and friends), plus scansets like `%99[^\n]`, "up to 99 characters that are not a newline". That is how the demo reads a whole name, spaces included.

## malloc

The user heap in [`stdlib.c`](/src/ulibc/stdlib.c) is a close cousin of the kernel heap we built in Chapter 9. Both are boundary-tag allocators, with a header and a footer around every block so that a freed block can find and merge with its neighbors on either side. We will not go through the block layout again. Instead, let's look at what had to change when the same idea moved into a user program.

### Where the Memory Comes From

In the kernel heap, we get memory from the VMM by calling it directly. A user program cannot call the VMM. Instead, we ask for memory the only way a program can, with a system call:

```c
static arena_t *arena_create(size_t min_body) {
    size_t total = align_up(ARENA_HDR_SIZE + min_body, PAGE_SIZE);

    void *region = syscall_mmap(NULL, total, MMAP_RW);
    if (!region || region == (void *)(uintptr_t)-1) return NULL;
    // ...
}
```

`SYS_MMAP` hands the program a range of fresh pages in its own address space, at least 64 KiB at a time. Each such range becomes an **arena**, with a small header at the front and one big free block covering the rest. When no free block is big enough for a request, the heap creates another arena. When an arena becomes completely empty again while the heap has plenty of free memory elsewhere, we hand it back with `SYS_MUNMAP`.

Behind the scenes, those pages are lazy (Chapter 8). A program can ask for a 64 KiB arena and only ever touch its first page. Only that one page will ever be backed by real memory.

### Finding a Block Fast

The second difference is how free blocks are found. Instead of one list of free blocks, the user heap keeps 16 **bins**, each one holding free blocks of a range of sizes. Bin `i` holds blocks from `16 × 2^i` bytes up to just below twice that:

```c
static inline uint32_t bin_index(size_t size) {
    if (size < BLOCK_ALIGN) size = BLOCK_ALIGN;
    uint32_t i = (uint32_t)(63 - __builtin_clzll((uint64_t)(size >> 4)));
    return i < UHEAP_BIN_COUNT ? i : UHEAP_BIN_COUNT - 1;
}
```

`__builtin_clzll` counts the leading zeros of a 64-bit number. `63 - clz` is then the position of its highest set bit, in other words its base 2 logarithm, rounded down. For a 100 byte request, `100 >> 4` is 6, whose highest bit is bit 2.

We start the search in bin 2 (64 to 127 bytes). Small requests never have to wade through a list full of huge blocks, and the search usually ends in the very first bin we look at.

### Shrinking in Place

While comparing the two heaps line by line, I found that the user heap had inherited a bug we had already fixed in the kernel heap back in Chapter 9.

When `realloc` makes a block smaller, it does not move it. It cuts the unused tail off and turns that tail into a new free block. Now say the block right after the original was already free:

```
before realloc:   [ used block, 4 KiB          ][ free, 2 KiB ]
after realloc:    [ used, 64 B ][ free tail    ][ free, 2 KiB ]
                                 └─ never merged with ─┘
```

The new tail and the old free block sit side by side, both free, but they were never merged. A later request for 5 KiB would fit perfectly in the space the two of them cover together. Neither of them alone is big enough, though, so the heap maps a whole new 64 KiB arena instead.

We fix it the same way we fixed the kernel heap. When a used block is split, the new free tail goes through the merging code too.

To test it outside of GatOS, I compiled `stdlib.c` on my own machine with a fake `syscall_mmap` that counts how often it is called. Before the fix, that 5 KiB request triggered a new arena. After it, the request fits in the merged space. A 100,000 step random stress test of `malloc`, `free` and `realloc` (checking every byte of every block along the way) comes out clean too.

### Locking Without Spinning

Threads in the same process share one heap, which means `malloc` has to take a lock. In the kernel, that is a spinlock. It disables interrupts while it is held, so the holder can never be interrupted in the middle. A user program cannot turn off interrupts, though. That changes everything.

Say thread A takes the heap lock, and the timer interrupts it halfway through `malloc`. Thread B runs next and also calls `malloc`. B finds the lock taken and spins, waiting for A to release it. But A cannot release anything while it is not running, and it will not run again until B's quantum is over. On a single core, B burns its whole 10 ms waiting for something that cannot happen until it stops waiting.

So the user lock gives up the CPU instead of spinning:

```c
static inline void ulock_acquire(ulock_t* l) {
    // On one core, the holder can't let go until it runs again, so spinning would only burn
    // the rest of our time slice. Give the CPU back instead
    while (__sync_lock_test_and_set(&l->locked, 1))
        syscall_yield();
}
```

`__sync_lock_test_and_set` atomically writes 1 and returns the old value. It returns 0 only for the thread that actually got the lock. Everybody else yields, letting A run, finish and let go.

## Strings

[`string.c`](/src/ulibc/string.c) has the usual suspects: `memcpy`, `memset`, `strlen`, `strcmp` and friends. Most are a few obvious lines each, but two are worth a closer look.

`memcpy` and `memset` use a pair of x86 instructions made for exactly this, `rep movsq` and `rep stosq`. These copy (or fill) `rcx` 8-byte words, from `rsi` to `rdi`, in a single instruction:

```c
void *memcpy(void *dest, const void *src, size_t n) {
    void* d = dest;
    const void* s = src;
    size_t q = n >> 3;
    size_t r = n & 7;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(r) :: "memory");
    return dest;
}
```

The first instruction copies all the whole 8-byte words, while the second copies the 0 to 7 bytes left over. Modern CPUs have special hardware for `rep movs`. For large copies, it is about as fast as copying gets.

In `memmove`, we have to handle the case where the source and the destination overlap. If the destination starts *after* the source, copying front to back would overwrite source bytes before they were read:

```
copy "ABCDEF" two bytes to the right, front to back:
  start:      A B C D E F . .
  copy A:     A B A D E F . .      ← C is gone before we copy it
  copy B:     A B A B E F . .
  ...                              ← garbage
```

So in that case, `memmove` copies back to front instead, starting with the last byte. Every byte is read before anything gets written over it.

### strtol

`strtol` and `strtoul` turn text into numbers. `strtol("  -42abc", &end, 10)` returns -42 and points `end` at the `a`. That `end` pointer is how a caller tells "the number was 0" apart from "there was no number at all". If nothing was parsed, `end` has to point back at the start of the input.

I compared GatOS's versions against glibc on a list of tricky inputs. 26 cases came out different:

| Input | Base | GatOS returned | Correct |
|---|---|---|---|
| `"0"` | 0 | 0, nothing parsed | 0, one character parsed |
| `"0x"` | 0 or 16 | 0, nothing parsed | 0, the `0` parsed, `x` left over |
| `"+9"` | 10 | 0, nothing parsed (`strtoul`) | 9 |
| `"-17"` | 10 | 0, nothing parsed (`strtoul`) | -17 as an unsigned value |
| `"-"` | 10 | 0, one character parsed (`strtol`) | 0, nothing parsed |
| `"99999999999999999999"` | 10 | a wrapped around value | the largest value that fits |

This first one is the sneakiest. In base 0, a leading `0` means "this is an octal number". The old code skipped the `0` as a prefix, found no octal digits after it and reported that nothing was parsed. But `"0"` is a perfectly good number, it just happens to be the octal number 0.

We now build both functions on a shared parser that follows the C standard to the letter. It skips spaces and takes an optional sign. It only treats `0x` as a prefix if a hex digit actually follows it, and it stops (saturating at the largest representable value) when a number gets too big. All of those inputs now match glibc.

## The Math Library

A demo that draws a spinning donut needs `sin` and `cos`. Getting those right is genuinely hard. The naive approach (a few terms of a Taylor series) is accurate near 0, but falls apart for large inputs. Even for small ones, it is easy to be wrong in the last few digits.

So GatOS does not write its own. [`math.c`](/src/ulibc/math.c) is a port of **fdlibm** ("freely distributable libm"), the math library Sun Microsystems wrote in the early 90s. It is careful, battle-tested code that many other libraries (including Java's `StrictMath`) are based on. Its algorithms come with error bounds. Most of its functions are accurate to within one unit in the last place.

### Porting fdlibm

fdlibm was written for 32-bit machines. It looks at doubles as two 32-bit halves, through a pair of macros:

```c
#define __HI(x) *(1+(int*)&x)
#define __LO(x) *(int*)&x
```

`__HI(x)` is the upper 32 bits of a double (the sign, the exponent and the top 20 bits of the mantissa), and `__LO(x)` is the lower 32 bits (the rest of the mantissa). This kind of pointer cast is undefined behavior in modern C. Compilers are allowed to miscompile it (and they do). The port replaces every one of these with a union, which reads the same 64 bits as either a `double` or a `uint64_t`:

```c
typedef union { double f; uint64_t u; } dbl_cast;
```

Reading `__HI(x)` then becomes `(int32_t)(dc.u >> 32)`. Writing it is the tricky part, as we will see in a moment.

### Testing Against glibc

To check a math library, we need a reference to compare it to, plus a way to measure how wrong a result is.

The usual unit for that is the **ulp** (unit in the last place), the distance between a double and the next representable one. Doubles near 1.0 are spaced about 2.2 × 10⁻¹⁶ apart. Being off by 1 ulp there means being off in the 16th significant digit. A result that is within 1 ulp of the true value is about as good as a double can get.

I compiled GatOS's `math.c` on my own machine, renaming all of its functions so they would not clash with the system's. Then I compared every one of them against glibc, over 200,000 random inputs each, plus the usual troublemakers (0, -0, infinity, NaN, tiny and huge numbers). Most functions came out at 1 ulp or better. Five did not.

### Five Bugs

The first was in `pow`. `pow(-2, 3)` returned **8**.

fdlibm decides that a result is negative when x is negative and y is an odd integer. It works out the "x is negative" part from the sign bit of x's high word, `hx`. The port tested `hy`, the high word of **y**, instead. One letter. So `pow(-2, 3)` came out positive, while `pow(-2, -3)` happened to come out right, since there y is negative. This one would have broken any program that cubed a negative number.

The second was in `log1p`, which computes `log(1 + x)` accurately for tiny x. It was wrong in about the 7th significant digit. That is a lot for a function meant to be accurate to the 16th. Halfway through, fdlibm sets the exponent of a number by overwriting its high word (and only its high word):

```c
__HI(u) = hu | 0x3ff00000;    /* the low 32 bits of u are untouched */
```

The port did the same thing on the whole 64-bit value, with this mask:

```c
dcu.u = (dcu.u & 0x800fffff00000000ULL) | 0x3ff0000000000000ULL;
```

Look at the low 32 bits of that mask. They are all zero. The low word, half of the mantissa, got wiped out:

```
   63      52 51         32 31                    0
  ┌──┬────────┬─────────────┬───────────────────────┐
  │s │exponent│ mantissa hi │      mantissa lo      │
  └──┴────────┴─────────────┴───────────────────────┘
mask 0x800fffff00000000:       keeps mantissa hi,  ZEROES mantissa lo  ✗
mask 0x000fffffffffffff:       keeps mantissa hi,  keeps  mantissa lo  ✓
```

This fix keeps the low word, like `__HI` always did. `asinh`, `acosh` and `atanh` call `log1p` internally. They were wrong too, and are all fixed with it.

The third was `sqrt`, off by up to 3 ulp. That was strange. A square root is one of the few operations IEEE 754 requires to be *correctly rounded*, exactly the nearest double to the true value, every time. A look at the code explained it:

```c
i = 0x5fe6eb50c7b537a9 - (i >> 1);
double y = *(double*)&i;
y = y * (1.5 - (number * 0.5 * y * y));
// ... three more Newton steps
return number * y;
```

That is not fdlibm at all. It is the famous **fast inverse square root**, made legendary by the source code of Quake III. It treats the bits of a double as an integer, subtracts it from a magic constant to get a decent first guess at `1 / sqrt(x)`, refines it with Newton's method and multiplies back by x. This is a brilliant trick for a 1999 game engine. It is also never quite exact.

Every x86-64 CPU has a single instruction for this, `sqrtsd`. It is correctly rounded by definition, handles every special case and is faster than all of those multiplications. So we made `sqrt` that one instruction:

```c
double sqrt(double number)
{
    // sqrtsd is correctly rounded by IEEE 754, handles -0, NaN, infinity and denormals,
    // and negative inputs give NaN, exactly what sqrt has to return
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(number));
    return r;
}
```

The fourth was `cosh(710)`, which returned infinity. The true value is about 1.1 × 10³⁰⁸, just under the largest double. For inputs this big, `cosh(x)` is half of `e^x`. Since `e^710` alone would overflow, fdlibm computes `w = e^(x/2)` and returns `(w / 2) × w`. The port wrote it as `w * w * 0.5`, which squares first. `w * w` overflows to infinity. Halving infinity does not bring it back. Halving first keeps every step in range.

The last one was small. For huge inputs, `asinh(x)` and `acosh(x)` are `log(x) + ln(2)`, and the port added `ln2_hi` there. fdlibm stores `ln(2)` in two pieces that are meant to be used together, a high part with the low bits cut off (so it can be multiplied exactly) and a small low part. `ln2_hi` alone is short by 1.9 × 10⁻¹⁰. That showed up as an error of about 1,700 ulp. We now add the full constant.

With all five fixed, every function is within 1 to 3 ulp of glibc. That small leftover is honest fdlibm accuracy, since glibc goes to great lengths to round every result correctly, while fdlibm settles for "almost always within 1 ulp". `pow` also keeps fdlibm's promise of returning integer powers exactly. I checked all 561 of them that fit in a double.

>[!NOTE]
>User code is compiled with `-ffast-math`. That lets the compiler assume there are no NaNs or infinities, and rearrange floating point math as if it were exact. Code like fdlibm relies on the exact IEEE behavior and can break badly under it. I reran the whole comparison with `-ffast-math` at two optimization levels, and the results stayed the same.

## Turning the Machine Off

Two of the system calls from Chapter 11 do something no ordinary program would dare. `SYS_POWEROFF` (12) turns the machine off, while `SYS_REBOOT` (13) restarts it. GatOS has no users or permissions yet, so any program may call them. The kernel's own `shutdown` and `reboot` commands end up in the same two functions, in [`power.c`](/src/kernel/sys/power.c).

Both sound like one line of code. Neither is, since there is no x86 instruction for "turn off". Software has to ask the chipset on the motherboard, and every chipset wants to be asked differently.

### Shutting Down with ACPI

ACPI hides those differences behind two things: a register, and a value to write into it. In Chapter 10, we saw the FADT in passing (the table with the confusing `"FACP"` signature). Among many other things, it holds the I/O port of the **PM1a control register**, the one that controls power.

The value is harder to get at. It lives in the **DSDT**, a much bigger table that is not really a table at all. It is compiled bytecode, in a language called **AML**, that describes the whole motherboard. Somewhere inside it sits a definition like this one (shown here in its source form):

```
Name (_S5_, Package () { 0x05, 0x05, 0, 0 })
```

`_S5_` stands for sleep state 5, "soft off". Its first number is `SLP_TYP`, the magic value this chipset wants for "off". We write it into bits 10 to 12 of PM1a, together with the `SLP_EN` bit (bit 13), and the power goes out.

A real AML interpreter runs to tens of thousands of lines. We cheat instead. `power_get_s5` scans the DSDT for the four bytes `_S5_`, then decodes the few bytes after them by hand. Here are the real bytes from QEMU's DSDT:

```
  08   5F 53 35 5F   12   06    04    00     00   00   00
  │    └── _S5_ ──┘  │    │     │     │      └────┴────┴── the other three
  │                  │    │     │     └── SLP_TYPa = 0 (ZeroOp)
  │                  │    │     └── 4 elements
  │                  │    └── PkgLength: 6 more bytes
  │                  └── PackageOp
  └── NameOp, "this is a definition"
```

On QEMU, the magic value happens to be 0. On real boards it is usually a small number like 5 or 7, stored as a `0A` byte prefix followed by the number itself, which the decoder also handles.

Scanning for four bytes is fragile, though, and it found the first bug. The name `_S5_` can show up more than once in the DSDT. A method that runs at shutdown might *refer* to it, for example.

The old scan took the first match it found, so on a board where a reference came first, it decoded random bytecode as a sleep value. We now only accept a match with a `NameOp` (`08`) right in front of it, optionally with a `\` (root prefix) in between, so only the actual definition counts.

The second bug was in what happens before that write. Some firmware boots in a legacy mode, where the chipset belongs to the firmware itself, not the operating system. To take it over, we write a special value (`ACPI_ENABLE`) to a port called `SMI_CMD`, both from the FADT, then wait for the `SCI_EN` bit in PM1a to come on.

The old code did this every time, even when the firmware had already switched to ACPI mode. It also gave up after about a thousand port reads, roughly a millisecond, which is not much time for firmware that might be busy. We now skip the handover when `SCI_EN` is already set, and wait up to about 300 ms otherwise.

On QEMU, `shutdown` now closes the window a moment after it is typed.

### Rebooting

There is no single reliable way to reset a PC, so `reboot` tries several, from most to least trustworthy:

1. The **ACPI reset register**. Newer FADTs describe a register and a value to write into it that resets the machine. Since the firmware itself tells us where it is, this is the safest choice.
2. Port **`0xCF9`**, the reset control register on Intel style chipsets. Writing `0x02` then `0x06` asks for a full reset. This works on a lot of machines, but it is a guess.
3. The **PS/2 controller**. Its command `0xFE` pulses the CPU's reset line, a trick from the IBM PC AT days that still works on a lot of hardware.
4. A **triple fault**, the last resort. We load an IDT with a limit of zero and run `int3`.

That last one deserves a closer look, since it is a reset nobody asked the hardware for:

```
 int3  ──► the IDT is empty, so the CPU cannot find a handler
       ──► that is a fault in itself: #GP, which it cannot deliver either
       ──► two faults in a row: #DF (double fault), also undeliverable
       ──► a fault while delivering #DF: triple fault
       ──► the CPU gives up and signals a shutdown, and the board resets it
```

The old code had the first two in the opposite order, so it always tried the guess before the register the firmware described. On a board where `0xCF9` means something else, guessing first is not a great idea. The order is now the one above. On QEMU, `reboot` brings the machine back to GRUB.

### Measuring Watts

The dashboard from Chapter 14 shows how much power the CPU is drawing, in watts. That number comes from **RAPL** (Running Average Power Limit), a set of MSRs that both Intel and AMD CPUs have. One of them counts the energy the CPU package has used since power on, in tiny steps.

The step size comes from another MSR. Its bits 8 to 12 hold the **energy status unit** (ESU), and one step is 1 / 2^ESU joules. A common ESU is 14, which makes a step about 61 microjoules. To turn that into watts, we read the counter twice and divide the energy by the time in between:

```
  ESU = 14                 one step = 1 / 16384 J
  read 1:  1,000,000       at t = 10,000 ms
  read 2:  1,196,608       at t = 10,500 ms
  energy:  196,608 steps = 12 J
  power:   12 J / 0.5 s  = 24 W
```

`power_avg_watts` does the same math in integers, scaled by 10 so the dashboard can print one decimal (it returns `240` for 24.0 W). The counter is only 32 bits wide. With 61 µJ steps, it wraps around after about 262,000 joules (some 45 minutes at 100 W). Subtracting two `uint32_t` values gets the right difference even across a wrap, since the subtraction wraps the same way.

The hard part is knowing whether RAPL is there at all. AMD advertises it with a CPUID bit (bit 14 of `EDX` in leaf `0x80000007`), and that check was right. On Intel, the old code checked bit 3 of leaf 6, which is not RAPL. That bit is reserved. On a real Intel machine, the check always failed, and the dashboard never showed a single watt.

Intel has no CPUID bit for RAPL. Linux decides by the CPU model number, from a long list of models. We do something simpler: we just try reading the MSR. The catch is that reading an MSR that does not exist raises `#GP`, and an unexpected `#GP` in the kernel is a panic.

So we let `#GP` happen, and recover from it. `rdmsr_safe` labels its `rdmsr` instruction, along with a second spot right after it:

```c
__asm__ volatile(
    "mov $1, %[ok]\n"
    ".global msr_safe_insn\n"
    "msr_safe_insn: rdmsr\n"
    "jmp 1f\n"
    ".global msr_safe_fail\n"
    "msr_safe_fail: xor %[ok], %[ok]\n"
    "1:\n"
    : [ok] "=&r"(ok), "+a"(lo), "+d"(hi)
    : "c"(msr)
    : "memory");
```

If the MSR exists, `rdmsr` succeeds, the `jmp` skips over the failure path and `ok` stays 1. If it does not, the CPU raises `#GP` with `rip` pointing at `msr_safe_insn`. The interrupt dispatcher checks for exactly that address before panicking:

```c
if (vec == INT_GENERAL_PROTECTION && context->iret_rip == (uint64_t)msr_safe_insn) {
    context->iret_rip = (uint64_t)msr_safe_fail;
    return context;
}
```

It moves the saved `rip` forward to `msr_safe_fail`, so when the handler returns, execution continues there. `ok` becomes 0, and the caller learns the MSR is missing. Nothing else about the fault matters. The function is marked `noinline` so there is only ever one copy of those labels.

>[!NOTE]
>Linux does the same thing on a much bigger scale. Every instruction in the kernel that is allowed to fault (reading user memory, probing MSRs and so on) gets an entry in a table, the **exception table**, pairing it with the address to continue at. The page fault and `#GP` handlers look up the faulting `rip` in that table before deciding something has gone wrong.

One more surprise showed up while testing this. Under KVM, the bogus MSR in the test raised `#GP` as expected. Under QEMU's emulator, it did not, and the read simply returned 0. A zero energy unit would mean every step is a whole joule, so we also treat an ESU of 0 as "no RAPL". Either way, virtual machines usually leave RAPL out, so the dashboard shows `N/A` instead.

## The Donut

Every OS project needs one program that shows everything working at once. Ours is a spinning ASCII donut, in [`uproc.c`](/src/kernel/uproc.c). The idea is borrowed from Andy Sloane's famous `donut.c` (2006), a C program shaped like a donut that prints a spinning donut. GatOS's version is the same math, written out longhand.

This makes it a nice test, since it needs almost everything. It uses `malloc` for its buffers, `sin` and `cos` every frame, floating point math in ring 3 (which brings in lazy FPU switching), a big `write` every frame and escape sequences on the console. If any of those layers is broken, the donut shows it right away.

### Drawing a Torus

A donut is a **torus**, which is what you get by taking a circle and spinning it around an axis. The math follows that description almost word for word.

First, the circle. Take a circle of radius 1, centered 2 units away from the axis:

```
(x, y) = (2 + cos θ, sin θ)        for θ from 0 to 2π
```

Then spin that circle around the vertical axis, by an angle φ. Every point of the circle sweeps out a ring. All those rings together make the surface of the torus. In the code, `j` walks θ around the small circle (90 steps) while `i` walks φ around the axis (314 steps), and `h = cosj + 2.0f` is the circle's distance from the axis at the current θ.

This gives us about 28,000 points on the surface. Next, we rotate the whole donut by two angles, `A` and `B`. They grow a little every frame, and that is what makes it spin. The formulas look scary in the code, but they are just two rotation matrices multiplied out by hand.

### From 3D to the Screen

To turn a 3D point into a position on the screen, we use **perspective projection**. Things farther away look smaller. We divide by the distance:

```c
float D = 1.0f / (sini * h_sinA + sinj_cosA_p5);   // 1 / z, with the donut pushed 5 units away
int x = center_x + (int)(30.0f * D * ( ... ));
int y = center_y + (int)(15.0f * D * ( ... ));
```

`D` is one over the point's distance from the viewer. A point twice as far lands twice as close to the center of the screen. The horizontal scale is twice the vertical one (30 against 15) because a character cell is about twice as tall as it is wide. Without that, the donut would come out squashed.

Two points of the donut can land on the same character, one on the front and one on the back. The front one has to win. That is what the `z` buffer is for. For every cell on the screen, we remember the `D` of the closest point drawn there so far. A new point only replaces it if its `D` is bigger (closer):

```c
if (D > z[o]) {
    z[o] = D;
    int N = (int)(8.0f * ( ... ));   // how much this bit of surface faces the light
    // ...
    b[o] = shading[N];
}
```

This is called **z-buffering**. Real 3D graphics cards do the same thing, per pixel.

Finally, shading. Every point on a torus has a **normal**, the direction its surface faces. The dot product of the normal with the direction the light comes from says how directly that bit of surface faces the light. `N` turns that into a number from 0 to 11. It picks a character from `.,-~:;=!*#$@`, darkest to brightest.

### Not Calling sin 28,000 Times

Every frame needs the sine and cosine of all 90 θ steps and all 314 φ steps. Calling `sin` and `cos` for every one of the 28,000 points would be a lot of work for numbers that follow a perfectly regular pattern.

Instead, we use the angle addition formulas:

```
sin(a + d) = sin a · cos d + cos a · sin d
cos(a + d) = cos a · cos d - sin a · sin d
```

If we already know `sin a` and `cos a`, and `sin d` and `cos d` for a fixed step `d`, we get the next angle's sine and cosine with four multiplications and two additions:

```c
float ns = sini * COS_DI + cosi * SIN_DI;
float nc = cosi * COS_DI - sini * SIN_DI;
sini = ns; cosi = nc;
```

`SIN_DI` and `COS_DI` are computed once, at startup. After that, the inner loops never call `sin` or `cos` again. Only the two frame rotation angles need real calls, four per frame.

### Putting It on Screen

We draw each frame into `b`, an array of characters the size of the screen, then write it out in one go, with `ESC[H` in front:

```c
memcpy(out_buf + 3, b, buffer_size);
syscall_write(out_buf, buffer_size + 3);
```

`ESC[H` moves the cursor to the top left corner. The rest of the frame overwrites the whole screen, row by row. The console's trick from Chapter 13 makes this cheap. A cell that already holds the right character costs a comparison instead of a glyph draw, and most of the screen around the donut never changes.

### Sixty Frames a Second

Until now, the donut drew frames as fast as it possibly could, forever. It was a fun way to show off preemption (the rest of GatOS stayed perfectly responsive while one program used every cycle it could get), but it also meant a machine running the donut was never idle. The dashboard showed the CPU at 100% for as long as it ran.

The donut now paces itself to 60 frames a second, 16 ms per frame. We measure how long a frame took to draw with `SYS_TIME_NS`, then sleep away whatever is left:

```c
uint64_t spent = syscall_time_ns() - frame_start;
if (spent < FRAME_NS) syscall_sleep((FRAME_NS - spent) / 1000000);
```

Drawing a frame takes well under a millisecond, so the donut now spends almost all of its time asleep. On the dashboard, the CPU went from 100% to about 4%. This donut looks exactly the same.

## Putting It All Together

There is no new bootstrap code in this chapter. User programs are started by `uapps`, which we saw in the last chapter:

```c
void uapps(void) {
    process_t* proc = process_create("demo", NULL);
    sched_add(thread_create(proc, "thread_a", demo_threadA, NULL, true, 0));
    sched_add(thread_create(proc, "thread_b", demo_threadB, NULL, true, 0));

    process_t* proc2 = process_create("donut", NULL);
    sched_add(thread_create(proc2, "donut_thread", donut_sim, NULL, true, 0));
}
```

Instead, let's do something different and follow one frame of the donut through every layer of GatOS we have built:

1. The timer fires and the scheduler switches to the donut's thread. It loads the donut's page tables into `cr3`, points the TSS and `cpu_local` at its kernel stack and returns into ring 3 with `iretq`.
2. The donut calls `memset` on its buffers. The first time, those buffers were brand new memory from `malloc` (which got it from `SYS_MMAP`). The first touch of every page page faults, and the VMM maps in a fresh frame from the PMM, lazily.
3. It calls `sin` and `cos` from fdlibm. If some other thread used the FPU since the donut last ran, the first floating point instruction raises `#NM`. The lazy FPU handler loads the donut's FPU registers back.
4. It computes 28,000 points with a z-buffer and the angle addition trick, building the frame in `b`.
5. It calls `syscall_write`. The `syscall` instruction lands in the entry stub, which switches stacks. The dispatcher copies the frame out of user memory, checking every page along the way.
6. `tty_write` takes the TTY's output lock and hands the bytes to the console as one batch. The escape sequence parser sees `ESC[H`, the UTF-8 decoder turns every other byte into a character, and every cell that changed is marked dirty.
7. The console flushes the dirty cells. Each character becomes a glyph from the font, written into the write-combining framebuffer two pixels at a time.
8. The donut asks `SYS_SLEEP_MS` for the rest of its 16 ms. The scheduler puts it in the sleep tree, while the idle thread puts the core to sleep until the next timer deadline.

That is every chapter of this book, in about a millisecond.

GatOS can now run programs that print, read, allocate memory, do floating point math and draw on the screen, each in its own address space, on its own TTY, while sharing one core. This is the end of our technical run. It's been awesome so far, but we deserve a break for our sanity now. Next chapter will be the final one, and we'll introspectively look at what we've built as a whole. Every good thing eventually comes to an end.
