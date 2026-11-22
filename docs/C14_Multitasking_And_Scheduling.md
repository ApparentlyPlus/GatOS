# Chapter 14: Multitasking and Scheduling

Count the threads on your computer some time. Odds are, there are a few thousand of them running right now, on a CPU with maybe 8 or 16 cores. A core can only run one thing at a time. Still, your music keeps playing, your downloads keep going and this page scrolls the moment you ask it to. How?

The cores take turns, very quickly. Each thread runs for a few milliseconds, gets paused, and waits while the others get their turn, over and over, hundreds of times a second. A few milliseconds is far too short for anyone to notice, so everything looks like it runs at once. That trick is called **multitasking**, and in this chapter, we build it for GatOS.

This is where GatOS stops being one program that runs from top to bottom, and becomes a system that runs many programs at once on a single core. Almost everything we have built so far shows up here. The timer drives it, the VMM gives every program its own memory, the TSS catches its interrupts, and the TTYs give each program a screen.

Grab a coffee. There are a lot of moving parts, but none of them are hard on their own. We are going to take them one at a time.

>[!NOTE]
>The code for this chapter lives in [`process.c`](/src/kernel/sys/process.c), [`scheduler.c`](/src/kernel/sys/scheduler.c) and [`ISR.S`](/src/arch/x86_64/cpu/ISR.S), along with their headers.

## The Multitasking Picture

Like memory and text before it, multitasking is a stack of ideas that only make sense together. Before we look at any code, let's build a mental model of all of them, including how a single core can look like it runs several programs at the same time.

There are four pieces to it:

1. **Processes**, which are running programs, each with its own memory
2. **Threads**, which are the things that actually run, inside a process
3. **The scheduler**, which decides which thread runs next
4. **The context switch**, which is how the CPU actually moves from one thread to another

### What is a process?

A process is a program that is running, together with everything it owns. In GatOS, that is three things. It has an **address space**, its own VMM with its own page tables. Address `0x400000` in one program has nothing to do with address `0x400000` in another. It has a **TTY**, the screen and keyboard it talks to. And it has a list of **threads**.

The important part is the address space. Two processes cannot see each other's memory at all, because their page tables simply do not map it. A process that crashes, or scribbles all over its own memory, cannot touch anybody else's.

>[!NOTE]
>Keep in mind that a process never runs by itself. It is a container, and nothing more. Only its threads run.

### What is a thread?

A thread is one line of execution, meaning a place in the code, the values in the registers and a stack. If a process is a kitchen, a thread is a cook in it. Every cook works on their own recipe at their own pace, but they all share the same counters and fridge.

Threads in the same process share the address space, and with it all of the program's memory. Each thread has its own registers, its own stack and its own FPU state. Two threads can be in completely different places in the code at the same time.

When a thread is not running, all of that has to be saved somewhere. Otherwise, it could never pick up where it left off. In GatOS, we keep a copy of every thread's registers inside its `thread_t`, along with a kernel stack and (for user threads) a stack in its process's address space.

>[!NOTE]
>Keep in mind that there are **many** threads, but on a single core, only **one** of them is actually running at any moment.

### What does the scheduler do?

The scheduler decides which thread runs next and for how long. We are going to use the simplest fair policy there is, **round robin**. Every thread that is ready to run waits in a queue. The one at the front runs for a short slice of time, 10 milliseconds, called a **quantum**. When its quantum is over, it goes to the back of the queue. Then the next one runs.

Not every thread is ready to run, though. A thread waiting for a key press, or sleeping for a second, has nothing to do. Those threads are kept out of the queue entirely. They only come back when whatever they are waiting for actually happens (a key, a timeout, an event from a device). When nobody at all is ready, a special **idle thread** runs. All it does is put the core to sleep until something changes.

### What is a context switch?

A context switch is the moment the CPU stops running one thread and starts running another. Saving the old thread's registers and loading the new thread's sounds simple, but there is a catch. Code cannot just decide to stop running in the middle of a loop. Something has to interrupt it.

That something is the timer. Every 10 milliseconds (or whenever the scheduler asked to be woken), the LAPIC timer fires an interrupt. The CPU saves the running thread's position, our interrupt stub saves the rest of its registers, and the scheduler gets a look at it. If it is time for someone else, the scheduler simply returns a *different* thread's saved registers. The interrupt then "returns" into that thread instead.

>[!IMPORTANT]
>This is the key idea of the whole chapter. It is worth reading twice. An interrupt saves everything about the running thread, while the return from an interrupt restores everything about a thread. Nothing says it has to be the same thread. A context switch is an interrupt that returns somewhere else.

### Two Threads, One Core

Let's put the pieces together and watch two threads, A and B, share one core:

```
time ───────────────────────────────────────────────────────────────────────►

      ┌──────────────┬──────────────┬────────┬─────────────────────────────┐
core  │    A runs    │    B runs    │ A runs │           B runs            │
      └──────────────┴──────────────┴────────┴─────────────────────────────┘
                     ▲              ▲        ▲
                   timer          timer    A calls read() and blocks,
                    tick           tick    so B runs early

queue     [ B ]          [ A ]        [ B ]      [ ]  (A waits on the TTY)
```

1. A is running while B waits in the queue. The timer fires.
2. The scheduler saves A's registers, puts A at the back of the queue, and loads B's. B runs from exactly where it last stopped.
3. 10 ms later, the timer fires again. The same thing happens the other way around.
4. This time, A asks for input before its quantum is over. There is nothing to read yet. A gives up the CPU early, and instead of going back into the queue, it goes onto the TTY's list of waiting threads. Now only B is left. It runs until a key wakes A up.

Neither A nor B ever knows any of this happened. As far as each of them can tell, it has the CPU all to itself, it just occasionally runs a little slower. That illusion is what multitasking is.

In the rest of the chapter, we build each piece in that picture: processes and threads first, then the context switch, then the scheduler around it.

## Processes

A process in GatOS is a small struct:

```c
typedef struct process {
    pid_t pid;
    char name[MAX_PROCESS_NAME];
    vmm_t* vmm;             // Address space
    tty_t* tty;             // Associated terminal
    
    thread_t* threads;      // Linked list of threads in this process
    
    struct process* next;   // Next process in the system
} process_t;
```

We give every process a **PID** (process ID), a number that identifies it, handed out in order starting at 1. Then come the three things from the overview: its VMM, its TTY and its threads. All processes sit in one linked list.

### Making a Process

The first thing we do in `process_create` is give the new process an address space of its own:

```c
proc->vmm = vmm_create(USER_CODE_VIRT_ADDR, 0x00007FFFFFFFF000);
```

That is a brand new VMM, managing the lower half of the address space. Like every VMM, its upper half is shared with the kernel. This is why the kernel keeps working no matter whose page tables are loaded.

A fresh address space is empty, though. The program has to be in it to run. We do not load programs from a disk yet (there is no disk driver to load them with). The user programs are compiled right into the kernel image, in sections of their own (`.user_text`, `.user_rodata`, `.user_data` and `.user_bss`). So in `process_create`, we map those sections into the new address space:

```c
uintptr_t t_phys = (uintptr_t)&USER_TEXT_LOAD_ADDR;
size_t tsz = align_up((uintptr_t)&USER_TEXT_END - (uintptr_t)&USER_TEXT_START, PAGE_SIZE);
if (tsz > 0) {
    void* out = NULL;
    vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)USER_CODE_VIRT_ADDR, tsz, VM_FLAG_USER | VM_FLAG_EXEC | VM_FLAG_FOREIGN, (void*)t_phys, &out);
    if (st != VMM_OK) goto map_fail;
}
```

We map the code straight from where it sits in the kernel image, with `VM_FLAG_FOREIGN`, so that destroying the process never hands those pages back to the PMM. Code and read-only data can be shared like that, since no process can change them. Every process maps the same physical pages.

Writable data is different. If two processes shared the same `.data` pages, one could change the other's global variables. Instead, `.data` gets fresh pages in every process, with the original contents copied in:

```c
vmm_status_t st = vmm_alloc_at(proc->vmm, (void*)d_virt, dsz, VM_FLAG_USER | VM_FLAG_WRITE, NULL, &out);
if (st != VMM_OK) goto map_fail;

for (uintptr_t off = 0; off < dsz; off += PAGE_SIZE) {
    uint64_t phys;
    if (vmm_get_physical(proc->vmm, (void*)(d_virt + off), &phys))
        kmemcpy((void*)PHYSMAP_P2V(phys), (void*)PHYSMAP_P2V(d_phys + off), PAGE_SIZE);
}
```

We give `.bss` fresh pages too, but skip the copy, since the VMM hands out zeroed memory and `.bss` is supposed to start out as zeros anyway.

Finally, we give the process a TTY. Most processes ask for a new one, with a three row sticky header showing their name and PID (the header from last chapter). A few kernel processes share the kernel's TTY instead.

### The First Processes

By the time the user sees anything, a handful of processes already exist:

| PID | Process | What it is |
|---|---|---|
| 1 | `idle_proc` | Holds the idle thread |
| 2 | `kproc` | Holds `kernel_main`, the code that has been running since boot |
| 3 | `dashboard` | The system monitor behind Ctrl+Shift+Esc |
| 4 and up | `demo`, `donut`, ... | The user programs |

PIDs 1 and 2 are special. We will see in a moment why `kernel_main` gets to be a thread at all.

## Threads

Here is the thread struct:

```c
typedef struct thread {
    tid_t tid;
    char name[MAX_THREAD_NAME];
    struct process* process;
    
    thread_state_t state;
    cpu_context_t context;  // Saved CPU state
    void* kstack;           // Base of the kernel stack
    void* ustack;           // Virtual address of the user stack
    uint64_t fs_base;       // Thread Local Storage base

    // FPU/SSE/AVX state
    uint8_t* fpu;
    uint8_t fpu_raw[FPU_AREA_SIZE + FPU_AREA_ALIGN - 16] __attribute__((aligned(16)));

    uint64_t wake_at;       // Timestamp for waking up
    avl_node_t sleep_node;  // AVL tree node for sleep queue

    struct thread* next;    // Next thread in the process (linked list)
    struct thread* rnext;   // Next thread in the scheduler's ready queue
} thread_t;
```

Most of these fields are the "everything about a thread that has to survive while it is not running" from the overview:

* `context` is a `cpu_context_t`, the same structure the interrupt stub builds on every interrupt. This holds every general purpose register, plus the instruction pointer, stack pointer and flags. This is where the thread's registers live while it waits.
* `kstack` is the thread's own **kernel stack**, 16 KiB of it. Whenever this thread enters the kernel (through an interrupt or a system call), the kernel runs on this stack.
* `ustack` is the thread's **user stack**, inside its process's address space. Kernel threads do not have one.
* `fs_base` is the thread's TLS pointer, set through `SYS_SET_FS_BASE`. Since it is per thread, it has to be saved and restored with the rest.
* `fpu` is where the thread's FPU and SSE registers go, in the area we sized and aligned for exactly this.
* `wake_at` and `sleep_node` are for sleeping. `rnext` links the thread into whichever queue it is waiting in.

### Why Every Thread Needs Its Own Kernel Stack

Picture two threads, A and B, that both make a system call that blocks (say, reading a key). A enters the kernel, starts the syscall and goes to sleep halfway through, with half a call stack of kernel functions still on its stack. Then B runs and makes the same system call.

If both used the same kernel stack, B's calls would land right on top of A's sleeping frames and destroy them. When A woke up, it would return into garbage. So we give every thread its own kernel stack, and every time we switch threads, we tell the CPU where the new thread's kernel stack is (more on that during the context switch).

This is also why the TSS started out with a single stack in `rsp0`. That stack was a placeholder until threads existed.

### Building a Thread by Hand

A new thread has never run, so it has no saved registers to resume from, and in `thread_create`, we have to make some up. The idea is to fill in `context` so that, the first time the scheduler "returns" into this thread, it lands at the start of its entry function, with the right arguments and a fresh stack.

For a kernel thread, that looks like this:

```c
thread->context.iret_flags = 0x202; // IF enabled
// ...
thread->context.iret_cs = KERNEL_CS;
thread->context.iret_ss = KERNEL_DS;
thread->context.iret_rip = (uint64_t)thread_wrap;
thread->context.iret_rsp = stack_top - 8;

thread->context.rdi = (uint64_t)entry;
thread->context.rsi = (uint64_t)arg;
```

**What is happening here:**
* `iret_flags` of `0x202` means interrupts are enabled (bit 9), plus bit 1, which is always set. A thread that started with interrupts off could never be interrupted. Nothing could ever switch away from it.
* The code and stack segments are the kernel's.
* The instruction pointer is `thread_wrap`, a tiny function that calls the real entry point and then `sched_exit`, in case it ever returns. Without it, a thread function that simply returns would return to whatever garbage sits above its stack.
* `rdi` and `rsi` are the first two arguments in the SysV calling convention. When `thread_wrap(entry, arg)` starts, it finds its arguments right where it expects them.

The stack pointer has one subtle detail. Why `stack_top - 8`, and not just the top of the stack?

The SysV ABI says that when a function starts, `rsp + 8` must be a multiple of 16. Normally, the `call` instruction makes that true, since it pushes an 8 byte return address onto a stack that was 16 byte aligned.

But nobody calls `thread_wrap`. We jump straight into it with `iretq`. So we have to fake the state a `call` would have left behind, with the stack sitting 8 bytes below a 16 byte boundary, as if a return address were there:

```
stack_top        ──► ┌──────────────────────────┐  16 byte aligned
                     │ (where a return address  │
                     │  would have been pushed) │
stack_top - 8    ──► ├──────────────────────────┤  rsp when thread_wrap starts
                     │                          │
                     │   thread_wrap's frames   │
                     │   grow down from here    │
                     ▼                          ▼
```

Getting this wrong does not crash right away. It crashes the first time the compiler uses an aligned SSE instruction on a stack variable, possibly a long way from the cause.

A user thread is built the same way, with three differences. Its segments are the user ones, which makes the `iretq` drop it to ring 3. Its instruction pointer is `userspace_start`, the ring 3 version of `thread_wrap`. It calls the entry point and then makes the `SYS_EXIT` system call. And its stack is a 64 KiB lazy allocation in its own address space:

```c
vmm_status_t alloc_status = vmm_alloc(process->vmm, USER_STACK_SIZE, VM_FLAG_USER | VM_FLAG_WRITE | VM_FLAG_LAZY, NULL, &thread->ustack);
```

`VM_FLAG_LAZY` means none of those 64 KiB exist yet. The first time the thread pushes something, it page faults, and the VMM's fault handler maps in a page on the spot. A thread that only ever uses a few hundred bytes of stack only ever costs one page.

### Turning kernel_main into a Thread

One thread does not get built by hand at all, and that is the code that is already running. When `sched_init` runs, we are deep inside `kernel_bootstrap`, called from `kernel_main`, still on the boot stack from Chapter 3. That code has to keep running after the scheduler starts. It has to become a thread like any other, one that can be switched away from and back.

We create a thread without a context with `thread_create_bootstrap`, mark it as `T_RUNNING` (which is the truth, since it is the code doing all of this), and make it the current thread:

```c
// Wrap current context into a thread so it can be preempted and resumed
cur = thread_create_bootstrap(kproc, "kernel_main");
if (!cur) panic("Failed to bootstrap kernel main thread!");

// Use the boot stack for kernel_main to keep the current execution stack valid
// The boot stack is 32 KiB, we treat the top 16 KiB as the kernel stack
extern char KERNEL_STACK_TOP;
cur->kstack = (void *)((uintptr_t)&KERNEL_STACK_TOP - KERNEL_STACK_SIZE);
```

Its `context` is empty, but that is fine. A running thread does not need saved registers. The first timer interrupt will save them, like it would for any other running thread. From then on, `kernel_main` is just thread 2.

## Thread States

At any moment, a thread is in one of five states:

```c
typedef enum {
    T_READY,
    T_RUNNING,
    T_BLOCKED,
    T_SLEEPING,
    T_DEAD
} thread_state_t;
```

And here is how a thread moves between them:

```
                     created
                        │
                        ▼
                  ┌──────────┐   scheduler picks it    ┌──────────┐
          ┌──────►│  READY   ├────────────────────────►│ RUNNING  │
          │       └──────────┘◄────────────────────────┤          │
          │                      quantum is over       └─┬──┬──┬──┘
          │                                              │  │  │
          │   woken up          waits for something      │  │  │
          ├──────────────────┐  ┌──────────┐             │  │  │
          │ (a key, an event)└──┤ BLOCKED  │◄────────────┘  │  │
          │                     └──────────┘                │  │
          │   time is up        ┌──────────┐  sched_sleep   │  │
          └─────────────────────┤ SLEEPING │◄───────────────┘  │
                                └──────────┘                   │
                                ┌──────────┐  exits, crashes   │
                                │   DEAD   │◄──────────────────┘
                                └──────────┘
```

Only one thread is `RUNNING` at a time. `READY` threads wait in the scheduler's queue, `SLEEPING` threads wait in a tree sorted by wake-up time, and `BLOCKED` threads wait on whatever they are blocked on (a TTY's wait list, for example). `DEAD` threads wait to be cleaned up. We will meet every one of those queues in this chapter.

## The Context Switch

Now for the heart of it. Back in Chapter 6, the interrupt dispatcher got a signature that looked a little odd at the time:

```c
cpu_context_t* interrupt_dispatcher(cpu_context_t* context)
```

It takes the saved state of whatever was interrupted, and it *returns* a saved state for the stub to restore. Every handler so far has just returned the same pointer it was given. The scheduler is the first one that does not.

### What the Stub Does

Let's walk through one timer interrupt that switches from thread A to thread B, starting with the CPU itself.

When the interrupt arrives, the CPU first picks a stack. If A was running in ring 3, the CPU switches to the stack in the TSS's `rsp0` field. That is A's kernel stack (the scheduler made sure of it when A started running).

If A was already in ring 0, it stays on the current stack. Either way, it pushes the **iret frame**: A's stack segment, stack pointer, flags, code segment and instruction pointer. Then it jumps into our stub. Our stub then pushes the vector number, a dummy error code and all fifteen general purpose registers.

At this point, A's entire state sits on A's kernel stack, in exactly the layout of a `cpu_context_t`, 22 values in all. Then the stub does something new:

```asm
// Pass a pointer to the saved-context frame to the dispatcher.
mov rdi, rsp

// Switch to the per CPU scheduler stack before calling into C
// ...
.do_switch:
    mov rsp, rax
.no_sched_stack:
    and rsp, -16
    call interrupt_dispatcher

    // Restore the stack pointer
    mov rsp, rax
```

Before calling into C, it moves to a separate stack, the **scheduler stack**, which belongs to no thread at all. We will see why in a moment. The timer handler calls `sched_schedule`, and whatever pointer comes back in `rax` becomes the new stack pointer. The stub pops fifteen registers off it, skips the vector and error code, then runs `iretq` to pop the iret frame.

### What the Scheduler Does

In `sched_schedule`, we get a pointer to A's saved state, and the first thing we do is copy it into A's `thread_t`:

```c
if (cur) {
    // Copy the cpu_context_t from wherever ISR.S built it into the embedded field in the thread struct
    // From this point the kstack is no longer referenced by any live pointer, so thread_destroy may free it freely.
    cur->context = *ctx;
    cur->fs_base = read_msr(MSR_FS_BASE);
    // ...
}
```

After that, A's state lives in A's thread struct, safe and sound. We pick B, and return a pointer to B's saved context (also inside a thread struct, of course):

```c
return &cur->context;
```

Now the trick. The stub sets `rsp` to that pointer and pops registers off it. So for a few instructions, the "stack" the CPU pops from is actually the `context` field inside B's `thread_t`. The pops load B's registers, and then `iretq` loads B's instruction pointer, its flags and, crucially, its stack pointer. In one instruction, the CPU is back on B's own stack, at the exact instruction B was interrupted at, with every register exactly as B left it.

Here is the whole thing as one picture:

```
  thread A running
        │  timer interrupt
        ▼
  CPU pushes iret frame onto A's kernel stack
  stub pushes 15 registers there too      ── A's context, on A's stack
        │
        │  stub switches to the scheduler stack, calls C
        ▼
  sched_schedule:
     copy A's context into A's thread_t   ── A is now safe to leave
     put A at the back of the queue
     pick B from the front
     return &B->context
        │
        │  stub: rsp = &B->context
        ▼
  stub pops 15 registers from B's thread_t
  iretq pops rip, cs, rflags, rsp, ss    ── and we are on B's stack
        │
        ▼
  thread B running, where it stopped last time
```

### Why a Separate Stack?

So why does the stub bother switching to a scheduler stack? Because of what happens when a thread dies. A dying thread's last act is to enter the scheduler, which then frees its kernel stack.

If we were still running *on* that stack, we would be standing on memory we just gave away. The next allocation could overwrite its own return addresses. Running the scheduler on a stack no thread owns sidesteps the whole problem, and this is why we bother with a separate one at all. The copy into `thread_t` is what makes it safe. Once the context is copied out, nothing points into the old stack any more.

### The Rest of the Switch

Registers are not the only per-thread state. Before returning, we update three more things:

```c
// Only switch VMM if address space actually changed
if (old_proc != nxt->process) {
    if (nxt->process && nxt->process->vmm) {
        vmm_switch(nxt->process->vmm);
    } else {
        // Fallback to kernel VMM if next thread has no process or no VMM
        vmm_switch(NULL);
    }
}
// ...
if (cur->kstack) {
    uint64_t stack_top = (uint64_t)cur->kstack + KERNEL_STACK_SIZE;
    tss_set_rsp0(stack_top);

    // Update local CPU structure for syscall entries
    cpu_local.kernel_stack = stack_top;
}

write_msr(MSR_FS_BASE, cur->fs_base);
```

**What is happening here:**
* If B belongs to a different process, we load its page tables. Writing `cr3` throws away the TLB (the CPU's cache of address translations). That is expensive, and two threads of the same process do not need it.
* The TSS's `rsp0` and `cpu_local.kernel_stack` both point at B's kernel stack. The first is where the CPU switches on an interrupt from ring 3. The second is where the `syscall` stub switches. Without these two lines, B's next system call would land on A's kernel stack.
* B's TLS pointer goes back into `FS_BASE`.

That is the entire context switch. This is surprisingly little code for something that sounds so dramatic.

## The Scheduler

With switching taken care of, the scheduler's actual decision is the easy part. It keeps three collections of threads:

* The **ready queue**, a linked list of `READY` threads, in the order they will run
* The **sleep tree**, an AVL tree of `SLEEPING` threads, sorted by when they should wake up
* The **dead list**, a linked list of `DEAD` threads, waiting to be cleaned up

### Round Robin

Adding a thread to the ready queue puts it at the back:

```c
void sched_add(thread_t* thread) {
    if (!thread) return;

    if (thread->state == T_DEAD) {
        sched_add_dead(thread);
        return;
    }

    thread->state = T_READY;
    thread->rnext = NULL;

    if (!rq_head) {
        rq_head = thread;
        rq_tail = thread;
    } else {
        rq_tail->rnext = thread;
        rq_tail = thread;
    }
}
```

And picking the next thread takes it from the front. Every time `sched_schedule` runs, it puts the outgoing thread back into the right collection, according to its state:

```c
if (cur->state == T_RUNNING) {
    cur->state = T_READY;
    if (cur != idle) {
        sched_add(cur);
    }
} else if (cur->state == T_SLEEPING) {
    sched_add_sleep(cur);
} else if (cur->state == T_DEAD) {
    sched_add_dead(cur);
}
```

A thread that was still running had its quantum taken away. This thread goes to the back of the queue. A thread that asked to sleep goes into the sleep tree, while a thread that died goes onto the dead list. A thread that is `BLOCKED` goes nowhere, since whoever it is waiting on already has it on their own list.

Let's trace three threads, A, B and C, all ready to run, with A running first:

| Tick | Running | Ready queue | What happened |
|---|---|---|---|
| 0 | A | B, C | |
| 10 ms | B | C, A | A's quantum ended, A went to the back |
| 20 ms | C | A, B | |
| 25 ms | A | B | C called `sched_sleep(100)`, and went into the sleep tree |
| 35 ms | B | A | |
| 125 ms | ... | ..., C | C's time was up, so it rejoined at the back |

Everybody gets the same 10 ms turns. Nobody can hog the core. That is all "fair" means here.

### The Idle Thread

When the ready queue is empty, we run the idle thread:

```c
static void idle_thread_entry(void* arg) {
    (void)arg;
    while (1) {
        cpu_idle_on((void* volatile*)&rq_head);
        if (rq_head) sched_yield();
    }
}
```

`cpu_idle_on` is the idle helper from Chapter 9. It puts the core into its deepest allowed sleep while watching `rq_head`, the front of the ready queue. The core wakes up on any interrupt, or, on CPUs with `mwait`, the moment anything writes to `rq_head`.

If that means someone became ready, the idle thread yields at once. The idle thread is never put into the ready queue itself (notice the `cur != idle` check above), since running it is what happens when the queue is empty.

### Sleeping

In `sched_sleep`, we do not spin, and we do not wake up every 10 ms to check the clock either. We write down when the thread should wake up, mark it `SLEEPING` and give up the CPU:

```c
void sched_sleep(uint64_t ms) {
    if (!cur || !sched_on) return;

    bool iflag = intr_save();
    cur->state = T_SLEEPING;
    cur->wake_at = get_uptime_ms() + ms;

    sched_yield();
    intr_restore(iflag);
}
```

`sched_yield` is just `int $32` (the timer's vector, raised by software), which runs exactly the same path as a timer tick. A thread giving up the CPU early and a thread losing it at the end of its quantum look the same to the scheduler.

The sleeping thread then goes into the sleep tree, an AVL tree (the same one the VMM uses) sorted by `wake_at`. On every run, we look at the smallest entry and wake every thread whose time has come:

```c
avl_node_t* sn = avl_min(&sleep_tree);
while (sn) {
    thread_t* sleeper = AVL_ENTRY(sn, thread_t, sleep_node);
    if (sleeper->wake_at > now) break;
    avl_node_t* next_sn = avl_next(sn);
    avl_remove(&sleep_tree, sn);
    sched_add(sleeper);
    sn = next_sn;
}
```

Since the tree is sorted, the loop stops at the first thread that is not due yet. If a hundred threads are sleeping and none is due, that costs one comparison. Two threads with the same `wake_at` are told apart by their TID in the comparison function, since a tree cannot hold two equal keys.

>[!NOTE]
>`int $32` sends the dispatcher down the hardware interrupt path, which ends with an EOI to the LAPIC. A software interrupt never set a bit in the LAPIC's In-Service Register, leaving this EOI with nothing to clear. This is harmless, since a thread only ever yields when no real interrupt is in service. Still, it is the kind of thing worth knowing about before it surprises you.

### Tickless Timing

A plain timer fires every 10 ms, forever. That is wasteful when nothing is going on. An idle system would wake up 100 times a second, just to find out it still has nothing to do.

On CPUs with the TSC-deadline timer, the scheduler instead decides, every time it runs, when it next needs to be woken:

* If the next thread is a real one, the deadline is the end of its 10 ms quantum, or the next sleeper's wake-up time if that is sooner.
* If the next thread is the idle thread, there is no quantum to enforce. The deadline is the next sleeper's wake-up time. If nobody is sleeping at all, the timer is switched off completely. The core sleeps until some other interrupt (a key, a USB event) arrives.

That is called **tickless** operation. On an idle GatOS with the dashboard open, the timer fires once per second (for the dashboard's redraw), and that is it. With the dashboard closed, it does not fire at all.

### Measuring CPU Usage

The dashboard shows how busy the CPU is. To know that, we keep two counters in the scheduler, "total" and "idle". The dashboard samples them every second and shows `(total - idle) / total`.

They used to count *switches*. Every time the scheduler ran, `total` went up by one, and `idle` went up by one if the idle thread was picked. With a periodic 10 ms timer, switches happen at a steady rate. Counting them is then the same as measuring time.

Tickless operation broke that. On an idle system, here is what one second looks like:

1. The dashboard thread wakes up, redraws and goes back to sleep. That is one switch to the dashboard, counted as busy.
2. We pick the idle thread and arm the timer for one second later. The core sleeps. That is one switch to idle.
3. A second later, the cycle repeats.

That is one busy switch and one idle switch per second. The dashboard showed **50%** on a machine doing practically nothing. I only caught it when I ran GatOS under KVM with the host CPU, since plain QEMU has no TSC-deadline timer and falls back to the periodic tick, where the counting happens to work.

The counters now measure time instead. Every time the scheduler runs, we charge the nanoseconds since the last switch to whoever was running:

```c
uint64_t now_ns = get_uptime_ns();
if (last_switch_ns) {
    uint64_t ran = now_ns - last_switch_ns;
    ticks_total += ran;
    if (cur == idle) ticks_idle += ran;
}
last_switch_ns = now_ns;
```

The same idle machine now shows 0%. With the donut spinning, it shows 100%.

## Blocking and Waking

We have used blocking all over the place in the last few chapters: a TTY read waiting for a key, the xHCI driver waiting for its controller, a writer waiting for the TTY's output. Now that we know how the scheduler works, we can see that all of them follow the same recipe.

To block, a thread does three things, with interrupts off:

1. It sets its own state to `T_BLOCKED`.
2. It puts itself on the wait list of whatever it is waiting for.
3. It calls `sched_yield`.

The scheduler sees a `BLOCKED` thread and, as we saw above, does not put it anywhere. From here on, it is only reachable through that wait list. To wake it up, whoever has what it was waiting for (an interrupt handler, usually) takes it off the wait list and calls `sched_add`. That marks it `READY` and puts it at the back of the queue.

Interrupts have to be off for the first two steps, for the lost wakeup reason from last chapter. If the event arrived between the thread deciding to wait and the thread being on the list, the wakeup would find nobody to wake.

There is one more case worth thinking through. What if the wakeup arrives after the thread is on the wait list, but *before* it has actually called `sched_yield`? Here is the order of events:

1. The thread puts itself on the list, sets `T_BLOCKED` and turns interrupts back on.
2. The interrupt fires right away. The handler takes the thread off the list and calls `sched_add` on it. The thread is now `READY` and sitting in the ready queue, even though it is still running.
3. The thread calls `sched_yield`. The scheduler looks at it.

This works out, thanks to that `if` at the top of the scheduler. The thread's state is `READY` now, not `RUNNING`. We do not add it to the queue a second time (it is already there), and it runs again on its next turn. No wakeup lost, and no thread in the queue twice.

Calling `sched_add` from an interrupt handler is safe in general, since on a single core, an interrupt handler runs with interrupts off. Nothing can interrupt the handler halfway through changing the queue. The scheduler itself only runs from an interrupt too.

## Dying

A thread dies in one of three ways: its function returns (and `thread_wrap` or `userspace_start` cleans up after it), it makes the `SYS_EXIT` system call, or it crashes. The first two end up in `sched_exit`:

```c
void sched_exit(void) {
    if (!cur) return;

    bool iflag = intr_save();
    cur->state = T_DEAD;
    LOGF("[SCHED] Thread '%s' (TID: %u) exited.\n", cur->name, cur->tid);

    proc_hdr_update(cur->process);

    sched_yield();

    // the scheduler will never reschedule a T_DEAD thread
    intr_restore(iflag);
    __builtin_unreachable();
}
```

A thread cannot free itself. It is running on its own kernel stack, and freeing that stack would pull the floor out from under its own feet. Instead, it marks itself `DEAD` and yields. The scheduler, safely on its own stack, puts it on the dead list. On every run, we empty that list:

```c
while (dead_head) {
    thread_t* thread = dead_head;
    dead_head = thread->rnext;
    // ... unlink the thread from its process

    thread_destroy(thread);

    // We protect PID 1 (Idle) and PID 2 (Kernel Main)
    if (proc && proc->threads == NULL && proc->pid > 2) {
        // ... print "[Process X has terminated]" on its TTY
        process_destroy(proc);
    }
}
```

`thread_destroy` frees the thread's kernel stack, its user stack and the struct itself. When a process loses its last thread, the process goes too. `process_destroy` frees its address space. That is the **reaper**.

### Freeing the Floor Anyway

While writing this section, I asked myself a simple question. When the reaper destroys a process, whose address space is loaded in `cr3` at that moment?

Usually, the dying one's. The last thread of a process dies, and the scheduler starts running on its own stack. The first thing it does is clean up. It has not picked the next thread yet, which means it has not switched address spaces yet either. Here is what used to happen:

1. The process's last thread calls `sched_exit` and yields. `cr3` holds the process's PML4.
2. The reaper calls `process_destroy`, which in turn calls `vmm_destroy`. Every page table of the process is freed, PML4 included. `cr3` still points at that PML4.
3. The scheduler goes on reaping, picks the next thread and only then switches `cr3` to something valid.

Between steps 2 and 3, the CPU is running on page tables that belong to the PMM's free list. This did not crash, but only by luck. The upper half of a PML4 (the kernel's half) survives being freed, the PMM only scribbles on the first few bytes, and nothing allocated a page in that window. That is three separate pieces of luck.

The fix sits in `vmm_destroy` itself, where no caller can get it wrong:

```c
// Never free page tables the CPU is still using. When the scheduler reaps the last thread
// of a process, that process's address space is usually the one loaded in cr3
if (current_vmm == &vmm->public) vmm_switch(NULL);
```

A new test creates an address space, switches to it and destroys it, then checks that `cr3` ends up on the kernel's page tables.

### Lists Under the Reaper

There was a second problem in the same area. The reaper runs from the timer interrupt, and it changes two linked lists (a process's list of threads, and the list of all processes). But those lists are also changed by ordinary code, with interrupts on. `uapps` creates the user programs right after interrupts come on. `process_create` used to add a process like this:

```c
proc->next = proc_list;
proc_list = proc;
```

If the timer fires between those two lines, and the reaper happens to remove the process at the head of the list, here is what happens:

1. We set `proc->next` to the current head, process X.
2. The timer fires. The reaper removes X, sets `proc_list` to X's successor and frees X.
3. Back in `process_create`, we set `proc_list` to the new process, whose `next` still points at the freed X.

The list now leads into freed memory. The fix follows the same rule as everywhere else in this chapter. Anything the scheduler changes from an interrupt can only be changed with interrupts off. In `process_create`, `thread_link` and `process_destroy`, we now make every list change between an `intr_save` and an `intr_restore`.

## Faults in Userspace

The third way to die is to crash. Left alone, a user program dividing by zero would take the whole kernel down with a red screen, since the dispatcher panics on any exception without a handler. So the scheduler registers a hook for exceptions that come from ring 3:

```c
// A broken program only takes itself down, not the whole system
if ((context->iret_cs & 3) == 3 && user_fault) {
    cpu_context_t* next = user_fault(context);
    if (next) {
        check_frame(next, vec);
        return next;
    }
}
```

In `sched_fault`, we write a message to the program's own TTY, the way a Unix shell would:

```
[Thread 5 from demo (PID 4)] Segmentation Fault at 0x0 (RIP: 0x401234)
```

Then it marks the thread `DEAD` and calls the scheduler. The scheduler returns some other thread's context, and the dispatcher returns *that* instead of the crashed one. The broken thread never runs again, and the reaper cleans it up on the next pass.

>[!NOTE]
>Only the faulting thread dies, not its whole process. Linux kills the entire process, since a crash in one thread usually means shared memory is in a bad state for all of them. GatOS keeps it simpler for now. The process goes away once its last thread does.

Exceptions from ring 0 still panic, as they should. A kernel bug means the kernel's own state cannot be trusted. Carrying on would only make it worse.

## Lazy FPU

The last piece of per-thread state is the FPU, which Chapter 9 set up and then left hanging. Saving and restoring the FPU, SSE and AVX registers takes `fxsave` or `xsave`, moving up to 832 bytes each way. Doing that on every context switch is expensive. Most switches do not even need it, because most threads never touch a floating point register.

So we do it **lazily**, using a feature x86 has had since the 286, the **TS** (Task Switched) bit in `cr0`. While TS is set, the first FPU or SSE instruction raises a `#NM` exception ("device not available") instead of running. The idea is to do nothing at switch time and wait to see whether the new thread actually uses the FPU.

We set TS whenever we switch to a thread, and we remember which thread's state is currently sitting in the FPU registers (the `fpu_owner`). When a thread touches the FPU, it traps:

```c
static cpu_context_t* fpu_nm_handler(cpu_context_t* ctx) {
    __asm__ volatile("clts" ::: "memory");
    // ...
    if (fpu_owner != cur) {
        if (fpu_owner)
            fpu_save(fpu_owner->fpu);
        fpu_restore(cur->fpu);
        fpu_owner = cur;
    }
    return ctx;
}
```

In the handler, we clear TS, save the old owner's registers into its thread, load the current thread's and make it the owner. Then it returns to the very instruction that trapped. This time, it runs normally.

Let's trace two threads. A does math, while B only prints text.

1. A runs and uses the FPU. It traps once, its state gets loaded and A becomes the owner.
2. Switch to B. TS is set, but B never touches the FPU. Nothing happens. A's state just sits in the registers.
3. Switch back to A. A is still the owner. Its registers are exactly as it left them.
4. B and A can be switched back and forth a hundred times like this without a single FPU byte being saved or loaded.

Step 3 used to have a small inefficiency. The scheduler used to set TS on every switch, even when the next thread *was* the owner. That cost a pointless trap just to clear TS again. It now checks first:

```c
if (cur == fpu_owner) __asm__ volatile("clts" ::: "memory");
else set_cr0_ts();
```

>[!IMPORTANT]
>Lazy FPU only works because the kernel never touches the FPU itself. Every kernel file is compiled with `-mno-sse` and friends, except `klibc/stdio.c`, since it needs floating point to print `%f`. I checked the compiled kernel. That file only touches FPU registers while actually formatting a float, something the kernel never does. If kernel code used the FPU behind the scheduler's back, it would silently overwrite whichever thread owned it.

>[!NOTE]
>Linux and Windows moved away from lazy FPU switching in 2018, after a CPU bug called LazyFP (a cousin of Spectre) showed that a thread could speculatively read registers that still held another thread's FPU state. They now save and restore the FPU eagerly on every switch. For a hobby kernel, the lazy version is still the more interesting one to learn from.

## The Dashboard

The dashboard behind Ctrl+Shift+Esc is a kernel thread with its own hidden TTY. While it is on screen, it wakes up every second, collects stats from every subsystem we have built (CPU, PMM, heap, processes) and redraws its screen. While it is hidden, it blocks and costs nothing. Most of its thousand lines are a pretty layout of columns, colors and bars. There is not much to learn from them.

There is one thing worth mentioning, since it is the same lesson as the reaper. The dashboard used to walk the process list while drawing, with interrupts on. A process could be freed by the reaper right under its feet. We now copy what it needs into a small table with interrupts off, and draw from that copy instead.

## Putting It All Together

Here is the end of `kernel_bootstrap()`, plus what `kernel_main` does next:

```c
// Enable multitasking and userspace
process_init();
sched_init();
xhci_hotplug_init();
// ...

// Dashboard and final touches
dash_init();
// ...

// Let the good times roll
intr_on();
```

```c
// Enqueue userspace apps
uapps();
```

In `sched_init`, we create the idle thread, turn the running code into the `kernel_main` thread, and register the `#NM` handler and the userspace fault hook. From that moment, there is a current thread for the timer handler to hand to the scheduler. Interrupts are still off, though, until the very last line of bootstrap. Once `intr_on` runs, the first timer interrupt performs the first real context switch.

Back in `kernel_main`, `uapps` creates the demo and donut processes and hands their threads to `sched_add`. Then `kernel_main` itself goes on to its "Type anything you want" loop. It blocks on its TTY, and the scheduler hands the core to whoever is ready.

Each piece builds on the ones before it. The timer from Chapter 10 forces the switches, and the interrupt stub from Chapter 6, whose dispatcher could always return a different context, makes them possible. The VMM from Chapter 8 gives every process its own address space, lazily backs every user stack, and keeps sleeping threads in its AVL tree.

The TSS and `cpu_local` from Chapter 9 send every interrupt and system call to the right kernel stack, and its FPU area holds each thread's floating point state. Finally, the TTYs from last chapter give each process its own screen, and its threads a place to block.

We now have a kernel that runs many programs at once, each in its own address space, on its own screen. What we have not looked at yet is the programs themselves.

Everything that runs in ring 3 has been compiled into the kernel image, along with a little C library of its own. The next chapter goes to userspace. We look at how those programs are built and what they can do, then take the spinning donut apart.
