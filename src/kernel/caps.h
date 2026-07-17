/*
 * caps.h - Capability macro contract between appa and GatOS
 *
 * appa infers which OS capabilities a compiled Gata program actually needs
 * and passes them down as -D macros (see CapabilityDefines in appa's
 * Program.cs). GatOS uses these to #ifdef out whole subsystems it doesn't
 * need to build for a given program, shrinking the kernel image to roughly
 * what the program actually uses.
 *
 *   GATA_CAP_MEM         - the heap (slab/vmm/heap) is needed.
 *   GATA_CAP_THREADS     - any process/thread is declared (kernel or user
 *                           context, doesn't matter - both need the full
 *                           scheduler/process/tty/dashboard/syscall stack).
 *                           Implies GATA_CAP_INPUT (the dashboard it pulls in
 *                           needs a working ALT+TAB/CTRL+SHIFT+ESC keyboard
 *                           IRQ regardless of whether the program reads input).
 *   GATA_CAP_INPUT       - the program reads input (keyboard), or THREADS
 *                           is on (see above).
 *   GATA_CAP_TIME        - the program reads the clock (Time.Nanos/Millis via
 *                           _env_time_ns). Implies the interrupt subsystem,
 *                           whose timer tick is what the uptime counter is.
 *   GATA_CAP_FRAMEBUFFER - output renders to the framebuffer console.
 *   GATA_OUTPUT_SERIAL   - output goes to the COM1 serial port instead.
 *   GATA_KBD_DEFAULT     - PS/2 only.
 *   GATA_KBD_EXTERNAL    - + USB HID (xHCI/PCI), no hotplug watch thread.
 *   GATA_KBD_HOTPLUG     - + USB hotplug watch (runs as a kernel thread).
 *
 * This header only ever ADDS defines to satisfy an implication; it never
 * removes one a caller passed explicitly. Include it before relying on any
 * of the macros above so the implications below are guaranteed to hold,
 * regardless of which exact set the build invocation passed in.
 *
 * The implication rules are mirrored in three places that must stay in sync
 * (guarded by the template-matrix CI workflow):
 *   1. this header (the compile-time truth)
 *   2. appa's ResolveCaps (src/CLI/Program.cs in the Appa repo)
 *   3. run.py's capability parser (implications section in main())
 */

#pragma once

// Threads pull in the whole multitasking stack (scheduler/process/tty/
// dashboard/syscall), every piece of which allocates internally regardless
// of what the program's own thread bodies do - so THREADS implies MEM.
#if defined(GATA_CAP_THREADS) && !defined(GATA_CAP_MEM)
#define GATA_CAP_MEM
#endif

// The dashboard's ALT+TAB/CTRL+SHIFT+ESC cycling (kernel/drivers/dashboard.c,
// built whenever THREADS is on) only ever receives a keypress through the
// keyboard IRQ handler in kernel/drivers/keyboard.c - which is dead weight,
// entirely absent from the build, without GATA_CAP_INPUT. That handler has
// nothing to do with whether the Gata *program* ever reads input itself, so
// a threaded program that never calls Console.InputLine() would otherwise
// build a dashboard nothing could ever bring up - THREADS supersedes every
// other capability, so it implies INPUT too.
#if defined(GATA_CAP_THREADS) && !defined(GATA_CAP_INPUT)
#define GATA_CAP_INPUT
#endif

// The USB hotplug watch runs as its own kernel thread (xhci_hotplug_init),
// and needs the rest of the USB stack (xHCI over PCI) plus PS/2 as the
// fallback path - so HOTPLUG implies EXTERNAL + DEFAULT + THREADS.
#if defined(GATA_KBD_HOTPLUG)
#  if !defined(GATA_KBD_EXTERNAL)
#  define GATA_KBD_EXTERNAL
#  endif
#  if !defined(GATA_CAP_THREADS)
#  define GATA_CAP_THREADS
#  endif
#endif

#if (defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)) && !defined(GATA_KBD_DEFAULT)
#define GATA_KBD_DEFAULT
#endif

// xHCI device enumeration allocates kernel-heap structures even for a single
// one-time scan (no thread required for that part) - so either USB keyboard
// level needs a heap, regardless of whether THREADS ends up implied too.
#if (defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)) && !defined(GATA_CAP_MEM)
#define GATA_CAP_MEM
#endif

// Either external or hotplug-capable USB keyboard support requires input.
#if (defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)) && !defined(GATA_CAP_INPUT)
#define GATA_CAP_INPUT
#endif

// ACPI/APIC/the timer tick are needed whenever the scheduler needs a timer
// IRQ (THREADS), the keyboard needs IOAPIC IRQ routing (INPUT), or the program
// reads the clock (TIME - get_uptime_ns is the timer subsystem's counter, which
// only advances once the tick is armed). All of these map ACPI tables / MMIO /
// per-CPU structures through vmm_alloc, so this implies a heap too - a build
// with none of the three never brings up APIC at all (exceptions are handled
// straight off the IDT, no APIC needed), so it's the one case that can still
// go fully memory-free.
#if defined(GATA_CAP_THREADS) || defined(GATA_CAP_INPUT) || defined(GATA_CAP_TIME)
#define GATA_NEEDS_INTERRUPT_SUBSYS
#  if !defined(GATA_CAP_MEM)
#  define GATA_CAP_MEM
#  endif
#endif

// Output mode: exactly one of these should be set; default to framebuffer
// if a build invocation (e.g. a hand-written run.py profile) forgot to set
// either, so existing default behavior is preserved.
#if !defined(GATA_OUTPUT_SERIAL) && !defined(GATA_CAP_FRAMEBUFFER)
#define GATA_CAP_FRAMEBUFFER
#endif
