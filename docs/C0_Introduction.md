# Chapter 0: Introduction

Welcome to GatOS’s documentation! This is your guide to the internals of GatOS.

The road to a working kernel is messy, full of rabbit holes, and rarely well-documented. What you’ll find here are the notes from that journey — clear explanations of how GatOS works, written both as a reference for its design and as a record of the process.

Think of this as part technical guide, part personal log. It’s here to make the tough parts a little easier, and maybe a bit more fun. I hope it proves to be a valuable resource for your own explorations into the foundations of computing.

And if something doesn’t make sense, feel free to reach out — I don’t bite.

## So, what is bare metal code anyway?

Everything we write from here on out will be bare metal code. So what does that even mean?

In short: it means talking directly to the hardware, without an operating system in between to hold our hand. This means no standard libraries, no syscalls, no debugger, no safety nets. When you write bare metal code, you *are* the operating system.

Want to print something to the screen? You don’t call printf — that doesn't exist here. You write your own printf that pokes values straight into the hardware's video memory. If you want to store data, you don’t have a filesystem — you’re dealing with raw disk sectors. If you want to run multiple tasks, well, you have to build a scheduler yourself.

It’s both terrifying and exciting. Terrifying because you don’t get the conveniences you’re used to, and exciting because you’re in complete control. You decide how memory is managed, how processes run, how devices are talked to. Every line of code shapes the foundation of the system.

So, “bare metal code” isn’t a special language or toolchain — it’s just regular code (usually C and assembly) running with no layers in between you and the machine.

## What are we building?

Before diving in, let’s make one thing clear: **GatOS is not meant to be a full standalone operating system**. At least, not in the way Linux or Windows are. Instead, GatOS is best thought of as the **low-level API** on which everything else is built.

Here’s the bigger picture:

1. **Gata (the language):**

   * Think of Gata as our “Java-like” high-level programming language.
   * Programmers write their applications in Gata, using familiar concepts like objects, classes, and methods.

2. **Libgata / Gata.Core (the standard library):**

   * This is the runtime library for Gata.
   * It provides the objects, classes, and higher-level features that programmers rely on.
   * But under the hood, these features are implemented by calling directly into GatOS.

3. **GatOS (the kernel-level API):**

   * This is the bare-metal foundation.
   * It exposes low-level functionality like memory management, process scheduling, I/O, and kernel operations.
   * Gata.Core depends on these functions to “bridge the gap” between high-level abstractions and the hardware.

4. **Appa (the compiler):**

   * Appa takes user-written Gata code and transpiles it into C.
   * That C code links against `libgata` (which itself is backed by GatOS).
   * The result is a unified C program that can be compiled all the way down to bare-metal machine code.

When you put it all together:

**User code (in Gata) → calls Libgata (Gata.Core) → which calls GatOS APIs → which run directly on the hardware.**

The final output is a bootable ISO containing everything wired up from top to bottom.

> [!NOTE]
> I haven’t yet defined the grammar or syntax of Gata. For now, I describe it as “Java-like” only as an example, to give a rough idea of how it will work. In reality, Gata will be purpose-built for OS development and designed to integrate seamlessly with GatOS.

There are also details I’m skipping in this simplified overview. For instance, GatOS is intended to be modular: in the future you’ll be able to swap out schedulers, choose different memory management models, toggle between graphics and console mode, enable or disable paging, and even pick the kernel’s bitsize (32-bit vs 64-bit). All of these will eventually be configurable options within a Gata project.

That said, I’m just one person. For the purposes of my thesis, the focus will be on building a working demo of Pawstack with minimal functionality to showcase (I/O, memory, and hopefully scheduling). Once that’s done, I plan to expand GatOS’s modularity and eventually port it to other architectures.

## Kernel development 101

In order to load your bare metal code (the kernel), a few important things need to happen first. Kernels are actually pretty high-level compared to the earliest steps in the boot process.

It's important to make a few distinctions when we are dealing with hardware level code:

1. The CPU architecture matters a lot. x86, ARM, RISC-V all have different modes, different registers, and different startup expectations. The code you write for one architecture won’t just “work” on another.

2. Your kernel's bitsize is crucial. A 32-bit kernel is very different from a 64-bit Kernel. The bitsize changes calling conventions, available registers, address space, and how the CPU interprets instructions. It also changes how you set up things like segmentation and paging.

3. Hardware vendors differ. Beyond what the CPU provides, each device family (chipset, NIC, disk controller) often has its own registers and requirements. Other than the basic services the firmware gives you, drivers are your responsibility if you want to support a wide range of hardware.

> [!NOTE]
> Imagine that linux - a hardened, tested and highly maintained kernel - still doesn't support a wide range of devices because it lacks drivers. One such example is Razer headphones!

When your machine powers on, the CPU doesn’t jump straight into your kernel. It starts in a very primitive state, usually running firmware code (like the vendor's BIOS or UEFI) that knows just enough to find and load a bootloader. The bootloader then sets up a more usable environment: it switches the CPU into the right mode, sets up memory mappings, and finally hands control over to your kernel.

By the time your kernel starts running, a lot of heavy lifting has already been done for you. That’s why, in the grand scheme of things, kernels live “above” the raw startup details. Your job at that point is to take control of the hardware in a structured way: initialize devices, manage memory, schedule tasks, and provide services that higher-level code can rely on.

So while kernel development feels like you’re scraping against the metal, the truth is you’re already standing on a carefully prepared launchpad. The real challenge is deciding what your kernel should do once you’re in charge, and how to avoid completely corrupting evereything in the process.

**Because make no mistake** - OS Development is very delicate. One wrong move and everything falls apart.

## Bootstrapping

Enough theory — let's get a little practical.

Remember how I mentioned that in bare metal code, the CPU architecture is crucial? For GatOS, we must choose an instruction set architecture (ISA) to target. While the industry is increasingly adopting ARM, the initial version of GatOS will focus exclusively on x86_64 (AMD64).

This decision is driven by the scope of my thesis and the goal of delivering a functional demo. Also, most desktop computers and laptops still run on x86_64 processors, so we'll be covering a wide range of devices.

A future port to ARM, while desirable, would necessitate a significant rewrite and a dedicated cross-compilation toolchain — suffice it to say "not for now".

The documentation assumes foundational knowledge in several key areas. We will not be covering these basics:

1. **Assembly Language:** Proficiency with x86_64 assembly, including how an assembler generates machine code.

2. **The C Programming Language:** A deep understanding of C is required, as it is the primary language for GatOS. Some kernels prefer to use C++ for bare metal code, which is quite difficult to set up but gives you OOP versatility later on.

3. **Systems Programming Tools:** Familiarity with linkers, cross-compilers, Makefiles, and compiler flags is essential.

4. **Low-Level Concepts:** You should understand what libc functions do, how they were implemented, which implementations of libc exist (eg. Newlib, Musl) and architecture specific C quirks.

With that established, the following sections dive directly into the x86_64 boot process.

## An introduction to the x86_64 Boot Process

As I mentioned before, loading a kernel on x86_64 hardware is a staged process, where control is gradually handed over from the firmware to increasingly sophisticated portions of your code. A key aspect of OS development on this platform is that **you decide** how much of this process to implement yourself and where to begin execution. This journey progresses through three fundamental CPU modes, each offering different capabilities:

- **16-bit "Real Mode":** This is the initial state the CPU powers on in. It's a backwards-compatible mode with severe limitations: memory access is restricted to the first 1MB using a 20-bit address space (via the segment:offset addressing model), and there is no hardware-based memory protection, meaning any code can overwrite any other code or data. Before the introduction of protected mode, real mode was the only available mode for x86 CPUs; and for backward compatibility, all x86 CPUs start in real mode when reset. The BIOS (Basic Input/Output System) operates in this mode, and this is where the initial bootloader is loaded and begins execution. Its primary job is to load the next stage and prepare the CPU to leave this primitive state.

- **32-bit "Protected Mode":** This mode is a major step forward. It introduces fundamental modern features like virtual memory, paging, and hardware-enforced memory protection rings (which prevent user applications from accessing kernel memory). To enter this mode, the bootloader must configure a basic Global Descriptor Table (GDT) and disable the old BIOS interrupts. A kernel can choose to operate entirely in this 32-bit mode. However, for a 64-bit OS, this mode primarily acts as a necessary stepping stone. It is used to set up the more advanced data structures required to enter 64-bit mode, such as paging.

- **64-bit "Long Mode":** This is the ultimate target mode for a modern x86_64 kernel. It provides access to the full 64-bit register set (RAX, RBX, etc.), a flat 64-bit virtual address space, and a more modern programming environment. **Not all CPUs support this mode.** Therefore, a crucial responsibility of the 32-bit protected mode stage is to check for the availability of Long Mode (via the CPUID instruction) before attempting the transition. Failure to do so on an incompatible CPU would lead to a crash.


> [!NOTE]
> While the initial bootloader starts in 16-bit Real Mode, its capabilities are severely limited. To overcome these restrictions, many bootloaders implement a two-stage design:
>
> **Stage 1 (Real Mode):** The initial, small bootloader is used as a stub with the purpose of loading a larger, more powerful second stage of the bootloader. Its job is simply to get this more sophisticated code into memory.
>
> **Stage 2 (Protected Mode):** The second-stage bootloader then switches the CPU into 32-bit Protected Mode. This grants it the ability to access more than 1MB of memory and use modern CPU features. From this advantaged position, it can perform the setup required by the kernel.
>
> Only after completing all necessary preparation in Protected Mode does the bootloader finally hand over execution to the kernel, which can then begin running in its intended mode (be that 32-bit Protected Mode or 64-bit Long Mode).

## Bootloader Choice for GatOS

Given that GatOS is not an educational project, implementing our own bootloader from scratch is an impractical diversion. The process is notoriously complex, time-consuming, and, crucially, orthogonal to our primary goal of building a modern kernel. Therefore, we will leverage an existing, well-supported bootloader to handle the low-level hardware initialization and transition the CPU into a clean, high-level state.

This decision naturally leads to the question: which bootloader should we use?

Two primary candidates emerge:

### GRUB 2 (Grand Unified Bootloader, version 2)

GRUB is the ubiquitous, battle-tested standard on x86 systems. Its main advantage for kernel development is its support for the Multiboot specification.

**The Multiboot Advantage:** Multiboot provides a standardized contract between the bootloader and the kernel. By crafting a small Multiboot header in our kernel, we can instruct GRUB to:

- Load the kernel in a 32-bit entry point.
- Place the CPU into a known-good 32-bit protected mode state.
- Provide a memory map of the system through the multiboot2 struct.
- The multiboot2 struct, which contains additional boot information (command line, module locations, etc.), is passed to the kernel by GRUB.

**The Trade-off:** This convenience comes with a constraint. The kernel is loaded in 32-bit mode, meaning GatOS's very first entry point must be 32-bit code. We are then responsible for writing the subsequent assembly to check for CPU features (like Long Mode) and perform the final transition into 64-bit mode ourselves.

> [!NOTE]
> The Multiboot specification is an open standard that provides kernels with a uniform way to be booted by Multiboot-compliant bootloaders. The reference implementation of the Multiboot specification is provided by GRUB. It has many versions, which are detailed in [OSDev Notes](https://github.com/dreamportdev/Osdev-Notes/blob/master/01_Build_Process/02_Boot_Protocols.md).

### The Limine Bootloader

Limine is a modern, protocol-oriented bootloader designed with newer systems and features in mind.

**The Modern Advantage:** Limine's significant benefit is its ability to load the kernel directly into 64-bit Long Mode. It can handle the entire transition from real mode to protected mode and finally to long mode itself, configuring crucial requirements like paging and setting up the GDT according to how we configure it. This means the kernel can be written almost entirely in 64-bit C from the very first instruction, drastically simplifying our early boot code.

**The Trade-off:** By letting the bootloader set up the advanced environment, we cede a degree of low-level control. The initial state of the CPU and memory management is defined by Limine's protocol rather than our own precise specifications. Furthermore, while growing in popularity, it lacks the universal installation and troubleshooting resources available for GRUB.

> [!NOTE]
> For more information on how each of these work, I refer you once again to the [OSDev Notes](https://github.com/dreamportdev/Osdev-Notes/blob/master/01_Build_Process/02_Boot_Protocols.md) book, which is aimed at providing detailed explanations for beginners. 

As for GatOS, while I initially wanted to use Limine, I decided against it given that GatOS is made with modularity in mind, so we need to be able to control everything. GRUB it is!

## Project Roadmap

To close this introduction, I would like to present the roadmap of GatOS as I envision it, and what functionality GatOS should expose by the end of the project, organized by subsystem and progressively by level of abstraction.

### 1. **Core Runtime APIs (Gata.Core)**

These are the essential services that every Gata program relies on:

* **Basic I/O**

  High-level input/output operations for interactive programs:

  * Write text to the console.
  * Read keyboard input.

* **Memory Management**

  Safe, dynamic memory allocation for all Gata objects:

  * Allocate memory for objects (userland or kernel, depending on realm).
  * Release memory (userland or kernel, depending on realm).

* **Threading and Concurrency**

  Enable multitasking and concurrent execution in user programs:

  * Expose CPU information.
  * Start a new thread.
  * Wait for a thread to finish.
  * Pause execution for a defined period.
  * `mutex` and related functions — safe synchronization between threads.
  * Create concurrent userland processes.
  * Manage running processes (start, kill).

These APIs form the **foundation of Gata.Core**, allowing high-level code to run safely without worrying about the hardware or kernel internals.

---

### 2. **Extended Runtime (Later)**

As Gata grows, additional libraries will build on the core API to provide more sophisticated functionality:

#### a) **Gata.Filesystems**

Provides access to storage devices and files:

* Enumerate disks and partitions.
* Detect and interact with filesystems (FAT32, exFAT, NTFS).
* Basic file operations: open, read, write, delete.

#### b) **Gata.Net**

Enables networking for applications:

* Open TCP/UDP sockets.
* Send and receive data (GET/POST, streaming).
* Support for higher-level protocols.

#### c) **Gata.Peripherals**

Audio, Mouse, Keyboard, Camera APIs for device control:

* Play sound streams.
* Access peripherals such as speakers or audio devices.
* Control volume, channels, and basic mixing.
* Query mouse positions.
* Keyboard events.
* Camera video streams.

#### d) **Gata.Graphics**

Provides a high-level graphics abstraction:

* Create and manage windows.
* Handle user input events (mouse, keyboard).
* Draw shapes, text, and images.
* Query and set cursor positions.

---

### 3. **Key Principles**

1. **Gata Always Sees the High-Level API**

   Gata programs never directly manipulate physical memory, paging, or devices. All such operations are abstracted behind safe, consistent APIs. The idea is to only expose the fundamental building blocks. Anything that ***can*** be written in Gata ***should*** be written in Gata, not GatOS.

2. **Incremental Expansion**

   * Phase 1: Core I/O, memory, and threading.
   * Phase 2: Filesystems and networking.
   * Phase 3: Sound and graphics.

3. **Modularity and Future-Proofing**

   GatOS is designed so that the kernel can be extended or swapped without breaking Gata programs. For example, different schedulers, memory managers, or graphics backends can be added later. All kernel tweaks will be handled through Appa.