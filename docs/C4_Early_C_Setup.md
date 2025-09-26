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

Instead, I’ll focus on a few key implementation details and give a high-level explanation of how our parser works.

### The Dependencies

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
uint64_t get_canonical_kend(bool virtual);
uint64_t get_canonical_kstart(bool virtual);

extern uintptr_t KPHYS_END;
extern uintptr_t KPHYS_START;
```

At first glance, this might look redundant — why do we need both “normal” and “canonical” getters? Let’s break it down.

* **`get_kstart` / `get_kend`**
  These return the *current* kernel boundaries. They are backed by the static variables `KSTART` and `KEND`, which may be updated at runtime. For example, when GatOS builds the physmap, it shifts `KEND` forward to make room for new page tables. Calling these functions ensures you always get the *latest, adjusted values*.

* **`get_canonical_kstart` / `get_canonical_kend`**
  These return the *original values* defined by the linker symbols. In other words, they give you the “canonical” kernel range as it was at boot time, untouched by runtime adjustments. These are useful when you need the baseline, fixed reference points for the kernel’s location.

Both variants take a `bool virtual` argument. If `true`, the function converts the address into its higher-half (virtual) equivalent using `KERNEL_P2V`. If `false`, you get the physical address directly.

This dual system gives us flexibility:

* Use **canonical values** when you need to reference the kernel’s fixed layout.
* Use **current values** when you want the runtime-adjusted state (e.g., after physmap expansion).

By centralizing this logic in `paging.c`, the rest of the kernel doesn’t need to worry about linker symbols, runtime tweaks, or physical/virtual conversions. Everything just calls these wrappers and gets the correct answer for the current context.

>[!CAUTION]
> This document is **incomplete**. Please check back tomorrow for the full version.

