/*
 * caps.h - Capability macros shared between appa and GatOS
 *
 * appa infers what a Gata program needs and passes it down as -D macros (CapabilityDefines in
 * Program.cs). GatOS #ifdefs out the subsystems the program doesn't use, to shrink the image.
 *
 *   GATA_CAP_MEM         - the heap (slab/vmm/heap)
 *   GATA_CAP_THREADS     - any process/thread, kernel or user realm (the full scheduler/process/
 *                          tty/dashboard/syscall stack). Implies INPUT, the dashboard needs the
 *                          keyboard IRQ whether or not the program reads input
 *   GATA_CAP_INPUT       - the program reads input, or THREADS is on
 *   GATA_CAP_TIME        - the program reads the clock (_env_time_ns). Implies the interrupt
 *                          subsystem, whose timer tick is the uptime counter
 *   GATA_CAP_FRAMEBUFFER - output renders to the framebuffer: console driver, font, dashboard
 *   GATA_OUTPUT_SERIAL   - output goes to COM1 instead: kernel output, program output (env bridge,
 *                          SYS_WRITE), screen control as ANSI (SYS_TTY_CTRL), input echo and the
 *                          panic report. A TTY still exists under THREADS for its line discipline
 *                          and input buffer, but tty->console is NULL
 *   GATA_KBD_DEFAULT     - PS/2 only
 *   GATA_KBD_EXTERNAL    - + USB HID (xHCI/PCI), no hotplug watch thread
 *   GATA_KBD_HOTPLUG     - + USB hotplug watch (a kernel thread)
 *
 * This only ever ADDS defines to satisfy an implication, never removes one a caller passed.
 * Include it before relying on any of the macros above.
 *
 * The implication rules live in three places that must stay in sync (the template-matrix CI
 * checks it): this header, appa's ResolveCaps (src/CLI/Program.cs in the Appa repo) and
 * run.py's capability parser.
 */

#pragma once

// Threads pull in the whole multitasking stack (scheduler/process/tty/dashboard/syscall) and all of
// it allocates, so THREADS implies MEM.
#if defined(GATA_CAP_THREADS) && !defined(GATA_CAP_MEM)
#define GATA_CAP_MEM
#endif

// The dashboard's ALT+TAB/CTRL+SHIFT+ESC cycling (built with THREADS) only gets keypresses from
// keyboard.c's IRQ handler, which isn't in the build without INPUT. A threaded program that never
// reads input would get a dashboard nothing can bring up, so THREADS implies INPUT too.
#if defined(GATA_CAP_THREADS) && !defined(GATA_CAP_INPUT)
#define GATA_CAP_INPUT
#endif

// The hotplug watch is its own kernel thread (xhci_hotplug_init) and needs xHCI over PCI plus PS/2
// as the fallback, so HOTPLUG implies EXTERNAL + DEFAULT + THREADS.
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

// xHCI enumeration allocates from the kernel heap even for a single one-time scan, so either USB
// keyboard level needs MEM, THREADS or not.
#if (defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)) && !defined(GATA_CAP_MEM)
#define GATA_CAP_MEM
#endif

// External or hotplug USB keyboard support requires input.
#if (defined(GATA_KBD_EXTERNAL) || defined(GATA_KBD_HOTPLUG)) && !defined(GATA_CAP_INPUT)
#define GATA_CAP_INPUT
#endif

// ACPI/APIC/the timer tick are needed when the scheduler wants a timer IRQ (THREADS), the keyboard
// needs IOAPIC routing (INPUT), or the program reads the clock (TIME, get_uptime_ns only advances
// once the tick is armed). They all map ACPI tables/MMIO/per-CPU structures through vmm_alloc, so
// this implies MEM too. With none of the three, APIC never comes up (exceptions come straight off
// the IDT), the one case that can still go fully memory-free.
#if defined(GATA_CAP_THREADS) || defined(GATA_CAP_INPUT) || defined(GATA_CAP_TIME)
#define GATA_NEEDS_INTERRUPT_SUBSYS
#  if !defined(GATA_CAP_MEM)
#  define GATA_CAP_MEM
#  endif
#endif

// Exactly one output mode should be set. Default to framebuffer if a build (say a hand-written
// run.py profile) forgot to, so existing behavior holds.
#if !defined(GATA_OUTPUT_SERIAL) && !defined(GATA_CAP_FRAMEBUFFER)
#define GATA_CAP_FRAMEBUFFER
#endif
