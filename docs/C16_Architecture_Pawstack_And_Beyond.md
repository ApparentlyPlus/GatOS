# Chapter 16: Architecture, Pawstack and Beyond

Welcome to the last chapter! If you have read all the way from the first serial byte to the spinning donut, thank you. That is a long way to walk with someone. I hope it was worth it.

This chapter is a little different from the others. There is almost no new kernel code in it. Instead, we step back and look at GatOS from the outside: how it is tested, how it was shaped to sit under a compiler, what sits on top of it, and where it goes next. Most of it is about ideas rather than registers. You can relax a bit this time.

>[!NOTE]
>The code for this chapter lives in [`tests/`](/src/tests/) on the `next` branch, and in [`caps.h`](https://github.com/ApparentlyPlus/GatOS/blob/appa-template/src/kernel/caps.h) on the `appa-template` branch. Gata, libgata and Appa live in their own repositories.

## Testing GatOS

Many chapters in this book end with "and here is how I checked it". So let's see where those checks actually run.

GatOS has a second build, the **test build**. Building with `python run.py all test` adds `-DTEST_BUILD`, which swaps `kernel_main` for a different entry point, `kernel_test`. It brings the kernel up in the same order as a normal boot, then runs ten test suites against the live subsystems instead of starting the demos:

```
 boot           pmm            vmm            slab           heap
 spinlock       timers         input          tty            multitasking
```

That comes to 346 tests, each printing one line over serial. A test is just a function that returns `true` or `false`, with a couple of macros to make failures readable:

```c
static bool t_rdmsr_safe(void) {
    uint64_t efer = 0;
    TEST_ASSERT(rdmsr_safe(MSR_EFER, &efer));
    TEST_ASSERT(efer & EFER_LMA);
    return true;
}
```

If an assertion fails, it logs the condition and the line number. The suite then moves on to the next test. With `headless timeout=200s`, we run the whole thing without a window, and QEMU is killed if something hangs (which makes it usable from a script).

The tests run inside the kernel itself, with no host machine to fall back on. That matters more than it sounds. A test for the scheduler really does create threads and really does preempt them. A bug in the context switch shows up as a crash in the test build, instead of a wrong number on someone's screen.

One lesson from writing this book is worth passing on: run the tests on more than one kind of CPU. QEMU has two very different ways of running a guest. Its emulator (TCG) translates every instruction in software, while KVM runs the guest directly on the real CPU. They disagree more often than you would expect.

In the last chapter, reading an MSR that does not exist raised `#GP` under KVM, but quietly returned 0 under the emulator. A test that only ever ran on one of them would have missed half of that story.

On top of that, every push to `main` builds and tests GatOS on Linux, macOS and Windows. A change that breaks the toolchain on one of them gets caught right away.

## The Kernel as an API

Way back in Chapter 0, I said GatOS is not really meant to be an operating system in the way Linux or Windows are. It is meant to be an **API**, the layer something else is built on. Now that the whole kernel exists, we can see what that means in practice.

Every subsystem in GatOS is a header, a handful of functions and a set of status codes. The PMM gives out frames through `pmm_alloc` and reports `PMM_ERR_OOM` when it runs out. The VMM maps ranges through `vmm_alloc` and `vmm_alloc_at`. The scheduler takes threads through `sched_add`. Nothing above a subsystem reaches into its internals:

```
 ┌───────────────────────────────────────────────────────────────┐
 │  user programs, ulibc                                   C15   │
 ├───────────────────────────────────────────────────────────────┤
 │  processes, threads, the scheduler                      C14   │
 │  the console, TTYs, the dashboard                       C13   │
 │  PCI, xHCI, USB keyboards                               C12   │
 │  system calls, keyboard input                           C11   │
 │  ACPI, APIC, timers                                     C10   │
 ├───────────────────────────────────────────────────────────────┤
 │  GDT, CPU features, the kernel heap                      C9   │
 │  the VMM                                                 C8   │
 │  the PMM, the slab                                       C7   │
 │  interrupts, panics, spinlocks                           C6   │
 ├───────────────────────────────────────────────────────────────┤
 │  the physmap, early C, long mode, the build          C1 to C5 │
 └───────────────────────────────────────────────────────────────┘
                              the hardware
```

Each layer only talks to the ones below it, through their headers. This means any one of them can be replaced without the others noticing. A better heap only has to keep `kmalloc` and `kfree` working. A different scheduler only has to keep `sched_add`, `sched_yield` and the sleep calls. This is not an accident, since it is exactly what something sitting *above* the kernel needs: a fixed set of functions it can call, no matter how they are implemented underneath.

That something is Pawstack.

## The Upper Stack

So far, GatOS has been the whole story. In Pawstack, it is the bottom layer. Let's go through what sits on top of it, one piece at a time.

### Gata in a Page

**Gata** is the language programs are written in. It is meant to feel like a language you already know, with classes, generics, tagged unions and real error handling, while never doing anything a kernel cannot afford (there is no garbage collector, for example).

The one idea worth knowing for this chapter is the **realm**. A Gata program says which of its parts run in the kernel and which run in ring 3:

```gata
import Console;

realm kernel {
    entry func Main() {
        Console.PrintLine("Hello from the kernel!");
    }
}

realm userspace {
    foreground process App {
        thread Main {
            entry func Run() {
                Console.PrintLine("Hello from userspace!");
            }
        }
    }
}
```

The kernel realm has an entry function that runs at boot, right where GatOS's own `kernel_main` would. The userspace realm declares processes and threads, which map directly onto the processes and threads from Chapter 14.

### Appa

**Appa** is the compiler. It does not produce machine code itself. Instead, it turns a Gata program into C, then hands that C to the same GCC that builds GatOS:

```
  main.g ──► lex, parse, type check ──► lower ──► emit C
                                                     │
          ┌──────────────────────────────────────────┘
          ▼
   kmain.c      the kernel realm, plus the boot code
   uproc.c      the userspace realm
   umain.c      uapps, which creates the processes and threads
          │
          ▼
   copy the GatOS template ──► drop the files in ──► compile ──► link ──► GRUB ──► ISO
```

The three files on the left should look familiar. `uproc.c` is caught by the `*uproc*` pattern in the linker script and lands at `0x400000`. `umain.c` is kernel code that hands user addresses to `thread_create`. Everything we saw in the last chapter, from the outside, was really this pipeline.

Running `appa build` in a project directory does all of it. A few seconds later, there is a bootable ISO.

### One Call, Two Lowerings

Here is the interesting part. In the example above, both realms call `Console.PrintLine`. But printing in the kernel and printing in ring 3 are completely different things. The kernel can write to a TTY directly. A user program has to make a system call.

So how does one Gata function do both?

The answer is a small file we carry in every project, the **environment**. It defines a few dozen low level functions, all named `_env_` something, once for each realm. Here is `_env_write`, the function every print ends up in, from the GatOS environment:

```c
// kernel realm
static inline void _env_write(const char* d, int n) {
    tty_t* t = active_tty;
    if (sched_active()) {
        thread_t* cur = sched_current();
        if (cur && cur->process && cur->process->tty) t = cur->process->tty;
    }
    if (t) tty_write(t, d, (size_t)n);
}

// userspace realm
static inline void _env_write(const char* d, int n) {
    if (d && n > 0) syscall_write(d, (size_t)n);
}
```

(The real kernel version also handles serial only builds and builds without threads, which I left out here.)

The kernel version finds the right TTY and calls `tty_write`, the function from Chapter 13. The user version calls `syscall_write`. From there, the bytes take exactly the path we traced in the last chapter. `Console.PrintLine` itself has no idea which one it is calling. Appa compiles it twice (once into each realm), and each copy picks up its own `_env_write`.

### libgata

**libgata** is Gata's standard library, the equivalent of `ulibc` one level up. It has about 30 modules (strings, lists, maps, math, time, threads and locks, console input and output) in around 7,200 lines of Gata.

Almost none of it knows about GatOS. Every module is written in plain Gata on top of the `_env_` functions. This means porting Pawstack to a different kernel would mean rewriting one environment file. Nothing in the library would change. In fact, there already is a second environment, for running Gata programs as normal programs on Linux or macOS. It maps `_env_write` onto the C library's `fwrite` to standard output. The exact same library runs there too.

### The Template and the Capability Gates

There is one problem left. GatOS has a lot in it: a scheduler, TTYs, a dashboard, a USB stack. A Gata program that prints one line does not need any of that. Why should its image carry it?

Appa ships its own copy of GatOS, on the `appa-template` branch. It is the same kernel, with every subsystem wrapped in `#ifdef`s like this one:

```c
// Dead weight without a heap to manage, see GATA_CAP_MEM in caps.h.
#ifdef GATA_CAP_MEM
...
#endif // GATA_CAP_MEM
```

While compiling, Appa looks at what the program actually uses. Does it allocate? Does it read input? Does it declare any processes or threads? Does it read the clock? From the answers, it passes **capability** macros down to GCC, like `-DGATA_CAP_MEM` or `-DGATA_CAP_THREADS`. The kernel then only includes the subsystems those macros switch on.

Capabilities are not independent, though. Threads need the scheduler, the scheduler needs a timer interrupt, and the timer needs the APIC, which maps its registers through the VMM. So [`caps.h`](https://github.com/ApparentlyPlus/GatOS/blob/appa-template/src/kernel/caps.h) adds whatever a capability implies, before anything else is compiled:

```
   THREADS ─────► INPUT ──────┐
      │                       ▼
      │             the interrupt subsystem ──► MEM
      │              (ACPI, APIC, timers)        ▲
      │                       ▲                  │
      │           TIME ───────┘                  │
      └──────────────────────────────────────────┘
```

Here is what that buys us, with real builds of the same kernel:

| Build | Capabilities | Kernel size |
|:------|:-------------|------------:|
| GatOS on `next`, with all its demos | everything | 370 KB |
| A Gata program with three cooperating threads | mem, input, threads | 218 KB |
| The hello program above | mem, input, threads | 194 KB |
| A one line kernel only program, on screen | mem | 117 KB |
| The same, printing to serial | mem | 100 KB |

A kernel only program on serial is less than a third of the full kernel. The rules live in three places (`caps.h`, Appa and `run.py`), which all have to agree. A separate CI job builds the template with a list of representative capability sets on every push to `appa-template`. If one of those `#ifdef` seams breaks, it breaks there first.

## What Grows Without a Chapter

From here on, GatOS will keep changing, but most of those changes will not get a chapter. The architecture is done. What is left is mostly three kinds of work:

* **New drivers**, like a disk controller or a network card. They plug in the way xHCI did, over PCI. To the rest of the kernel, each one is just one more header.
* **Better versions of existing subsystems**, like a faster heap or a smarter scheduler. As long as they keep their functions and status codes, nothing above them needs to know.
* **Bugs.** There will be more of them, as every chapter of this book has shown.

None of that changes how GatOS fits together. Documenting every driver would turn this book into a hardware manual. So we stop the book here, even though the kernel keeps going.

## Looking Back

Before we look forward, let's take a moment to see how far we came. Here is the whole book, one line per chapter:

| Chapter | What it gave us |
|:--------|:----------------|
| C1 | A cross compiler, a build script and a bootable ISO |
| C2 | The ideas behind pages, page tables and virtual memory |
| C3 | Long mode, and a first page table, in assembly |
| C4 | C code, serial output and the multiboot information |
| C5 | The physmap, so the kernel can touch any physical byte |
| C6 | Interrupts, a panic screen and spinlocks |
| C7 | The PMM and the slab, for physical memory |
| C8 | The VMM, for virtual memory |
| C9 | The GDT, CPU feature detection and the kernel heap |
| C10 | ACPI, the APIC and timers |
| C11 | System calls and keyboard input |
| C12 | PCI and USB keyboards through xHCI |
| C13 | A real console, TTYs and the dashboard |
| C14 | Processes, threads and a scheduler |
| C15 | User programs, a C library and a spinning donut |

That is about 18,000 lines of kernel code (not counting the font, the printf and the math library we borrowed), another 5,800 lines of tests, and a bit under 140,000 words explaining all of it.

The number I find most interesting, though, is not any of those.

Almost every chapter of this book found bugs in code that already worked. A heap that never merged two free neighbours, a math function wrong in the seventh digit, a TTY parser that printed half of every color code. None of them showed up in normal use. They showed up when I sat down to explain, line by line, why the code was correct, only to find that it was not.

If there is one thing I would recommend to anyone building something like this, it is that. Write it down as if someone else will read it. Someone will, and it might be you.

## Venturing Onward

So what comes next? A few big pieces are missing from GatOS. Each of them already has a foundation in what we built, so let's go through them one at a time.

### Filesystems

Filesystems are the obvious one. They need a disk driver first, and the xHCI chapter already did most of the hard part (finding a device over PCI, mapping its registers and talking to it through rings in memory). AHCI (for SATA disks) and NVMe both work that way.

On top of a disk driver, we would put a small VFS layer, a common interface every filesystem implements. Then come the filesystems themselves. FAT goes first, since it is simple and every machine can read it, then exFAT, ext4 and maybe NTFS. A filesystem also brings back the loader we skipped in the last chapter. Once programs can live on a disk, GatOS can read an ELF file and build a process from it, the way Linux does.

### Networking

Networking follows the same pattern. A network card is one more PCI device with rings in memory (the virtio network card QEMU emulates is a good first target). Above it sits a protocol stack (Ethernet, ARP, IP, UDP, then TCP) and a socket interface as system calls.

### SMP

SMP, running on more than one core, is closer than it looks. The APIC chapter already brings up the local APIC, which is also how the other cores get woken up. `cpu_local` was built per CPU from the start, every core has its own timer, and every spinlock already disables interrupts while held. The scheduler is what needs real work, with a run queue per core and a way to move threads between them.

Graphics is the big one: a windowed desktop, the mouse, a GPU driver and video. Those all depend on each other, so they get the whole GatOS 3.0 section below.

### How They Plug In

Every one of these connects to Pawstack the same way. We give a new subsystem a capability gate, so programs that do not use it do not pay for it. Then we add a few new `_env_` functions to the environment file, with a libgata module on top of them (a `File` module, a `Socket` module).

Appa itself does not change at all. The compiler only ever sees the environment. To a Gata programmer, a whole new part of the kernel shows up as one more `import`.

## GatOS 3.0

That brings us to the next milestone, GatOS 3.0. It will not be documented chapter by chapter like this one. The plan is to bring everything above together, turning Pawstack from "a toolchain for one kind of OS" into a general purpose **OS construction kit**. Pick the parts you need, write the rest in Gata, and get a bootable image.

The most interesting design problem on that road is the screen: moving from one full screen console at a time to a real windowed desktop, with many programs drawing side by side and a compositor putting them together. So let's look at it properly.

### What Is Baked In Today

Right now, everything about how GatOS looks lives inside the kernel. Let's list what we put there, since it is more than it seems:

* **The console** ([`console.c`](/src/kernel/drivers/console.c)) draws glyphs straight into the framebuffer (only for whichever console belongs to `active_tty`).
* **The TTYs** ([`tty.c`](/src/kernel/drivers/tty.c)) are the windows. Every process gets one, while `active_tty` decides which one is on screen.
* **Switching** is hardcoded. [`input.c`](/src/kernel/drivers/input.c) checks every key press for Alt+Tab (`tty_cycle`), Alt+F4 (`tty_destroy`) and Ctrl+Shift+Esc (`dash_toggle`) before any program sees it.
* **Input** reaches programs as characters. A key press goes through `keycode_to_ascii`, then into the TTY's line discipline. A program only ever reads bytes.
* **The dashboard** is a kernel thread that draws into its own TTY.

Put together, that is a tiny desktop environment, written in C and compiled into the kernel. It has a window manager (`active_tty` and `tty_cycle`), keyboard shortcuts (in `input.c`) and a system monitor (the dashboard). Anyone who wants a different one has to edit the kernel.

That is fine for a console. It does not work for a GUI. Everyone wants their desktop to look different: floating windows, tiling, a single full screen app on a kiosk, a game that owns every pixel. Baking one of those into the kernel would be choosing for every Pawstack user at once.

### Mechanism, Not Policy

There is an old rule in OS design that fits this perfectly: the kernel should provide **mechanism**, and leave **policy** to programs. The mechanism is "a program can put pixels on the screen and receive key presses". The policy is "windows have title bars, and Alt+Tab switches between them".

Today, GatOS mixes the two. For a GUI, the kernel would only keep the mechanism, as four building blocks:

1. **Surfaces.** A surface is a block of memory a program draws pixels into, plus a width, a height and a format. To the kernel, it is just pages. The VMM can already map the same physical frames into two address spaces (that is what `VM_FLAG_FOREIGN` does for user code in every process). A surface can be shared between the program that draws it and the program that puts it on screen, without copying.
2. **Input events.** Instead of turning key presses into characters, the kernel would hand out the raw `key_event_t` it already builds (the key code, pressed or released, the modifiers), and later mouse events too. Characters, shortcuts and line editing become a library's job.
3. **The display grant.** Exactly one process, the **compositor**, gets the framebuffer mapped into its address space (write combining, like the console has it now) and receives every input event. It owns the list of windows, where each one sits, which one is on top, and who gets which key. Every pixel on the screen passes through it.
4. **Channels.** Programs need a way to talk to the compositor: "my surface changed, in this rectangle", "you have the keyboard now", "the user resized you". That is a small message passing system call, the first real IPC GatOS would have.

Input follows the same idea. The compositor knows which window is focused, so it is the only one that can decide where a key goes. Here is how one key press would travel, compared with today:

```
 today                                    with a compositor

 keyboard IRQ                             keyboard IRQ
   │                                        │
   ▼                                        ▼
 input.c: Alt+Tab? Alt+F4?                input.c: queue the raw event
   │                                        │
   ▼                                        ▼
 keycode_to_ascii                         compositor process (ring 3)
   │                                        │  decides: a shortcut? which window?
   ▼                                        ▼
 active_tty's line discipline             channel message to the focused app
   │                                        │
   ▼                                        ▼
 the program reads a byte                 the app gets a key event
```

Everything on the right side below the IRQ is a choice someone made in Gata, not in the kernel.

### Windows and Compositing

In a windowed desktop, no program draws to the screen directly. Every app draws into its own surface, at its own size, as if it had a small screen of its own. The compositor is the only one that ever touches the framebuffer. It keeps a list of windows, each with a position and a place in the stack, and builds every frame by painting them from the bottom of the stack to the top:

```
   app surfaces (each in its own memory)          the screen (the compositor's view)

   ┌───────────────┐                              ┌───────────────────────────────────┐
   │ terminal      │ 640x400, at (40, 60)         │  ┌──────────────┐                 │
   └───────────────┘                              │  │ terminal  ┌──┴───────────┐     │
   ┌───────────────┐                              │  │           │ donut        │     │
   │ donut         │ 480x300, at (420, 200)  ──►  │  └───────────┤              │     │
   └───────────────┘                              │              └──────────────┘     │
   ┌───────────────┐                              │  ┌─────────────────────────────┐  │
   │ taskbar       │ 1280x32, at (0, 768)         │  │ taskbar                     │  │
   └───────────────┘                              └──┴─────────────────────────────┴──┘
```

Apps never learn where their window is, or whether anything covers it. That is the compositor's whole job. It is also what makes every desktop policy possible. Floating windows, tiling, a dock, wobbly animations or a single full screen app are all just different ways of choosing positions and stacking order, inside the compositor.

The stacking order also lets the compositor skip work. A part of a window that is covered by another one never needs to be copied, since it would be painted over right away. So when the donut redraws a frame, the compositor only copies the part of its damaged rectangle that is actually visible.

### Events and the Mouse

A windowed desktop without a mouse is not much of one. GatOS does not read a mouse at all yet, but both of the places one could come from are already half there.

The PS/2 controller has two ports. Port 1 is the keyboard, and port 2 is the mouse. [`i8042.c`](/src/kernel/drivers/i8042.c) deliberately leaves port 2 switched off, and the keyboard handler throws away any byte that came from it.

Turning it on takes a controller command (`0xA8`), then a command to the mouse itself (`0xF4`, "start sending data", sent with a `0xD4` prefix so the controller forwards it to port 2). Last, IRQ 12 gets routed through the I/O APIC, the same way Chapter 10 routed IRQ 1.

From then on, every movement arrives as a 3 byte packet. Say we move the mouse 10 pixels left and 5 up, while holding the left button:

```
 byte 0   0x19   0 0 0 1 1 0 0 1
                 │ │ │ │ │ │ │ └─ left button: down
                 │ │ │ │ │ │ └─── right button: up
                 │ │ │ │ │ └───── middle button: up
                 │ │ │ │ └─────── always 1
                 │ │ │ └───────── X is negative
                 │ │ └─────────── Y is positive
                 └─┴───────────── no overflow
 byte 1   0xF6   X movement: -10 (with the sign bit from byte 0)
 byte 2   0x05   Y movement: +5
```

Notice that PS/2 counts Y upwards, while the screen counts it downwards, so the driver flips it. A USB mouse is even simpler. The xHCI driver already looks for "boot protocol" HID devices, and only accepts keyboards (protocol 1). A boot mouse is protocol 2, and its report is a buttons byte followed by signed X and Y bytes, which the driver could diff the same way `usb_kbd_feed` diffs key reports.

Both drivers end in the same place: an **event**. Today, a key press becomes a `key_event_t` and, a few lines later, a character. In a windowed system, we would stop at the event and make it a little more general:

```c
typedef struct {
    uint64_t time_ns;               // from get_uptime_ns, when it happened
    uint8_t  type;                  // key, motion, button or wheel
    uint8_t  modifiers;             // Ctrl, Shift, Alt, like today
    union {
        struct { uint16_t keycode; bool pressed; } key;
        struct { int16_t dx, dy; }               motion;
        struct { uint8_t button; bool pressed; } button;
        struct { int8_t delta; }                 wheel;
    };
} input_event_t;
```

The kernel puts these into one queue, read by the compositor, much like the TTY's input buffer from Chapter 13 (except it holds events instead of characters). The timestamp matters more than it looks. It lets the compositor tell a double click from two single ones, and it lets an app know how old an event is by the time it arrives.

A mouse can send a lot of events. A USB mouse polled at 1000 Hz sends up to a thousand motion events a second, while the screen only updates 60 times. There is no point drawing the pointer 16 times per frame, so the compositor **coalesces** them: it adds up all the motion that arrived since the last frame, and moves the pointer once.

The pointer itself is just one more window, the smallest and topmost one, drawn last on every frame. Moving it damages two tiny rectangles (where it was, and where it is), a few kilobytes of pixels at most.

### Clicks, Focus and Dragging

When a button goes down, the compositor has to work out which window it landed on. That is called **hit testing**, and the stacking order makes it easy: walk the windows from the top of the stack down, and the first one whose rectangle contains the pointer gets the click.

Before passing it on, the compositor subtracts the window's position, so the app receives coordinates relative to its own top left corner. Apps still never learn where they are on screen.

Clicking also decides **focus**. A common policy is "click to focus": the clicked window moves to the top of the stack and becomes the one that gets keyboard events. Another is "focus follows the mouse". Either way, it is a few lines in the compositor, and the kernel never needs to know which one is in use.

Dragging a window is where this all comes together. Here is what happens when the user grabs a window's title bar and moves it:

1. The button goes down over the title bar. The compositor sees it is a title bar (it drew it), so it does not pass the click to the app. Instead, it starts a **grab**: until the button comes back up, every motion event belongs to the compositor.
2. Every frame, it adds the coalesced motion to the window's position.
3. It damages the window's old rectangle and its new one, and redraws only those.
4. The button comes up, the grab ends, and events go back to normal.

Moving a window changes two regions of the screen at once, the place it left and the place it arrived. Say the donut window (480 by 300) moves 12 pixels to the right in one frame. The damaged area is the union of the two rectangles, 492 by 300, which is about 590 KB of pixels. That is a lot more than a blinking cursor, but still far less than the 4 MB of a full screen.

The app does not redraw anything during all of this. Its surface stays exactly the same. Only its position changes, so the compositor copies the surface to its new spot and repaints the uncovered strip from whatever was underneath. On Linux, this is the difference between an old X11 desktop, where moving a window made every app below it repaint, and a modern compositing one, where apps never notice.

Whether the title bar is drawn by the compositor or by the app is, again, a policy choice. A compositor that draws every title bar looks the same everywhere. One that lets apps draw their own (and only asks them to say "start a move now") lets every app look the way it wants. Both work on top of the same four building blocks.

### What Gets Rewritten

Surprisingly little of the code we wrote has to go. Most of it moves.

`input.c` keeps reading keyboards, but loses its three hardcoded shortcuts. Those become the compositor's business. `tty_switch` and `tty_cycle` disappear entirely, since the compositor decides what is on screen.

The console survives, but moves to userspace. A terminal window is just a program that draws glyphs into a surface. `console.c` already keeps a backbuffer of cells, tracks which ones are dirty and draws runs of glyphs. If we point its `fb` at a surface instead of the real framebuffer, it becomes a terminal emulator. Its dirty cells turn into exactly the "this rectangle changed" messages the compositor wants.

The dashboard becomes a normal program too. It would need one new system call to read the process list and CPU times, which today it reads straight out of kernel memory.

The TTYs stay, but shrink back to what they are in Unix: a line discipline and an input queue, for programs that only want to read and write text. A terminal window would give its program a TTY, the same way a terminal emulator on Linux does.

One piece must not move: the crash console. When the kernel panics, the compositor might be the thing that broke. The panic screen keeps its own direct path to the framebuffer, the way it has one now.

### A Desktop Written in Gata

With those four building blocks in the kernel, a desktop environment is just a Gata program. libgata would grow a module for it, with two halves. The client half is for apps: create a surface, draw into it, tell the compositor what changed, read events. The server half is for compositors: accept new surfaces, compose them onto the screen, route events.

A very small compositor could look something like this. (This is a sketch of where the API might go, not code that exists today.)

```gata
import Gui;

realm userspace {
    display process Desktop {
        thread Compose {
            entry func Run() {
                let screen = Gui.Screen();
                while (true) {
                    let ev = Gui.NextEvent();
                    match (ev) {
                        case Key(k) {
                            if (k.alt && k.code == Gui.Key.Tab) { Gui.RaiseNext(); }
                            else { Gui.SendToFocused(ev); }
                        }
                        case Damage(win, rect) {
                            screen.Compose(Gui.Windows(), rect);
                        }
                    }
                }
            }
        }
    }
}
```

The `display` keyword would be how a process asks for the display grant, in the same spirit as `foreground` asks for a TTY today. Appa would see it and turn on a `GATA_CAP_GUI` capability. The kernel would then include the surface, event and channel code. A program without a GUI would not pay for any of it.

The important part is what is *not* in the kernel. Alt+Tab is a line in someone's Gata program. Window borders, a taskbar, a tiling layout, animations: all of them would be libraries and programs anyone can write, share or throw away. Two Pawstack users could build two completely different desktops on the same kernel, without touching a line of C.

### How It Scales

A GUI moves a lot more memory than a console. Let's put some numbers on it. A 1280 by 800 screen at 4 bytes per pixel is about 4 MB. Redrawing all of it 60 times a second means writing about 240 MB every second into the framebuffer. That is possible, but wasteful. On slower machines, it is not possible at all.

The console already showed us the way out: only draw what changed. A blinking cursor in a terminal is a few hundred pixels, not 4 MB. So the whole design revolves around **damage**: every message about a surface carries the rectangle that changed, and the compositor only copies those rectangles. In the common case (one window typing, one clock ticking), a frame is a few kilobytes.

Copies are the next cost. Since surfaces are shared memory, the compositor reads the app's pixels straight out of the app's own pages. The only copy is the final one, into the framebuffer.

To keep the app from drawing into a surface while the compositor is reading it, each window gets two buffers. The app draws into one, sends "commit", and starts on the other, while the compositor reads the first. Nobody ever holds a lock on pixels. They just pass buffers back and forth, like the donut and the console passing a finished frame.

Memory scales well too. An 800 by 600 window is about 1.9 MB per buffer, but with `VM_FLAG_LAZY`, pages nobody has touched yet cost nothing. A big window that is mostly one color costs much less than its size.

Timing is the last piece. Without a real GPU driver, there is no vertical sync signal to tell us when the screen refreshes. The compositor would pace itself with a timer instead, exactly like the donut does with its 16 ms frames.

The next section shows how a GPU driver takes over from that timer.

With SMP, all of this spreads out nicely. The compositor runs on one core while apps draw on the others. Since they only share buffers and short messages, they hardly ever wait for each other. The one thing the scheduler would need is a notion of **priority**, so the compositor gets the CPU the moment input arrives, even when every other core is busy.

### The GPU

Everything so far has the CPU touching every pixel. That is how GatOS works today: GRUB picks a video mode before the kernel even starts (`gfxmode=auto`), hands over a plain block of memory, and the console writes pixels into it one store at a time. There is no way to change the resolution, no signal for when the screen refreshes, and no help from the graphics hardware at all.

A **GPU driver** changes that. The first one would be for **virtio-gpu**, the virtual GPU QEMU emulates. It is a PCI device, and it is driven through rings in memory, much like xHCI. Instead of writing pixels, the driver sends commands. For 2D work, there are only a handful of them:

| Command | What it does |
|:--------|:-------------|
| `RESOURCE_CREATE_2D` | Creates an image on the GPU side, with a size and a format |
| `RESOURCE_ATTACH_BACKING` | Tells the GPU which of our physical pages hold its pixels |
| `SET_SCANOUT` | Picks which image the screen shows |
| `TRANSFER_TO_HOST_2D` | Says "this rectangle of the pages changed" |
| `RESOURCE_FLUSH` | Puts that rectangle on screen |

Look at how well these line up with what we already designed. A resource is a surface. Attaching backing pages means the GPU reads an app's surface straight out of the app's own memory. On our side, there is no copy at all, not even the last one into the framebuffer. A transfer of one rectangle is exactly a damage message. The compositor's job barely changes. It just sends commands where it used to copy pixels.

virtio-gpu also has a separate queue just for the pointer. The compositor uploads the pointer image once, and from then on, moving it is a single `MOVE_CURSOR` command with the new position. The pointer stops costing any pixel writes at all, which is why real desktops use a **hardware cursor** whenever they can.

With a GPU driver, timing gets better too. The driver can tell when the GPU has finished with a frame (through a **fence**, a marker the GPU signals once it gets past a point in the command stream), so the compositor no longer has to guess with a 16 ms timer. And since modes are set by the driver, the resolution can change at runtime, for example when a window is made full screen.

3D is a much bigger step. A real GPU runs programs of its own (shaders), and apps need to send it command buffers full of draw calls.

The kernel's job there is not to understand those commands, but to share the GPU fairly: give each process its own GPU memory, check that a command buffer only touches that memory, and take turns submitting them. Linux splits this into two halves (**KMS** for setting modes and putting images on screen, **DRM** for everything else), and that split is worth copying.

Drivers for real Intel, AMD or NVIDIA GPUs are huge projects on their own, which is exactly why virtio-gpu comes first. The important part is that the compositor and the apps only ever see surfaces, damage and fences. Whether those end up in a framebuffer, in virtio-gpu or in a real GPU is the driver's business.

### Video and Codecs

Playing a video sounds like it needs a lot from the kernel. It needs surprisingly little, though, because most of the work is plain computation.

Video is huge without compression. One 1080p frame, in the format video usually uses (YUV 4:2:0, about 1.5 bytes per pixel), is about 3.1 MB. At 30 frames a second, that is around 93 MB every second. A typical H.264 stream for the same video is about 5 megabits a second, roughly 150 times smaller. A **codec** is the code that turns one into the other: an encoder compresses, and a decoder turns the compressed stream back into frames.

Decoding is just math. It reads bytes, does a lot of integer and SIMD arithmetic, and writes pixels. It needs no hardware and no system calls in its inner loops, so a decoder is a library that runs entirely in ring 3, the same way the math library did in the last chapter.

GatOS already handles the hard part of that. The lazy FPU code from Chapter 14 saves state with `xsave` whenever the CPU supports it, so a decoder using AVX registers in ring 3 keeps them across context switches.

Once a frame is decoded, it still has to become something the screen understands. Video frames are in YUV (one brightness value per pixel, with the color stored at a quarter of the resolution), while the screen wants RGB. Converting means a few multiplications per pixel, about 2 million times per 1080p frame.

SIMD makes that cheap on a CPU. A GPU can make it free, since most GPUs can show a YUV image directly, as an **overlay plane** on top of everything else.

The rest is timing. Every compressed frame carries a **presentation timestamp**, the moment it should appear on screen. A player decodes a few frames ahead into a small queue of surfaces, and shows each one when `SYS_TIME_NS` reaches its timestamp. If decoding falls behind, it skips frames instead of drifting.

With sound (which needs an audio driver first, and Intel HD Audio is one more PCI device with rings in memory), the audio clock becomes the one everything else follows, since people notice a skip in sound much more than a dropped frame.

Many GPUs also have dedicated video engines that decode H.264 or H.265 in hardware. Using one is a GPU driver feature, like any other command. A player would try the hardware path first and fall back to the software decoder.

For Pawstack, this means codecs never go into the kernel at all. The kernel provides memory, time, surfaces and (later) audio. A decoder is a library, written in Gata or ported from C the way fdlibm was. And because the window, the surface and the timing all come from the same building blocks, a video player is just one more window to the compositor.

## Ambitions

That last point leads into the things I want to improve in what already exists.

The scheduler is the first. Right now, every thread gets the same quantum. A thread that just woke up waits its turn like everyone else. Priorities (and a way for interactive threads to earn them) would make the whole system feel faster without making it any faster. After that comes SMP scheduling, with run queues per core and some sense of which core a thread last ran on.

The heap and the slab could be faster too, with per CPU caches so cores do not fight over one lock. The `_S5_` scan from the last chapter should eventually become a real AML interpreter, which would also bring proper power management, like sleep and battery status on laptops.

And GatOS needs permissions. Today, any program can call `SYS_POWEROFF`. That is fine when every program on the machine was compiled into it by you, but it stops being fine the moment programs come from a disk.

None of these change the architecture. They are exactly the kind of work from "What Grows Without a Chapter". That is the point. The skeleton is done. From here, GatOS mostly gets better at being itself.

## Author's Ending Note

When I started GatOS, I had never written a line of kernel code. I did not know what a page table was, or why anyone would need more than one kind of interrupt controller. I learned most of it the hard way, usually at 3 in the morning, staring at a QEMU window that had just rebooted itself for the hundredth time.

This book started as notes to myself, so I would not forget how things worked. Somewhere along the way, it turned into something I hoped other people could use: the guide I wish I had when I started. Resources for this kind of work are scattered, often outdated and rarely written for beginners. If these chapters made even one part of it less scary for you, then they did their job.

GatOS is not finished. It never really will be. That is part of the fun. There is always one more driver, one more bug, one more thing that works on QEMU and not on real hardware. But the core is here, it boots, it runs programs, and it spins a donut. That is more than I thought I would ever build when I wrote the first line of Chapter 1.

Thank you for reading along, all the way to the end. If something did not make sense, or you found a bug (you probably did), or you built something on top of GatOS, I would love to hear about it. As I said in the very first chapter: "I don't bite".

See you in GatOS 3.0.
