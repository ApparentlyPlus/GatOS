<p align="center">
  <img src="docs/assets/gatos.svg" alt="GatOS" width="480">
</p>

<h1 align="center">A Modular x86_64 Kernel for Custom Operating Systems</h1>

<p align="center">
  <a href="https://github.com/ApparentlyPlus/GatOS/actions/workflows/linux.yml"><img src="https://github.com/ApparentlyPlus/GatOS/actions/workflows/linux.yml/badge.svg" alt="GatOS Linux"></a>
  <a href="https://github.com/ApparentlyPlus/GatOS/actions/workflows/windows.yml"><img src="https://github.com/ApparentlyPlus/GatOS/actions/workflows/windows.yml/badge.svg" alt="GatOS Windows"></a>
  <a href="https://github.com/ApparentlyPlus/GatOS/actions/workflows/macOS.yml"><img src="https://github.com/ApparentlyPlus/GatOS/actions/workflows/macOS.yml/badge.svg" alt="GatOS macOS"></a>
  <a href="#license"><img src="https://img.shields.io/badge/License-Custom-red.svg" alt="License: Custom"></a>
  <img src="https://img.shields.io/badge/kernel-v2.1.9-0deedd" alt="Kernel v2.2.0">
  <img src="https://img.shields.io/badge/arch-x86__64-1263cf" alt="x86_64">
</p>

GatOS is a cleanly designed, modular kernel serving as the foundational layer for building custom operating systems. It is also part of my undergraduate thesis at the [University of Macedonia](https://www.uom.gr/en/dai), and serves as the backbone of a configurable OS-building toolchain called PawStack.

It manages memory, schedules threads, runs your code in userspace, drives the display and supports USB devices. [What's Inside the Kernel](#whats-inside-the-kernel) has the full house tour.

And getting it running really is just 2 commands. Check [Getting Started](#getting-started) if you want to run it without reading the rest.

> [!NOTE]
> This is a student project, written solo as an undergraduate thesis, so expect the occasional rough edge and the odd bug. That said, I believe it is as close to production ready as it can be for its scope, so feel free to deploy it and play around. 

The first section of this README focuses on providing some insight as to the vision of this project. If you'd rather skip the philosophy, the technical part starts at [What's Inside the Kernel](#whats-inside-the-kernel).

## Table of Contents

- [Project Overview & Background](#project-overview--background)
- [What's Inside the Kernel](#whats-inside-the-kernel)
- [What's *not* Inside the Kernel](#whats-not-inside-the-kernel)
- [Getting Started](#getting-started)
- [Building the Toolchain from Source](#building-the-toolchain-from-source)
- [Testing](#testing)
- [Development](#development)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)
- [So... what now?](#so-what-now)


## Project Overview & Background

### What is PawStack?

"PawStack" is just the name I decided to use for a development toolchain that aims to drastically simplify OS development. It allows you to write code just like you would for a regular program — but instead of compiling to an application, your code is compiled directly into a complete, bootable operating system image.

This means your program ***is*** the operating system.

PawStack handles the complex parts of turning your code into low-level machine instructions that run on real hardware or emulators. The goal is to let you focus on building your OS's features without worrying about the usual technical challenges involved in OS development.

The whole toolchain is comprised of 3 components:

| Component | Description | Status |
|-----------|-------------|--------|
| **GatOS** | The current project. A modular kernel forming the core of PawStack, exposing APIs and syscalls for core OS functionality. | **Feature Complete** |
| **[Gata](https://github.com/ApparentlyPlus/Gata)** | A custom high-level programming language for writing operating systems. It *feels* like a modern language, but is built with features that make low-level development simpler and more approachable. | **Usable, Stabilizing** |
| **[Appa](https://github.com/ApparentlyPlus/Appa)** | The compiler for Gata. It takes in Gata source code and transpiles it into C code that calls GatOS's APIs, constructing the kernel based on the code's logic by leveraging the modularity of GatOS's design. | **Usable** |

> [!TIP]
> Gata and Appa now live in their own repositories. Gata ships a standard library ([`libgata`](https://github.com/ApparentlyPlus/Gata/tree/main/libgata)) with collections, strings, math, sync and time primitives, a VS Code extension, and a book-length language guide. Appa is now a production transpiler with a full frontend, IR, and C backend. My vision has come a long way!

Technically, GatOS is not the end of the toolchain. Even after you have a version of GatOS generated for your Gata logic, the kernel itself still needs to be compiled into bare-metal machine code. This final build stage is handled by a GCC-based compilation toolchain, with packaging done through tools like grub-mkrescue and xorriso to produce a bootable image.

### Build Pipeline

```mermaid
graph LR
    A[Gata Source Code] --> B[Appa Compiler]
    B --> C[Custom GatOS Configuration]
    C --> D[GCC Toolchain]
    D --> E[Bootable OS Image]
```

> [!WARNING]
> It should also be noted that GatOS does not include its own bootloader, relying instead on GRUB for loading.

### What's with these names?

Glad you asked! Here's the story behind them:

**GatOS** is a playful pun on the Greek word *gatos* (meaning "male cat"), with the "OS" tacked on for "Operating System". It was inspired by a similar, more educationally focused project called [Skyl-OS](https://github.com/Billyzeim/Skyl-OS) — another pun, this time on *skylos* (meaning "male dog") — created by a close friend of mine.

Following the same "cat" theme, I named the high-level language of the toolchain "**Gata**" — Greek for "female cat." It felt like the perfect fit for the language developers will use to interact with the toolchain, write code, and build their projects.

Finally, the compiler in the toolchain is called **Appa**. The name is inspired from the flying bison in Nickelodeon's animated series *"Avatar: The Last Airbender"*, a loyal companion to the main cast. The "bison" part is intentional — it's a direct nod to [GNU Bison](https://github.com/akimd/bison), the well-known syntax analysis tool used in building compilers.

"**PawStack**" is just a blend of comp-sci lingo and the animal based naming convention — perfect name for describing the entire toolchain ;)

### What is your university thesis on?

In short, my thesis focuses on developing a functional demo of the PawStack toolchain and thoroughly documenting its inner workings.

When I began, I had zero prior experience in OS development. Because of that, I see this as a great opportunity not only to deliver the demo, but also to create concise write-ups detailing my journey — what steps I took, the mistakes I made, what I omitted, what could be improved, and the features I implemented.

The end goal is for this to serve as a helpful reference in a field where accessible, beginner-friendly resources are scarce.

### Are you crazy?

Yes, absolutely. Name **one** other person who's trying to finish a 4-year degree in 3 years *while* building an entire operating system toolchain as their thesis.

This is either a feat of legendary ambition or an elaborate self-inflicted stress experiment. Possibly both.

Update: it's both. The kernel works, it's fast, it's robust, it's cleanly written and it does what the original vision asked of it. It even talks to external USB keyboards via xHCI. 

Update<sup>2</sup>: I am also very tired, thank you for asking.

## What's Inside the Kernel

GatOS targets **x86_64 long mode**, boots via **Multiboot2/GRUB**, and runs entirely in the higher half, with all of physical RAM mirrored into a physmap. 

If none of these terms make sense to you, I recommend ordering the [documentation special](#documentation) from today's menu. 

For the more tech savvy among you, here's the gist of what the kernel supports:

### Memory Management

Every allocator sits in a strict layer above the one below it, and each is initialized in a fixed order (`PMM → slab → VMM → heap`) because each genuinely depends on the last.

| Layer | Notes |
|---|---|
| **Physmap** | The entirety of physical RAM is mapped at a fixed virtual base, so the kernel can touch any physical page without temporary mappings. |
| **PMM** | Physical page **buddy allocator** with power-of-two free lists across 32 orders, upward coalescing on free, and a magic-tagged header on every free block so corruption is caught rather than propagated. It is also firmware-aware, meaning the kernel image, firmware regions and MMIO holes are registered as exclusion ranges *before* the free lists are ever populated. |
| **Slab** | Named object caches for hot fixed-size structures, carving PMM pages into objects instead of burning a full page on a 64-byte allocation. Tracks partial/full/empty slabs per cache. |
| **VMM** | Per-address-space page table root plus a sorted list of VM objects. Flags cover write/exec/user, device memory (`VM_FLAG_DEVICE`), foreign physical ranges the VMM must never free (`VM_FLAG_FOREIGN`), and lazily-backed regions (`VM_FLAG_LAZY`). |
| **Demand paging** | `VM_FLAG_LAZY` regions are backed on first touch. The `#PF` handler checks the faulting process's address space first, falls back to the kernel's, allocates and zeroes a page, and returns. User stacks and `mmap` regions cost nothing until actually used. |
| **Paging** | 4-level paging that automatically promotes to **2MiB huge pages** whenever a mapping is large and aligned enough to allow it, which drops an entire tier of page tables and a lot of TLB pressure. |
| **Heap** | `kmalloc`/`kfree`/`krealloc`/`kcalloc` over **boundary-tag coalescing** with **segregated free-list bins**, growing on demand by requesting more virtual memory from the VMM. Blocks carry magic numbers and red zones. |
| **PAT** | Page Attribute Table reprogrammed so the framebuffer is **write-combining** and MMIO is properly uncacheable. |

Every one of these ships introspection built in: `pmm_dump_stats()`, `slab_dump_all_caches()`, `vmm_dump()`, `heap_dump()`, and a `*_verify_integrity()` on each, so "is the allocator lying to me?" is a question you can actually answer at runtime. Hooray, amirite? 

### Security

The kernel turns on the hardware protections it can and actually uses them:

* **NX / W^X**: `EFER.NXE` is enabled and the VMM sets `PAGE_NO_EXECUTE` on every mapping that isn't explicitly executable.
* **SMEP**: the CPU refuses to execute userspace pages while in ring 0.
* **SMAP**: supervisor access to user pages is denied by default. The syscall layer brackets its user copies with `stac`/`clac` and nothing else touches user memory directly.
* **Validated user pointers**: `vmm_check_buffer()` verifies a user buffer is mapped with the required flags *per chunk*, so a userspace pointer can't walk off its mapping mid-copy.
* **Bounded copies**: user buffers are copied in fixed 4KiB chunks into a stack buffer, rather than trusting a length or blowing the kernel stack.
* **Faults are contained**: a fault in ring 3 kills the offending process and reports why. Only kernel faults escalate to a full panic.

### CPU, Interrupts & Time

* GDT, TSS, IDT, and a full ISR/IRQ stub table feeding a single dispatcher, with all 20 architectural exceptions named and decoded.
* **CPUID-based feature detection**: vendor, brand string, family/model/stepping, core count, and a feature bitmap (SSE through AVX2, NX, SMEP, SMAP, VMX/SVM), with the useful ones enabled at boot.
* **ACPI** table parsing (RSDP with RSDT/XSDT support) and **APIC**: Local APIC plus I/O APIC with per-IRQ redirection, masking and unmasking.
* A four source timer stack: **PIT**, **HPET**, **LAPIC timer** and a calibrated **TSC**.
* **Tickless operation.** Where TSC-deadline is available, the LAPIC is armed for the *next actual event*, aka the earlier of the scheduler quantum or the next sleeping thread's wake time, instead of interrupting on a fixed period. With nothing to wake, the timer is stopped outright. There's a 10ms periodic fallback for CPUs without it.
* Uptime and sleep math done in 64- and 128-bit integer arithmetic, because the kernel builds with no floating point and no SSE on interrupt-sensitive paths.
* **Lazy FPU switching**: The FPU/SSE/AVX state area is per-thread and only swapped when a different thread actually uses it, so the kernel's own paths never pay for it.
* **MONITOR/MWAIT** idle, so an idle system parks the core in a low-power state instead of spinning (with `HLT` as fallback).
* Spinlocks with proper interrupt-state save and restore.

### Processes, Threads & Userspace

* Preemptive round-robin scheduler running on its **own dedicated stack**, with per-thread CPU accounting and deferred reaping of dead threads.
* Sleeping threads live in an **intrusive AVL tree keyed on wake time**, so "who wakes next?" is `O(log n)` and directly feeds the tickless timer.
* Processes and threads are separate objects: a process owns an address space and a TTY, a thread owns a kernel stack, a user stack, saved CPU context, FPU state and a TLS base.
* **Ring 3 userspace** via `SYSCALL`/`SYSRET`, with the `STAR`/`LSTAR`/`FMASK` MSRs configured, a separate `.user_text` linker section, per-thread user stacks, and per-thread TLS through `FS_BASE`.

> [!TIP]
> The `STAR` MSR is deliberately programmed with a user base of `0x13` rather than the obvious value, because some Intel microarchitectures don't force `SS.RPL = 3` on `SYSRET` and hand you back a subtly wrong stack segment. That one took *a while* to fix.

Currently exposed syscalls:

| # | Syscall | # | Syscall |
|---|---|---|---|
| 1 | `SYS_EXIT` | 7 | `SYS_SLEEP_MS` |
| 2 | `SYS_WRITE` | 8 | `SYS_READ` |
| 3 | `SYS_MMAP` | 9 | `SYS_TTY_CTRL` |
| 4 | `SYS_MUNMAP` | 11 | `SYS_TIME_NS` |
| 5 | `SYS_SET_FS_BASE` | 12 | `SYS_POWEROFF` |
| 6 | `SYS_YIELD` | 13 | `SYS_REBOOT` |

### Drivers & I/O

* **Framebuffer console** with a bitmap font, a full character backbuffer, **per-cell dirty tracking**, batched writes and deferred rendering. It also decodes **UTF-8** and handles **ANSI escape sequences**, and supports sticky header rows that never scroll away.
* **Dynamic TTY subsystem**: TTYs are created and destroyed at runtime in a linked list, each with a 4KiB ring buffer, a canonical-mode line discipline with echo, and a wait queue that properly blocks and wakes reader threads. `ALT+Tab` cycles, `ALT+F4` closes, and the kernel TTY is protected from both.
* **PS/2 keyboard** (i8042) and a from-scratch **USB xHCI** driver: controller reset, device slots, command/event rings, scratchpad buffers, and **hotplug**, so you can plug a USB keyboard in mid-session. Falls back to PS/2 when no USB device is present.
* **PCI** enumeration for device discovery.
* **Serial** (COM1/COM2) for debug output, spinlock-protected so concurrent log writes from different threads don't interleave.
* **Power management**: ACPI poweroff and reboot, plus **RAPL** energy sampling to report average package wattage.

### Diagnostics

* A **live kernel dashboard** on `CTRL+SHIFT+ESC`: CPU brand and feature list, current power draw in watts, CPU utilisation, physical memory usage and fragmentation, heap and slab statistics, and a scaled table of every process and thread with its state, rendered to fit whatever the framebuffer gives it.
* A **crash console** that owns a separate shadow buffer sized at boot, so a panic can render a full red-screen report, including reason, decoded exception, error code, faulted address with a decoded access/mode/cause breakdown, `RIP`/`CS`/`RSP`/`SS` and expanded `RFLAGS`, and still scroll back through it instead of dumping one screen and dying.
* `QEMU_LOG()` and `LOGF()` for serial-side tracing, plus dump helpers like `QEMU_DUMP_PMT` and companion host scripts in [`docs/tools/`](./docs/tools/) that parse a raw page-table dump into something readable.

### Libraries

* **`klibc`**: kernel-side `string.h` with hand-written assembly fast paths for the hot memory routines, a `stdio.h` with buffered `printf`, and a **generic intrusive AVL tree** with fully non-recursive traversal (embed the node in your struct, supply a comparator, get `O(log N)` for free).
* **`ulibc`** is the userspace counterpart: `stdio`, `stdlib`, `string`, `math`, spinlocks, and raw syscall wrappers.


## What's *not* Inside the Kernel

Equally important that you hear it from me now rather than discover it three hours in. None of these are things that broke or that I gave up on. They're things the PawStack model genuinely does not need, so I spent that time on the parts it does.

| Not here | Why not |
|---|---|
| **SMP / multiple cores** | GatOS detects your core count, prints it proudly on the dashboard, and then politely uses exactly one of them. The scheduler, the allocators and the locking are all written for a single CPU. AP startup and per-CPU slab caches are the biggest item on the "after the thesis" list. |
| **A filesystem** | No VFS, no disk driver, no `open()`. Your Gata program is compiled *into* the kernel image, so there is nothing to load from disk at runtime, and therefore nothing that needs to go looking for it.* |
| **A network stack** | Same story. Nothing in the toolchain's model asks for one yet, and half-implementing TCP is a fantastic way to lose a semester.* |
| **`fork()` / `exec()` / ELF loading** | Processes are created by the kernel at boot rather than spawned from executables. Userspace code lives in its own linker section and ships inside the image. |
| **A bootloader** | GRUB already does this, and does it considerably better than I would have. |

> [!NOTE]
> If you came here looking for a general-purpose OS to run arbitrary programs on, this isn't it, and it was never trying to be. If you came here to compile *one* program into a bootable image that owns the entire machine, you are in exactly the right place.

**For filesystems and networking: These are subsystems that, if implemented in GatOS, can be easily wired up to libgata for high level support, I just didn't have the time. I am but a student, after all.*


## Getting Started

Building and running GatOS is designed to be exceedingly simple. If you have **Python 3.13+**, you can go from zero to running the kernel in two steps:

```bash
# 1. Install and configure the portable toolchain
python3 setup.py

# 2. Build and run the kernel in QEMU
python3 run.py
```

That's it!

> [!IMPORTANT]
> The kernel itself is feature complete and what's there is tested and stable. Internal APIs can still shift between releases though, so don't pin your project to a specific internal interface quite yet.

### How is this possible?

Starting with `GatOS v1.7.5`, the entire build toolchain (GCC, Binutils, QEMU, GRUB, mtools, xorriso) is statically cross-compiled and bundled.

This means no dependency hell, and full portability:

* You **do not need** to install `qemu`, `gcc`, or `make` on your host machine.
* The build runs identically on **Linux** (including WSL), **Windows**, and **macOS** (Intel & Apple Silicon).

### Advanced Usage

While `python3 run.py` is enough for most showcases, the script accepts several arguments for development. Usage is `python run.py [COMMAND] [BUILD PROFILE] [RUN OPTIONS]`.

**Commands:**

| Command | Description |
| --- | --- |
| `all` | Clean, build, and run. This is the default if no command is given. |
| `build` | Build the ISO only, without launching QEMU. |
| `clean` | Remove all build artifacts. |
| `help` | Show the full help menu. |

**Build profiles:**

| Profile | Description |
| --- | --- |
| `default` | Standard debug build. |
| `test` | Builds the in-kernel test image (`-DTEST_BUILD`). |
| `fast` | `-O2` optimizations. |
| `vfast` | `-O3` aggressive optimizations. Requires interactive confirmation, because it can (but probably won't) misbehave. |

**Run options:**

| Option | Description |
| --- | --- |
| `headless` | Run QEMU without a GUI (uses `-nographic`). |
| `timeout=XX` | Kill QEMU after the given duration (e.g. `10s`, `2m`, `1h`). |

Examples:

```bash
# Clean build artifacts
python run.py clean

# Build without running in QEMU
python run.py build

# Build and run an optimized image
python run.py all fast

# Aggressively optimized, no GUI
python run.py all vfast headless

# Run, but don't let QEMU outlive its welcome
python run.py all timeout=30s

# Display all available commands and options
python run.py help
```

> [!TIP]
> Optimized builds also run LTO and strip the final binary automatically (except for macOS because it apparently has special needs).

### Keyboard Shortcuts

Once the kernel is up:

| Shortcut | Action |
| --- | --- |
| `ALT + Tab` | Cycle between active TTYs |
| `ALT + F4` | Close the current TTY (the kernel TTY is protected) |
| `CTRL + SHIFT + ESC` | Toggle the kernel dashboard |


## Building the Toolchain from Source

> [!CAUTION]
> **This is strongly discouraged**, even for experienced developers.
> Attempt this only if you fully understand the scale of the undertaking.

While you *can try* to build the entire toolchain statically for your own platform if you do not wish to rely on the prebuilt binaries, please understand the following realities:

* The process is **extremely complex**, fragile, and heavily dependent on the host environment.
* It required **over 3 weeks** of nonstop trial and error to complete the provided build.
* Multiple components required **custom patching**, chaining patches on top of patches just to get them to compile.
* Keeping these builds working across platforms and versions would essentially require maintaining **an entirely separate project in its own right**.
* **No support will be provided** for source builds, because quite frankly it is outside the scope of the project.

Because I understand how difficult this process is, a small collection of experimental, incomplete, and largely unmaintained build scripts is included under [`docs/toolchain/`](./docs/toolchain/).

They:

* are **not guaranteed to work**,
* are **not tested**,
* can **break without warning**, and
* will almost certainly require **manual intervention** and fresh patching for newer upstream releases.

These scripts exist solely for transparency and educational insight, not as a supported or reliable build pipeline. For almost all users, including developers, using the prebuilt portable toolchain is the strongly recommended and intended workflow.


## Testing

Since `v1.7.5`, a test suite has been included in the kernel itself. It is built to be run in a live environment, which means the kernel itself will run the tests if you instruct it to do so.

The suite is around **500 assertions across 8 subsystems**: the PMM (including coalescing and exclusion behaviour), the slab allocator, the VMM, the heap's binned free lists, timers, spinlocks, the TTY subsystem, and multitasking. Since it runs live inside the kernel, it exercises the real allocators against real hardware state rather than a mocked-out host build.

### Running Tests

To build a GatOS Test image, all you need to do is specify it in `run.py`:

```bash
python run.py build test
```

To run it (aka, to run the tests):

```bash
python run.py test
```

### Current Testing Approach

Currently, most of the tests are ran locally before deployment. It is not pretty, but it works while the core functionality is being developed. There are workflows that check the debug log output for the built-in tests as well.

If you want to automate the test process in a server (just like my Github runners), you can run a headless version of QEMU with a timeout, like so:

```bash
python run.py test headless timeout=15s
```

The timeout is for the runner to stop QEMU after the specified time has elapsed. After that, you are free to write your own logic to parse `debug.log` and see if any tests have failed.


## Development

### Repository Layout

```
src/
├── arch/x86_64/     Boot assembly, CPU (GDT/IDT/ISR/syscall entry), paging, multiboot2
├── kernel/
│   ├── drivers/     Console, TTY, keyboard, i8042, xHCI, PCI, serial, dashboard, font
│   ├── memory/      PMM, VMM, heap, slab
│   ├── sys/         ACPI, APIC, scheduler, process, syscall, timers, power, panic, spinlock
│   ├── bootstrap.c  Staged kernel init sequence
│   └── kmain.c      Thin entry point
├── klibc/           Kernel-side libc + AVL tree
├── ulibc/           Userspace libc + syscall wrappers
└── tests/           In-kernel live test suite
targets/x86_64/      Linker script and GRUB config
docs/                Writeups, toolchain scripts, tooling
```

### Development Workflow

The development process follows a pretty standard Git workflow:

1. **Feature Branches**: New features are developed in separate branches (`memory`, `paging`, `threading`, `userspace`, `drivers`, `QOL`, and friends)
2. **Testing**: Write tests, or frankly, just see if it runs
3. **CI Validation**: GitHub Actions runs automated checks on the QEMU serial output across Linux, Windows, and macOS
4. **Merge**: Successfully tested branches get merged into `next`, and `next` into `main`

### Debugging

The main debugging tool is the `QEMU_LOG()` function. It's your best friend for figuring out what's happening (or not happening) in the kernel:

```c
QEMU_LOG("Kernel booting...", TOTAL_DBG);
// ... some code ...
QEMU_LOG("Memory manager initialized", TOTAL_DBG);
// ... more code ...
QEMU_LOG("Ready to handle interrupts", TOTAL_DBG);
```

You can also use all functions defined in `debug.h`, such as `QEMU_DUMP_PMT` for example, which dumps your page table structure in QEMU. There are companion scripts under [`docs/tools/`](./docs/tools/) for parsing those dumps into something a human can read.


## Documentation

A *lot* of documentation and writeups are available in the [`docs/`](./docs/) folder. This includes development notes, architecture decisions, learning resources, and basically everything I've figured out (or struggled with) during this journey. Whatever question you have, it's probably answered there.

The writeups are structured as chapters, and currently run to roughly **50,000 words**, officially closer to a book than to release notes:

| Chapter | Topic |
| --- | --- |
| [C0](./docs/C0_Introduction.md) | Introduction |
| [C1](./docs/C1_Setup_And_Build_Pipeline.md) | Setup and the build pipeline |
| [C2](./docs/C2_Memory_Foundational_Concepts.md) | Memory: foundational concepts |
| [C3](./docs/C3_Entering_Long_Mode.md) | Entering long mode |
| [C4](./docs/C4_Early_C_Setup.md) | Early C setup |
| [C5](./docs/C5_The_Physmap.md) | The physmap |
| [C6](./docs/C6_Interrupts_Panics_And_Spinlocks.md) | Interrupts, panics and spinlocks |
| [C7](./docs/C7_PMM_And_Slab.md) | The PMM and the slab allocator |

Please note that the documentation is NOT complete as of now. There will be a total of 15 chapters to cover the entirety of GatOS' internals, so this is roughly halfway there.

## Contributing

Contributions are not open since this is my thesis and thus must be my work alone. I need to be able to demonstrate that I understand every piece of code in this project, which means I have to write it myself.

However, you can still:
- **Report Issues**: If you find bugs or have questions, feel free to open issues
- **Provide Feedback**: Suggestions and feedback are always welcome through issues
- **Follow Along**: Watch the repository if you're interested in seeing how this progresses

The one exception is the documentation. Writeups under [`docs/`](./docs/) *are* open to corrections and clarity fixes via pull request.

Once the thesis is complete, I might consider opening it up for contributions, but that's a decision for future me.

## License

This project is licensed under a strict custom license that does not allow for replication of the code without explicit consent. I am unsure how this project will be used in the future, so the licensing is restrictive for now.

See the [LICENSE](LICENSE) file for details.

The restrictive nature is partly due to academic requirements and partly because I haven't decided what I want to do with this project long-term. This may change after thesis completion.

## Acknowledgments

- [Skyl-OS](https://github.com/Billyzeim/Skyl-OS) - A fantastic educational OS project from my dear friend, u/Billyzeim
- [The OS-Dev Wiki](https://wiki.osdev.org/Expanded_Main_Page) - The best starting place for OS development, with plenty of information on how to start.
- [MittOS64](https://github.com/thomasloven/mittos64) - Very good documentation that helped me through a lot of the struggles so far
- [Simple-OS](https://github.com/httpe/simple-os) - An already self-hosted modern kernel with libc ported, plenty useful for peeking inside implementations
- [OS-Series](https://github.com/davidcallanan/os-series/) - Helped me boostrap the entire project
- [OSDev-Notes](https://github.com/dreamportdev/Osdev-Notes/tree/master) - A book like no other, perfect for understanding every single detail of OS development

## So... what now?

This project isn't really all that exciting on its own, because GatOS is not meant to be a standalone kernel. Where things get exciting is with [The Gata Programming Language](https://github.com/ApparentlyPlus/Gata) and [Appa](https://github.com/ApparentlyPlus/Appa), the transpiler for Gata, which lowers your code to target a custom GatOS build! 

Why don't you setup appa and try writing your first Gata program? You are 10 lines of Gata code away from your very own first, custom, real, awesome operating system!


## Note to Readers

This repository is mostly done now, so expect a few changes here and there, a few releases that fix spurious bugs, etc. The most important thing to look forward to is the new documentation chapters, so keep an eye out for those.
