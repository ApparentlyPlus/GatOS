# Chapter 3: Entering Long Mode

Alas, the moment you've all been waiting for! I am proud to announce that, in this chapter, we will actually jump into the bootstrapping code of GatOS! We’ll bring to life everything discussed in the previous chapter, along with a few other essential checks. But first, let’s lay the groundwork necessary.

## GatOS's Project Structure

GatOS's Makefile, introduced in Chapter 1, is designed around a specific project layout. Below, I’ll outline the key directories relevant to our current work:

```
.
├── Makefile
├── src
│   ├── headers
│   └── impl
│       ├── kernel
│       └── x86_64
│           └── boot
└── targets
    └── x86_64
        ├── iso
        │   └── boot
        │       └── grub
        │           └── grub.cfg
        └── linker.ld
```

Here’s a quick guide to how these folders are organized:

1. All GatOS source code lives under `./src/`.
2. Inside `./src`, code is split between `headers/` and `impl/`.
3. Most C source files that are related to the kernel go into `impl/kernel/`.
4. Boot-related C code belongs in `impl/x86_64/`.
5. Boot-time assembly code is placed in `impl/x86_64/boot/`.
6. Header files, in general, reside in `headers/`.
7. For better organization, `impl/` can include subdirectories like `impl/memory/`.
8. Similarly, `headers/` can be subdivided: for example, into `headers/memory/`. These can then be included using syntax like `#include <memory/header.h>`.
9. The `targets/` directory holds configuration files and metadata required during compilation.
10. During the build, two new folders will be generated: `build/` (for intermediate artifacts) and `dist/` (for final output files).

With this structure in place, we’re ready to start coding our journey into long mode.

## The GRUB Configuration

A GRUB configuration file is essential for telling the bootloader how to load our kernel. It specifies what to display in the boot menu, which files to load, and what parameters to pass to the kernel. Without this configuration, GRUB wouldn't know how to properly initialize and execute our operating system.

Here's GatOS's GRUB configuration (`grub.cfg`):

```grub
set timeout=0
set default=0

menuentry "GatOS" {
    multiboot2 /boot/kernel.bin
    boot
}
```

This configuration does several important things:

- `set timeout=0` - Immediately boots the default entry without showing a menu
- `set default=0` - Selects the first menu entry as default
- `menuentry "GatOS"` - Creates a boot menu entry labeled "GatOS"
- `multiboot2 /boot/kernel.bin` - Loads our kernel using the Multiboot2 protocol
- `boot` - Executes the loaded kernel

>[!NOTE]
> GatOS ships *along with GRUB*, meaning that GRUB comes pre-packaged with GatOS inside the final ISO image. 

## The Multiboot2 Header

I won't go into excessive detail here, as we've already covered what Multiboot is and why we need it. The key point is that for GRUB to provide us with the Multiboot2 information struct, we must first declare our kernel as Multiboot2-compliant.

For those interested in the finer details, you can always refer to the official [GRUB Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html). Below, I'll quote the most relevant parts:

> An OS image must contain an additional header called Multiboot2 header, besides the headers of the format used by the OS image. The Multiboot2 header must be contained completely within the first 32768 bytes of the OS image, and must be 64-bit aligned. In general, it should come as early as possible, and may be embedded in the beginning of the text segment after the real executable header.

This means we need to embed the Multiboot2 header within our kernel image so GRUB can recognize it at boot time and pass us the necessary data structure. The header format is as follows:

> The layout of the Multiboot2 header must be as follows:
>
> | Offset | Type | Field Name    | Note     |
> | :----: | :--: | :------------ | :------- |
> | 0      | u32  | magic         | required |
> | 4      | u32  | architecture  | required |
> | 8      | u32  | header_length | required |
> | 12     | u32  | checksum      | required |
> | 16-XX  |      | tags          | required |
>
> The fields 'magic', 'architecture', 'header_length' and 'checksum' are defined in Header magic fields, 'tags' are defined in Header tags. All fields are in native endianness. On bi-endian platforms native-endianness means the endiannes OS image starts in.

Here is a description of each field:

| Field Name    | Description                                                                                                                                                                                                                        |
| :------------ | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **magic**     | The magic number identifying the header, which must be the hexadecimal value `0xE85250D6`.                                                                                                                                         |
| **architecture** | Specifies the Central Processing Unit Instruction Set Architecture. Since 'magic' isn't a palindrome it already specifies the endianness ISAs differing only in endianness recieve the same ID. '0' means 32-bit (protected) mode of i386. '4' means 32-bit MIPS. |
| **header_length** | Specifies the length of the Multiboot2 header in bytes, including all magic fields and tags.                                                                                                                                   |
| **checksum**  | A 32-bit unsigned value which, when added to the other magic fields (i.e., 'magic', 'architecture', and 'header_length'), must result in a 32-bit unsigned sum of zero.                                                              |

As for the tags, the specification says:

> Tags constitutes a buffer of structures following each other padded when necessary in order for each tag to start at 8-bytes aligned address. Every structure has following format:

The structure of each tag is as follows:

| Bytes | Type | Field |
| :---: | :--: | :---- |
| 0-1   | u16  | type  |
| 2-3   | u16  | flags |
| 4-7   | u32  | size  |

- **type**: The lower portion contains an identifier for the contents of the rest of the tag.
- **size**: Contains the total size of the tag, including the header fields.
- **flags**: If bit 0 (the 'optional' bit) is set, the bootloader may ignore this tag if it lacks relevant support.

Tags are terminated by a special tag with type `0` and size `8`. Since we don't care about tags, we will just add this special tag in our header.

### Implementing the Multiboot2 Header

Now, let's put the specification into practice. Following our project structure, we'll create a file called [`header.S`](/src/impl/x86_64/boot/header.S) inside the [`impl/x86_64/boot`](/src/impl/x86_64/boot/) directory.

This assembly file will define a special section for our Multiboot2 header, called `.multiboot_header`. We'll use two symbols, `header_start` and `header_end`, to mark its boundaries. Between these symbols, we'll declare the required constants exactly as the specification dictates:

```asm
.intel_syntax noprefix
.section .multiboot_header, "a"

header_start:
    # Magic number identifying Multiboot2 header
    .long 0xe85250d6

    # Architecture (0 = i386 protected mode)
    .long 0

    # Length of this header (from header_start to header_end)
    .long header_end - header_start

    # Checksum (magic + architecture + length must sum to 0)
    .long -(0xe85250d6 + 0 + (header_end - header_start))

    # End tag indicating the end of the Multiboot header
    .short 0    # type
    .short 0    # flags
    .long 8     # size

header_end:
```

This implementation is straightforward if you're familiar with assembly. 

Now, we need to ensure the `.multiboot_header` section we just created is positioned at the beginning of our kernel image, as the specification requires. This placement is handled by GatOS's linker script:

```linker
.boot : AT(ADDR(.boot) - KERNEL_VIRTUAL_BASE) ALIGN(4K)
{
	KEEP(*(.multiboot_header))
}
```

This section is defined first in our linker script, guaranteeing that the Multiboot2 header will reside within the first 32,768 bytes of the OS image. The `KEEP` directive is crucial, as it instructs the linker to preserve this section intact, preventing it from being optimized away or modified during the linking process.

## GatOS's Entry Point

This is where our kernel begins its execution. After GRUB loads our kernel into memory and hands over control, the first instruction will be executed from this entry point. Remember that GRUB boots us into 32-bit protected mode, so our initial assembly code will run in this environment.

Our linker script explicitly declares that the symbol `start` marks our kernel's entry point:

```linker
ENTRY(start)
```

This means we need to create an assembly file that defines this `start` symbol, which will serve as the gateway to our kernel's bootstrapping process. 

We therefore create a new file called [`boot32.S`](/src/impl/x86_64/boot/boot32.S) inside the [`impl/x86_64/boot`](/src/impl/x86_64/boot/) directory:

```asm
.intel_syntax noprefix

.global start

.section .text
.code32

start:  # Our kernel's entry point
```

This file establishes the foundation for our kernel's execution:

- `.intel_syntax noprefix` specifies that we'll be using Intel assembly syntax rather than the default AT&T syntax
- `.global start` makes the `start` symbol visible to the linker, fulfilling the `ENTRY(start)` declaration in our linker script
- `.section .text` places the following code in the executable `.text` segment
- `.code32` indicates that the subsequent instructions are 32-bit protected mode code
- `start:` defines the actual entry point label where execution begins

From here on out, we are free to write our 32-bit assembly instructions in the `start:` function. **Welcome to GatOS!**

## Early Boot Setup

We are officially in kernel-land. GRUB has booted us in 32-bit protected mode, and now we need to set up the necessary environment to make the jump to 64-bit long mode.

The *very first thing* we must do is initialize the stack pointer (`esp`) to point to the top of our stack. This is crucial because it enables us to call functions and handle interrupts properly. 

Fortunately, using the linker's `KERNEL_STACK_TOP` symbol makes this straightforward:

```asm
mov esp, offset KERNEL_STACK_TOP
```

>[!CAUTION]
> *That should be it, right?* **No, this is completely wrong.** 
>
>In the previous chapter, we covered how **all of our symbols** are *linked* at Higher Half addresses (i.e. `0xFFFFFFFF80000000` onward) but are *loaded* in lower half addresses (i.e. `0x10000` onward).
>
> The program thinks that `KERNEL_STACK_TOP` is a symbol somewhere in the higher half, and if we move a pointer to it inside `esp`, we'd be overflowing a 32-bit register with a 64-bit value. This would end up setting the stack incorrectly, and our kernel would crash before it even started.

The solution to this problem was presented in the last section of the previous chapter. We will use the C preprocessor (which is compatible with GAS) to convert *any* symbol value from its *link* address to its *load* address.

>[!NOTE]
> The `offset` keyword is used here to get the address of the `KERNEL_STACK_TOP` symbol rather than its value. This ensures we're loading the pointer to the stack's top location into the `esp` register.

GatOS defines the following macros in `paging.h`:

```c
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000

#ifdef __ASSEMBLER__

#define KERNEL_V2P(a) ((a) - KERNEL_VIRTUAL_BASE)
#define KERNEL_P2V(a) ((a) + KERNEL_VIRTUAL_BASE)
#else

#include <stdint.h>
#define KERNEL_V2P(a) ((uintptr_t)(a) & ~KERNEL_VIRTUAL_BASE)
#define KERNEL_P2V(a) ((uintptr_t)(a) | KERNEL_VIRTUAL_BASE)

#endif
```

Therefore, we can include `paging.h` in our `boot32.S` and call `KERNEL_V2P` to convert the Higher Half *link* address into the correct lower half *load* address:

```asm
#include <memory/paging.h>

.intel_syntax noprefix

.global start

.extern KERNEL_STACK_TOP

.section .text
.code32

start:
	# Set stack pointer to top of the stack defined by linker symbol
	mov esp, offset KERNEL_V2P(KERNEL_STACK_TOP)
```

With the stack correctly set up, we can now create as many functions as we want and call them from inside `start`.

## Error Handling

We're about to perform a series of critical checks in our 32-bit assembly code to set up the transition to long mode. Since any failure during this process would be catastrophic, we need a way to handle errors gracefully. This means implementing an error function that we can call when something goes wrong.

However, we face a significant challenge: at this early stage, we cannot easily print error messages to the screen. In the following chapter, we'll explore printing mechanisms in depth, but for now, here are the two primary methods available for output in bare metal environments:

1. **The VGA Buffer**: The traditional method for text output, where characters are written directly to a memory-mapped region at address `0xB8000`. Each character requires two bytes: one for the ASCII value and one for color attributes. This method is relatively straightforward but limited to text mode.

2. **Video Memory**: A more complex approach that involves programming the graphics card directly to set a video mode and manipulate framebuffer memory. This allows for graphical output but requires significant hardware initialization and understanding of display protocols.

For our immediate error handling needs, we'll implement a super simple VGA-based error function. Here's the actual implementation we'll use:

```asm
error:
    mov dword ptr [0xb8000], 0x4f524f45  # "ER" (white-on-red)
    mov dword ptr [0xb8004], 0x4f3a4f52  # "R:" (white-on-red) 
    mov dword ptr [0xb8008], 0x4f204f20  # "  " (spaces)
    mov byte ptr [0xb800a], al           # error code character
    hlt
```

**Understanding the VGA Buffer Addressing**

The key to understanding this code lies in how the VGA text buffer is structured. The buffer starts at physical address `0xB8000`, and each character on screen requires two bytes of memory: one for the ASCII character code and one for the color attributes. 

When we write `mov dword ptr [0xb8000], 0x4f524f45`, we're writing 4 bytes (a double word) to the first four character positions. The increment by 4 bytes for each subsequent write (`0xb8004`, `0xb8008`) is crucial because:

- **0xB8000**: Bytes 0-3 → Characters 1-2 (4 bytes covering 2 characters)
- **0xB8004**: Bytes 4-7 → Characters 3-4  
- **0xB8008**: Bytes 8-11 → Characters 5-6
- **0xB800A**: Byte 10 → Character 6's ASCII value specifically

The beauty of this memory-mapped approach is that **any write to these addresses is immediately reflected on the physical display**. The VGA hardware continuously scans this memory region and renders its contents to the screen without any software intervention required.

So really, to display an error, all we have to do is:

```asm
mov al, 'G'
jmp error

# Prints "ERR: G" in the top left of the screen
```

## The Multiboot2 Check

As per the GRUB Multiboot2 documentation:

> When the boot loader invokes the 32-bit operating system, the machine must have the following state:
> 
> ‘EAX’ Must contain the magic value ‘0x36d76289’; the presence of this value indicates to the operating system that it was loaded by a Multiboot2-compliant boot loader (e.g. as opposed to another type of boot loader that the operating system can also be loaded from).
>
> ‘EBX’ Must contain the 32-bit physical address of the Multiboot2 information structure provided by the boot loader (see Boot information format).

Therefore, for the sake of rigor, we should implement a check to verify that the bootloader was Multiboot2 compliant. Since GRUB is Multiboot2 compliant — and that's what GatOS ships with — this check is primarily a formality.

However, it does serve an important purpose. If someone in the future wants to implement their own bootloader to load GatOS, it must be Multiboot2 compliant because GatOS depends on the Multiboot2 information structure. If the bootloader isn't compliant, this check will catch that early, preventing GatOS from continuing with invalid assumptions.

We can do this quite trivially in assembly:

```asm
check_multiboot:
	cmp eax, 0x36d76289
	jne .no_multiboot
	ret
.no_multiboot:
	mov al, 'M'    # Error code 'M' for "Multiboot"
	jmp error
```

The Multiboot2 specification also tells us that `ebx` contains the physical address of the Multiboot2 information structure. We need to preserve this pointer carefully because it contains vital boot information that our kernel will need later.

Here's our strategy: we transfer the pointer from `ebx` to `edi` early in the boot process. 

```asm
mov edi, ebx
```

This proactive move is crucial due to what happens when we transition to long mode:

1. **Register Behavior During Mode Transition**: When we jump to 64-bit long mode, 32-bit registers are extended to 64 bits. However, they're handled differently:
   - `ebx` will be zero-extended to `rbx` (the upper 32 bits become zero)
   - `edi` will be zero-extended to `rdi` (the upper 32 bits become zero)

2. **The Critical Difference**: While both registers get extended, we choose `edi` deliberately because of x86_64 calling conventions. In the System V AMD64 ABI, the first parameter to a function is passed in `rdi`.

3. **Seamless C Integration**: By storing the Multiboot2 struct pointer in `edi` now, it will automatically be available in `rdi` when we call our `kernel_main` function in C. This means we can simply declare our main function as:

   ```c
   void kernel_main(void* mb_info);
   ```
   And the pointer will be ready to use without any additional setup.

## The CPUID Check

Before we can proceed with checking for long mode support, we first need to verify that the CPU supports the `CPUID` instruction itself. This is necessary because `CPUID` is the instruction we'll use to query the processor's capabilities, including whether it supports 64-bit long mode.

The way we do this is actually pretty nifty:

```asm
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, 'C'    # Error code 'C' for "CPUID"
    jmp error
```

**How This Check Works:**

The `CPUID` instruction detection relies on a clever trick with the EFLAGS register. Bit 21 of EFLAGS is the ID bit — if the CPU supports `CPUID`, this bit can be modified; if not, attempts to change it will be **ignored**. So, the plan is:

1. **Save Original EFLAGS**: We push EFLAGS to the stack and pop it into `eax`, then copy to ECX for safekeeping
2. **Toggle the ID Bit**: We XOR `eax` with `1 << 21` (bit 21) to flip the ID flag
3. **Attempt to Modify**: We push the modified value back to EFLAGS via the stack
4. **Read Back and Compare**: We read EFLAGS again and compare with our original value
5. **Check for Change**: If the values are equal, the CPU ignored our modification attempt, meaning `CPUID` is not supported

## The Long Mode Check

Once we've confirmed that the CPU supports the `CPUID` instruction, our next critical step is to verify whether it supports 64-bit long mode. This check is essential because GatOS is designed to be a 64-bit kernel.

```asm
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    
    ret
.no_long_mode:
    mov al, 'L'    # Error code 'L' for "Long Mode"
    jmp error
```

**How This Check Works:**

This two-step verification process ensures the CPU genuinely supports long mode:

1. **Check for Extended CPUID Support**: 
   - We load `0x80000000` into `eax` and execute `CPUID`
   - The result in `eax` tells us the highest extended function number supported
   - If the value is less than `0x80000001`, the CPU doesn't support the extended functions we need

2. **Verify Long Mode Bit**:
   - We call extended function `0x80000001` to get extended feature bits
   - The result in EDX contains feature flags, where bit 29 indicates long mode support
   - We test this bit with `test edx, 1 << 29` - if zero, long mode is not available

## The SSE Check

This check was introduced in GatOS version `1.5.5`, making it a relatively recent addition. Its purpose is to enable support for the `xmm` family of instructions, which are essential for floating-point operations and SIMD computations.

>[!NOTE]
> Streaming SIMD Extensions (SSE) were introduced with the Pentium III and provide 70 additional instructions to the Intel instruction set. The key advantage of SSE is its SIMD capability, which allows executing a single instruction on multiple data elements in parallel, significantly increasing data throughput.

```asm
check_SSE:
    mov eax, 1
    cpuid
    test edx, 1<<25
    jz .no_SSE
    
    # Enable SSE
    mov eax, cr0
    and eax, 0xFFFBFFFF       # clear CR0.EM (bit 2)
    or  eax, 0x00000002       # set   CR0.MP (bit 1)
    mov cr0, eax
    mov eax, cr4
    or  eax, (1<<9) | (1<<10) # set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10)
    mov cr4, eax
    ret
.no_SSE:
    mov al, 'S'    # Error code 'S'
    jmp error
```

**How This Check Works:**

This verification process ensures the CPU supports SSE and properly configures the system to use it:

1. **SSE Capability Detection**:
   - We call CPUID function `1` to get standard feature flags
   - Test bit 25 in EDX - this indicates whether the CPU supports SSE instructions
   - If the bit is not set, SSE is not available and we jump to error

2. **SSE Feature Enablement**:
   - **CR0 Modifications**: 
     - Clear the EM bit (bit 2): Disables FPU emulation, enabling actual SSE hardware
     - Set the MP bit (bit 1): Enables proper coprocessor monitoring for exceptions
   - **CR4 Modifications**:
     - Set OSFXSR (bit 9): Enables SSE instructions and FXSAVE/FXRSTOR operations
     - Set OSXMMEXCPT (bit 10): Enables SSE exception handling

>[!CAUTION]
> Without this check to enable SSE if supported, floating point operations will result in a `#UD` fault. If your kernel doesn't handle that through ISRs, it will crash.

## Setting up the Page Tables

As discussed in the previous chapter, GatOS pre-allocates page tables capable of mapping `1GB` worth of memory for early boot. To achieve this, GatOS hardcodes the following page table structure into the kernel image:

- 1 PML4 (Page Map Level 4)
- 1 PDPT (Page Directory Pointer Table) 
- 1 PD (Page Directory)
- 512 PTs (Page Tables)

This configuration requires `2060KB` (roughly `2MB`) of reserved, harcoded space. While this may seem substantial, it's an acceptable compromise because it provides the kernel with sufficient mapped memory in order to set up the *physmap* — a direct mapping of all physical memory — later in the boot process. 

The *physmap* is absolutely crucial for our kernel, as it is a prerequisite for the *PMM (Physical Memory Manager)* and the *VMM (Virtual Memory Manager)* to work.

**Why 1GB Initially?**

The `1GB` mapping capacity strikes an optimal balance:
- It provides enough headroom for the kernel to operate during early initialization
- The amount of hardcoded memory reserved for page tables is acceptable

>[!TIP]
> If the numbers or table hierarchy seem unclear — particularly why we need exactly these quantities of each table type — I recommend reviewing the detailed explanation in Chapter 2. The previous chapter covers the x86_64 paging structure and the calculations behind these specific allocations.

We allocate these page tables contiguously within the `.bss` section of our kernel, which is designed for uninitialized memory. To clearly distinguish each table, we define individual symbols for every level of the paging hierarchy:

```asm
.section .bss
.align 4096
PML4:
    .skip 4096
PDPT:
    .skip 4096
PD:
    .skip 4096
PT:
    .skip 4096 * 512
```

**Key Details:**

- **Memory Alignment**: The `.align 4096` directive ensures each table begins on a 4KB boundary, which is mandatory for x86-64 page tables. The processor requires page tables to be aligned to their size (4KB).

>[!NOTE]
> Technically, since we `ALIGN(4K)` the `.bss` section in our linker, and we also declare each page table to be `4KB`, this is mostly a formality.

- **Memory Reservation**: The `.skip` directive reserves the exact amount of memory needed for each table type:
  - **PML4**: 4096 bytes (512 entries × 8 bytes each)
  - **PDPT**: 4096 bytes (512 entries × 8 bytes each)  
  - **PD**: 4096 bytes (512 entries × 8 bytes each)
  - **PT**: 4096 × 512 bytes (512 page tables, each 4096 bytes)

- **Symbol Creation**: Each label (`PML4`, `PDPT`, `PD`, `PT`) creates a named symbol that we can reference from our assembly code when setting up the page table entries. The `PT` symbol points to the first of our 512 `PTs`, so we can access the `i`-th `PT` at `PT + 4096*i`.

We have two primary objectives for our initial page table mappings:

1. **Identity Mapping**: Map the first `1GB` of physical memory to identical virtual addresses (`virt = phys`) for every `4KB` page in the range `[0x0, 1GB]`. This ensures our code continues to execute correctly during the transition to long mode.

2. **Higher-Half Mapping**: Map the same `1GB` of physical memory to the higher-half virtual address space starting at `KERNEL_VIRTUAL_BASE`. This creates the mapping `[KERNEL_VIRTUAL_BASE, KERNEL_VIRTUAL_BASE + 1GB]` virtual → `[0x0, 1GB]` physical.


The identity mapping is temporary — once we're securely executing in the higher half, we'll remove it.

>[!IMPORTANT]
> Our goal when entering long mode is to execute in the higher half of the 64-bit address space. This design directly aligns with how our linker script *links* symbols — it expects them to be accessible at their virtual addresses in the higher-half region. 
>
> For a detailed exploration of why higher-half kernels are essential, revisit Chapter 2.

This dual-mapping strategy provides a smooth transition path: the identity mapping keeps us running during the switch to long mode, while the higher-half mapping prepares us for the final kernel memory layout.

---

### The Intuition

The mapping strategy is surprisingly straightforward. The key insight is that **a single Page Directory (PD) maps exactly 1 GB of memory**. Our job is to build one fully populated `PD`, then reuse it from different `PDPT` and `PML4` entries to cover the two virtual ranges we care about.

> [!TIP]
> Why does one PD equal 1 GB?
>
> * Each PD has 512 entries → each points to a Page Table (PT).
> * Each PT has 512 entries → each points to a 4 KB frame.
> * Total: `512 × 512 × 4 KB = 2^30 = 1 GB`.


### The Ranges We Need

We need to access the same `1 GB` of physical memory (`0x0 – 0x3FFFFFFF`) from two different virtual ranges:

* **Low half:** `[0x00000000, 0x3FFFFFFF]` virtual → `[0x00000000, 0x3FFFFFFF]` physical
* **High half:** `[0xFFFFFFFF80000000, 0xFFFFFFFFBFFFFFFF]` virtual → `[0x00000000, 0x3FFFFFFF]` physical

Pay close attention because what we will do here is crucial. If we use [`virt_breakdown.py`](tools/virt_breakdown.py), we get:

```
> Enter address: 0xFFFFFFFF80000000
Virtual Address: 0xFFFFFFFF80000000
  PML4 Index : 0x01FF (511)
  PDPT Index : 0x01FE (510)
  PD   Index : 0x0000 (0)
  PT   Index : 0x0000 (0)

> Enter address: 0x0
Virtual Address: 0x0000000000000000
  PML4 Index : 0x0000 (0)
  PDPT Index : 0x0000 (0)
  PD   Index : 0x0000 (0)
  PT   Index : 0x0000 (0)
```

Notice that only the **PML4 and PDPT indices differ**. From the PD level downward, the mappings are identical. We can leverage this. 

All we have to do is make sure that both ranges inevitably go through the same `PD` when being translated. That way, we can access our `1GB` both through the high half and the low half (identity map).

---

### Why This Works

Think of the hierarchy:

```
PML4 → PDPT → PD → PT → Physical Frame
```

* For `0x0`, the path is: `PML4[0] → PDPT[0] → PD → PTs`
* For `0xFFFFFFFF80000000`, the path is: `PML4[511] → PDPT[510] → PD → PTs`

Both paths converge on the **same PD**, which already holds a complete `1 GB` mapping. That’s the trick: once we’re at the PD, the lower levels don’t care which higher-level indices we came from.

---

### Why Share the PD?

Because **the PD is where the actual 1 GB mapping lives**. If we built separate `PDs` for both ranges, we’d have to duplicate all 512 PTs and their entries — a massive waste, since they’d point to the same physical frames anyway.

By reusing a single `PD`, both virtual ranges resolve to the same physical memory without duplication.

---

### Putting It All Together

In the end, it’s just three steps:

1. Fill all 512 `PTs` and make a single `PD` point to them.
2. Make `PDPT[0]` and `PDPT[510]` point at that `PD`.
3. Make `PML4[0]` and `PML4[511]` point at the aforementioned `PDPT`.

That’s it — the same `1GB` of physical memory is accessible from two virtual ranges.

>[!IMPORTANT]
> I know this sounds complex, but it's actually very simple. Since our `PD` holds `1GB` worth of mappings, we can just access it in 2 different ways:
>
>1. `PML4[0] → PDPT[0] → PD → ...`
>2. `PML4[511] → PDPT[510] → PD → ...`
>
> The first maps the range `[0x0, 1GB]` and the second maps the range `[0xFFFFFFFF80000000, 0xFFFFFFFF80000000 + 1GB]`. 
>
> Really, just populating different `PML4` and `PDPT` indices allows us to access that `1GB` from different starting points in virtual memory. That's all there is to it.

---

### The Implementation

Implementing this is actually the hardest part of our bootstrapping phase. Let's break down the code step by step:

#### 1. Setting Up the PML4

```asm
# PML4[0] entry points to the base address of our PDPT table.
# This maps the lower half (identity mapping)
mov eax, offset KERNEL_V2P(PDPT) 
or eax, 0b11  # Set Present and Read/Write flags
mov dword ptr [KERNEL_V2P(PML4)], eax

# PML4[511] entry also points to the same PDPT table (for higher half)
mov dword ptr [KERNEL_V2P(PML4) + 511 * 8], eax
```

Here we configure two entries in the PML4 table:
- **Entry 0**: Maps virtual addresses starting at `0x0` to our PDPT
- **Entry 511**: Maps virtual addresses in the kernel half (`0xFFFF...`) to the same PDPT

The `KERNEL_V2P()` macro converts kernel virtual addresses to physical addresses since we're still accessing symbols at their *linked* addresses.

#### 2. Configuring the PDPT

```asm
# PDPT[0] entry points to the base address of our PD table.
mov eax, offset KERNEL_V2P(PD)
or eax, 0b11  # Present + Read/Write
mov dword ptr [KERNEL_V2P(PDPT)], eax

# PDPT[510] should also point to PD (for higher half)
mov dword ptr [KERNEL_V2P(PDPT) + 510 * 8], eax
```

Now we set up the PDPT to point to our Page Directory:
- **Entry 0**: For low-half mapping (`0x0` to `0x3FFFFFFF`)
- **Entry 510**: For high-half mapping (`0xFFFFFFFF80000000` to `0xFFFFFFFFBFFFFFFF`)

#### 3. Populating the PD

```asm
mov ecx, 0
.PD_loop:
    mov eax, ecx
    shl eax, 12 
    add eax, offset KERNEL_V2P(PT)  # eax = PT + i*4KB as explained before
    or eax, 0b11                    # Present + Read/Write
    mov ebx, ecx
    shl ebx, 3                      # Multiply by 8 (entry size)
    mov dword ptr [KERNEL_V2P(PD) + ebx], eax
    inc ecx
    cmp ecx, 512
    jne .PD_loop
```

This loop creates 512 entries in the Page Directory, each pointing to a different Page Table. The calculation `ecx × 4096` ensures each Page Table is properly aligned in memory.

#### 4. Setting Up the Page Tables
```asm
mov ecx, 0
.PT_loop:
    mov eax, ecx
    shl eax, 12      # Multiply by 4096 to get physical address
    or eax, 0b11     # Present + Read/Write
    mov ebx, ecx
    shl ebx, 3       # Multiply by 8 (entry size)
    mov dword ptr [KERNEL_V2P(PT) + ebx], eax
    inc ecx
    cmp ecx, 512 * 512
    jne .PT_loop
```

This is the most intensive part - we populate all 512 × 512 = 262,144 page table entries:
- Each entry maps a 4KB physical frame
- The calculation `ecx × 4096` creates a direct identity mapping
- We map exactly 1GB of physical memory (`262,144 × 4KB = 1GB`)

### The Complete Picture

After this setup, our page table hierarchy looks like this:

```
PML4[0]  → PDPT[0]  → PD → PTs → Physical 0x0 to 0x3FFFFFFF
PML4[511] → PDPT[510] → (same PD) → (same PTs) → (same physical memory)
```

The beauty of this approach is that we only need to populate the page tables once, but we can access the same physical memory through two different virtual address ranges. This gives us both identity mapping (for the early boot transition) and higher-half kernel mapping (for long-term operation) with minimal memory overhead.

## Setting up the GDT

You might remember the Global Descriptor Table (GDT) from Chapter 2 - that legacy system for memory segmentation. While 64-bit mode largely relegates the GDT to a compatibility role, we still need to set up a minimal version for the processor to function properly. 

In long mode, the GDT's purpose shifts significantly:
- **Memory protection is handled by paging**, not segmentation
- **But segment registers still exist** and require valid descriptors
- **Privilege levels are still enforced** through code segment descriptors

Think of it this way: we're setting up the GDT not because we need its memory management features, but because the CPU architecture requires it as part of the 64-bit transition protocol for compatibility.

```asm
.section .rodata
gdt64:
	.quad 0 # null descriptor
gdt64_code_segment = . - gdt64
	.quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) # 64-bit code segment descriptor
gdt64_pointer:
	.word . - gdt64 - 1         # limit (size of GDT - 1)
	.long KERNEL_V2P(gdt64)     # base address of GDT (32-bit)
```

### Understanding the 64-bit GDT Structure

#### 1. The Null Descriptor
```asm
.quad 0 # null descriptor
```
- **Architecture requirement** - The first GDT entry must be zero
- **Safety mechanism** - Prevents accidental use of uninitialized segment registers

#### 2. The Code Segment Descriptor
```asm
.quad (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
```
This compact bitfield defines a 64-bit code segment:
- **Bit 43**: Descriptor type (1 = code/data segment)
- **Bit 44**: Segment type (1 = code segment)  
- **Bit 47**: Present bit (1 = segment is valid)
- **Bit 53**: 64-bit segment (enables long mode)

Notice what's **missing**: base addresses, limits, and granularity settings that were essential in 32-bit mode. In 64-bit mode, these fields are ignored - the segment effectively covers the entire address space, bypassing the GDTs initial purpose, which was segmentation.

#### 3. The GDT Pointer Structure
```asm
gdt64_pointer:
	.word . - gdt64 - 1         # limit (size of GDT - 1)
	.long KERNEL_V2P(gdt64)     # base address of GDT
```
This structure is loaded into the GDTR register:
- **Limit**: Size of the GDT minus one (16 bits)
- **Base**: Physical address of the GDT (32-bit in compatibility mode)

### Why This Minimal Approach Works

In 64-bit mode:
- **CS, DS, ES, SS are effectively ignored** for memory addressing
- **Base addresses are treated as zero** regardless of descriptor contents
- **Limits are not enforced** (except for FS and GS, which we're not using)
- **The main purpose is to establish privilege level** and architecture mode

This minimal GDT gives us exactly what we need: a valid code segment that tells the processor "we're now executing 64-bit code at privilege level 0," without the complexity of the full 32-bit GDT structure.

## Enabling Paging And Entering Long Mode

This is a critical moment where the memory mapping we've painstakingly set up finally becomes active. We're getting there!

```asm
enable_paging:
    # Set CR3 to address of PML4
	mov eax, offset KERNEL_V2P(PML4)
	mov cr3, eax

    # Enable PAE (Physical Address Extension)
	mov eax, cr4
	or eax, 1 << 5 # enable PAE
	mov cr4, eax

    # Enable Long Mode via IA32_EFER MSR
	mov ecx, 0xC0000080 # IA32_EFER MSR
	rdmsr
	or eax, 1 << 8 # enable long mode
	wrmsr

    # Enable paging in CR0
	mov eax, cr0
	or eax, 1 << 31 # enable paging
	mov cr0, eax

	ret
```

### Step-by-Step Breakdown

#### 1. Setting CR3 - The Page Table Base
```asm
mov eax, offset KERNEL_V2P(PML4)
mov cr3, eax
```
- **CR3** register points to the physical address of the PML4 table
- This tells the CPU where to find our page table hierarchy

#### 2. Enabling PAE - Physical Address Extension
```asm
mov eax, cr4
or eax, 1 << 5  # Set bit 5 (PAE enable)
mov cr4, eax
```
- **PAE** allows addressing more than 4GB of physical memory
- Required for 64-bit paging structures (4-level paging)
- Without PAE, we'd be limited to 32-bit paging schemes

#### 3. Enabling Long Mode - The IA32_EFER MSR
```asm
mov ecx, 0xC0000080  # IA32_EFER MSR number
rdmsr                # Read Model Specific Register
or eax, 1 << 8       # Set LME bit (Long Mode Enable)
wrmsr                # Write back to MSR
```
- **IA32_EFER** is a Model Specific Register that controls extended features
- Bit 8 (**LME**) enables long mode when paging is activated
- This step alone doesn't switch modes - it prepares the CPU for the transition

#### 4. Enabling Paging - The Final Switch
```asm
mov eax, cr0
or eax, 1 << 31  # Set bit 31 (Paging Enable)
mov cr0, eax
```
- **This is the point of no return** - paging becomes active immediately
- The CPU combines the LME bit from IA32_EFER with CR0's paging bit
- The processor transitions from protected mode to long mode
- All memory accesses now go through our page tables

### What Happens Next

After executing these instructions:
1. **Virtual addresses are translated** through our 4-level page tables
2. **Our dual mapping becomes active**: 
   - `0x0` → physical `0x0` (identity mapping)
   - `0xFFFFFFFF80000000` → physical `0x0` (kernel mapping)
3. **The CPU is now in 64-bit long mode**, but still executing 32-bit code
4. **We need to reload segment registers** and jump to 64-bit code

### Important Considerations

- **Order matters**: This specific sequence is required for a smooth transition
- **We're still in compatibility mode**: The CPU is in 64-bit mode but executing 32-bit instructions until we load a 64-bit code segment
- **The next step** will be to jump to our higher-half kernel code using an absolute far jump

## The Far Jump to 64-bit Long Mode

Are you tired of all the setup so far and ready for the big payoff? I know I am! This is where all our preparation culminates in the actual transition to 64-bit mode.

First, we need to create a new assembly file, [`boot64.S`](/src/impl/x86_64/boot/boot64.S), specifically designed for 64-bit code. This will be our landing point after the architecture switch from our current 32-bit code in [`boot32.S`](/src/impl/x86_64/boot/boot32.S).

### Setting Up the 64-bit Code Environment

```asm
#include <memory/paging.h>

.intel_syntax noprefix

.global long_mode_start

.section .text
.code64
long_mode_start:
    # This is where we want to land after the far jump
```

Key elements of this setup:
- **`.code64` directive**: Tells the assembler we're writing 64-bit instructions
- **`long_mode_start`**: Our entry point symbol that we'll jump to from 32-bit code
- **Global declaration**: Makes the symbol accessible to other files

### Connecting the 32-bit and 64-bit Worlds

To bridge between our two assembly files, we declare the 64-bit entry point as external in our 32-bit code:

```asm
.extern long_mode_start
```

This tells the 32-bit assembler that `long_mode_start` is defined elsewhere but we need to reference it.

### The Actual Transition Code

Back in our 32-bit `boot32.S`, we perform all the previous steps discussed to jump into 64-bit mode:

```asm
#include <memory/paging.h>

.intel_syntax noprefix

.global start
.global PML4

.extern long_mode_start
.extern KERNEL_STACK_TOP

.section .text
.code32

start:
	# Set stack pointer to top of the stack defined by linker symbol
	mov esp, offset KERNEL_V2P(KERNEL_STACK_TOP)

	# Transfer the multiboot information as discussed
	mov edi, ebx

	# Verify multiboot magic number
	call check_multiboot

	# Verify CPUID instruction support
	call check_cpuid

	# Verify CPU supports long mode (64-bit)
	call check_long_mode

	# Verify and enable SSE support
	call check_SSE

	# Set up paging structures for long mode
	call setup_page_tables

	# Enable paging and enter long mode
	call enable_paging

	# Load the 64-bit GDT descriptor
	lgdt [KERNEL_V2P(gdt64_pointer)]

	# Far jump to 64-bit kernel entry point via code segment selector
	jmp gdt64_code_segment:KERNEL_V2P(long_mode_start)

	# Halt if for some reason jump returns
	hlt
```

This far jump is critical because:
1. **It loads the CS register** with our new 64-bit code segment selector
2. **The processor validates** that we're actually transitioning to a 64-bit segment
3. **It begins executing** at the physical address of our 64-bit code

### Why This Architecture Matters

This two-file approach separates concerns cleanly:
- **`boot32.S`**: Handles the 32-bit bootstrapping process
- **`boot64.S`**: Contains pure 64-bit kernel code
- **The far jump**: Acts as a clean architectural boundary between the two worlds

Once we land in `long_mode_start`, we're finally executing genuine 64-bit code with our higher-half memory mapping active. This means we can start using virtual addresses like `0xFFFFFFFF80000000` and leave the physical addressing constraints of the boot process behind us. 

### Transition to High Memory

We've successfully jumped to 64-bit mode, but we're still executing from low memory because our far jump targeted `KERNEL_V2P(long_mode_start)`. Now it's time to complete the transition to our higher-half kernel addresses. This is a crucial step that moves us from the physical memory layout to our proper virtual memory space.

```asm
#include <memory/paging.h>

.intel_syntax noprefix

.global long_mode_start
.extern kernel_main

.section .text
.code64
long_mode_start:

    # If for some reason the multiboot information
    # is not passed, we will just halt the system.
    cmp rdi, 0
    je .no_multiboot_info

    # NULLIFY all data segment registers
    mov ax, 0x0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    movabs rax, offset upper_memory
    jmp rax

.no_multiboot_info:
    hlt


upper_memory:
    mov rax, KERNEL_VIRTUAL_BASE
    add rsp, rax

    mov rax, 0x0
    mov ss, rax
    mov ds, rax
    mov es, rax

    pushq 0x8
    movabs rax, offset .transition_to_c
    push rax
    retfq

.transition_to_c:
    # RDI contains the multiboot information
    # which is passed from the bootloader.
    # It is handled by the kernel as an argument.
    movabs rax, offset kernel_main
    call rax

    hlt
```

### Step-by-Step Breakdown

#### 1. Validation and Initial Setup

```asm
cmp rdi, 0
je .no_multiboot_info
```
- **Multiboot validation**: Check if RDI contains a valid Multiboot information structure pointer
- **Safety measure**: Halt if no boot information was provided (shouldn't happen)

#### 2. Clearing Segment Registers

```asm
mov ax, 0x0
mov ss, ax
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
```
- **Clean slate**: Nullify all data segment registers
- **64-bit convention**: In long mode, these are largely ignored but should be set to known values

#### 3. Jump to Higher-Half Code

```asm
movabs rax, offset upper_memory
jmp rax
```
- **Absolute jump**: Use `movabs` to load the full 64-bit address of our high-memory code
- **How this works**: Remember that *any symbol*, including `upper_memory`, has been *linked* at a high address. Now that we are not restricted by our register size, *and the higher half mapping exists*, we can simply jump to any high half linked symbol!

>[!NOTE]
> By jumping to the *linked* address of `upper_memory` (i.e. not using `KERNEL_V2P`), we are effectively jumping into a high half address!

#### 4. Adjusting the Stack Pointer
```asm
upper_memory:
    mov rax, KERNEL_VIRTUAL_BASE
    add rsp, rax
```
- **Critical adjustment**: Add `KERNEL_VIRTUAL_BASE` (0xFFFFFFFF80000000) to RSP
- **Stack relocation**: Moves the stack from physical to virtual address space
- **Function calls**: Ensures return addresses and local variables use virtual addresses

#### 5. Far Return to Establish Code Segment

```asm
pushq 0x8
movabs rax, offset .transition_to_c
push rax
retfq
```

This clever sequence performs a far return to reload CS:
- **Push code segment selector** (0x8 points to our 64-bit code segment in GDT)
- **Push target address** (`.transition_to_c` with virtual addressing)
- **`retfq`** performs far return, loading CS with the selector and jumping to the virtual address

#### 6. Final Handoff to C Code

```asm
.transition_to_c:
    movabs rax, offset kernel_main
    call rax
    hlt
```
- **Kernel entry**: Call our main C kernel function with proper virtual addressing
- **Boot information**: RDI still contains the Multiboot info pointer passed from our 32-bit stub.
- **Halt fallback**: If kernel_main returns (which it shouldn't), halt the system

### Why This Complex Dance?

This multi-stage transition ensures:

- **Clean segment state**: All segment registers are properly initialized
- **Proper stack operation**: The stack pointer uses virtual addresses before we make function calls
- **Correct code execution**: The far return ensures we're fully in the higher-half address space
- **C compatibility**: The C kernel receives parameters correctly and can use normal pointer arithmetic

After this sequence completes, we're fully operating in our higher-half kernel environment at `0xFFFFFFFF80000000`. 

### The Final Handoff to C

After completing the complex transition to higher-half memory, we finally arrive at the destination we've been working toward: handing control over to our main C kernel. This is where the assembly bootstrapping ends and the high-level kernel initialization begins.

GatOS's `kernel_main` function is defined in [`main.c`](/src/impl/kernel/main.c) with a straightforward signature:

```c
void kernel_main(void* mb_info) {
    // Welcome to C!
}
```

This function serves as the architectural boundary between low-level assembly setup and high-level kernel code. The single parameter `mb_info` contains the Multiboot information structure pointer that was carefully preserved in the RDI register throughout our boot process.

To access this C function from our assembly code, we need to declare it as an external symbol in `boot64.S`:

```asm
.extern kernel_main
```

The C kernel now operates in a clean 64-bit environment, in high memory. The journey from GRUB to 64-bit C code is complete!