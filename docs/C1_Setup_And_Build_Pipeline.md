# Chapter 1: Setup and Build Pipeline

In the last chapter, we chose x86_64 as the instruction set for GatOS, with the goal of running the kernel in 64-bit long mode. That naturally leads to the next question: *where do we actually begin?* Most developers, at this point, are eager to jump straight into coding.

Tempting though this might be, it is also where many projects go off the rails. If we start hacking things together just to get something running, the setup quickly becomes fragile. It works fine while the project is small, but as soon as complexity grows, every change demands extra manual steps or a round of painful refactoring. Tiny mistakes can cost hours. Before long, the “quick and dirty” approach ends up slowing progress instead of speeding it up.

Picture being deep into the project and discovering that your whole foundation was flawed, and now, everything has to change.

That’s why the smarter first step is to establish a solid build pipeline. This isn’t busywork, it’s an investment. A well-structured pipeline gives you a reliable, repeatable way to build, no matter how large the project becomes or how the code is organized. More importantly, it clears mental space: instead of wrestling with the toolchain, you get to focus on what matters — designing and writing the kernel itself.

So yes, it’s tempting to dive right into the code. But laying the foundation with a robust build system will pay off many times over, saving us frustration and keeping momentum steady as GatOS grows more complex.

## What is a Freestanding Environment?

**Short answer:**

* Your program runs **without assuming the presence of an operating system** or its standard runtime libraries.
* You only have access to what the C or C++ standards define as *freestanding* (like `stdint.h`, `stddef.h`, etc.).
* You can provide your own startup code, memory management, and runtime support.

**Long answer:**

By default, when you compile a C program, it doesn’t start execution at `main()`. Instead, it begins at a function called `_start()`, provided by the C standard library. This function sets up global state, handles environment variables and command-line arguments, calls global constructors (in C++), and only then invokes `main()`.

For normal applications, this invisible setup makes life easier. But when writing an operating system, it becomes a problem: we can’t rely on the host OS, its libraries, or any runtime support, because these things don't exist. A program built without these dependencies is called **freestanding**.

In a freestanding environment, we assume nothing — not even the standard library, since it requires OS support. The kernel must provide everything itself, including its entry point and memory layout. That’s why a **linker script** is required: the linker doesn’t know where the kernel should start unless we tell it. We'll talk more about the linker script later on in this document.

C and C++ both define a handful of headers that are safe to use in freestanding code. For C, common ones include `<stdint.h>` and `<stddef.h>`. Compilers may also offer extra freestanding headers — for example, GCC and Clang provide `<cpuid.h>` for working with the x86 `cpuid` instruction.

>[!NOTE]
>Technically, a kernel *can* use certain compiler runtime routines or utility libraries, but the rule of thumb is to start with nothing and add back only what’s explicitly freestanding.

## What is Cross Compilation?

When you write code for an operating system, the compiler translates your source code into machine code that runs on a specific CPU architecture. Usually, if you’re building for the same architecture as your computer (for example, x86_64), you can just use your normal compiler.

But sometimes you want your OS to run on a **different CPU architecture** — like building a RISC-V OS while working on an x86 machine. In that case, you need a **cross compiler**: a compiler that generates code for one architecture while running on another.

Even when building for the same CPU, using a cross compiler can be helpful. It lets you control exactly how the code is built, rather than relying on the host system’s compiler, which might add extra assumptions or features you don’t want for a kernel.

The most common compilers for OS development are **GCC** and **Clang**. Both can do the job; the differences are mostly in style and minor features rather than core capabilities. 

For GatOS, we are going to use the **GCC cross compiler for x86_64.**

## The Assembler

When writing a kernel, some assembly code is unavoidable, especially during the bootstrap phase before jumping into C. That means we need an assembler.

>[!NOTE]
>An assembler translates assembly code into an object file containing machine code. This object file is then linked with other object files to produce the final executable.

There are two common choices: **NASM** and **GNU Assembler (GAS)**. Usually, **NASM** is the industry standard given the simplicity of the intel syntax. However, there are also some drawbacks to it:

### NASM

* Uses Intel-style syntax, which is often considered easier to read for beginners.
* Has its own preprocessor with support for macros, constants, and conditional assembly.
* **Cannot** directly include or parse C headers — any constants or structures from C code must be manually converted or generated.
* Simpler and cleaner for small hobby OS projects where tight integration with C code is not required.

### GAS

* Uses AT\&T syntax by default, but supports Intel syntax via `.intel_syntax` if desired.
* Works seamlessly with the **C preprocessor (`cpp`)**, allowing direct inclusion of C headers, macros, and constants.
* Ideal for projects where assembly and C need to share definitions, reducing duplication and errors.
* Integrates smoothly with GCC/Clang toolchains, including cross-compilation and multi-architecture support.

For GatOS, **the proper assembler to use is GAS for a couple of reasons:**

* It allows sharing constants and structures directly between C and assembly, which is critical in a kernel where low-level code and data definitions are tightly coupled.

* It provides a more maintainable and scalable workflow compared to NASM, especially as the project grows.

* The integration with standard toolchains and preprocessors simplifies building and testing across different architectures.


> [!IMPORTANT] 
> I initially started GatOS using **NASM**, but when I later implemented a higher-half kernel (which we’ll cover in a future document), everything fell apart. **NASM has no real integration with C**, making the project a nightmare to maintain. In hindsight, I had to redo almost everything to switch to GAS. Don’t make the same mistake — choose GAS from the start. It supports Intel syntax, works seamlessly with GCC, C, and C++, and even lets you include C/C++ headers, which are properly preprocessed.

## What is a Linker Script?

Before we dive into linker scripts, it helps to understand what a **linker** is. 

A **linker** is a tool that takes one or more object files generated by a compiler or assembler and combines them into a single executable, library, or kernel image. It resolves references between files, such as function calls and global variables, assigns final memory addresses, and incorporates any necessary runtime or system code, producing a complete program that can be loaded and run by the CPU.

>[!NOTE]
> In GatOS, since we are using the GCC x86_64 cross compiler, we are also going to use the **GCC x86_64 linker**.

Now, a **linker script** is a text file written in the linker's own command language. It acts as a **blueprint** or a **project manager** for the linker (`ld`). Its primary job is to take the various input sections (`.text`, `.data`, `.bss`, etc.) from all your compiled object files (`.o`) and decide:

1.  **Where** in the output binary's memory space to place them (their **load address** or **virtual memory address**).
2.  **In what order** to place them.
3.  How to handle symbols and alignment.

### Common Linker Sections

* **`.text`** – Contains executable code (functions and instructions).
* **`.rodata`** – Read-only data, such as constants and string literals. Cannot be modified at runtime.
* **`.data`** – Initialized global and static variables. Stored in RAM and can be modified at runtime.
* **`.bss`** – Uninitialized global and static variables. The linker reserves space for them, and they are zeroed at startup.
* **`.boot`** – Early boot or bootstrap code, executed before the main kernel code.
* **`.stack`** – Reserved memory for stack space. Some kernels define a dedicated section, though it may just be allocated in `.bss`.
* **`.heap`** – Reserved memory for dynamic allocation (if defined in the linker script).

**Note:** These sections help the linker organize memory layout, ensuring code, read-only data, initialized variables, and uninitialized variables are placed in the correct locations in the final binary.


## Why is it Especially Critical for 64-bit Kernels?

The need for a custom linker script is universal for kernels, but 64-bit architectures introduce specific complexities that make it non-negotiable.

### a) The Higher-Half Kernel Design

This is the most important reason. Nearly all modern 64-bit kernels are **higher-half kernels**. This means they are linked to run in the **upper half of the virtual address space**. We are going to go in depth about this in another document, but here is a short description for the curious:

*   **Physical Memory:** RAM exists at physical addresses starting from `0x0` to `0x...(however much RAM you have)`.
*   **Virtual Memory:** The CPU's memory management unit (MMU) translates these physical addresses into virtual addresses that programs see.
*   **The x86_64 Design:** The x86_64 architecture defines a "canonical" address space. The most significant 16 bits of a 64-bit address must be all 0s or all 1s. This effectively splits the 2^64 address space into two halves:
    *   **Lower Half:** `0x0000000000000000` to `0x00007FFFFFFFFFFF` (User space)
    *   **Higher Half:** `0xFFFF800000000000` to `0xFFFFFFFFFFFFFFFF` (Kernel space)

**Why do this?**
1.  **Isolation and Security:** It cleanly separates kernel space from user space. User-space code cannot even *address* kernel memory, preventing accidental or malicious modification.
2.  **Convenience:** The kernel can have a single, fixed virtual address for all of its code and data, regardless of where it's loaded in physical memory. This simplifies the kernel's memory management code immensely.
3.  **The "-mcmodel=kernel" Flag:** The compiler flag `-mcmodel=kernel` is designed specifically for this! It assumes all kernel code and data will be located in the top 2GB of the 64-bit address space, allowing it to generate efficient code for accessing kernel symbols.

**The Linker Script's Role:** The linker script is what *enforces* this design. It tells the linker, "Place all the kernel's sections starting at virtual address `0xFFFFFFFF80000000` (upper 2GB of virtual address space)."

The bootloader might load the kernel at physical address `0x100000` (1 MiB), but the kernel's code *expects* to be running at its high-half virtual address. The early boot code in the kernel is responsible for setting up paging to make this mapping happen.

>[!IMPORTANT]
> Concepts like paging, the higher-half kernel, virtual vs. physical memory, and the separation between userland and kernel space might seem unfamiliar right now. Don’t worry! We’ll cover all of these topics in detail in future chapters. This is just a brief overview for readers who already have some background.

### b) Precise Control over the Boot Process

When the bootloader (GRUB) hands control to your kernel, it does so at a very specific entry point. The linker script ensures that this entry point (usually a symbol like `start` or `_start`) is placed at the very beginning of the binary output. The bootloader will jump to the first byte of the loaded kernel image, so the first thing there *must* be executable code.

A default linker script might place a read-only data section first, which would cause the CPU to interpret data as code and crash immediately.

### c) Managing the "BSS" Section

The BSS section contains statically allocated variables that are initialized to zero (e.g., `static char buffer[4096];`).

*   **In a user program,** the OS loader automatically zeros this memory before the program starts.
*   **In a kernel,** there is no loader. The kernel itself is responsible for this.

The linker script does two crucial things:
1.  **Defines the BSS Symbols:** It calculates the start and end addresses of the BSS section.
2.  **Doesn't Store Zeros:** It tells the linker that the BSS section should take up space in the *memory image* but should not take up space in the actual *disk image* (the `kernel.bin` file). This makes the kernel binary much smaller.

### d) Ensuring Correct Alignment

CPU architectures have specific alignment requirements for code and data structures for performance and correctness reasons (e.g., SSE instructions often require 16-byte alignment). The linker script allows you to specify the alignment of sections (e.g., `ALIGN(4K)` to align to a page boundary), which is essential for setting up paging tables later.


## Dissecting a Typical Kernel Linker Script

Let’s examine GatOS’s `linker.ld`, which implements a higher-half x86\_64 kernel:

```ld
ENTRY(start)

KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000;
KPHYS_START = 0x10000;

SECTIONS
{
    . = KPHYS_START + KERNEL_VIRTUAL_BASE;
    KVIRT_START = .;

    .boot : AT(ADDR(.boot) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        KEEP(*(.multiboot_header))
    }

    .text : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.text)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.data)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        *(.bss*)
    }

    .stack : AT(ADDR(.stack) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
    {
        KERNEL_STACK_BOTTOM = .;
        . = . + 0x8000; /* 32 KiB stack */
        KERNEL_STACK_TOP = .;
    }

    . = . + 0x1000; /* Skip a 4KB block for safety */
    
    KPHYS_END = . - KERNEL_VIRTUAL_BASE;
    KVIRT_END = .;
}
```

### Key Concepts and Sections

#### 1. **Higher-Half Kernel: Virtual vs Physical**

* The kernel is **linked to run at a high virtual address** (`KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000`), but the bootloader loads it at a lower **physical address** (`KPHYS_START = 0x10000`).
* The `AT()` directive tells the linker:

  > “Place this section in the binary at this **physical load address**, but reference it in the code using this **virtual address**.”
* Example (`.boot` section):

  * **Virtual Address (VMA):** `0xFFFFFFFF80010000`
  * **Physical Load Address (LMA):** `0x10000`

This separation allows the kernel to start executing before paging is enabled (at the physical address) and seamlessly continue at the virtual address once paging is active.

---

#### 2. **The `.boot` Section and `.multiboot_header`**

* `.multiboot_header` is a **symbol defined in assembly** that the Multiboot-compliant bootloader looks for (at a specific physical address) to verify and load the kernel.
* By placing it in `.boot` and marking it with `KEEP()`, the linker ensures the header is **not discarded** during linking, and that it exists right after `0x10000` physical.

---

#### 3. **Other Sections**

| Section   | Purpose                                                                                                                                                |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `.text`   | Executable code (functions, kernel routines).                                                                                                          |
| `.rodata` | Read-only data like constants and strings.                                                                                                             |
| `.data`   | Initialized global/static variables.                                                                                                                   |
| `.bss`    | Uninitialized global/static variables (zeroed at startup).                                                                                             |
| `.stack`  | Reserved memory for the kernel stack. Provides symbols `KERNEL_STACK_BOTTOM` and `KERNEL_STACK_TOP` for assembly code to initialize the stack pointer. |

* All sections are aligned to **4 KiB** pages for consistency with paging.
* Skipped memory (`. = . + 0x1000`) ensures there’s a safe block for temporary use or metadata.

---

#### 4. **Memory Boundary Symbols**

* `KPHYS_END` – Physical end of the kernel image; important for the physical memory manager (PMM).
* `KVIRT_START` / `KVIRT_END` – Virtual start and end of the kernel; used by the virtual memory manager (VMM) to know the kernel’s address range.

---

#### 5. **Workflow: Bootloader vs Kernel**

1. **GRUB’s perspective:**

   * Loads the kernel at `0x10000` (physical memory).
   * Finds `.multiboot_header` to verify that it's a multiboot compliant kernel.
   * The multiboot header must be located within the first 8192 bytes of the OS image.
   * Jumps to `start` (physical address), in 32-bit protected mode.

2. **Kernel’s perspective:**

   * Early assembly (`start`) is in 32-bit protected mode. It sets up the stack, page tables, and prepares for the long mode jump.
   * Maps `KERNEL_VIRTUAL_BASE + X → Physical Address X`.
   * Once paging is enabled, the kernel continues execution **at the high virtual address**, as linked.

## GatOS’s Project Structure

Before we get to the build system, it helps to know where everything lives. Here’s the full layout of the `src/` directory:

```
src/
├── arch/
│   └── x86_64/
│       ├── boot/          # Early boot assembly (header.S, boot32.S, boot64.S)
│       ├── cpu/           # CPU-level code (GDT, IDT, ISR.S, syscall_entry.S, io.h, msr.h)
│       └── memory/        # Paging structures and early memory setup (paging.c/h, layout.h)
├── kernel/
│   ├── kmain.c            # The kernel entry point
│   ├── drivers/           # Device drivers (console, serial, tty, keyboard, font, dashboard, pci, xhci…)
│   ├── memory/            # Dynamic memory management (pmm, slab, vmm, heap)
│   └── sys/               # Core subsystems (panic, acpi, apic, scheduler, process, syscall, timers…)
├── klibc/                 # Kernel-side standard library (avl, stdio, string, math)
├── ulibc/                 # Userspace standard library (linked into Ring 3 programs)
└── tests/                 # Kernel test suite
```

The **include root** is `src/`. This means `#include <kernel/drivers/console.h>` resolves to `src/kernel/drivers/console.h`, and `#include <arch/x86_64/cpu/io.h>` resolves to `src/arch/x86_64/cpu/io.h`. 

Also, there is no separate `headers/` folder. Header files live alongside their implementation files, organized by subsystem.

## The Build System

GatOS used to use a Makefile. I switched to a Python build script ([`run.py`](/run.py)) because it gave me flexibility that Make just doesn’t have: parallel compilation across all CPU cores, conditional build profiles, integrated QEMU launching with timeout support, and easy cross-platform compatibility without any path gymnastics.

The concept is the same as any build system: find all source files, compile them, link, package. But now it’s Python driving the whole thing instead of Make recipes.

* `setup.py` handles **toolchain provisioning**.
* `run.py` handles the **day-to-day build, packaging, and QEMU run loop**.

That split is deliberate. Kernel development has two separate problems:

1. We want a predictable cross-platform toolchain onto the machine, and
2. we want to easily fire up the kernel.

We don't want to rely on the host OS to provide the right binaries for us (GCC, xorriso, GRUB, etc.), but rather, we want to ensure that a good and tested toolchain is accessible.

## One Time Toolchain Provisioning

The first script to know is [`setup.py`](/setup.py). Its job is not to compile GatOS itself. Its job is to make sure the portable prebuilt toolchain exists under `toolchain/` for the host operating system.

### What It Does

At startup, the script detects the host platform using Python’s `sys.platform` and selects an OS-specific archive:

* Linux downloads `x86_64-linux.zip`
* macOS downloads `x86_64-macOS.zip`
* Windows downloads `x86_64-win.zip`

Each archive is fetched from the project’s GitHub release assets and has a hardcoded SHA-256 checksum in the script. After downloading, `setup.py`:

1. verifies the archive hash,
2. extracts it into `toolchain/`,
3. removes the temporary ZIP file,
4. fixes executable permissions on Unix-like systems, and
5. on macOS, removes quarantine attributes and ad-hoc signs binaries so Gatekeeper does not block the bundled tools.

The final validation step checks that the expected binaries are present. That includes the cross-compiler, linker, GRUB tools, and QEMU.

### Why This Exists

Normally, OS dev tutorials tell you to manually build or install a cross-compiler, GRUB utilities, ISO tools, and QEMU. That works, but it creates a lot of machine-specific drift. `setup.py` avoids that by pulling in a known-good, prepackaged toolchain so the build behaves the same way across supported hosts.

For more information, check out the project's [README](/README.md#getting-started) file.

### How to Run It

```bash
python3 setup.py
```

If the toolchain directory already exists and looks valid, the script asks whether it should redownload and repair it or just keep the existing installation.

>[!NOTE]
> This script installs the portable toolchain into the repository itself, under `toolchain/x86_64-linux`, `toolchain/x86_64-macos`, or `toolchain/x86_64-win`, depending on the host OS. 
>
> `run.py` then uses those local binaries directly instead of relying on globally installed host tools. This means that if you wish to delete the toolchain, you can just delete the `toolchain` folder. EVerything is self contained!

## Build, Package, and Run

Once `setup.py` has populated the toolchain, the main development loop goes through `run.py`.

This script replaces the old Makefile-style workflow with a Python driver that can do all of the following in one place:

* discover source files automatically,
* compile in parallel,
* apply build profiles,
* link and strip the kernel,
* generate the bootable ISO,
* start QEMU,
* optionally run headless, and
* optionally enforce a timeout for automated test runs.

### Basic Usage

The default behavior is:

```bash
python3 run.py
```

That means it will:

1. verify the portable toolchain,
2. clean old build artifacts,
3. rebuild the kernel and ISO, and
4. boot the result in QEMU.

### Supported Commands

`run.py` recognizes four top-level commands:

| Command | Description |
| --- | --- |
| `python3 run.py` | Default full cycle: clean, build, then run in QEMU. |
| `python3 run.py build` | Clean and build the ISO, but do not launch QEMU. |
| `python3 run.py clean` | Remove build artifacts, generated ISO output, temporary boot files, and `debug.log`. |
| `python3 run.py help` | Print the built-in help menu. |

### Build Profiles

Build profiles are passed as positional arguments next to the command:

| Profile | Effect |
| --- | --- |
| `default` | Standard debug-oriented build. |
| `test` | Adds `-DTEST_BUILD` and uses the `fast` optimization set. |
| `fast` | Uses `-O2` and related optimization flags. |
| `vfast` | Uses aggressive `-O3`-style flags and asks for confirmation before continuing. |

Examples:

```bash
python3 run.py
python3 run.py build test
python3 run.py fast
python3 run.py vfast
```

### Run Options

When QEMU is launched, two extra runtime switches are supported:

| Option | Effect |
| --- | --- |
| `headless` | Adds `-nographic` and runs QEMU without the graphical window. |
| `timeout=30s` | Stops QEMU after a fixed duration. Supported suffixes are `s`, `m`, and `h`. |

Examples:

```bash
python3 run.py headless
python3 run.py test headless timeout=10s
```

This is especially useful when you want automated test boots or CI style smoke runs without leaving QEMU open indefinitely.

## What `run.py` Actually Does

At a high level, the script performs the following steps.

### 1. Verify the Local Toolchain

Before doing anything else, `run.py` resolves the host-specific toolchain directory and checks that the expected binaries exist. If they do not, it aborts with a clear message telling you to run:

```bash
python3 setup.py
```

This is important because the build script does not assume your system `PATH` contains the right compiler, linker, GRUB utilities, or QEMU binary.

### 2. Discover Sources Automatically

The script walks `src/` recursively and picks up:

* every `*.c` file, and
* every `*.S` file.

That means adding a new C or assembly source file anywhere under `src/` automatically makes it part of the next build. No Makefile edits, no hand-maintained source lists.

### 3. Compile in Parallel

Compilation uses Python’s `multiprocessing.Pool`, with one worker per CPU core.

For each source file, the object output mirrors the source tree inside `build/`. For example, a file in `src/kernel/drivers/` becomes an object file under `build/kernel/drivers/`.

Kernel and userspace code are treated slightly differently:

* kernel code gets Link Time Optimizations (`-flto`) plus explicit floating point restrictions such as `-mno-sse`, `-mno-sse2`, `-mno-mmx`, and `-mno-80387`,
* userspace code gets `-ffast-math`, and
* assembly is compiled through GCC with preprocessor support enabled.

That distinction matters because the kernel should not casually rely on FPU or SIMD state before the OS has complete control over saving and restoring it.

>[!IMPORTANT]
>We will talk about FPU and userspace later on in the documents. For now, you dont need to know any of this.

### 4. Link and Strip the Kernel

After compilation, all object files are linked into `dist/x86_64/kernel.bin` using the cross GCC driver with linker options forwarded through `-Wl,...`.

The binary is then stripped to remove unnecessary symbol data from the final boot image.

### 5. Build the Boot Media

The packaging stage has two parts:

1. `grub-mkstandalone` creates `BOOTX64.EFI` inside `targets/x86_64/iso/EFI/BOOT/`.
2. `grub-mkrescue` turns the populated ISO tree into a bootable image under `dist/x86_64/`.

The ISO filename is generated from the kernel version (eg. "GatOS-v2.0.0.iso"). Test builds also embed `Test-Build` in the filename (eg. "GatOS-v2.0.0-Test-Build.iso").

### 6. Launch QEMU

`run.py` automatically boots the newest generated ISO with QEMU, unless the `build` flag is passed in (which instructs it to just perform a build).

QEMU is launched with certain flags including:

* `-serial mon:stdio` to route `COM1` serial directly to standard output (your host's terminal),
* `-serial file:debug.log` to route `COM2` serial output in a log file, and
* `-cpu kvm64,+smep,+smap` to emulate a more realistic x86_64 environment.

>[!IMPORTANT]
>We will talk about serial and how the kernel uses it in later chapters.

## Compiler and Linker Flags

The baseline C compilation flags are:

```text
-m64 -ffreestanding -nostdlib -fno-pic -mcmodel=kernel -mno-red-zone
-ffunction-sections -fdata-sections -I src/
```

These are the core freestanding kernel-development settings:

* **`-m64`** generates 64-bit code.
* **`-ffreestanding`** disables hosted-environment assumptions.
* **`-nostdlib`** prevents implicit linkage against the host C runtime.
* **`-fno-pic`** keeps the kernel at a fixed code model rather than position independent userspace conventions.
* **`-mcmodel=kernel`** matches the higher-half 64-bit kernel layout discussed earlier.
* **`-mno-red-zone`** avoids the x86_64 red zone, which is unsafe for kernels because interrupts can clobber it.
* **`-ffunction-sections`** and **`-fdata-sections`** make dead-code elimination more effective.
* **`-I src/`** sets the include root to the source tree.

The linker stage effectively applies:

```text
-nostdlib -flto -g
-Wl,-n,--gc-sections,--no-relax,-Ttargets/x86_64/linker.ld
```

Important parts here are:

* **`--gc-sections`** to discard unreachable code and data,
* **`--no-relax`** to avoid linker relaxations that could disturb the kernel’s carefully controlled layout, and
* **`-T targets/x86_64/linker.ld`** to force the custom linker script we analyzed earlier in this chapter.

## Summary of the Current Build Flow

The modern GatOS workflow is now:

1. **Provision toolchain:** `python3 setup.py` downloads and validates the host-specific portable toolchain into `toolchain/`.
2. **Verify environment:** `python3 run.py` checks that those local binaries exist and are usable.
3. **Compile:** all `*.c` and `*.S` files under `src/` are compiled in parallel into `build/`.
4. **Link:** the objects are linked into `dist/x86_64/kernel.bin` with the linker script.
5. **Stage boot assets:** GRUB files and the kernel are copied into the ISO directory tree under `targets/x86_64/iso/`.
6. **Create ISO:** GRUB tooling produces a bootable image in `dist/x86_64/`.
7. **Run:** if requested, QEMU boots the newest ISO and mirrors serial output to `debug.log`.

>[!IMPORTANT]
>You don't really have to know any of this. This chapter simply describes how GatOS's build system works internally. To run the kernel, all you need to do is call `setup.py` and `run.py`. That's all!