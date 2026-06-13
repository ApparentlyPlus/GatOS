# Chapter 4: Early C Setup

In the previous chapter, we successfully made the transition to long mode and completed our journey by handing off execution to C through the `kernel_main` function. Now we're ready to build upon that foundation.

This chapter focuses on establishing the essential utilities our kernel needs to operate effectively. We'll cover:

1. **Serial Output**: Introducing `serial.c` as our primary early diagnostic tool. We'll look at COM1 and COM2 initialization, polling the Line Status Register, and the `debug.c` macros (`QEMU_LOG`, `LOGF`).

2. **klibc**: A brief look at the introduction of `string.c`.

3. **Multiboot2 Parsing**: Updating the parsing section to reflect how `multiboot2.c` now safely relocates the parsed data into a preallocated higher-half buffer (`multiboot_buffer`).

4. **Paging Cleanup**: Updating the identity unmapping section to reflect the new `cleanup_kpt` and `unmap_identity` logic.

Once we have these core utilities in place, we'll be ready for the next major step: in Chapter 5, we'll explore paging from a higher level perspective and construct the **physmap**, reserving and setting up the necessary page tables to map all of physical RAM into virtual memory.

But first, a quick note about what *isn't* here anymore.

## What Happened to VGA?

As mentioned in Chapter 3, GatOS skips VGA text mode entirely. The reason is straightforward: by requesting a framebuffer from GRUB using the framebuffer tag in our Multiboot2 header, we asked for a linear graphics mode. Once that happens, VGA text mode is no longer available. The two are mutually exclusive.

Beyond that, VGA output isn't especially useful for early debugging anyway. It is legacy and doesn't play well with modern UEFI systems. Also, if the kernel crashes, anything written to the screen disappears with it. Serial output, by contrast, is transmitted immediately and captured by QEMU before a crash can obscure it. We'll use the framebuffer later to build a proper graphical console. For now, serial handles everything we need.

## Early Diagnostics

Usually, the first major hurdle during early C setup is **debugging**. In bare-metal development, debugging can be frustrating, sometimes even borderline impossible. Tools like `gdb` can help, but they require specialized setup and are limited to stepping through assembly instructions, which isn't always practical.

When I was developing GatOS, the biggest issue I ran into was page faults. Tracking down the cause often felt like playing Russian roulette — would this be the day I finally figured it out, or the day my monitor didn't survive the rage?

That's why setting up **early diagnostics** is critical. This section won't cover building a full test suite, but it will show how to configure QEMU's serial output so you can log kernel messages directly to `stdio` on your host OS.

With this in place, you can actually *see* what's going on (or more importantly what's *not* going on) inside your kernel. And since kernel development doesn't leave much room for traditional testing, these logs quickly become your best debugging tool.

>[!IMPORTANT]
> In reality, the best debugging tool is *Interrupt Service Routines (ISRs)*. Almost always, when a fault occurs, an interrupt is called to handle it. Therefore, if you have set up routines to capture and handle these faults, you'll likely get a lot more info on what caused them. We will cover interrupts in depth in a later chapter.

## Talking to the QEMU Serial Output

You cannot use `printf` for debugging in the kernel. The thing about bare metal is that if *any* fault occurs, the kernel crashes immediately, and anything printed to the screen (probably) goes down as with it. In the best case, it gets semi printed (yes, that's a thing), and the screen looks like [Caine from TADC crashing out](https://www.reddit.com/media?url=https%3A%2F%2Fi.redd.it%2F3o7vqeilkqjg1.jpeg).

This means that logging messages with `printf` is completely useless in this context. Even if we wanted to use something like `printf`, we don't really have it implemented, do we? So, what do we do?

The solution is to use the serial port: QEMU can forward all serial output directly to your host's standard I/O, allowing you to safely log messages from inside the kernel even if it crashes later.

GatOS's `run.py` launches QEMU with two serial flags:

```
-serial mon:stdio        →  COM1  →  your terminal
-serial file:debug.log   →  COM2  →  debug.log on disk
```

COM1 output appears immediately in your terminal. COM2 is silently saved to `debug.log` on disk. We'll see shortly how these two channels serve different purposes.

---

### Serial Initialization

The first step is to set up the serial ports. All serial functionality lives in [`kernel/drivers/serial.h`](/src/kernel/drivers/serial.h) and [`kernel/drivers/serial.c`](/src/kernel/drivers/serial.c). The driver supports COM1 through COM4, each represented by a `serial_port_t` enum:

```c
typedef enum {
    SERIAL_COM1 = 0,
    SERIAL_COM2 = 1,
    SERIAL_COM3 = 2,
    SERIAL_COM4 = 3
} serial_port_t;
```

To initialize a specific port, we call `serial_init_port`. To bring up both COM1 and COM2 at once, we use `serial_init_all`. Both ultimately run the same initialization sequence. This configures the baud rate (fixed at 38400), disables interrupts, and sets up the FIFO buffers:

```c
void serial_init_port(serial_port_t port) {
    uint16_t port_base = get_port_base(port);

    outb(port_base + 1, 0x00);    // Disable interrupts
    outb(port_base + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(port_base + 0, 0x03);    // Set divisor to 3 (38400 baud)
    outb(port_base + 1, 0x00);
    outb(port_base + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(port_base + 2, 0xC7);    // Enable FIFO, clear them, 14-byte threshold
    outb(port_base + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}
```

The function `outb` is defined in [`io.h`](/src/arch/x86_64/cpu/io.h) along with similar x86 serial calls. These are native x86 instructions that perform serial I/O operations:

```c
/*
 * outb - Writes a byte to an I/O port
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * inb - Reads a byte from an I/O port
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * outw - Writes a word to an I/O port
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * inw - Reads a word from an I/O port
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * io_wait - Small delay for I/O operations
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}
```

At this point, I should probably make a note and explain this for anyone interested. However, it is absolutely safe to skip the details and either copy-paste this code into your kernel if you are following along, or just assume that this needs to be present and move on. Realistically, we are looking at 1990s hardware conventions here. 

>[!NOTE]
>For most readers, understanding what the `serial_init_port` function does is pointless, because it's largely legacy stuff that just needs to be configured in this way for serial to work. However, if anyone is curious enough, well, here goes.
>
>#### Step 1: Pick the Port Base
>
>```c
>uint16_t port_base = get_port_base(port);
>```
>
>The `serial_port_t` enum identifies which COM port we want:
>
>```c
>typedef enum {
>    SERIAL_COM1 = 0,
>    SERIAL_COM2 = 1,
>    SERIAL_COM3 = 2,
>    SERIAL_COM4 = 3
>} serial_port_t;
>```
>
>And `get_port_base` simply maps that enum to `0x3F8`, `0x2F8`, and so on.
>
>#### Step 2: Disable Interrupts on the UART
>
>```c
>outb(port_base + 1, 0x00);
>```
>
>Offset `+1` is the **Interrupt Enable Register** when the DLAB bit is clear (we'll see what that is next). Writing `0x00` disables all UART-generated interrupts.
>
>What are interrupts you may ask? Well, for now, all you need to know is that an interrupt just stops CPU execution and expects your code to handle it before continuing. If you don't have code that handles certain interrupts, then depending on the severity of the interrupt, it can either crash the kernel or just be ignored.
>
>Here, we want deterministic setup. The UART should not start producing interrupt-driven behavior before we even know what interrupts are.
>
>#### Step 3: Turn On DLAB
>
>```c
>outb(port_base + 3, 0x80);
>```
>
>Offset `+3` is the **Line Control Register**. Bit 7 of that register is the **Divisor Latch Access Bit**, or **DLAB**.
>
>When DLAB is set, offsets `+0` and `+1` stop meaning "data register" and "interrupt enable register" and start meaning "divisor low byte" and "divisor high byte".
>
>That sounds obscure until you realize what it gives us: access to the baud-rate divisor.
>
>The UART's internal clock is traditionally 115200 baud. To get a lower baud rate, you divide that clock by a programmable divisor.
>
>So if we want 38400 baud:
>
>$$
>\frac{115200}{38400} = 3
>$$
>
>That means the divisor should be `3`.
>
>#### What is baud?
>
>Baud is the number of symbol changes per second on the wire. A "symbol" is a change in the electrical signal.
>
>In simple serial communication like RS-232 (what our UART uses), one symbol = one bit. So for our purposes:
>
>```
>Baud rate = Bits per second
>```
>
>When we set the divisor to 3, we get 38,400 baud, which means 38,400 bits per second on the wire.
>
>#### What Does This Actually Mean Physically?
>
>Imagine you're sending the character 'A' (ASCII 0x41 = binary 01000001) over a serial cable at 38,400 baud.
>
>The UART takes that byte and sends it bit-by-bit, with timing controlled by a clock. At 38,400 baud:
>
>Each bit takes 1/38400 seconds ≈ 26 microseconds
>
>To send 8 data bits + 1 start bit + 1 stop bit = 10 bits total
>
>One character takes about 260 microseconds to transmit
>
>The baud rate determines how fast the electrical signal on the wire toggles between high and low voltages.
>
>#### Step 4: Write the Divisor
>
>```c
>outb(port_base + 0, 0x03);
>outb(port_base + 1, 0x00);
>```
>
>Because DLAB is set, these two writes go into the divisor latch.
>
>- Low byte: `0x03`
>- High byte: `0x00`
>
>Together, that is the 16-bit divisor `0x0003`.
>
>So now the UART is configured for 38400 baud.
>
>Why 38400 and not the full 115200? No idea dude, I just saw it on tutorials online, and they all seem to be using this, so I trust whatever 1997 kiddo came up with it.
>
>#### Step 5: Switch Back to Normal Line Control
>
>```c
>outb(port_base + 3, 0x03);
>```
>
>This clears DLAB again and sets the line format.
>
>`0x03` means:
>
>- 8 data bits
>- no parity
>- 1 stop bit
>
>This format is commonly called **8N1**. If you have ever seen serial settings in a terminal emulator, that is what you were looking at. If not, well, don't worry, neither have I.
>
>#### Step 6: Enable and Clear the FIFO
>
>```c
>outb(port_base + 2, 0xC7);
>```
>
>Offset `+2` is the **FIFO Control Register**.
>
>`0xC7` configures the FIFO and clears it.
>
>The useful takeaway here is not memorizing every bit. It is understanding why we touch this register at all: a FIFO buffer lets the UART absorb a few bytes internally instead of forcing us to synchronize perfectly one byte at a time.
>
>Also, clearing the FIFO during init is just good hygiene. If firmware or a previous stage left junk in it, we do not want to inherit that state.
>
>Feel like a grandpa for doing all of this yet? It gets worse.
>
>#### Step 7: Modem Control Lines
>
>```c
>outb(port_base + 4, 0x0B);
>```
>
>Offset `+4` is the **Modem Control Register**.
>
>This sets lines like DTR, RTS, and OUT2.
>
>If that sounds like old modem-era baggage, that is because it is. But PC UART emulation still expects some of these bits to be set for the port to behave normally, especially in virtualized environments.
>
>This is one of those places where early hardware interfaces still carry the fossil record of ancient design decisions. You do not need to understand what any of this does, but you must have your code configure it.

---

### Checking Readiness

Before writing data, you need to ensure the port is ready. This is done by polling the **Line Status Register**, located at `port_base + 5`:

```c
int serial_is_ready_port(serial_port_t port) {
    uint16_t port_base = get_port_base(port);
    return inb(port_base + 5) & 0x20;
}
```

Bit 5 is the **Transmitter Holding Register Empty** (THRE) flag. When it's set, the hardware is ready for the next byte. This prevents data corruption by ensuring we don't write when the transmit buffer is full.

---

### Writing Characters and Strings

To send a character:

```c
void serial_write_char_port(serial_port_t port, char c) {
    while (!serial_is_ready_port(port));   // Wait until THR is empty
    outb(get_port_base(port), (uint8_t)c);
}
```

Strings are written one character at a time, with a special case: when `\n` is encountered, a carriage return (`\r`) is also sent for compatibility with most terminals:

```c
void serial_write_port(serial_port_t port, const char* str) {
    while (*str) {
        if (*str == '\n')
            serial_write_char_port(port, '\r');
        serial_write_char_port(port, *str++);
    }
}
```

There's also a length-based variant:

```c
void serial_write_len_port(serial_port_t port, const char* str, size_t len);
```

Useful when dealing with non-null-terminated buffers.

---

### Writing Hexadecimal Values

Kernel developers often need to log raw numbers (e.g., register states, memory addresses). To support this, the implementation includes helpers for printing values in hex.

Each function breaks down the number into nibbles (4-bit chunks) and writes them using a shared helper:

```c
void serial_write_hex8_port(serial_port_t port, uint8_t value);
void serial_write_hex16_port(serial_port_t port, uint16_t value);
void serial_write_hex32_port(serial_port_t port, uint32_t value);
void serial_write_hex64_port(serial_port_t port, uint64_t value);
```

For example, `serial_write_hex32_port(SERIAL_COM1, 0xCAFEBABE)` would print:

```
CAFEBABE
```

All of the above functions are also available in a default-port form targeting COM1:

```c
void serial_write(const char* str);
void serial_write_char(char c);
void serial_write_hex32(uint32_t value);
// ... and so on
```

>[!NOTE]
> All of the above functionality is implemented and available through [`kernel/drivers/serial.h`](/src/kernel/drivers/serial.h) and [`kernel/drivers/serial.c`](/src/kernel/drivers/serial.c).


## Debugging on Top of Serial Output

With serial I/O in place, we can finally build higher-level debugging utilities for GatOS. Rather than writing raw strings directly to the serial port, the kernel provides structured debug functions that log messages, track execution progress, and even dump the state of the page tables.

This approach makes debugging much more manageable in a bare-metal environment, where traditional debuggers are impractical. When combined with QEMU's serial flags, all debug logs can be streamed into your terminal, redirected to a file, or piped into external tools for analysis.

All of these utilities are declared in [`kernel/debug.h`](/src/kernel/debug.h):

```c
void QEMU_LOG(const char* msg, int total);
void QEMU_GENERIC_LOG(const char* msg);
void LOGF(const char* fmt, ...);
```

### Logging with Counters

The first utility is `QEMU_LOG`, which makes it easier to trace execution flow by attaching a counter to each log entry.

```c
void QEMU_LOG(const char* msg, int total);
```

* **Counter**: Each log entry is prefixed with `[X/Y]`, where `X` increments per call and `Y` is a caller-specified total.
* **Message**: The provided string is appended after the counter.
* **Output**: Written directly to COM1 (your terminal).

For example:

```c
QEMU_LOG("Parsing multiboot structure", 5);
```

Will produce:

```
[1/5] Parsing multiboot structure
```

This is invaluable for tracking initialization sequences step by step. For example:

```c
#include <kernel/debug.h>
#define TOTAL_DBG 2

void my_function() {
    QEMU_LOG("Made it to my_function - things are working!", TOTAL_DBG);

    // Your code here

    QEMU_LOG("Still alive after doing stuff", TOTAL_DBG);
}
```

If the kernel dies in between the two logs, you know where to look.

### LOGF

`LOGF` is intentionally different from `QEMU_LOG`. Rather than writing to COM1, it writes to **COM2**, which gets saved to `debug.log` on disk:

```c
void LOGF(const char* fmt, ...);
```

`LOGF` is used internally throughout the kernel's subsystems for low-level status messages. The idea is that `debug.log` accumulates a detailed trace of everything the kernel has been doing in the background, without cluttering your host's terminal.

It also supports full format strings:

```c
LOGF("[PAGING] Removed identity mapping\n");
LOGF("[PAGING] Reserved physmap tablespace (%d MiB)\n", table_bytes / MEASUREMENT_UNIT_MB);
```

### Dumping the Page Table Structure

The last utility, `QEMU_DUMP_PML4`, provides a recursive walk of the kernel's paging hierarchy.

```c
void QEMU_DUMP_PML4(void);
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
> The function `QEMU_DUMP_PML4` relies on a few page table definitions declared in `paging.h`:
>
>```c
>#define PAGE_PRESENT    (1ULL << 0)
>#define PAGE_WRITABLE   (1ULL << 1)
>#define PAGE_USER       (1ULL << 2)
>#define PAGE_NO_EXECUTE (1ULL << 63)
>#define ADDR_MASK       0x000FFFFFFFFFF000UL
>#define PAGE_SIZE       0x1000UL
>#define PAGE_ENTRIES    512
>#define FRAME_MASK      0xFFFFF000
>```

The raw output of `QEMU_DUMP_PML4` can be overwhelming, especially on large systems. Fortunately, QEMU makes it easy to redirect serial output into a file:

```bash
qemu-system-x86_64 [...] -serial stdio > dump.txt
```

The resulting `dump.txt` can then be parsed using [`parse_pmt.py`](tools/parse_pmt.py), which provides an interactive environment for you to play around with your mappings.

>[!TIP]
>For more information on helper tools and how to use them, check out the README in the [`tools`](tools) directory.

## klibc

We've glossed over another important topic: the C standard library (`libc`). Porting a full `libc` implementation is quite cumbersome, especially when we don't yet have the underlying system calls that many `libc` functions depend on. However, we still need basic library functionality to make kernel development practical.

For this reason, GatOS includes a standalone `klibc`, living under [`src/klibc/`](/src/klibc/). It provides only what the kernel actually needs, when it needs it. For now, we only care about `string.c`.

### Memory and String Manipulation

[`klibc/string.h`](/src/klibc/string.h) and [`klibc/string.c`](/src/klibc/string.c) give us essential memory and string manipulation functions. All names carry a `k` prefix to differentiate the kernel's `libc` from the userland `libc` that we will later introduce:

```c
void* kmemset(void* dest, int c, size_t n);
void* kmemcpy(void* dest, const void* src, size_t n);
size_t kstrlen(const char* str);
int    kstrcmp(const char* s1, const char* s2);
// ... and more
```

These functions massively accelerate our string handling development and eliminate the need to reinvent basic utilities.

>[!WARNING]
> While a full `libc` port might happen eventually, it remains irrelevant to GatOS's current goals. We're implementing only what we actually need, when we need it.

### On `printf` and beyond

Of course, a primitive write function only goes so far. Eventually, you'll want formatted output:

```c
kprintf("Loaded %d modules at 0x%lx\n", module_count, load_address);
```

But implementing your own full-blown `printf` — especially one that supports padding, and format specifiers — is a *lot* of work.

Instead of reinventing the wheel, GatOS adopts [Marco Paland's `printf` / `sprintf` implementation for embedded systems](https://github.com/mpaland/printf). His work is credited directly in the source code.

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

For GatOS specifically, the output function was initially wired up to pipe directly through `serial_write_char`, so everything printed with `kprintf` would end up on COM1.

>[!IMPORTANT]
>Currently, GatOS has matured enough to have a full fledged TTY subsystem, so the `printf` code has been changed. We will talk about those changes in later chapters. 
>
> All you need to know for now is that you can follow the instructions on Marco's project and wire up his fully functional `printf` to output in QEMU's serial using the `serial_write_char` function discussed earlier.
>
> The current implementation lives in [`klibc/stdio.h`](/src/klibc/stdio.h) and [`klibc/stdio.c`](/src/klibc/stdio.c).

## Parsing the Multiboot2 Struct

Yes, I know, I know. Back to the dreaded multiboot we go again. But hey, last time, I promise.

In the previous chapter, we passed the Multiboot2 struct into `kernel_main` via `rdi`. Now it's finally time to make use of it. GatOS includes a dedicated Multiboot2 parser, located in [`arch/x86_64/multiboot2.h`](/src/arch/x86_64/multiboot2.h) and [`arch/x86_64/multiboot2.c`](/src/arch/x86_64/multiboot2.c).

I'm not going to dive into the full details of parsing the Multiboot2 struct here — that would take far too long, and it's beyond the scope of this documentation. If you'd like to roll your own parser, I highly recommend the [GNU Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html). It lays everything out clearly and even provides sample code you can learn from.

Instead, I'll focus on a few key implementation details and give a high-level explanation of how our parser works.

### The Kernel Range

Up until now, when talking about paging, we've mostly focused on the `P2V`/`V2P` macros and the page tables. But there's another concept that GatOS tracks closely: **the kernel range**.

Formally, this range is `[KVIRT_START, KVIRT_END]`. In practice, though, GatOS primarily cares about `KVIRT_END`. That's because it treats `KVIRT_START` as a fixed value (`0xFFFFFFFF80000000`) which also implies that GatOS treats `KPHYS_START` as `0x0`. I've explained the reasoning for this setup in earlier sections.

Both `KVIRT_START`/`KVIRT_END` and their physical counterparts `KPHYS_START`/`KPHYS_END` are made available in C code via linker symbols. What's important to understand is that GatOS will later adjust `KEND` internally when building the *physmap*. This is necessary to make room for new page tables, ensuring that all of RAM can be mapped into virtual space. Keep this in mind because it will play a crucial role in the next chapter.

> [!NOTE]
> From this point forward, we'll just refer to **`KEND`** as a general concept. Whether it's `KPHYS_END` or `KVIRT_END` doesn't really matter — both point to the same boundary, just seen from different perspectives (physical vs virtual).

To make this clean and maintainable, it's best to **wrap these linker symbols** inside `paging.h` and `paging.c`. That way, the rest of the kernel always queries `KEND` through the wrapper functions. This has two benefits:

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

At first glance, this might look redundant — why do we need both "normal" and "linker" getters? Let's break it down.

* **`get_kstart` / `get_kend`**
  These return the *current* kernel boundaries. They are backed by the static variables `KSTART` and `KEND`, which may be updated at runtime. For example, when GatOS builds the physmap, it shifts `KEND` forward to make room for new page tables. Calling these functions ensures you always get the *latest, adjusted values*.

* **`get_linker_kstart` / `get_linker_kend`**
  These return the *original values* defined by the linker symbols. In other words, they give you the "linker" kernel range as it was at boot time, untouched by runtime adjustments. These are useful when you need the baseline, fixed reference points for the kernel's location.

Both variants take a `bool virtual` argument. If `true`, the function converts the address into its higher-half (virtual) equivalent using `KERNEL_P2V`. If `false`, you get the physical address directly.

This dual system gives us flexibility:

* Use **linker values** when you need to reference the kernel's fixed layout.
* Use **current values** when you want the runtime-adjusted state (after physmap expansion).

By centralizing this logic in `paging.c`, the rest of the kernel doesn't need to worry about linker symbols, runtime tweaks, or physical/virtual conversions. Everything just calls these wrappers and gets the correct answer for the current context.

### The Parser

At boot, the multiboot structure is placed in lower memory, and we can access it only because of the lower-half identity mapping established during early initialization.

To handle this safely, the kernel first parses the multiboot structure in its original, lower-half form. After parsing, each pointer inside the structure is translated to its higher-half equivalent, and all relevant data is copied into a **preallocated buffer** reserved within the kernel's address space. Since this buffer resides in the higher half (inside the kernel range), the information remains accessible even after the lower half is unmapped.

This buffer is declared in [`kmain.c`](/src/kernel/kmain.c):

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

>[!IMPORTANT]
> `multiboot_init` must be called before removing the identity map. The parser reads from the lower-half address `mb_info` during initialization. Access that address after the identity map is gone and you'll get a page fault.

We can then use the `multiboot_parser_t` object to access any number of functions. For example:

```c
void kernel_main(void* mb_info) {

    multiboot_parser_t multiboot = {0};

    [...] // Initialization

    // Dump the memory map
    multiboot_dump_memory_map(&multiboot);
}
```

>[!NOTE]
>For the full list of functions that the parser supports, you can always look at [`arch/x86_64/multiboot2.h`](/src/arch/x86_64/multiboot2.h).


## Removing the Identity Map

Now that the kernel is fully running in the higher half, the multiboot structure has been parsed and copied there, and all debugging tools are linked within the kernel range and accessible from the higher half, there is no longer any need to maintain the lower-half identity mapping.

GatOS performs this cleanup in one sweep. Before doing so, it's helpful to have a small helper function for flushing the TLB. This ensures that any cached mappings are refreshed after we alter the page tables.

Flushing the TLB is simple: just reload the `cr3` register, which holds the address of our `PML4` table:

```c
void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}
```

We also need a function to retrieve the current PML4:

```c
uint64_t* getPML4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t*)KERNEL_P2V(cr3);
}
```

>[!IMPORTANT]
> Remember that `cr3` always holds the physical address of our *PML4*. Therefore, we use `KERNEL_P2V` to access it from higher memory.

### Cleanup Kernel Page Tables

The sweep is implemented in `cleanup_kpt(start, end)`. This function takes a physical address range and rebuilds the page table hierarchy so that **only that range remains mapped in the higher half**. Everything else, including the identity mapping and any other stray entries, is zeroed out.

So for example, if I called `cleanup_kpt(0x1000, 0x2000)`, then it would tweak the page tables so that ONLY the virtual range `[0xFFFFFFFF80001000, 0xFFFFFFFF80002000]` is accessible. Anything else? Page fault. As you can see, only high memory remains.

We call it with the full kernel range:

```c
cleanup_kpt(0x0, get_kend(false));
```

Internally, the function:

1. Determines which PML4, PDPT, PD, and PT indices are needed to cover the given range as a higher-half virtual mapping.
2. Zeros every PML4 entry except the one pointing to the kernel's higher-half region.
3. Zeros every PDPT entry except the one used by the kernel.
4. Zeros every PD entry that falls outside the kernel's range.
5. Zeros every PT entry that maps a page beyond the end of the kernel.
6. Calls `flush_tlb`.

### If we wanted to be explicit?

Recall from Chapter 3 that our `PML4` and `PDPT` entries eventually point to the same `PD`, but through different indices:

* `PML4[511] -> PDPT[510] -> PD` for the higher half
* `PML4[0] -> PDPT[0] -> PD` for the lower half (identity)

After `cleanup_kpt`, `PML4[0]` and `PDPT[0]` are already zeroed. But if you want to be explicit, we can define a function, `unmap_identity`, that manually cleans up the lower half mapping:

```c
void unmap_identity(void) {
    int64_t* PML4 = getPML4();
    uint64_t* PDPT = PML4 + PAGE_ENTRIES * PREALLOC_PML4s;
    PML4[0] = 0;
    PDPT[0] = 0;
    flush_tlb();
    LOGF("[PAGING] Removed identity mapping\n");
}
```

This isn't really necessary, but you can use it for learning purposes.

Putting it all together in `kmain.c`:

```c
#define TOTAL_DBG 3

// Multiboot high half buffer 
// Note: this is linked in high memory and is inside the kernel range
static uint8_t multiboot_buffer[8 * 1024];

void kernel_main(void* mb_info) {
	// Init serial
	serial_init_port(COM1_PORT);
	serial_init_port(COM2_PORT);
	QEMU_LOG("Kernel main reached, normal assembly boot succeeded", TOTAL_DBG);

	// Multiboot comes next since we need to parse the memory map and other info before we can safely initialize memory management
	multiboot_parser_t multiboot = {0};
	multiboot_init(&multiboot, mb_info, multiboot_buffer, sizeof(multiboot_buffer));

	if (!multiboot.initialized) {
		QEMU_LOG("[KERNEL] Failed to initialize multiboot2 parser!", TOTAL_DBG);
		return;
	}
	QEMU_LOG("Multiboot structure parsed and copied to higher half", TOTAL_DBG);

	// Cleanup
	cleanup_kpt(0x0, get_kend(false));
	QEMU_LOG("Unmapped all memory besides the higher half kernel range", TOTAL_DBG);
}
```

Finally, in `paging.h`, the preallocated page table counts used in the pointer arithmetic above are defined as:

```c
#define PREALLOC_PML4s  1
#define PREALLOC_PDPTs  1
#define PREALLOC_PDs    1
#define PREALLOC_PTs    512
```

These reflect the layout established during bootstrap in `boot32.S`. Since the tables are allocated contiguously in `.bss`, arithmetic like `PML4 + PAGE_ENTRIES * PREALLOC_PML4s` skips 512 entries forward to reach the start of the PDPT.

>[!IMPORTANT]
> Any attempt to access a lower-half address after this cleanup will cause a page fault. This is intentional. The kernel has no reason to operate in low memory anymore.

With that, we can finally move on to some actual kernel work. Ready for the physmap? I hope you are!