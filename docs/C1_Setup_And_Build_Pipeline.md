# Chapter 1: Setup and Build Pipeline

In the last chapter, we chose x86_64 as the instruction set for GatOS, with the goal of running the kernel in 64-bit long mode. That naturally leads to the next question: *where do we actually begin?* Most developers, at this point, are eager to jump straight into coding.

Tmpting though this might be, it is also where many projects go off the rails. If we start hacking things together just to get something running, the setup quickly becomes fragile. It works fine while the project is small, but as soon as complexity grows, every change demands extra manual steps or a round of painful refactoring. Tiny mistakes can cost hours. Before long, the “quick and dirty” approach ends up slowing progress instead of speeding it up. 

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
> I initially started GatOS using **NASM**, but when I later implemented a higher-half kernel (which we’ll cover in a future document), everything fell apart. **NASM no real integration with C**, making the project a nightmare to maintain. In hindsight, I had to redo almost everything to switch to GAS. Don’t make the same mistake — choose GAS from the start. It supports Intel syntax, works seamlessly with GCC, C, and C++, and even lets you include C/C++ headers, which are properly preprocessed.

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

## What is a Makefile (and make)?

A **Makefile** is a script-like file that defines how a project is built, and `make` is the tool that reads this script and executes it. In a complex project like an operating system kernel, there are many moving parts: C source files, assembly files, header files, linker scripts, and tools to package the final image. `make` **orchestrates all of these elements**, making sure everything happens in the correct order.

Instead of manually running compiler commands, assembling files, invoking the linker, and then creating a bootable image, `make` automates the entire workflow. It determines which files have changed and only rebuilds the necessary parts, saving time while avoiding human error. A Makefile tells `make`:

* **Which files to compile:** C, C++, or assembly sources.
* **Which compiler, assembler, and linker to invoke:** For example, `gcc` for C, `as` or GAS for assembly, and `ld` for linking.
* **Which linker scripts to use:** Ensuring the kernel is loaded at the correct virtual and physical addresses.
* **How to package the final output:** Such as creating a bootable ISO or copying the kernel to a disk image.
* **Dependency management:** If a header or assembly file changes, `make` ensures all affected files are rebuilt automatically.

TL;DR: `make` is the **central coordinator of the build process**. It connects the compiler, assembler, linker, and auxiliary tools into a single, repeatable pipeline. Without `make`, you would have to manually track every file, command, and build step, which is an error-prone and time-consuming task. With `make` and a well-structured Makefile, building even a complex kernel becomes reliable, efficient, and fully automated.


## Makefile Targets and Structure

At the heart of a Makefile are **targets**. A target is essentially a goal that `make` tries to produce: it could be a compiled object file, a linked kernel image, or even a fully bootable ISO. Each target is associated with:

1. **Dependencies:** Files that the target depends on. If any dependency changes, `make` knows the target must be rebuilt. For example, a compiled object file depends on its corresponding C or assembly source file and any headers it includes.
2. **Commands:** The shell commands `make` executes to build the target from its dependencies. For a C source file, this might be invoking `gcc` with the correct flags; for an assembly file, calling `as` or GAS; and for the final kernel image, invoking the linker with a linker script.

A typical Makefile target looks like this:

```make
kernel.bin: main.o start.o linker.ld
    ld -T linker.ld -o kernel.bin main.o start.o
```

* **`kernel.bin`** is the target.
* **`main.o start.o linker.ld`** are the dependencies.
* **`ld -T linker.ld -o kernel.bin main.o start.o`** is the command that builds the target.

---

### Common Types of Targets

* **Object files (`*.o`)** – Individual compilation units generated from C or assembly files.
* **Executable/kernel image (`kernel.bin`)** – The final linked output that the bootloader can load.
* **Clean target** – Deletes temporary or intermediate files to start a fresh build:

```make
clean:
    rm -f *.o kernel.bin
```

* **Phony targets** – Targets that don’t correspond to actual files, like `all` or `install`. These are often used to group multiple build steps.

---

### How `make` Orchestrates Everything

When you run `make kernel.bin`, it performs the following automatically:

1. Checks all dependencies (`*.c`, `*.h`, `*.S`) to see if they have changed since the last build.
2. Compiles C and assembly files into object files if needed.
3. Links all object files using the correct linker script.
4. Produces the final kernel binary at the correct load address.
5. Optionally packages the kernel into a bootable image or ISO.

All of this happens in the correct order without requiring you to manually run dozens of commands. In a kernel project, this ensures that changes in a single header or assembly file trigger all necessary rebuilds automatically, saving time and preventing errors.

## GatOS's Makefile

The Makefile of GatOS is essentially the cornerstone of the build process. It's what ties everything that was said above together!

Before we dissect it, it is helpful to have a tree of the project folder structure:

```
.
├── docs                            # The documentation
├── Makefile                        # The makefile is in the root folder
├── src                             # The root source code folder
│   ├── headers                     # Folder that contains the headers (.h files)
│   │   ├── libc                    # Ported libc function headers
│   │   └── memory                  # Headers that are related to memory management
│   └── impl                        # Folder that contains the implementations (.c files)
│       ├── kernel                  # Kernel implementation source files
│       ├── libc                    # libc implementation source files  
│       └── memory                  # Memory related implementation source files
│       └── x86_64                  # Early boot function implementations
│           └── boot                # Boot time assembly implementation files
└── targets                         # Targets (for makefile) output folder
    └── x86_64
        ├── iso                     # ISO generation folder
        │   ├── boot                
        │   │   └── grub
        │   │       └── grub.cfg    # GRUB config file (how to load the kernel)    
        │   └── EFI                 # EFI BOOT files for UEFI support
        │       └── BOOT            
        └── linker.ld               # The linker script for the project
```

Alright, with that in mind, let's take a look at GatOS's Makefile:

```make
# Toolchain configuration
CC := x86_64-elf-gcc
LD := x86_64-elf-ld

# Compilation and preprocessing flags
CFLAGS := -m64 -ffreestanding -nostdlib -fno-pic -mcmodel=kernel -I src/headers -g
CPPFLAGS := -I src/headers -D__ASSEMBLER__
LDFLAGS := -n -nostdlib -T targets/x86_64/linker.ld --no-relax -g

# Directories
SRC_DIR := src/impl
HEADER_DIR := src/headers
BUILD_DIR := build
DIST_DIR := dist/x86_64
ISO_DIR := targets/x86_64/iso

# UEFI output path (we will create this file)
UEFI_DIR := $(ISO_DIR)/EFI/BOOT
UEFI_GRUB := $(UEFI_DIR)/BOOTX64.EFI

# Discover all C and Assembly sources recursively
C_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.c')
ASM_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.S')

# Generate corresponding object file paths
C_OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRC_FILES))
ASM_OBJ_FILES := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC_FILES))
OBJ_FILES := $(C_OBJ_FILES) $(ASM_OBJ_FILES)

# Default target
.PHONY: all
all: iso

# Compile C source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

# Compile Assembly (.S) source files with preprocessing
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(@D)
	$(CC) -c $(CPPFLAGS) $< -o $@

# Link everything into a flat binary
.PHONY: build
build: $(OBJ_FILES)
	@mkdir -p $(DIST_DIR)
	$(LD) $(LDFLAGS) -o $(DIST_DIR)/kernel.bin $^

# Build a standalone EFI executable (grub) that embeds your grub.cfg
# This creates $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI which UEFI firmware will look for on removable media
$(UEFI_GRUB): $(ISO_DIR)/boot/grub/grub.cfg
	@mkdir -p $(@D)
	# embed the grub.cfg from the ISO tree into a standalone EFI binary
	grub-mkstandalone --format=x86_64-efi --output=$@ \
		--locales="" --fonts="" \
		"boot/grub/grub.cfg=$(ISO_DIR)/boot/grub/grub.cfg"

# Generate ISO image (BIOS + UEFI hybrid)
.PHONY: iso
iso: build $(UEFI_GRUB)
	@mkdir -p $(ISO_DIR)/boot
	cp $(DIST_DIR)/kernel.bin $(ISO_DIR)/boot/kernel.bin
	grub-mkrescue -o $(DIST_DIR)/kernel.iso $(ISO_DIR) || \
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $(DIST_DIR)/kernel.iso $(ISO_DIR);

# Clean all build and dist files
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) dist $(ISO_DIR)/boot/kernel.bin $(UEFI_GRUB)
```

***Woah.*** That's a lot, right? Well, don't worry! It's simpler than it looks! 

Let's break everything down.

This Makefile automates the process of building a freestanding (no operating system) x86_64 kernel from C and Assembly source files. It then packages this kernel into a bootable ISO image that can be run in an emulator (like QEMU) or on real hardware. The ISO supports both **legacy BIOS** and modern **UEFI** boot methods.

---

### Section 1: Toolchain Configuration

```makefile
CC := x86_64-elf-gcc
LD := x86_64-elf-ld
```

*   **`x86_64-elf-gcc` (The Compiler):** This is the **cross-compiler**. It runs on your host system (e.g., Linux) but produces machine code for a different target system (a bare-metal x86_64 environment). We cannot use the system's default `gcc` (e.g., `x86_64-linux-gnu-gcc`) because it would link against the host's standard libraries (like `libc`), which don't exist in a kernel.
*   **`x86_64-elf-ld` (The Linker):** This is the corresponding cross-linker. It takes all the compiled object files (`.o`) and combines them into a single executable binary according to the instructions in the linker script. The `-elf` output format is a generic format suitable for bare-metal targets.

---

### Section 2: Compilation and Linking Flags

```makefile
CFLAGS := -m64 -ffreestanding -nostdlib -fno-pic -mcmodel=kernel -I src/headers -g
CPPFLAGS := -I src/headers -D__ASSEMBLER__
LDFLAGS := -n -nostdlib -T targets/x86_64/linker.ld --no-relax -g
```

*   **`CFLAGS` (C Compiler Flags):**
    *   `-m64`: Generate 64-bit code.
    *   `-ffreestanding`: The compilation target is a freestanding environment (no OS). It assumes standard library functions like `malloc` or `printf` may not exist.
    *   `-nostdlib`: Do not link the standard C library (`libc`) or compiler runtime libraries (`libgcc`) by default. The kernel provides its own implementations for everything it needs.
    *   `-fno-pic`: Do not generate Position-Independent Code. The kernel is loaded at a fixed, known memory address.
    *   `-mcmodel=kernel`: Use the "kernel" code model, which is designed for code that will reside in the negative 2GB of the address space (the higher half of the virtual address space, a common kernel design).
    *   `-I src/headers`: Add the `src/headers` directory to the search path for `#include` directives.
    *   `-g`: Include debugging information (like line numbers), which is crucial for debugging with `gdb`.

*   **`CPPFLAGS` (C Preprocessor Flags):**
    *   These flags are used when preprocessing Assembly (`.S`) files.
    *   `-I src/headers`: Also allow assembly files to use headers.
    *   `-D__ASSEMBLER__`: Define a macro `__ASSEMBLER__`. This is sometimes used in header files to conditionally exclude C-specific code when being included in an assembly file.

*   **`LDFLAGS` (Linker Flags):**
    *   `-n`: Set the text section to be readable and writable (`nmagic`).
    *   `-nostdlib`: Same as for the compiler; don't use standard libraries.
    *   `-T targets/x86_64/linker.ld`: Use the custom **linker script**. This is the most important flag. The linker script dictates the memory layout of the kernel: where the `.text` (code), `.data` (initialized data), `.bss` (uninitialized data) sections are placed in memory. This is critical for the kernel to boot correctly.
    *   `--no-relax`: Don't perform certain optimizations that can interfere with the precise layout the linker script expects.
    *   `-g`: Include debugging information in the final binary.

---

### Section 3: Directory Structure

```makefile
SRC_DIR := src/impl
HEADER_DIR := src/headers
BUILD_DIR := build
DIST_DIR := dist/x86_64
ISO_DIR := targets/x86_64/iso
UEFI_DIR := $(ISO_DIR)/EFI/BOOT
UEFI_GRUB := $(UEFI_DIR)/BOOTX64.EFI
```

This defines the project's structure:
*   **`SRC_DIR`**: Where the source code lives.
*   **`BUILD_DIR`**: Where all the intermediate `.o` object files are stored. Keeping them separate from source is good practice.
*   **`DIST_DIR`**: Where the final outputs (the `kernel.bin` and `kernel.iso`) are placed.
*   **`ISO_DIR`**: The directory structure that will be turned into the final ISO image. It mimics the layout of a CD-ROM filesystem.
*   **`UEFI_GRUB`**: The path for the final GRUB EFI executable that will be built.

---

### Section 4: Automatic Source File Discovery

```makefile
C_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.c')
ASM_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.S')
C_OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRC_FILES))
ASM_OBJ_FILES := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC_FILES))
OBJ_FILES := $(C_OBJ_FILES) $(ASM_OBJ_FILES)
```

This is a powerful feature of Make.
1.  **`find` command**: Recursively searches `SRC_DIR` for all files ending in `.c` and `.S`.
2.  **`patsubst` function**: For each source file found (e.g., `src/impl/memory/paging.c`), it generates a corresponding path for the object file (e.g., `build/memory/paging.o`).
3.  **`OBJ_FILES`**: The complete list of all object files that need to be built and linked.

**Why?** You don't have to manually list every source file. Adding a new `.c` file anywhere in `src/impl` automatically includes it in the build.

---

### Section 5: Compilation Rules

```makefile
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(@D)
	$(CC) -c $(CPPFLAGS) $< -o $@
```

These are **pattern rules**.
*   **`%.o: %.c`**: "How to build a `.o` file from a `.c` file with the same name."
*   **`@mkdir -p $(@D)`**: Before compiling, create the necessary subdirectories inside the `build/` folder to mirror the source structure. `$(@D)` is the directory part of the target file.
*   **`$(CC) -c ...`**: The actual compilation command. The `-c` flag tells `gcc` to compile but not link.
*   **`$<`**: An automatic variable that expands to the first prerequisite (the source file).
*   **`$@`**: An automatic variable that expands to the target (the object file).

---

### Section 6: Linking Rule (`build` target)

```makefile
build: $(OBJ_FILES)
	@mkdir -p $(DIST_DIR)
	$(LD) $(LDFLAGS) -o $(DIST_DIR)/kernel.bin $^
```

*   **Prerequisite: `$(OBJ_FILES)`**: This target depends on all object files being up-to-date.
*   **Command:** The linker `LD` is called with all the flags (`LDFLAGS`) and the list of all object files (`$^` is an automatic variable that expands to *all* prerequisites). It outputs the final `kernel.bin` flat binary.

---

### Section 7: UEFI Bootloader Creation

```makefile
$(UEFI_GRUB): $(ISO_DIR)/boot/grub/grub.cfg
	@mkdir -p $(@D)
	grub-mkstandalone --format=x86_64-efi --output=$@ \
		--locales="" --fonts="" \
		"boot/grub/grub.cfg=$(ISO_DIR)/boot/grub/grub.cfg"
```

*   **Tool: `grub-mkstandalone`**: This is a GRUB tool that creates a single, self-contained EFI executable. This executable contains GRUB itself and the specified configuration file (`grub.cfg`) embedded inside it.
*   **Why?** UEFI firmware looks for a file at the exact path `\EFI\BOOT\BOOTX64.EFI` on removable media. This rule builds that file.
*   **`"boot/grub/grub.cfg=..."`**: This syntax tells `grub-mkstandalone` to take the file from the host system at `$(ISO_DIR)/boot/grub/grub.cfg` and embed it inside the EFI executable under the path `/boot/grub/grub.cfg`, which is where GRUB will look for it at runtime.

---

### Section 8: ISO Generation (`iso` target)

```makefile
iso: build $(UEFI_GRUB)
	@mkdir -p $(ISO_DIR)/boot
	cp $(DIST_DIR)/kernel.bin $(ISO_DIR)/boot/kernel.bin
	grub-mkrescue -o $(DIST_DIR)/kernel.iso $(ISO_DIR) || \
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $(DIST_DIR)/kernel.iso $(ISO_DIR);
```

This is the final packaging step.
1.  **Prerequisites:** It depends on the `kernel.bin` being built and the `BOOTX64.EFI` being created.
2.  **`cp ...`**: Copies the kernel binary into the ISO directory structure.
3.  **Tool: `grub-mkrescue`**: This tool creates a **hybrid ISO image** that can be booted from both a CD-ROM *and* a USB drive.
    *   It looks in the `$(ISO_DIR)` and creates an ISO from its contents.
    *   It automatically installs a **BIOS bootloader** (from the `i386-pc` GRUB module directory) for legacy boot.
    *   It includes the `$(UEFI_GRUB)` file we built for UEFI boot.
    *   The `||` (or) operator provides a fallback command in case the first one fails, specifying the path to the GRUB modules explicitly, which is often necessary on non-GRUB-based host systems.

---

### Section 9: The `all` and `clean` Targets

```makefile
.PHONY: all
all: iso

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) dist $(ISO_DIR)/boot/kernel.bin $(UEFI_GRUB)
```

*   **`all`**: The default target. Running `make` with no arguments will build the `iso` target.
*   **`.PHONY`**: Tells Make that these targets are not actual files. This prevents conflicts if a file named `all` or `clean` ever exists.
*   **`clean`**: Deletes all generated files and directories, allowing for a fresh build.

## Summary of the Build Flow

1.  **Compile:** `make` finds all `.c` and `.S` files and compiles them into `.o` files in the `build/` directory using the cross-compiler.
2.  **Link:** All `.o` files are linked into a single `kernel.bin` using the cross-linker and the linker script.
3.  **Package for UEFI:** The `grub-mkstandalone` tool creates a UEFI bootloader (`BOOTX64.EFI`) with the GRUB configuration embedded inside it.
4.  **Create ISO:** The `kernel.bin` and `BOOTX64.EFI` are placed into the ISO directory structure. `grub-mkrescue` then creates a bootable hybrid ISO image from this directory.
5.  **Run:** The resulting `dist/x86_64/kernel.iso` can be booted in QEMU or written to a USB drive.


## Build Automation Scripts

To streamline the development process, GatOS includes two convenience scripts that handle environment setup and execution.

### 1. `setup.sh` - One-Time Toolchain Installation
This script is designed to be run **once** to install the complete cross-compilation toolchain and all necessary dependencies on your system.

**Purpose:** To automate the often complex process of setting up a correct kernel development environment, ensuring everyone uses the same tools.

**What it installs:**
*   **Essential Build Tools:** `make`, `as`, `gcc` (host compiler)
*   **Cross-Compiler Toolchain:** The entire `x86-64-elf` toolchain, including:
    *   `x86-64-elf-gcc` (Cross-Compiler)
    *   `x86-64-elf-binutils` (Cross-Assembler and Linker)
    *   `x86-64-elf-gdb` (Cross-Debugger)
*   **Emulation & Imaging:**
    *   `qemu-system-x86` (QEMU emulator to run the kernel)
    *   `grub-common`, `grub-pc-bin`, `grub-efi-amd64-bin` (Tools for creating bootable ISO images for both BIOS and UEFI)
    *   `xorriso` and `mtools` (Utilities for constructing the ISO filesystem)

**Supported Environments:**
*   Debian-based distributions (Debian, Ubuntu, Pop!_OS, etc.)
*   Windows Subsystem for Linux (WSL) using a Debian/Ubuntu image

**Usage:**
```bash
# Make the script executable (only needed once)
chmod +x setup.sh

# Run the script. It will ask for your sudo password to install packages.
./setup.sh
```

>[!IMPORTANT]
> After running this script, you **must close and restart your terminal** for the newly installed tools (especially the cross-compiler) to be properly detected in your shell's path.

### 2. `run.sh` - The Development Loop
This script automates the daily development workflow: cleaning, building, and testing the kernel.

**Purpose:** To provide a fast and consistent one-command build-test loop, significantly speeding up development and testing.

**What it does:**
1.  **Checks for Dependencies:** Verifies that `make` and `qemu` are available. If not, it reminds you to run `setup.sh` first.
2.  **Cleans Previous Builds:** Runs `make clean` to remove any old object files and ensure a fresh build.
3.  **Compiles Everything:** Executes `make` to build the latest source code into a bootable ISO.
4.  **Launches the Kernel:** Automatically starts QEMU and boots the new ISO, allowing you to test your changes instantly.

**Usage:**
```bash
# After running setup.sh and restarting your terminal, use this for development
./run.sh
```

>[!IMPORTANT]
> GatOS is an ambitious, long-term project. As it grows in scope and complexity, the underlying build system and toolchain will be refined and extended to support new features, improve performance, and enhance portability. 
>
> This is not a how-to document, but it serves as a basis for the current build system. If you understand how it works, you won't have any trouble following future changes.