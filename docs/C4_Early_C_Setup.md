# Chapter 4: Early C Setup

In the previous chapter, we successfully made the transition to long mode and completed our journey by handing off execution to C through the `kernel_main` function. Now we're ready to build upon that foundation.

This chapter focuses on establishing the essential utilities our kernel needs to operate effectively. We'll cover:

1. **VGA Text Output**: Implementing proper screen printing through the VGA buffer so our kernel can communicate with us during boot and debugging.

2. **Multiboot2 Parsing**: Creating a file dedicated to copying the multiboot2 struct to the higher half and parsing it.

3. **Early Diagnostics**: Setting up some primitive functions to verify our system is working as expected.

4. **Unmapping the identity map**: After we ensure everything we need is in the higher half, we will delete the identity map from the page tables.


Once we have these core utilities in place, we'll be ready for the next major step: in Chapter 5, we'll explore paging from a higher-level perspective and construct the **physmap** - we'll reserve and setup the necessary page tables to map the entirety of physical RAM into virtual memory.

But first things first - let's give our kernel the ability to talk to us by implementing VGA text output.

## What is the VGA text mode?

The VGA (Video Graphics Array) text mode is a legacy display standard that remains accessible and widely used in operating system development, especially during early boot phases. It provides a simple, hardware-independent way to display text on screen without requiring complex graphics drivers.

### Boot Method Considerations

**Legacy BIOS/CSM Boot:**

- VGA text mode is **natively available** when booting via Legacy BIOS or Compatibility Support Module (CSM)
- The firmware initializes VGA hardware during early boot
- This is the **simplest path** for kernel development

**Modern UEFI Systems:**

- UEFI firmware typically **does not support** VGA text mode
- Uses more advanced graphics modes (GOP - Graphics Output Protocol)

### Basic Characteristics

**Screen Layout:**
- **80 columns × 25 rows** of character cells
- **16 colors** for foreground and background
- Each character occupies **2 bytes** in memory
  - Byte 1: ASCII character code
  - Byte 2: Color attributes (4-bit foreground, 4-bit background)

**Memory Mapping:**
- The VGA buffer is **memory-mapped** to physical address `0xB8000`
- In our higher-half kernel, this becomes `0xFFFFFFFF800B8000`
- Writing to this memory region directly updates the screen

### Color Encoding

Each color attribute byte is structured as:

```
Bit:  7  6 5 4  3 2 1 0
     └─┘ └───┘  └─────┘
     (3)  (2)     (1)
```

1. **Foreground (bits 0-3)**: Text color (0-15)
2. **Background (bits 4-6)**: Background color (0-7)
3. **Blink (bit 7)**: Controls character blinking (usually disabled)

Common color values:
- `0x0`: Black
- `0x1`: Blue
- `0x2`: Green
- `0x3`: Cyan
- `0x4`: Red
- `0x7`: Light Gray (default foreground)
- `0x8`: Dark Gray
- `0xF`: White

### Why Use VGA Text Mode?

**Advantages for Kernel Development:**

- **Simplicity**: No complex initialization required
- **Reliability**: Works on virtually all x86 hardware
- **Immediate feedback**: Perfect for boot messages and debugging
- **Low memory usage**: Minimal overhead

**Limitations:**

- **Low resolution**: Only 2000 characters total
- **Limited to text**: No graphics capabilities
- **Legacy technology**: Eventually replaced by framebuffer graphics

Perfect, let’s finish this section in a way that feels like a natural continuation of your style so far. Instead of just dumping the code, we’ll **walk through the implementation piece by piece** and tie it back to the VGA text mode concepts we already covered.

## Implementing a Console

The goal is to build a **minimal VGA console wrapper** that lets us output text to the screen with color, scrolling, and clearing support. This is essentially GatOS's primitive `putc`.

We’ll split this into two files:

* **[`vga_console.h`](/src/headers/vga_console.h):** Public interface (colors + function prototypes)
* **[`vga_console.c`](/src/impl/kernel/vga_console.c):** Implementation (direct writes to the VGA buffer at `0xB8000`)

---

### 1. The Header: Colors & Function Prototypes

In `vga_console.h`, we define:

* **Color constants** (`0–15`) for foreground and background text
* **Core functions**: print a character, set colors, and clear the screen

```c
#define CONSOLE_COLOR_BLACK 0
#define CONSOLE_COLOR_BLUE 1
#define CONSOLE_COLOR_GREEN 2
...
#define CONSOLE_COLOR_WHITE 15

void console_print_char(char character);
void console_set_color(uint8_t foreground, uint8_t background);
void console_clear(void);
```

This mirrors the VGA color encoding we discussed earlier: **4 bits for foreground, 3 bits for background, plus the blink bit if you ever enable it.**

---

### 2. VGA Layout and Buffer Mapping

In `vga_console.c`, we start by setting up the basics:

* VGA runs at **80×25 characters** in text mode
* Each cell is represented by a struct:

  * `character` (ASCII code)
  * `color` (attribute byte: foreground + background)
* The **VGA buffer** is memory-mapped at `0xB8000`, which we access using `KERNEL_P2V` so it’s valid in higher-half addressing

```c
#include <vga_console.h>
#include <memory/paging.h>
#include <stddef.h>
#include <stdint.h>

const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*)KERNEL_P2V(0xb8000);
size_t col = 0; // Current column
size_t row = 0; // Current row

// Default color is white text on black background
uint8_t color = CONSOLE_COLOR_WHITE | CONSOLE_COLOR_BLACK << 4;
```

This gives us a “virtual screen” in memory. When we write here, the text instantly appears on the real display.

>[!NOTE]
> How is `stdint.h` and `stddef.h` available since we never implemented either of them? Well, our cross compiler has these predefined for us, so we can just use them. For more information regarding this, revisit Chapter 1.

---

### 3. Clearing the Screen

To keep things tidy, we first implement **row clearing**. This fills a row with spaces, using the current color:

```c
void clear_row(size_t row) {
    struct Char empty = { .character = ' ', .color = color };

    for (size_t col = 0; col < NUM_COLS; col++) {
        buffer[col + NUM_COLS * row] = empty;
    }
}
```

>[!IMPORTANT]
> Why does this work? Because the screen is **80 columns wide and 25 rows tall**, we can compute the index of a character cell with simple 2D → 1D math:
>
>```
>index = column + NUM_COLS * row
>```
>
>* `NUM_COLS * row` skips over all the full rows above the one we’re interested in.
>* Adding `column` selects the specific column within the row.
>
>For example:
>
>* Top-left corner (row = 0, col = 0) → index `0` → very first cell in memory.
>* Second column on the first row (row = 0, col = 1) → index `1`.
>* First column of the second row (row = 1, col = 0) → index `80`.
>
>This indexing math is why you see expressions like:
>
>```c
>buffer[col + NUM_COLS * row] = someChar;
>```
>
>It’s just converting a 2D (row, col) screen position into the correct linear position inside the VGA buffer.

And then use that to clear the **entire screen**:

```c
void console_clear() {
    for (size_t i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
}
```

---

### 4. Handling Newlines and Scrolling

Text should naturally flow down the screen. When we hit `\n` or reach the end of a row:

* If we’re not at the bottom, just move to the next line
* If we *are* at the bottom, scroll everything up by one row and clear the last row

```c
void print_newline() {
    col = 0;

    if (row < NUM_ROWS - 1) {
        row++;
        return;
    }

    // Scroll up
    for (size_t r = 1; r < NUM_ROWS; r++) {
        for (size_t c = 0; c < NUM_COLS; c++) {
            buffer[c + NUM_COLS * (r - 1)] = buffer[c + NUM_COLS * r];
        }
    }

    clear_row(NUM_ROWS - 1);
}
```

This is exactly how terminals like the Linux console behave.

---

### 5. Printing Characters

Now we can actually **output characters**:

* Handle newline (`\n`) specially
* Wrap text when the row is full
* Write the character and color directly into the buffer

```c
void console_print_char(char character) {
    if (character == '\n') {
        print_newline();
        return;
    }

    if (col >= NUM_COLS) {
        print_newline();
    }

    buffer[col + NUM_COLS * row] = (struct Char) {
        .character = (uint8_t) character,
        .color = color,
    };

    col++;
}
```

---

### 6. Setting Colors

Finally, we let the caller **choose text and background colors**:

```c
void console_set_color(uint8_t foreground, uint8_t background) {
    color = foreground | (background << 4);
}
```

This packs the colors into the proper VGA attribute byte format.
 And that’s it — with just these building blocks, we now have:

* Text printing to the screen (`console_print_char`)
* Color control (`console_set_color`)
* Clearing and scrolling (`console_clear`, `print_newline`)

## So, how do we print strings?

From here on out, it’s actually quite trivial. Since we already have `console_print_char`, we can build a simple string printer on top of it:

```c
void console_print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        console_print_char(str[i]);
    }
}
```

This function just loops over each character in the string until it hits the `'\0'` terminator, printing them one at a time.

With this, you can create whatever wrappers you like for your output. For instance:

```c
console_set_color(CONSOLE_COLOR_LIGHT_CYAN, CONSOLE_COLOR_BLACK);
console_print("Hello, world!\n");

console_set_color(CONSOLE_COLOR_LIGHT_GREEN, CONSOLE_COLOR_BLACK);
console_print("This is your kernel speaking.\n");
```

## On `printf` and beyond

Of course, a primitive `print` function only goes so far. Eventually, you’ll want formatted output:

```c
printf("Loaded %d modules in %.2f seconds\n", module_count, time_taken);
```

But implementing your own full-blown `printf` — especially one that supports **floats**, padding, and format specifiers — is a *lot* of work.

Instead of reinventing the wheel, we’ll borrow an existing **minimal `printf` implementation** and hook it up to our kernel. Since we already have `console_print_char`, adapting third-party `printf` code is as simple as swapping in our character output routine.


For **GatOS**, I’ve chosen to adopt [Marco Paland’s `printf` / `sprintf` implementation for embedded systems](https://github.com/mpaland/printf). His work is credited directly in the source code.

This implementation is truly exceptional:

1. A tiny yet **feature-rich** `printf`, `sprintf`, and `(v)snprintf` library
2. **Memory-safe**, with built-in checks
3. Compact — about **600 lines of code**
4. **Zero dependencies**: no external libraries, just a single module file
5. Full support for important **flags, width, and precision sub-specifiers**
6. Handles **decimal and floating-point** output with its own fast `itoa`/`ftoa`
7. **Reentrant and thread-safe**, malloc-free, no statics or global buffers
8. **Lint-clean, warning-free, coverity-clean**, and even **automotive ready**
9. Backed by an **extensive test suite** (>400 test cases)
10. In short: probably the **best standalone `printf` implementation** available online
11. Released under the permissive **MIT license**

You can grab the full implementation from Marco’s GitHub repository. For GatOS specifically, I’ve wired it up in [`vga_stdio.h`](/src/headers/vga_stdio.h) and [`vga_stdio.c`](/src/impl/kernel/vga_console.c). For more information about how to glue it in your own source, please check out the detailed instructions in his repo.

The naming (`stdio`) is deliberate — these files will eventually support not only `printf`, but also `scanf` once we have interrupts in place.

For now, GatOS supports a fully capable `printf`:

```c
#include <vga_console.h>
#include <vga_stdio.h>

void kernel_main(void* mb_info) {
    console_clear();
	printf("Hello %s", "world!"); // Prints "Hello world!" to the screen!
}
```

## Parsing the Multiboot2 Struct

Earlier, we passed the Multiboot2 struct into `kernel_main` via `rdi`. Now it’s finally time to make use of it. GatOS includes a dedicated Multiboot2 parser, located in [`multiboot2.h`](/src/headers/multiboot2.h) and [`multiboot2.c`](/src/impl/x86_64/multiboot2.c).

I’m not going to dive into the full details of parsing the Multiboot2 struct here — that would take far too long, and it’s beyond the scope of this documentation. If you’d like to roll your own parser, I highly recommend the [GNU Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html). It lays everything out clearly and even provides sample code you can learn from.

Instead, I’ll focus on a few key implementation details and give a high-level explanation of how our parser works. First, however, we need to look at a few dependencies.

### The Kernel Range

Up until now, when talking about paging, we’ve mostly focused on the `P2V`/`V2P` macros and the page tables. But there’s another concept that GatOS tracks closely: **the kernel range**.

Formally, this range is `[KVIRT_START, KVIRT_END]`. In practice, though, GatOS primarily cares about `KVIRT_END`. That’s because it treats `KVIRT_START` as a fixed value — `0xFFFFFFFF80000000` — which also implies that GatOS treats `KPHYS_START` as `0x0`. I’ve explained the reasoning for this setup in earlier sections.

Both `KVIRT_START`/`KVIRT_END` and their physical counterparts `KPHYS_START`/`KPHYS_END` are made available in C code via linker symbols. What’s important to understand is that GatOS will later adjust `KEND` internally when building the *physmap*. This is necessary to make room for new page tables, ensuring that all of RAM can be mapped into virtual space.

> [!NOTE]
> From this point forward, we’ll just refer to **`KEND`** as a general concept. Whether it’s `KPHYS_END` or `KVIRT_END` doesn’t really matter — both point to the same boundary, just seen from different perspectives (physical vs virtual).

To make this clean and maintainable, it’s best to **wrap these linker symbols** inside `paging.h` and `paging.c`. That way, the rest of the kernel always queries `KEND` through the wrapper functions. This has two benefits:

1. Other parts of the code automatically get the *current* value of `KEND` (even after tweaks).
2. All adjustments happen in one place (`paging.c`), keeping the rest of the codebase simple and safe.

In this light, we expose 4 new functions in `paging.h`:

```c
uint64_t get_kstart(bool virtual);
uint64_t get_kend(bool virtual);
uint64_t get_linker_kend(bool virtual);
uint64_t get_linker_kstart(bool virtual);

extern uintptr_t KPHYS_END;
extern uintptr_t KPHYS_START;
```

At first glance, this might look redundant — why do we need both “normal” and “linker” getters? Let’s break it down.

* **`get_kstart` / `get_kend`**
  These return the *current* kernel boundaries. They are backed by the static variables `KSTART` and `KEND`, which may be updated at runtime. For example, when GatOS builds the physmap, it shifts `KEND` forward to make room for new page tables. Calling these functions ensures you always get the *latest, adjusted values*.

* **`get_linker_kstart` / `get_linker_kend`**
  These return the *original values* defined by the linker symbols. In other words, they give you the “linker” kernel range as it was at boot time, untouched by runtime adjustments. These are useful when you need the baseline, fixed reference points for the kernel’s location.

Both variants take a `bool virtual` argument. If `true`, the function converts the address into its higher-half (virtual) equivalent using `KERNEL_P2V`. If `false`, you get the physical address directly.

This dual system gives us flexibility:

* Use **linker values** when you need to reference the kernel’s fixed layout.
* Use **current values** when you want the runtime-adjusted state (e.g., after physmap expansion).

By centralizing this logic in `paging.c`, the rest of the kernel doesn’t need to worry about linker symbols, runtime tweaks, or physical/virtual conversions. Everything just calls these wrappers and gets the correct answer for the current context.

### Libc Considerations

We've glossed over another important topic: the C standard library (`libc`). Porting a full `libc` implementation is quite cumbersome, especially when we don't yet have the underlying system calls that many `libc` functions depend on. However, we still need basic library functionality to make kernel development practical.

For this reason, I've implemented a standalone [`string.c`](/src/impl/libc/string.c) and [`string.h`](/src/headers/libc/string.h) library specifically for GatOS. This gives us essential string manipulation functions like:

- `memset` - Fill memory with a constant byte
- `memcpy` - Copy memory regions  
- `strlen` - Calculate string length
- `strcmp` - Compare strings

These functions massively accelerate our string handling development and eliminate the need to reinvent basic utilities.

>[!WARNING]
> While a full `libc` port might happen eventually, it remains irrelevant to GatOS's current goals. We're implementing only what we actually need, when we need it.

### The Parser

The parser’s implementation depends on both `string.h` utilities and knowledge of the kernel’s memory range. 

>[!IMPORTANT]
> In reality, it also depends on another function defined in `paging.h`:
>```c
>uintptr_t align_up(uintptr_t val, uintptr_t align);
>```

At boot, the multiboot structure is placed in lower memory, and we can access it only because of the lower-half identity mapping established during early initialization.

To handle this safely, the kernel first parses the multiboot structure in its original, lower-half form. After parsing, each pointer inside the structure is translated to its higher-half equivalent, and all relevant data is copied into a **preallocated buffer** reserved within the kernel’s address space. Since this buffer resides in the higher half (inside the kernel range), the information remains accessible even after the lower half is unmapped.

This buffer is declared in [`main.c`](/src/impl/kernel/main.c):

```c
static uint8_t multiboot_buffer[8 * 1024]; // 8KB should be more than enough
```

To initialize the parser, we declare a `multiboot_parser_t` on the stack and pass it, along with the buffer, into `multiboot_init`:

```c
static uint8_t multiboot_buffer[8 * 1024]; // 8KB should be more than enough

void kernel_main(void* mb_info) {
    // The parser object
    multiboot_parser_t multiboot = {0};
    
    // Initialize multiboot parser (copies everything to higher half)
    multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));
}
```

We can then use this `multiboot_parser_t` object to access any number of functions. For example:

```c
void kernel_main(void* mb_info) {

    multiboot_parser_t multiboot = {0};
    
    [...] // Initialization

    // Dump the memory map
    multiboot_dump_memory_map(&multiboot);
}
```

>[!NOTE]
>For the full list of functions that the parser supports, you can always look at [`multiboot2.h`](/src/headers/multiboot2.h).


## Early Diagnostics

Usually, the first major hurdle during early C setup is **debugging**. In bare-metal development, debugging can be frustrating — sometimes even borderline impossible. Tools like `gdb` can help, but they require specialized setup and are limited to stepping through assembly instructions, which isn’t always practical.

When I was developing GatOS, the biggest issue I ran into was page faults. Tracking down the cause often felt like playing Russian roulette — would this be the day I finally figured it out, or the day my monitor didn’t survive the rage?

That’s why setting up **early diagnostics** is critical. This section won’t cover building a full test suite, but it will show how to configure QEMU’s serial output so you can log kernel messages directly to `stdio` on your host OS. 

With this in place, you can actually *see* what’s going on — or more importantly, what’s *not* going on — inside your kernel. And since kernel development doesn’t leave much room for traditional testing, these logs quickly become your best debugging tool.

>[!IMPORTANT]
> In reality, the best debugging tool is *Interrupt Service Routines (ISRs)*. Almost always, when a fault occurs, an interrupt is called to handle it. Therefore, if you have set up routines to capture and handle these faults, you'll likely get a lot more info on what caused them as well. We will cover this in depth in Chapter 6.

Here’s a polished version of that section, structured to first give the big picture, then drill down into each function. I’ve kept the flow tutorial-style so it’s clear *why* we’re doing each step.

## Talking to the QEMU Serial Output

You cannot use `printf` for debugging in the kernel. If *any* fault occurs, the kernel crashes immediately, and anything printed to the screen is lost. This means that logging messages with `printf` is completely useless in this context. So, what do you do?

The solution is to use the serial port: QEMU can forward all serial output directly to your host’s standard I/O, allowing you to safely log messages from inside the kernel — even if it crashes later.

To enable this, always launch QEMU with the following flag:

```bash
-serial stdio
```

This will redirect COM1 output to your terminal, giving you a solid debugging channel.

---

### Serial Initialization

The first step is to set up the COM1 port. This configures the baud rate (here fixed at 38400), disables interrupts, and sets up the FIFO buffers.

```c
void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    // Disable interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1_PORT + 0, 0x03);    // Set divisor to 3 (38400 baud)
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}
```

Without this step, the port won’t behave predictably.

---

### Checking Readiness

Before writing data, you need to ensure the port is ready. This is done by polling the Line Status Register:

```c
int serial_is_ready(void) {
    return inb(COM1_PORT + 5) & 0x20;
}
```

This prevents data corruption by ensuring we don’t write when the transmit buffer is full.

---

### Writing Characters and Strings

To send a character:

```c
void serial_write_char(char c) {
    while (!serial_is_ready());   // Wait until THR is empty
    outb(COM1_PORT, (uint8_t)c);
}
```

Strings are written one character at a time, with a special case: when `\n` is encountered, a carriage return (`\r`) is also sent for compatibility with most terminals:

```c
void serial_write(const char* str) {
    while (*str) {
        if (*str == '\n')
            serial_write_char('\r');
        serial_write_char(*str++);
    }
}
```

There’s also a length-based variant:

```c
void serial_write_len(const char* str, size_t len);
```

Useful when dealing with non-null-terminated buffers.

---

### Writing Hexadecimal Values

Kernel developers often need to log raw numbers (e.g., register states, memory addresses). To support this, the implementation includes helpers for printing values in hex.

Each function breaks down the number into nibbles (4-bit chunks) and writes them using a shared helper:

```c
static void serial_write_hex_digit(uint8_t val);
void serial_write_hex8(uint8_t value);
void serial_write_hex16(uint16_t value);
void serial_write_hex32(uint32_t value);
void serial_write_hex64(uint64_t value);
```

For example, `serial_write_hex32(0xCAFEBABE);` would print:

```
CAFEBABE
```

>[!NOTE]
> All of the above functionality is implemented and available through [`serial.h`](/src/headers/serial.h)/[`serial.c`](/src/impl/kernel/serial.c).


## Debugging on Top of Serial Output

With serial I/O in place, we can finally build higher-level debugging utilities for GatOS. Rather than writing raw strings directly to the serial port, the kernel provides structured debug functions that log messages, track execution progress, and even dump the state of the page tables.

This approach makes debugging much more manageable in a bare-metal environment, where traditional debuggers are impractical. When combined with QEMU’s `-serial stdio` option, all debug logs can be streamed into your terminal, redirected to a file, or piped into external tools for analysis.


### Logging with Counters

The first utility is `DEBUG_LOG`, which makes it easier to trace execution flow by attaching a counter to each log entry.

```c
void DEBUG_LOG(const char* msg, int total);
```

* **Counter**: Each log entry is prefixed with `[X/Y]`, where `X` increments per call and `Y` is a caller-specified total.
* **Message**: The provided string is appended after the counter.
* **Output**: Written directly to the serial output.

For example:

```c
DEBUG_LOG("Parsing multiboot structure", 5);
```

Might produce:

```
[1/5] Parsing multiboot structure
```

This is invaluable for tracking initialization sequences step by step. For example:

```c
#include <debug.h>
#define TOTAL_DBG 2 # Number of total DEBUG_LOG calls

void my_function() {
    DEBUG_LOG("Made it to my_function - things are working!", TOTAL_DBG);
    
    // Your code here

    DEBUG_LOG("Still alive after doing stuff", TOTAL_DBG);
}
```

If the kernel dies in between the two logs, you know where to look.

### Dumping the Page Table Structure

The second utility, `DEBUG_DUMP_PMT`, provides a recursive walk of the kernel’s paging hierarchy.

```c
void DEBUG_DUMP_PMT(void);
```

This function traverses all levels of the x86_64 4-level paging structure:

* **PML4** → **PDPT** → **PD** → **PT** → **Physical Pages**

At each level, it prints the index and entry contents (lower 32 bits for brevity). This produces a tree-like structure that shows exactly which virtual pages are mapped and where they point in physical memory.

Example (truncated for clarity):

```
Page Tables:
PML4[0001]: 0000A003 -> PDPT
  PDPT[0002]: 00123003 -> PD
    PD[0040]: 00ABF003 -> PT
      PT[0010]: 04567003 -> PHYS
```

We can use this to see what's mapped and where it points to after we make changes to the page tables.

>[!IMPORTANT]
> The function `DEBUG_DUMP_PMT` relies on a few definitions we haven't yet discussed about, that GatOS declares in `paging.h`:
>
>```c
> #define PRESENT         (1ULL << 0)
> #define WRITABLE        (1ULL << 1)
> #define USER            (1ULL << 2)
>#define NO_EXECUTE      (1ULL << 63)
>#define ADDR_MASK       0x000FFFFFFFFFF000UL
>#define PAGE_SIZE       0x1000UL
>#define PAGE_ENTRIES    512
>#define PAGE_MASK       0xFFFFF000
>```

The raw output of `DEBUG_DUMP_PMT` can be overwhelming, especially on large systems. Fortunately, QEMU makes it easy to redirect serial output into a file:

```bash
qemu-system-x86_64 [...] -serial stdio > dump.txt
```

The resulting `dump.txt` can then be parsed using [`parse_pmt.py`](tools/parse_pmt.py), which provides an interactive environment for you to play around with your mappings.

>[!TIP]
>For more information on helper tools and how to use them, check out the README in the [`tools`](tools) directory.

## Removing the Identity Map

Now that the kernel is fully running in the higher half, the multiboot structure has been parsed and copied there, all debugging tools are linked within the kernel range and accessible from the higher half, and the VGA buffer operates in the higher half as well, there is no longer any need to maintain the lower-half identity mapping.

Removing the identity map is straightforward if you’ve followed the page table setup in Chapter 3 and understand the memory concepts from Chapter 2. Before doing so, it’s helpful — but not strictly necessary — to create a small helper function for flushing the TLB. This ensures that any cached mappings are refreshed after we alter the page tables.

Flushing the TLB is simple: just reload the `cr3` register, which holds the address of our `PML4` table:

```c
void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}
```

We also need a function to retrieve the current PML4. We can use inline assembly for that as well:

```c
uint64_t* getPML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)KERNEL_P2V(cr3);
}
```

>[!IMPORTANT]
> Remember that `cr3` always holds the physical address of our *PML4*. Therefore, we use `P2V` to access it from higher memory.

We can now remove the identity mapping. Recall from Chapter 3 that our `PML4` and `PDPT` entries eventually point to the same `PD`, but through different indices:

* `PML4[511] -> PDPT[510] -> PD` for the higher half
* `PML4[0] -> PDPT[0] -> PD` for the lower half (identity)

Since our `PD` covers `1 GB` of memory, removing the lower half mapping is as simple as clearing the corresponding entries:

```c
PML4[0] = 0;
PDPT[0] = 0;
```

This unlinks the lower-half path from the `PD`, making the lower `1 GB` range inaccessible in virtual memory.

Putting it all together:

```c
void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

uint64_t* getPML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)KERNEL_P2V(cr3);
}

void unmap_identity(){
    int64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + 512 * PREALLOC_PML4s;
    PML4[0] = 0;
    PDPT[0] = 0;
    flush_tlb();
}
```

Finally, in `paging.h`, we define the sizes of our preallocated page tables:

```c
#define PREALLOC_PML4s  1
#define PREALLOC_PDPTs  1
#define PREALLOC_PDs    1
#define PREALLOC_PTs    512
```
