# Chapter 10: ACPI, the APIC and Timers

The previous chapter ended with the memory subsystem complete. The PMM, the slab, the VMM and the heap are all online, the GDT has a TSS with real IST stacks behind it, and `cpu_init` has told us everything the CPU is willing to share. By any reasonable measure, GatOS is a real kernel now.

What we do not yet have is:

- Any way to hear the outside world. Since Chapter 6, the old PIC has been masked off and interrupts have been disabled. The only thing that can interrupt the kernel is the kernel's own mistakes.
- Any sense of time. We cannot sleep for 100 milliseconds or tell how long the system has been up, and the scheduler we will build in Chapter 14 has nothing to wake it up.
- Answers to two promises Chapter 6 left hanging. It called `disable_pic()` without explaining a single line of it, then sent an EOI after every hardware interrupt with nothing more than "we will explain the significance of EOIs in the APIC chapter".

This is the APIC chapter, and we're gonna fix all of that. It happens in three steps, and each one needs the one before it:

1. **ACPI**, to find the hardware. Modern interrupt controllers and timers do not live at fixed addresses. To learn where everything is, we have to read the tables the firmware left behind.
2. **The APIC**, to route interrupts. One controller sits inside every CPU core and another sits out by the devices, with a firmware table telling us how the wires between them are actually connected.
3. **Timers**, to keep time. The PC has four different clocks, none of them perfect. We will play them against each other until we can trust the best ones.

All three are called from `kernel_bootstrap()`, right after the heap:

```c
acpi_init(mb);
apic_init();
timer_init();
```

## Part 1: ACPI, the Firmware's Map

### Why We Need a Map

Think back to Chapter 4, when we set up the serial port. We wrote to port `0x3F8` and that was it. We never had to look that address up anywhere, because COM1 has lived at `0x3F8` since the original IBM PC in 1981. The PIC is the same story at `0x20` and `0xA0`, and so is the old timer chip at `0x40` that we will meet in Part 3. Those addresses are burned into every PC ever made.

That approach worked for a handful of devices from the 80s, but it does not scale. The interrupt controllers in this chapter are memory-mapped, and where they sit in physical memory is entirely up to the motherboard. A server might have three I/O interrupt controllers where a laptop has one. A timer chip might be there, or it might not. The kernel cannot guess any of this, and probing random physical addresses to see what answers is a great way to hang the machine.

The firmware, however, knows all of it, because it is the one that set the machine up in the first place. So the PC industry agreed on a standard format for the firmware to write that knowledge down and leave it in memory for the operating system: **ACPI**, the Advanced Configuration and Power Interface.

>[!IMPORTANT]
>What is ACPI, really?
>
>ACPI is a standard for how firmware describes a machine to the operating system. Most of what we care about is a set of **tables** in memory, each one describing one part of the machine: the interrupt controllers, the timers, the power management registers, and so on.
>
>ACPI is much bigger than tables, though. It also defines a whole bytecode language, AML, that the firmware uses to describe things like "how to turn this machine off" or "what to do when the lid closes". Running AML requires an interpreter, which is a serious project of its own. GatOS does not have one, and for everything in this chapter, the plain tables are enough.

The tables form a small tree:

```
RSDP  "where to start"
 └── RSDT or XSDT  "the list of every other table"
      ├── APIC  (the MADT, interrupt controllers)
      ├── HPET  (a timer)
      ├── FACP  (the FADT, power management)
      └── ...
```

The plan is simple. We find the root, follow it to the list, and read the tables we care about from there. Let's break it down piece by piece.

### Finding the RSDP

The root of the tree is the **RSDP**, the Root System Description Pointer. On old BIOS machines, finding it was a bit of a treasure hunt: the kernel had to scan a couple of memory ranges below 1 MiB, 16 bytes at a time, looking for the 8 byte signature `"RSD PTR "` (yes, with the trailing space). On UEFI machines, the firmware hands it over directly.

We get to skip both. Back in Chapter 4 we parsed the Multiboot2 information GRUB left us, and GRUB is nice enough to include a tag that holds a copy of the RSDP. `multiboot_acpi_rsdp` simply looks for that tag, preferring the newer kind if both are present:

```c
multiboot_acpi_t* multiboot_acpi_rsdp(multiboot_parser_t* parser) {
    multiboot_tag_t* tag = find_tag(parser, MULTIBOOT_TAG_TYPE_ACPI_NEW);
    if (!tag) {
        tag = find_tag(parser, MULTIBOOT_TAG_TYPE_ACPI_OLD);
    }
    return (multiboot_acpi_t*)tag;
}
```

There are two kinds of tag because there are two versions of the RSDP. Here they are, from [`acpi.h`](/src/kernel/sys/acpi.h):

```c
/* RSDP 1.0 */
typedef struct {
    char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
} __attribute__ ((packed)) RSDPDescriptor;

/* RSDP 2.0+ */
typedef struct {
    char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t ExtendedChecksum;
    uint8_t Reserved[3];
} __attribute__ ((packed)) RSDP2Descriptor;
```

Version 2 is just version 1 with a few more fields glued onto the end. This is how ACPI grows everything. The first 20 bytes never change, and old software can still read the parts it understands.

`Signature` is the `"RSD PTR "` string from before. `OEMID` names whoever made the firmware (the boot banner's "manufacturer" comes from here). `Revision` tells us which version we are holding, where `0` means ACPI 1.0 and `2` or higher means the longer struct. `RsdtAddress` is the physical address of the table list. The two checksum fields we will get to in a moment.

The interesting newcomer is `XsdtAddress`, a 64-bit pointer to a *second* table list. Why ACPI needed a second one will make sense once we look at the lists themselves.

### Two Versions, One Shape

`acpi_find_rsdp` takes the tag and turns either version into the longer struct. This way, the rest of the code only ever deals with one shape:

```c
if (acpi_tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW) {
    RSDP2Descriptor* rsdp2 = (RSDP2Descriptor*)acpi_tag->rsdp;
    return acpi_validate_rsdp(rsdp2) ? rsdp2 : NULL;
} else if (acpi_tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD) {
    RSDPDescriptor* rsdp1 = (RSDPDescriptor*)acpi_tag->rsdp;
    static RSDP2Descriptor rsdp2;

    kmemcpy(rsdp2.Signature, rsdp1->Signature, 8);
    rsdp2.Checksum = rsdp1->Checksum;
    kmemcpy(rsdp2.OEMID, rsdp1->OEMID, 6);
    rsdp2.Revision = rsdp1->Revision;
    rsdp2.RsdtAddress = rsdp1->RsdtAddress;
    rsdp2.Length = sizeof(RSDP2Descriptor);
    rsdp2.XsdtAddress = 0;
    // ...
    return acpi_validate_rsdp((RSDP2Descriptor*)rsdp1) ? &rsdp2 : NULL;
}
```

A version 2 RSDP is returned as is. A version 1 RSDP is copied field by field into a version 2 struct, with `XsdtAddress` set to `0` to mean "there is no XSDT". That struct is `static` because we hand out a pointer to it. A local variable would be gone the moment this function returns. Notice also that the *original* is what gets validated, never the copy, since the copy's checksum would not match anything.

### Checksums

Every ACPI structure carries a checksum. The rule behind it is wonderfully simple: **add up every byte of the structure, and the total must be 0** (in 8 bits, wrapping around at 256).

The firmware makes this true by choosing the right value for the checksum byte. Let's say a tiny 4 byte "table" holds `0x10`, `0x20` and `0x30`, plus one checksum byte. The first three add up to `0x60`, and the firmware therefore sets the checksum to `0x100 - 0x60 = 0xA0`. Now the total is `0x100`, or `0x00` once it wraps around in 8 bits.

If any single bit anywhere in the table changes, the sum is off by that bit's value and no longer comes out to `0`. It is a weak check, since two errors can cancel each other out, but it reliably catches the usual suspects: a wrong pointer that lands on random memory, or a table that something scribbled over.

GatOS has one helper for it in [`acpi.c`](/src/kernel/sys/acpi.c):

```c
bool acpi_checksum_ok(const void* table, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += ((const uint8_t*)table)[i];
    return sum == 0;
}
```

Because `sum` is a `uint8_t`, the wrap-around at 256 comes for free. We never have to take a modulo ourselves.

The RSDP is the odd one out, because it has *two* checksums:

```c
bool acpi_validate_rsdp(RSDP2Descriptor* rsdp) {
    if (!rsdp) return false;

    // The first 20 bytes have their own checksum, and 2.0 adds one over the whole structure
    if (!acpi_checksum_ok(rsdp, sizeof(RSDPDescriptor))) return false;
    return rsdp->Revision < 2 || acpi_checksum_ok(rsdp, rsdp->Length);
}
```

`Checksum` covers only the original 20 bytes. That way, ACPI 1.0 software can still validate a 2.0 RSDP without knowing the newer fields exist. `ExtendedChecksum` covers the whole structure. We only check it when the revision says there is one.

>[!NOTE]
>Fun fact! Until this chapter, the RSDP was the *only* ACPI structure GatOS checksummed. The root table and every table found through it were trusted blindly. The fix is two more calls to the same helper: `acpi_init` panics if the root table does not add up, and `acpi_find_table` skips any table that does not, as if it was never there. A broken MADT is worse than a missing one, because a missing one at least fails loudly.

### The Table Header

Every table past the RSDP starts with the same 36 byte header:

```c
typedef struct {
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;
    char OEMID[6];
    char OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__ ((packed)) ACPISDTHeader;
```

Only three of these fields matter to us. `Signature` is four characters saying what the table is, like `"APIC"`, `"HPET"` or `"FACP"`. `Length` is the size of the whole table in bytes, header included. `Checksum` is the byte that makes everything add up to `0`, as above.

This uniform header is what makes the tree walkable at all. We can look at any table without knowing what kind it is, and still learn its name and its size.

### The RSDT and the XSDT

The RSDP points at the list of every other table. That list also comes in two flavors:

```c
typedef struct {
    ACPISDTHeader sdt_header; // signature "RSDT"
    uint32_t sdt_addresses[];
} __attribute__ ((packed)) RSDT;

typedef struct {
    ACPISDTHeader sdt_header; // signature "XSDT"
    uint64_t sdt_addresses[];
} __attribute__ ((packed)) XSDT;
```

Both are the standard header followed by an array of physical addresses, one per table. The difference is the width of those addresses. The **RSDT** uses 32-bit addresses, which means every table has to live below 4 GiB. Back in 1996, nobody minded. The **XSDT** arrived with ACPI 2.0 and uses 64-bit addresses, letting tables live anywhere in physical memory. This is the second list the RSDP's `XsdtAddress` points at. It is also the reason version 2 of the RSDP had to exist.

`acpi_init` prefers the XSDT whenever the firmware provides one:

```c
if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
    xsdt_ok = true;
    rsdt_phys = rsdp->XsdtAddress;
} else {
    xsdt_ok = false;
    rsdt_phys = (uint64_t)rsdp->RsdtAddress;
}
```

You might have noticed that neither struct has a "number of entries" field. We do not need one, because the count falls out of `Length`. We take the header off and divide what is left by the size of one entry:

```c
entries_count = (root_header->Length - sizeof(ACPISDTHeader)) / sizeof(uint32_t); // RSDT
entries_count = (root_header->Length - sizeof(ACPISDTHeader)) / sizeof(uint64_t); // XSDT
```

An RSDT with a `Length` of 52, for example, holds `(52 - 36) / 4 = 4` tables.

>[!NOTE]
>The `sdt_addresses[]` member is a C feature called a *flexible array member*: an array with no size, at the very end of a struct. It takes up no space in `sizeof`, and indexing it simply reads the memory right after the header. It is a perfect fit for these tables. Each one is a fixed header followed by however many entries `Length` says.

### Reaching a Table

The RSDP hands us *physical* addresses, which the CPU cannot dereference. The PMM had the very same problem back in Chapter 7, and most of the time the answer is the same too:

```c
void* acpi_map_phys(uint64_t phys_addr, size_t size) {
    if (phys_addr == 0) return NULL;
    if (phys_addr + size <= kmem->physmap_end) return (void*)PHYSMAP_P2V(phys_addr);

    uint64_t page_offset = phys_addr & (PAGE_SIZE - 1);
    void* virt_addr = NULL;
    vmm_status_t status = vmm_alloc(NULL, align_up(size + page_offset, PAGE_SIZE), VM_FLAG_FOREIGN,
                                    (void*)(phys_addr - page_offset), &virt_addr);
    if (status != VMM_OK) {
        LOGF("[ACPI ERROR] Failed to map physical address 0x%lx (Status: %d)\n", phys_addr, status);
        return NULL;
    }

    return (void*)((uintptr_t)virt_addr + page_offset);
}
```

ACPI tables are firmware data sitting in RAM, usually near the very top of it. If the whole table falls inside the physmap from Chapter 5, it already has a virtual address and there is nothing for us to do but return `PHYSMAP_P2V(phys_addr)`. On QEMU, this is always the case, as the boot log shows:

```
[ACPI] Root SDT mapped at 0xFFFF800007FE236A (Phys: 0x7fe236a)
```

That virtual address is just `PHYSMAP_VIRTUAL_BASE + 0x7fe236a`.

Some machines put their tables above the end of usable RAM, though, and for those we need the VMM. This is the `ACPI tables above RAM` row in Chapter 8's "who asks for what" table: `VM_FLAG_FOREIGN` and nothing else. The mapping is read-only and cacheable, and because the memory is foreign, the VMM will never hand those pages to the PMM when we unmap them. They were never the PMM's to begin with.

The page offset math is worth slowing down for. The VMM only maps whole pages, while tables do not start on page boundaries. We therefore map the page the table *starts* in, and add the offset back on top. Let's trace a 512 byte (`0x200`) table at physical `0x100000F80`:

1. `page_offset = 0x100000F80 & 0xFFF = 0xF80`. The table starts `0xF80` bytes into its page.
2. We map from `0x100000F80 - 0xF80 = 0x100000000`, the start of that page.
3. The size is `align_up(0x200 + 0xF80, 0x1000) = align_up(0x1180, 0x1000) = 0x2000`, which is two pages.
4. We return the start of the mapping plus `0xF80`. The table begins there.

>[!TIP]
>Why `size + page_offset`, and not just `size`? Try it with just `size`. `align_up(0x200, 0x1000)` is a single page, but the table starts `0xF80` bytes into that page. Only its first `0x80` bytes fit. The remaining `0x180` bytes spill over into the next page (which we never mapped), and the first read past byte `0x80` page faults. Adding the offset accounts for the part of the first page that comes *before* the table.

`acpi_unmap_phys` is the reverse. It has to know which kind of address it was given:

```c
void acpi_unmap_phys(void* virt) {
    uintptr_t v = (uintptr_t)virt;
    if (!v || (v >= PHYSMAP_VIRTUAL_BASE && v < PHYSMAP_P2V(kmem->physmap_end))) return;
    vmm_free(NULL, (void*)align_down(v, PAGE_SIZE));
}
```

A physmap address was never mapped by us, leaving nothing to undo. Anything else came from `vmm_alloc`, and goes back through `vmm_free` from the start of its page.

### Bringing ACPI Online

There is a small chicken-and-egg problem in mapping a table. To map it, we need its size. Its size lives in its header. And to read the header, the table has to be mapped.

The way out is to map it twice. We first map just the header, which is always 36 bytes, and read `Length` from it. Then we unmap that and map the whole table:

```c
ACPISDTHeader* header = (ACPISDTHeader*)acpi_map_phys(rsdt_phys, sizeof(ACPISDTHeader));
// ...
uint32_t total_length = header->Length;
acpi_unmap_phys(header);

rsdt_virt = acpi_map_phys(rsdt_phys, total_length);
// ...
if (!acpi_checksum_ok(rsdt_virt, total_length)) {
    panic("Root SDT checksum mismatch.");
}
```

And that is all `acpi_init` does. It finds and validates the RSDP, picks the RSDT or the XSDT, maps it, and checks its checksum. The root table then stays mapped for good in `rsdt_virt`, since every table lookup from now on starts there.

### Finding a Table by Name

With the root table in hand, `acpi_find_table` walks its entries, peeks at each table's header, and returns the first one whose signature matches:

```c
for (size_t i = 0; i < entries_count; i++) {
    uint64_t table_phys = 0;

    if (xsdt_ok) {
        table_phys = ((XSDT*)rsdt_virt)->sdt_addresses[i];
    } else {
        table_phys = ((RSDT*)rsdt_virt)->sdt_addresses[i];
    }

    ACPISDTHeader* header_virt = (ACPISDTHeader*)acpi_map_phys(table_phys, sizeof(ACPISDTHeader));
    if (!header_virt) continue;

    bool match = (kstrncmp(header_virt->Signature, signature, 4) == 0);
    uint32_t length = header_virt->Length;
    acpi_unmap_phys(header_virt);

    if (match) {
        void* table = acpi_map_phys(table_phys, length);
        if (table && !acpi_checksum_ok(table, length)) {
            LOGF("[ACPI] %.4s checksum mismatch, ignoring it\n", signature);
            acpi_unmap_phys(table);
            continue;
        }
        return table;
    }
}
```

Every candidate gets the same two-step treatment as the root table. We map its header, compare four characters, and unmap it again. Only the match gets mapped in full, checksummed, and returned. The caller owns that mapping from then on, and should give it back with `acpi_unmap_phys` once it is done.

Pay attention to the `kstrncmp` with a length of 4. Signatures are *not* null terminated. `"APIC"` is followed directly by the `Length` field. A plain `kstrcmp` would happily keep reading into it.

>[!NOTE]
>Watch out for the names, they are a classic ACPI trap. The table everyone calls the **MADT** has the signature `"APIC"`, while the one everyone calls the **FADT** has the signature `"FACP"`. GatOS uses the FADT for shutting the machine down and rebooting, in `power.c`. We will get to it in a later chapter.

With that, we can ask the firmware about anything it wrote down. Time to ask it about interrupts.

## Part 2: The APIC

### Revisiting `disable_pic`

Chapter 6 told you the PIC is legacy hardware that GatOS turns off. It showed you `disable_pic()`, said it "uses functions we have explained", and moved on. Look at it again, though, because it does something odd for a function whose job is to *disable* a chip:

```c
void disable_pic(void) {
    // reinit the PIC to a known state before masking
    outb(PIC_COMMAND_MASTER, ICW_1);
    io_wait();
    outb(PIC_COMMAND_SLAVE, ICW_1);
    io_wait();

    // remap IRQs above the exception range (0-31)
    outb(PIC_DATA_MASTER, ICW_2_M);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_2_S);
    io_wait();

    outb(PIC_DATA_MASTER, ICW_3_M);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_3_S);
    io_wait();

    outb(PIC_DATA_MASTER, ICW_4);
    io_wait();
    outb(PIC_DATA_SLAVE, ICW_4);
    io_wait();

    outb(PIC_DATA_MASTER, 0xFF);
    outb(PIC_DATA_SLAVE, 0xFF);
}
```

Only the last two lines actually disable anything. Everything above them is a full *setup* of the chip. To understand why we bother configuring a PIC we are about to switch off, we need to know a bit more about how it works.

The PC has two 8259 PICs, a **master** and a **slave**, with 8 interrupt lines each. The slave's output is plugged into the master's line 2. This means the CPU only ever talks to the master. Setting them up is a fixed sequence of four "Initialization Command Words" (ICWs), sent to each chip in order:

* **ICW1** (`0x11`, sent to the command port) means "start initializing, and expect ICW4 at the end".
* **ICW2** (sent to the data port) sets which *vector* this chip's line 0 becomes. GatOS picks `0x20` (32) for the master and `0x28` (40) for the slave.
* **ICW3** describes how the two chips are wired together. The master gets a bitmask of which of its lines has a slave on it, `0x04` (bit 2). The slave gets its own line number on the master, `2`.
* **ICW4** (`0x01`) selects "8086 mode", the only mode anyone has used in decades.

ICW2 is the one that matters. When the BIOS hands over control, the master PIC's lines 0 to 7 are mapped to vectors 8 to 15. That was perfectly fine for the original 8086, but Intel later claimed vectors 0 to 31 for CPU exceptions. With the BIOS settings, line 0, the timer, raises vector 8, and vector 8 is the **double fault**. Every single timer tick would look like the CPU crashing.

Oh no!

Masking is the real off switch. Remapping first is insurance. The PIC is a chip from 1976 sitting behind firmware we did not write. If anything ever does slip out of it (a *spurious* interrupt, say, which we will meet shortly), we want it to land on vector 39, safely out of the exception range, where the CPU will not mistake it for a crash. So we move the PIC out of harm's way first, and only then switch it off.

`io_wait` from Chapter 4 is a write to port `0x80`, an unused port. It exists purely to waste a tiny bit of time. On real old hardware, the 8259 needed a moment to settle between commands.

### Why the PIC Had to Go

Chapter 6 listed the PIC's limitations. The most fundamental one is that there is exactly one of it for the whole machine: two chips, 15 usable lines, wired to one CPU. Imagine a machine with 16 cores. Which core should get the keyboard interrupt? The PIC's answer is always "the one I'm wired to". There is also no way at all for one core to interrupt another, something every multicore kernel needs.

Intel's answer was the **APIC**, the Advanced Programmable Interrupt Controller. Its key idea is to split the job in two:

```
 keyboard   timer chip   disk  ...
     │          │          │
     ▼          ▼          ▼
 ┌────────────────────────────┐
 │          I/O APIC          │  routes each device's line to a CPU
 └─────────────┬──────────────┘
               │  interrupt messages
      ┌────────┴─────────┐
      ▼                  ▼
 ┌──────────┐      ┌──────────┐
 │  LAPIC   │      │  LAPIC   │  one per core, decides when the core gets it
 │  core 0  │      │  core 1  │
 └──────────┘      └──────────┘
```

The **I/O APIC** sits out by the devices. Every device's interrupt line ends up on one of its pins, and for every pin, it keeps an entry saying which vector to raise, on which core.

The **Local APIC** (LAPIC) sits inside every CPU core. It receives the I/O APIC's messages, decides when its core is ready to take one, and has a few interrupt sources of its own, including a timer we will lean on heavily in Part 3.

GatOS runs on a single core, meaning only one LAPIC matters to us. The design is the same either way, though. We are about to set up both halves.

### The Local APIC

#### Checking for It

`lapic_init` in [`apic.c`](/src/kernel/sys/apic.c) starts by making sure there is a LAPIC at all:

```c
uint32_t a, b, c, d;
cpuid(1, 0, &a, &b, &c, &d);
if (!(d & (1 << 9))) {
    panic("System does not support APIC!");
}
```

This is CPUID leaf 1, `EDX` bit 9, the same leaf we read features from in Chapter 9. Every x86-64 CPU has a LAPIC. This is mostly a sanity check.

#### Finding Its Registers

The LAPIC is configured through **memory-mapped registers**. Just like the framebuffer back in Chapter 5, it is a range of physical addresses that does not lead to RAM at all. Reads and writes to that 4 KiB page go straight to the LAPIC. Its location is stored in an MSR, `IA32_APIC_BASE` (`0x1B`):

| Bits | Meaning |
| --- | --- |
| 8 | This core is the bootstrap processor (the one that started first) |
| 11 | The LAPIC is enabled |
| 12 to 51 | Physical address of the register page |

Here, we read the MSR, make sure the enable bit is set, and pull the address out of it:

```c
uint64_t apic_msr = read_msr(MSR_IA32_APIC_BASE);
if (!(apic_msr & MSR_APIC_BASE_ENABLE)) {
    LOGF("[APIC] LAPIC hardware disabled in MSR. Enabling...\n");
    apic_msr |= MSR_APIC_BASE_ENABLE;
    write_msr(MSR_IA32_APIC_BASE, apic_msr);
}

uint64_t phys_base = apic_msr & MSR_APIC_BASE_ADDR;
```

`MSR_APIC_BASE_ADDR` is `0x000FFFFFFFFFF000`, keeping bits 12 to 51. On practically every machine the base is `0xFEE00000`, but nothing forces the firmware to leave it there.

>[!NOTE]
>Fun fact! This mask used to be `0xFFFFF000`, which quietly throws away everything above bit 31. On a machine that moved its LAPIC above 4 GiB, GatOS would have mapped the wrong page and talked to whatever happened to live at the truncated address. Since the LAPIC sits at `0xFEE00000` on everything I have ever seen, QEMU included, it never mattered. It is one of those bugs you only find by asking "why that number?" while writing it down.

#### Mapping the Registers

With the physical address in hand, we map the page using the device flags from Chapter 8's table:

```c
if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_FOREIGN | VM_FLAG_DEVICE, (void*)phys_base, &virt_addr) != VMM_OK)
    panic("Failed to map LAPIC memory.");
lapic_base = (uint64_t)virt_addr;
```

This is where `VM_FLAG_DEVICE` finally pays off. It makes the page **uncacheable**, and for device registers that is mandatory. Consider the LAPIC timer's "current count" register. It counts down all on its own. With caching, our first read would fetch it into the CPU's cache, and every later read would get the same stale value straight back from the cache. The register would be ticking away and we would never see it move. Writes are even worse. A cached write might sit in the cache for a while before it reaches the device, and to a device, *when* a write happens is often the whole point.

#### Reading and Writing

Every LAPIC register is 32 bits wide, at a fixed offset into that page:

```c
void lapic_write(uint32_t reg, uint32_t value) {
    if (lapic_base == 0) return;
    *(volatile uint32_t*)(lapic_base + reg) = value;
}

uint32_t lapic_read(uint32_t reg) {
    if (lapic_base == 0) return 0;
    return *(volatile uint32_t*)(lapic_base + reg);
}
```

The uncacheable mapping takes care of the CPU. The `volatile` takes care of the *compiler*, for the reason Chapter 6 gave when we built spinlocks. Without it, GCC sees an ordinary memory access and assumes memory does not change behind its back. If we read the same register twice in a loop, it is allowed to read it once and reuse the value. If we write the same register twice, it is allowed to drop the first write. `volatile` tells the compiler that every single access here has a side effect. It must do exactly what we wrote. Keep this in mind, because it comes back in Part 3 as a bug.

#### Switching It On

The LAPIC is switched on in a slightly surprising place:

```c
lapic_write(LAPIC_SPURIOUS, LAPIC_SW_ENABLE | LAPIC_SPURIOUS_IV);
lapic_write(LAPIC_TPR, 0);
```

The **Spurious Interrupt Vector Register** holds the LAPIC's real on switch, bit 8 (`LAPIC_SW_ENABLE`). The MSR's enable bit only says "this LAPIC exists", while bit 8 here says "and it is accepting interrupts". The same register also holds a vector number (`0xFF` in our case) for spurious interrupts.

Chapter 6 told you spurious interrupts are harmless noise that the dispatcher ignores. Now we can say where they come from. Interrupt delivery is not instant. A device raises its line, the LAPIC signals the core, and the core begins the process of accepting the interrupt. If the device drops its line in the middle of that, by the time the core asks "which vector is this?", there is no interrupt left to name. The LAPIC cannot take its signal back. Instead, it answers with the spurious vector, its way of saying "sorry, false alarm".

The choice of `0xFF` is not arbitrary either. On some older CPUs, the bottom 4 bits of this vector are hardwired to `1`. Whatever you write, you get `xxxx1111` back. `0xFF` is the one choice that comes out the same everywhere.

That leaves the **Task Priority Register** (TPR), set to `0`. To explain what that means, we first need to look at how the LAPIC decides *when* to deliver an interrupt.

### Priorities, and Why We Send an EOI

This is the promise Chapter 6 made: why the dispatcher has to send an **EOI** (End of Interrupt) after every hardware interrupt, and what happens if it does not.

The LAPIC groups the 256 vectors into 16 **priority classes**, using the top 4 bits of the vector. Vectors `0x20` to `0x2F` are class 2, vectors `0x30` to `0x3F` are class 3, and so on, all the way up to `0xF0` to `0xFF` in class 15. A higher class means a higher priority.

The LAPIC also keeps two 256-bit registers, with one bit per vector in each:

* **IRR** (Interrupt Request Register) marks vectors that have arrived and are waiting.
* **ISR** (In-Service Register) marks vectors that were delivered to the core and whose handlers have not finished yet.

When an interrupt arrives, its IRR bit gets set. The LAPIC delivers it once its class is higher than everything currently in service, and higher than the TPR. Delivery moves the bit from the IRR to the ISR, and from that point on, everything of the same class or lower has to wait.

Let's trace this with the scheduler's timer on vector 32 (class 2), and a disk interrupt on vector 35 (also class 2) that arrives while the timer handler is still running:

1. The timer fires. IRR bit 32 is set. Nothing is in service and class 2 is above the TPR of 0. It is delivered, and ISR bit 32 is set.
2. The disk interrupt arrives. IRR bit 35 is set. But ISR bit 32 is class 2, and 35 is also class 2, not higher. It waits.
3. The timer handler finishes, and the dispatcher writes `0` to the EOI register.
4. The EOI clears the highest set ISR bit, 32. Nothing is in service anymore.
5. The disk interrupt is delivered, and ISR bit 35 is set.

Now replay the same sequence without step 3. ISR bit 32 stays set *forever*. Every interrupt in class 2 or below is blocked for good. That covers the timer, the keyboard, and pretty much every device we will ever have. The system simply goes quiet, without a crash or an error message to tell you why. This is why Chapter 6's dispatcher sends an EOI even for interrupts nobody registered a handler for. The LAPIC delivered *something*. That something has to be acknowledged.

The TPR is a floor on top of all this, meaning "don't deliver anything at or below this class". GatOS sets it to `0`, blocking nothing. A kernel could raise it temporarily to hold off unimportant interrupts while it does something urgent, without having to turn interrupts off entirely.

>[!TIP]
>Here is a question worth sitting with. Chapter 6's dispatcher returns *early* for the spurious vector, without sending an EOI, while every other hardware vector gets one. Why the exception?

The answer is that a spurious interrupt never set an ISR bit in the first place. It is the LAPIC saying "false alarm", not a real delivery. Keep in mind that an EOI does not carry a vector number: it always clears the *highest* bit that is set in the ISR. An EOI for a spurious interrupt would therefore clear somebody else's bit. If the spurious interrupt arrived while the timer handler was running, that EOI would mark the timer as finished while it is still running. A real interrupt could then barge in on top of it. That is why Intel's manual is very clear on this one: never send an EOI for a spurious interrupt.

### The Local Vector Table

Besides the interrupts that arrive from outside, the LAPIC has a few interrupt sources of its own that never go through the I/O APIC. Each of them gets a register in the **Local Vector Table** (LVT):

| Register | Source |
| --- | --- |
| `LAPIC_LVT_TIMER` (`0x320`) | The LAPIC's own timer (Part 3) |
| `LAPIC_LVT_THERMAL` (`0x330`) | The CPU is overheating |
| `LAPIC_LVT_PERF` (`0x340`) | A performance counter overflowed |
| `LAPIC_LVT_LINT0` (`0x350`) | Local interrupt pin 0 |
| `LAPIC_LVT_LINT1` (`0x360`) | Local interrupt pin 1 |
| `LAPIC_LVT_ERROR` (`0x370`) | The LAPIC itself detected an error |

Every LVT entry has roughly the same layout:

| Bits | Field |
| --- | --- |
| 0 to 7 | Vector |
| 8 to 10 | Delivery mode (`000` normal, `100` NMI, `111` ExtINT, ...) |
| 13 | Polarity, `1` means active low (LINT pins only) |
| 15 | Trigger mode, `1` means level (LINT pins only) |
| 16 | Mask |
| 17 to 18 | Timer mode (timer only) |

LINT0 and LINT1 are two physical pins on the CPU. What is wired to them depends on the motherboard, and as usual, the firmware tells us through ACPI. To read that, we need the most important table in this chapter.

### The MADT

The **MADT** (Multiple APIC Description Table, signature `"APIC"`) is ACPI's description of every interrupt controller in the machine. It begins with the usual header and two more fields:

```c
typedef struct {
    ACPISDTHeader header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed)) MADTHeader;
```

After that comes a list of variable-length **records**, back to back, until `Length` runs out. Each record starts with its type and its own length:

```c
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) MADTRecordHeader;
```

If this feels familiar, it should. It is the exact same idea as the Multiboot2 tags from Chapter 4. We do not need to understand a record in order to skip it: we read its `length`, jump ahead by that many bytes, and look at the next one.

```c
uint8_t* start = (uint8_t*)(madt + 1);
uint8_t* end = (uint8_t*)madt + madt->header.Length;

while (start < end) {
    MADTRecordHeader* header = (MADTRecordHeader*)start;
    // ... look at header->type ...
    start += header->length;
}
```

`madt + 1` is pointer arithmetic on a `MADTHeader*`. It points right past the header, at the first record. The record types GatOS cares about are:

| Type | Record | What it says |
| --- | --- | --- |
| 0 | Local APIC | "There's a core with this APIC ID" |
| 1 | I/O APIC | "There's an I/O APIC at this address, starting at this GSI" |
| 2 | Interrupt Source Override | "ISA IRQ `x` is actually wired to pin `y`" |
| 4 | Local APIC NMI | "This core's LINT pin `n` is wired to the NMI line" |
| 5 | Local APIC Address Override | "The LAPIC is at this 64-bit address" |

Type 5 is ACPI's own fix for the 32-bit problem we already saw with the RSDT, since `local_apic_address` in the header is only 32 bits wide. GatOS reads the base from the MSR instead (the MSR is always right) and ignores both.

### NMIs

`lapic_init` only cares about type 4. An **NMI** (Non-Maskable Interrupt) is vector 2, the one interrupt that `cli` cannot block. It is reserved for "drop everything" events, like hardware errors, memory parity failures, or a watchdog checking that the system has not hung. The firmware tells us which LINT pin the NMI line is wired to. Here, we program that pin's LVT entry to deliver an NMI:

```c
// NMI records name ACPI processor UIDs, not APIC IDs, so find ours in the LAPIC records first
uint32_t my_uid = 0xFFFFFFFF;
for (uint8_t* p = start; p < end; p += ((MADTRecordHeader*)p)->length) {
    MADT_LAPIC* rec = (MADT_LAPIC*)p;
    if (rec->header.type == MADT_TYPE_LAPIC && rec->apic_id == my_id) {
        my_uid = rec->acpi_processor_id;
        break;
    }
}

while (start < end) {
    MADTRecordHeader* header = (MADTRecordHeader*)start;
    if (header->type == MADT_TYPE_NMI) {
        MADT_NMI* nmi = (MADT_NMI*)header;
        // 0xFF targets all processors
        if (nmi->acpi_processor_id == 0xFF || nmi->acpi_processor_id == my_uid) {
            uint32_t lvt_reg = (nmi->lint == 0) ? LAPIC_LVT_LINT0 : LAPIC_LVT_LINT1;
            uint32_t lvt = LVT_DELIVERY_NMI;

            // Same polarity encoding as an ISO. NMIs are always edge triggered, so the
            // trigger bits don't matter
            if ((nmi->flags & 0x03) == 0x03) lvt |= LVT_ACTIVE_LOW;
            lapic_write(lvt_reg, lvt);
            LOGF("[APIC] Configured LINT%u as NMI\n", nmi->lint);
        }
    }
    start += header->length;
}
```

An NMI record can apply to a single core, or to all of them when its processor ID is `0xFF`. The first loop exists because of how a record names its core.

>[!CAUTION]
>A core has *two* different numbers, and they look identical. The **APIC ID** is the hardware's number for the core, the one the LAPIC's ID register gives you. The **ACPI processor UID** is the firmware's number for the same core. On small machines they are often the same. On big ones, they often are not. NMI records use the UID.
>
>The old version of this loop compared the record's UID straight against the LAPIC's APIC ID, under a comment that even said "match by local APIC ID". On QEMU, the only NMI record says `0xFF` (all processors). The comparison never ran, and nothing broke. On a machine with per-core records and mismatched numbering, a core would have programmed some *other* core's NMI pin, or none at all. The fix translates first: it finds the type 0 record whose APIC ID is ours, and uses the UID that record pairs it with.
>
>While I was in there, the record's polarity flags were being ignored as well. They use the same encoding as an Interrupt Source Override, and we will take that apart shortly.

On QEMU, this prints:

```
[APIC] Configured LINT1 as NMI
```

LINT1 as NMI is the classic PC wiring. LINT0 traditionally receives the old PIC's output, as a fallback path called "virtual wire mode". The firmware usually leaves it set up that way. GatOS does not touch it, and does not have to: the PIC on the other end of that wire is masked, and nothing ever comes out of it. Because of it, `disable_pic` still matters with an APIC in the machine. The PIC is still plugged into LINT0.

>[!NOTE]
>Modern CPUs also support **x2APIC**, a newer LAPIC mode where the registers are MSRs instead of a memory page. `lapic_init` notices it and logs it, and then cheerfully ignores it. In the words of the source: "we are NOT handling that lol, I got enough on my plate". The memory-mapped mode we use works on every x86-64 CPU, x2APIC ones included.

### The I/O APIC

With the LAPIC ready to receive, it is time to set up the other half. `ioapic_init` walks the MADT looking for a type 1 record:

```c
typedef struct {
    MADTRecordHeader header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) MADT_IOAPIC;
```

`io_apic_address` is the physical address of its registers. We map them with exactly the same device flags as the LAPIC. `global_system_interrupt_base` needs a little more explanation.

>[!IMPORTANT]
>What is a GSI?
>
>A **GSI** (Global System Interrupt) is ACPI's machine-wide numbering of every interrupt pin, across every I/O APIC. If a machine has two I/O APICs with 24 pins each, the first one covers GSIs 0 to 23 and the second one covers GSIs 24 to 47. The second I/O APIC's base is therefore 24, making its pin 3 GSI 27.
>
>GatOS only uses the first I/O APIC it finds. Every PC has at least one. The legacy devices are always on the first. Big servers with several would need more work here.

#### Two Registers for Everything

The I/O APIC is stingy with address space. Its whole register page contains exactly two registers:

* `IOREGSEL`, at offset `0x00`, selects which internal register we want to talk to.
* `IOWIN`, at offset `0x10`, is the window through which we read or write that register.

Every access is therefore two steps. We write the internal register's number into `IOREGSEL`, then read or write `IOWIN`:

```c
uint32_t ioapic_read(uint32_t reg) {
    if (ioapic_base == 0) return 0;
    *(volatile uint32_t*)(ioapic_base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t*)(ioapic_base + IOAPIC_IOWIN);
}
```

It is an index register and a data window, the same trick as the PIC's command and data ports, just in memory instead of I/O space. It also means the two steps must never be split up by someone else touching the I/O APIC in between. Right now, only boot code and driver setup ever touch it, with interrupts off. That cannot happen yet.

The first useful thing we read is the version register, internal register `0x01`:

```c
uint32_t ver = ioapic_read(IOAPIC_VER);
uint32_t count = ((ver >> 16) & 0xFF) + 1;
```

Bits 16 to 23 hold the index of the *last* redirection entry. The number of entries is one more than that. QEMU reports 23, for 24 entries.

#### Redirection Entries

Each pin has a 64-bit **redirection entry**, stored as two internal 32-bit registers starting at `0x10`. Entry `n` lives in registers `0x10 + 2n` (the low half) and `0x11 + 2n` (the high half):

```c
void ioapic_set_entry(uint8_t index, uint64_t data) {
    ioapic_write(IOAPIC_REDTBL + 2 * index, (uint32_t)(data & 0xFFFFFFFF));
    ioapic_write(IOAPIC_REDTBL + 2 * index + 1, (uint32_t)(data >> 32));
}
```

The low half looks a lot like an LVT entry. That is no coincidence, since it does the same job for an external pin:

| Bits | Field |
| --- | --- |
| 0 to 7 | Vector to raise |
| 8 to 10 | Delivery mode (`000`, fixed, a normal interrupt) |
| 11 | Destination mode (`0`, physical: the destination is an APIC ID) |
| 12 | Delivery status (read-only, "a message is on its way") |
| 13 | Polarity (`1` = active low) |
| 14 | Remote IRR (read-only, more on this soon) |
| 15 | Trigger mode (`1` = level) |
| 16 | Mask |
| 56 to 63 | Destination APIC ID |

#### Safe Defaults

`ioapic_init` begins by filling every entry with a safe default:

```c
uint32_t gsi_base = ioapic_rec->global_system_interrupt_base;
uint32_t bsp_id = lapic_get_id();

for (uint32_t i = 0; i < count; i++) {
    uint64_t entry = (1 << 16) | (32 + gsi_base + i);
    entry |= ((uint64_t)bsp_id << 56);
    ioapic_set_entry(i, entry);
}
```

Each entry is masked (bit 16), aimed at our own core, and raises vector `32 + GSI`. Pin 1 would raise vector 33, pin 4 would raise vector 36, and so on, right above the 32 exception vectors.

Nothing is unmasked here. That is deliberate. A pin only gets unmasked by the driver that owns it, once that driver's handler is registered. If we unmasked first and registered later, the very first interrupt would arrive with nowhere to go.

### Interrupt Source Overrides

This is where the PC's history catches up with us.

The PIC had 16 lines with fixed jobs. Line 0 was the timer, line 1 the keyboard, line 4 COM1, and so on. Those numbers are the **ISA IRQs**, and decades of software assume them. The I/O APIC was designed so that ISA IRQ `n` normally lands on pin `n`.

*Normally.* Some IRQs do not. The MADT tells us which ones, using **Interrupt Source Override** records (type 2):

```c
typedef struct {
    MADTRecordHeader header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) MADT_ISO;
```

Each record reads as "ISA IRQ `irq_source` is wired to GSI `global_system_interrupt`, with these flags". After filling in the defaults, `ioapic_init` walks these records and rewrites the matching pins:

```c
if (header->type == MADT_TYPE_ISO) {
    MADT_ISO* iso = (MADT_ISO*)header;
    if (iso->global_system_interrupt >= gsi_base &&
        iso->global_system_interrupt < gsi_base + count) {
        uint32_t io_index = iso->global_system_interrupt - gsi_base;
        LOGF("[APIC] ISO: IRQ %u -> GSI %u (Flags: 0x%X)\n",
             iso->irq_source, iso->global_system_interrupt, iso->flags);
        ioapic_redirect(io_index, 32 + iso->irq_source, bsp_id, iso->flags);
    }
}
```

Pay attention to the vector, `32 + irq_source` and not `32 + GSI`. The override keeps the *IRQ's* vector and moves it to a different *pin*. A driver that knows "the timer is IRQ 0" gets vector 32, no matter which pin the motherboard actually used.

This is what QEMU's MADT says:

```
[APIC] ISO: IRQ 0 -> GSI 2 (Flags: 0x0)
[APIC] ISO: IRQ 5 -> GSI 5 (Flags: 0xD)
[APIC] ISO: IRQ 9 -> GSI 9 (Flags: 0xD)
[APIC] ISO: IRQ 10 -> GSI 10 (Flags: 0xD)
[APIC] ISO: IRQ 11 -> GSI 11 (Flags: 0xD)
```

The first one is the famous one, and practically every PC has it. The timer is IRQ 0, but it is wired to pin **2**. In the original APIC wiring, pin 0 was reserved for the old PIC's output (the "virtual wire" path again). The timer had to go somewhere else. Pin 2 happened to be free, because on the PIC, line 2 was where the slave plugged into the master, and no device was ever wired there.

The other four keep their pin, and are only overridden for their flags. The ISO flags are two 2-bit fields:

| Bits | Field | `00` | `01` | `11` |
| --- | --- | --- | --- | --- |
| 0 to 1 | Polarity | bus default | active high | active low |
| 2 to 3 | Trigger | bus default | edge | level |

Let's decode `0xD`, or `1101` in binary. Bits 0 to 1 are `01`, meaning active high. Bits 2 to 3 are `11`, meaning level triggered. Those four lines are where QEMU's chipset routes PCI device interrupts, and PCI interrupts are level triggered, unlike the ISA default of edge triggered. The override is there to say so.

`ioapic_redirect` turns those flags into bits 13 and 15 of the redirection entry:

```c
uint8_t polarity = flags & 0x03;
uint8_t trigger = (flags >> 2) & 0x03;

if (polarity == 0x03) entry |= (1 << 13);
if (trigger == 0x03) entry |= (1 << 15);
```

Only `11` sets a bit. A value of `00`, "bus default", means active high and edge triggered for ISA. A cleared bit already says that.

### Edge and Level Triggering

We keep running into these two words. Let's pin them down.

An **edge triggered** line works like a doorbell. The device pulls the line up and lets go. The *change* itself is the interrupt. Once it has been rung, it has been rung. A **level triggered** line works more like holding a button down. The device pulls the line up and *keeps* it there until somebody deals with it, typically by reading a status register on the device.

Level triggering exists because devices share lines. Four PCI devices on one line can each hold it up, and as long as any of them is holding it, the interrupt stays active. With a doorbell, two devices ringing at nearly the same moment would merge into a single ring, and one of them would simply be forgotten.

Getting this wrong produces one of two classic failures:

* **A level line configured as edge.** The device holds the line up. The I/O APIC sees one rising edge and sends one interrupt. If the handler does not fully clear the device, or a second device on the same line also wants attention, the line just *stays* up. Without a new edge there is never a new interrupt, and the device goes silent.
* **An edge line configured as level.** A short pulse looks like a held line, and the I/O APIC sends the interrupt. Because the pin is level triggered, it then waits for an EOI before it looks at the line again. This waiting is what **Remote IRR** (bit 14) tracks, meaning "I've sent this one, and I'm waiting for the LAPIC to say it's done". It works out in the end, but it is fragile in ways that depend on timing.

This is also the *other* reason EOIs matter. For a level triggered pin, the LAPIC's EOI travels back to the I/O APIC and clears Remote IRR. Forget it, and that pin stays blocked even after the LAPIC side is fixed.

### Unmasking

Drivers switch their pin on with `ioapic_unmask`:

```c
void ioapic_unmask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPIC_REDTBL + 2 * irq);
    ioapic_write(IOAPIC_REDTBL + 2 * irq, low & ~(1 << 16));
}
```

We read the low half, clear bit 16, and write it back. Only the low half is touched, since that is where the mask lives. The keyboard will do this in Chapter 11, for pin 1.

>[!TIP]
>Look back at the timer's override with the default fill in mind. Pin 0 got the default entry, vector `32 + 0 = 32`. Pin 2 was rewritten by the override, to vector `32 + IRQ 0 = 32`. That is two pins with the same vector. Is that a problem?

It is not, as long as both of them stay masked (and they do). They also have to, for a reason that goes beyond the I/O APIC. The scheduler's tick (we will set it up in Part 3) comes from the **LAPIC timer**, and it raises vector 32 too. If the old timer chip's IRQ 0 were ever unmasked, its interrupts would arrive on the same vector as the scheduler's. The scheduler would suddenly be running on two clocks at once.

>[!NOTE]
>Fun fact! `timer_init` used to end with `ioapic_unmask(0)`, under a comment saying it unmasks "IRQ 0 (System Timer)". That single line holds three mistakes. The LAPIC timer never goes through the I/O APIC at all. There was nothing to unmask for it. `ioapic_unmask` takes a *pin*, and IRQ 0 sits on pin 2. And pin 0, the one it actually unmasked, is the old PIC's slot, carrying the same vector 32 as the scheduler. It only stayed harmless because the PIC was masked on its own end. The line is gone now. A test makes sure pin 0 stays masked.

### `apic_init`

All of Part 2 is wrapped up in a single call:

```c
void apic_init(void) {
    LOGF("[APIC] Beginning hardware interrupt controller setup...\n");

    // disable museum mode
    disable_pic();
    // enable lapic
    lapic_init();
    //enable ioapic for external interrupts
    ioapic_init();

    LOGF("[APIC] Interrupt subsystem is online.\n");
}
```

`disable_pic` runs here a second time, since `idt_init` in Chapter 6 already called it once. It is cheap. It also means the APIC setup does not depend on someone else having remembered.

The plumbing is now in place. Every I/O APIC pin is set up and masked, the LAPIC is accepting interrupts, and the CPU itself keeps interrupts off (`cli`) until the very end of `kernel_bootstrap()`. What we still do not have is anything that actually *fires*.

## Part 3: Timers

### What We Need From Time

A kernel needs time for three quite different things. The scheduler in Chapter 14 needs an **interrupt, later**, to take the CPU back from a program every few milliseconds whether the program likes it or not. The rest of the kernel needs a **clock it can read**, to answer questions like "how long has the system been up?" or "how long did that take?". And plenty of code needs to **wait**, where "sleep for 100 ms" has to mean 100 real milliseconds.

The PC has four different clocks, and none of them is good at all three:

| Clock | Can interrupt? | Can be read? | Frequency known? | Notes |
| --- | --- | --- | --- | --- |
| PIT | Yes | Yes, awkwardly | Yes, fixed | Ancient, slow to access |
| HPET | Yes | Yes, 64-bit counter | Yes, from its registers | Not always present |
| LAPIC timer | Yes, per core | Yes | **No** | Fast, private to its core |
| TSC | Through the LAPIC | Yes, one instruction | **No** | Fastest to read |

Look at the last two rows. The two best clocks, the ones built right into the CPU, are the two whose speed nobody tells us. The ones whose speed we *do* know are old, slow, or optional. The plan, then, is to measure the good clocks against the trustworthy ones. This is called **calibration**. It is the heart of this part. Before we get there, let's go through the four clocks one by one.

### The PIT

The **PIT** (Programmable Interval Timer, the Intel 8253/8254) is the oldest of the bunch. Its input frequency is fixed at **1,193,182 Hz**.

>[!NOTE]
>Fun fact! That weird number comes from the original IBM PC, which had a single crystal on the board, running at 14.31818 MHz. That is four times the frequency of the NTSC color TV signal, because the PC also had to be able to drive a TV. To save a part, everything else was derived from that one crystal by dividing it down, and dividing it by 12 gives 1.193182 MHz. Fifty years later, we are still counting time in units of an American color TV signal.

The PIT has three channels. We only use channel 0. Each channel counts down from a 16-bit value at that fixed rate. We program it through I/O ports, with `0x43` as the command port and `0x40` as channel 0's data port:

```c
void pit_set_oneshot(uint16_t ticks) {
    // Channel 0, Access Mode lobyte/hibyte, Mode 0 (Interrupt on Terminal Count), Binary
    outb(0x43, 0x30); 
    outb(0x40, (uint8_t)(ticks & 0xFF));
    outb(0x40, (uint8_t)((ticks >> 8) & 0xFF));
}
```

Here, the command byte `0x30` is `00 11 000 0` in binary. Bits 6 to 7 select channel 0, bits 4 to 5 say "low byte first, then high byte", bits 1 to 3 select mode 0 ("count down once, then signal"), and bit 0 selects plain binary counting. We then send the 16-bit count one byte at a time, because the port is only 8 bits wide.

That 16-bit counter is the PIT's big limitation. `0xFFFF` ticks at 1,193,182 Hz is about **54.9 ms**. That is the longest single interval the PIT can measure.

Reading the count back has a catch of its own. A naive read looks like this:

```c
// BROKEN
uint8_t low = inb(0x40);
uint8_t high = inb(0x40);
```

The count is 16 bits but the port is 8. It takes two reads, and the counter keeps ticking between them. Say the counter reads `0x0100` when we grab the low byte, `0x00`. Before we grab the high byte, it ticks down to `0x00FF`, making the high byte `0x00` too. We stitch together `0x0000`, a value the counter never actually had. That is off by 256 ticks, from one unlucky moment.

The fix is to tell the PIT to hold still first:

```c
static uint16_t pit_latch(void) {
    outb(0x43, 0x00);
    uint8_t low = inb(0x40);
    uint8_t high = inb(0x40);
    return (uint16_t)(((uint16_t)high << 8) | low);
}
```

The command `0x00` is a **latch** command for channel 0. It freezes a snapshot of the count. The next two reads return that snapshot, no matter how far the real counter has moved in the meantime.

### The HPET

The **HPET** (High Precision Event Timer) is the PIT's modern replacement. It has a 64-bit counter that only ever counts up, plus a few comparators that can raise interrupts. GatOS only uses the counter. It is memory-mapped, and ACPI tells us where it is with a table that is, conveniently, called `"HPET"`:

```c
struct HpetSdt {
    ACPISDTHeader header;
    uint32_t event_timer_block_id;
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t reserved0;
    uint64_t address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed))*hpet_table;
```

The only field we need is `address`, the physical address of the HPET's registers. `hpet_init` maps them with the same device flags as the APICs, and lays a struct over the mapping:

```c
typedef struct {
    uint32_t capabilities_low;
    uint32_t capabilities_high; // Bits 63:32 = Period in femtoseconds
    uint32_t reserved0;
    uint32_t reserved1;
    uint64_t configuration;
    uint64_t reserved2;
    uint64_t interrupt_status;
    uint64_t reserved3[25];     // Padding to reach 0xF0
    uint64_t main_counter;
    uint64_t reserved4;
} __attribute__((packed)) hpet_regs_t;
```

A struct over device registers has to match the hardware's layout byte for byte. The `reserved3[25]` padding is there purely to push `main_counter` to offset `0xF0`, where the hardware keeps it.

The most interesting field is the top half of the capabilities register. It holds the length of one tick in **femtoseconds**. A femtosecond is 10^-15 seconds, or a millionth of a nanosecond. QEMU reports:

```
[TIMER] HPET initialized. Period: 10000000 fs (100 MHz)
```

10,000,000 fs is 10 ns, meaning the counter ticks 100 million times per second. On real Intel boards, you will often see 69,841,279 fs instead (14.31818 MHz). That is the PC's TV crystal again, undivided this time.

Turning the HPET on takes two bits in its configuration register:

```c
hpet->configuration |= 0x01;
hpet->configuration &= ~0x02;
```

Bit 0 starts the counter. Bit 1, "legacy replacement", would make the HPET take over the PIT's and the real-time clock's interrupt lines. We want the PIT to keep working as a fallback and leave it off.

Before calibration is allowed to trust the HPET, `hpet_counter_advances` asks it three questions. First, is the counter 64 bits wide? That is capabilities bit 13. It matters because a 32-bit counter at 100 MHz wraps around every 43 seconds. Second, is the period sane? The spec says a tick can be at most 100 ns (100,000,000 fs). A `0` or anything larger means broken firmware. Third, does the counter actually *move*? It reads the counter, waits about 100,000 TSC ticks, and checks that the value changed. Some virtual machines advertise an HPET whose counter is frozen solid.

>[!CAUTION]
>The `volatile` lesson from the LAPIC registers applies here too, and GatOS got it wrong. The HPET pointer used to be a plain `hpet_regs_t*`. Look at what that means for the loop that checks the counter moves:
>
>```c
>uint64_t first = hpet_read_counter();
>uint64_t deadline = tsc_read() + 100000;
>while (tsc_read() < deadline) {
>    if (hpet_read_counter() != first) return true;
>}
>```
>
>`hpet_read_counter` is just `return hpet->main_counter`. To the compiler, that is an ordinary memory read. Nothing inside the loop writes to memory. It is therefore perfectly entitled to read `main_counter` *once*, before the loop, and compare that one stale value forever. The HPET would "never advance". We would fall back to the PIT for no reason at all. Every other polling loop in the file has the same problem, and `sleep_ms` before the scheduler starts could have spun forever.
>
>It worked anyway, because the compiler happened not to make that optimization. Nothing *made* it work. The pointer is now a `volatile hpet_regs_t*`. That turns "happened to work" into "has to work".

### The LAPIC Timer

The LAPIC has a timer of its own, the one we saw in the LVT. It counts down from an initial value at the CPU's bus frequency, divided by a configurable divisor, and three registers drive it:

* `LAPIC_TDCR` (divide configuration) sets the divisor. GatOS writes `0x03`, meaning "divide by 16". The encoding is famously strange: `0x0` is 2, `0x1` is 4, `0x2` is 8, `0x3` is 16, and then it jumps to `0x8` for 32. Just look it up every time.
* `LAPIC_TICR` (initial count) is where we write the starting value, and writing it starts the countdown.
* `LAPIC_TCCR` (current count) is read-only, and tells us where the countdown is right now.

When the count reaches zero, the LAPIC raises the vector stored in `LAPIC_LVT_TIMER`. The LVT's timer mode bits decide what happens after that. It can stop (one-shot), reload and go again (periodic), or use a third mode we will get to with the TSC.

The LAPIC timer is per-core, fast to access, and needs no I/O APIC pin. Unfortunately, nobody tells us its bus frequency.

### The TSC

The **TSC** (Time Stamp Counter) is a 64-bit counter inside the CPU itself. `rdtsc` reads it in a single instruction, making it the cheapest clock there is:

```c
uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
```

On old CPUs, the TSC counted actual clock cycles. It sped up and slowed down along with the CPU's frequency. That made it useless as a clock. Modern CPUs have an **invariant TSC** that ticks at a fixed rate no matter what the core is doing, and CPUID leaf `0x80000007` (`EDX` bit 8) says whether we have one. GatOS assumes the TSC is invariant, an assumption that holds for every x86-64 CPU from the last fifteen years or so, and for QEMU.

The TSC's frequency is sometimes reported through CPUID, but often it is not. It gets calibrated too.

### Calibration

#### The Core Idea

Calibration sounds fancy, but the idea is simple. We pick a **reference** clock whose frequency we know (the HPET or the PIT). We pick the **target** clocks we want to measure (the TSC and the LAPIC timer, both at once). We start everything, wait for a known amount of time on the reference, and count how far each target got in the meantime.

`calibrate_once` does one such measurement. Let's break it down piece by piece.

#### Setting Up the Targets

```c
lapic_write(LAPIC_TDCR, 0x03);       // Divisor 16
lapic_write(LAPIC_TICR, 0xFFFFFFFF); // Max count
uint64_t abort_at = tsc_read() + (uint64_t)CALIBRATE_MS * 100 * CALIBRATE_MIN_TSC;
bool complete = false;
```

Here, we set the LAPIC timer's divisor to 16 and start it counting down from `0xFFFFFFFF`, as far as it can possibly go. It cannot run out during the measurement. The TSC needs no setup, since it is always running.

`abort_at` is a safety deadline, 10 * 100 * 1,000,000 = 1 billion TSC ticks from now. If the reference clock is dead, the loop gives up at that point instead of spinning forever. Yes, we are using the TSC to time out the calibration *of the TSC*, but only as a very rough bound. Any real CPU's TSC runs at 100 MHz or more. A billion ticks is therefore at most 10 seconds. The deadline only has to stop the loop eventually, it does not have to be precise.

#### Waiting on the Reference

```c
if (use_hpet) {
    uint64_t target = ((uint64_t)CALIBRATE_MS * 1000000000000ULL) / hpet_period;
    uint64_t start = hpet_read_counter();

    lapic_start = lapic_read(LAPIC_TCCR);
    tsc_start = tsc_read();

    while (tsc_read() < abort_at) {
        if (hpet_read_counter() - start >= target) { complete = true; break; }
    }
}
```

`CALIBRATE_MS` is 10, for a 10 ms window. `target` is how many HPET ticks make up those 10 ms. A millisecond is 10^12 femtoseconds, and with QEMU's period, that is `10 * 10^12 / 10,000,000 = 1,000,000` HPET ticks.

We note where the HPET, the LAPIC timer and the TSC are, then spin until the HPET has moved a million ticks.

#### Reading the Results

```c
uint64_t lapic_end = lapic_read(LAPIC_TCCR);
uint64_t tsc_end = tsc_read();
```

The differences between start and end are how far each target got in 10 ms. The LAPIC timer counts down, giving a distance of `lapic_start - lapic_end`. The TSC counts up, giving `tsc_end - tsc_start`. A window shorter than `CALIBRATE_MIN_TSC` (1,000,000) TSC ticks is rejected as nonsense.

Let's put QEMU's numbers on it. The boot log says:

```
[TIMER] HPET: 3/3 samples, TSC 3700040 ticks/ms (3700 MHz), LAPIC 62501 ticks/ms
```

The TSC runs at about 3.7 GHz. The LAPIC timer did 62,501 ticks per millisecond with a divisor of 16. Its input clock therefore runs at `62,501 * 16 * 1000 = 1,000,016,000` Hz, or 1 GHz give or take 0.0016%. QEMU emulates the APIC timer at exactly 1 GHz, a nice confirmation that the whole thing works.

#### The PIT as a Fallback Reference

If the HPET is missing or broken, the PIT steps in as the reference:

```c
pit_set_oneshot(0xFFFF);
uint64_t settle = tsc_read() + 10000;
while (tsc_read() < settle) { }

uint16_t start_val = pit_latch();
uint16_t target = (uint16_t)((PIT_FREQUENCY / 1000) * CALIBRATE_MS);
// ... note the LAPIC and TSC ...
while (tsc_read() < abort_at) {
    uint16_t elapsed = (uint16_t)(start_val - pit_latch());
    if (elapsed >= target) { complete = true; break; }
}
```

The idea is the same. We start the PIT counting down from `0xFFFF`, give it a moment to load the count, and then wait for `1193 * 10 = 11,930` PIT ticks. That is well under the 65,535 the counter can hold, and it cannot run out halfway through. Since the PIT counts *down*, `start_val - pit_latch()` is how far it has gone.

#### Three Samples and a Median

`timer_calibrate_all` does not trust a single window. It runs three of them, and takes the **median** of each clock's results:

```c
static uint64_t median3(uint64_t x, uint64_t y, uint64_t z) {
    if (x > y) { uint64_t t = x; x = y; y = t; }
    if (y > z) { uint64_t t = y; y = z; z = t; }
    if (x > y) { uint64_t t = x; x = y; y = t; }
    return y;
}
```

Three compare-and-swaps sort the three values. The middle one is returned.

You might wonder why we take the median and not the average. The reason is what can go *wrong* with a single window. Something can steal time in the middle of it. On real hardware, that is the firmware's System Management Mode, which can freeze the whole machine for a while to do its own thing. In a virtual machine, it is the host deciding to run something else for a bit. Either way, the reference clock keeps going while our loop does not get to check it. We notice the 10 ms late, and by then the TSC has gone further than it should have.

Say three windows measure 37.00 million, 37.01 million, and, with one such hiccup, 52.0 million TSC ticks. The average is 42.0 million, 13% too fast. Every sleep in the system would come out 13% too short. The median is 37.01 million. As long as two of the three windows agree, one bad window cannot move the median at all.

#### Sanity Checks

The result goes through two more checks before anyone uses it:

```c
uint64_t hz = (dtsc / CALIBRATE_MS) * 1000ULL;
if (hz < TSC_HZ_MIN || hz > TSC_HZ_MAX) {
    LOGF("[TIMER] Calibrated TSC of %lu Hz is not a real frequency.\n", hz);
    panic("timer: TSC calibration produced an impossible frequency");
}
```

A TSC below 100 MHz or above 10 GHz means something went badly wrong. It is better to panic now than to run the whole system on a broken clock. Then, if the CPU reports its own TSC frequency (leaves `0x15` and `0x16` on Intel, a P-state MSR on AMD), `tsc_hz_declared` compares the two and warns if they differ by more than 10%. QEMU does not report one:

```
[TIMER] Hardware reports no TSC frequency; measurement stands alone.
```

The fallbacks chain all the way down. We use the HPET if it is there and alive, the PIT if the HPET produced nothing usable, and a panic if even the PIT fails. The results end up in `tsc_tpm` (TSC ticks per millisecond) and, through `lapic_set_tpm`, in the APIC code's own `ticks_per_ms`.

### Starting the Tick

With calibrated clocks, `timer_init` can finally put everything together:

```c
void timer_init(void) {
    boot_tsc = tsc_read();
    
    hpet_init();
    timer_calibrate_all();

    // The LAPIC timer interrupts its own core directly, so there's no I/O APIC pin to unmask
    irq_register(INT_FIRST_INTERRUPT, (irq_handler_t)timer_handler);

    // Enable TSC Deadline tickless mode if supported
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1u << 24)) {
        tsc_deadline_mode = true;
        LOGF("[TIMER] TSC-Deadline tickless mode enabled.\n");
    } else {
        lapic_timer_periodic(10000, INT_FIRST_INTERRUPT);
        LOGF("[TIMER] TSC-Deadline not available; using 10ms periodic timer.\n");
    }
}
```

`boot_tsc` is "time zero" for the uptime functions we will see shortly. The handler goes on vector 32, the first one after the exceptions. For now it does very little: it counts a tick and calls into the scheduler we will build in Chapter 14.

After that, there are two ways to keep the scheduler ticking.

#### Periodic Mode

The classic way is to program the LAPIC timer to fire every 10 ms, forever:

```c
void lapic_timer_periodic(uint32_t us, uint8_t vector) {
    if (ticks_per_ms == 0) return;
    uint32_t ticks = (uint32_t)(((uint64_t)us * ticks_per_ms) / 1000);

    lapic_write(LAPIC_TDCR, 0x03); // Divide by 16
    lapic_write(LAPIC_LVT_TIMER, vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TICR, ticks);
}
```

With QEMU's numbers, 10,000 µs becomes `10000 * 62501 / 1000 = 625,010` LAPIC ticks. The divisor has to be 16 again, the same one we calibrated with, or `ticks_per_ms` would be describing a different clock. This is the mode GatOS uses on QEMU's default `kvm64` CPU, since it does not offer the alternative.

#### TSC-Deadline Mode

The modern way is to tell the LAPIC "fire when the TSC reaches *this exact value*", instead of "fire every 10 ms". CPUID leaf 1, `ECX` bit 24 says whether the CPU supports it. Arming it takes two steps: we put the LVT timer in TSC-deadline mode (bits 17 to 18 set to `10`), then write the target TSC value to MSR `0x6E0`, `IA32_TSC_DEADLINE`:

```c
void lapic_tsc_arm(uint64_t tsc_deadline, uint8_t vector) {
    lapic_write(LAPIC_LVT_TIMER, (uint32_t)vector | LVT_TIMER_TSC_DEADLINE);
    // The SDM wants MFENCE between the MMIO write to the LVT and the WRMSR, lfence doesn't order stores
    __asm__ volatile("mfence" ::: "memory");
    tsc_deadline_arm(tsc_deadline);
}
```

The advantage is that a deadline does not have to be periodic. If the CPU is idle and nothing needs to wake up for 2 seconds, we can set the deadline 2 seconds out. The CPU then sleeps through the 200 ticks it would otherwise have woken up for. This is called **tickless** operation, and it is how `timer_arm_next` works. That function belongs to the scheduler, making it a Chapter 14 story. In this mode, `timer_init` does not arm anything. The first deadline is armed the first time the scheduler runs.

>[!CAUTION]
>Mind the fence. The LVT write is a store to uncached memory, and `wrmsr` to the deadline MSR is *not* a serializing instruction. Without a fence, the CPU is free to perform the MSR write while the LVT store is still on its way, arming a deadline while the timer is still in its old mode. Intel's manual spells out the fix, an `mfence` between the two.
>
>The code used to have an `lfence` there instead. It looks like the right kind of instruction, but `lfence` only orders *loads*. The problem here is a store. That fence did nothing.

### Timers and Deep Sleep

Back in Chapter 9, `cpu_idle_init` picked the deepest C-state the CPU offers for `mwait`. Now that we know where the timer interrupt comes from, there is a catch.

In deep C-states, a core switches off as much of itself as it can, and on older CPUs, that includes the LAPIC timer. The core goes to sleep waiting for the timer to wake it up. The timer has stopped too. It will only wake up when some *other* interrupt happens to arrive (maybe a keypress, maybe never).

CPUs whose LAPIC timer keeps running in every C-state advertise it with **ARAT**, the "Always Running APIC Timer" bit, in CPUID leaf 6, `EAX` bit 2. `cpu_idle_init` now checks for it:

```c
// Without ARAT (Always Running APIC Timer), the LAPIC timer stops in C2 and deeper, and
// the deadline that should wake us never fires. C1 is the only safe choice then
cpuid(0, 0, &a, &b, &c, &d);
bool arat = false;
if (a >= 6) {
    cpuid(6, 0, &a, &b, &c, &d);
    arat = a & (1u << 2);
}
if (!arat) mwait_cstate_hint = 0x00;
```

Without ARAT, we give up on deep sleep. C1 (hint `0x00`) keeps the timer running everywhere.

>[!NOTE]
>Fun fact! This one was not caught by any test. It could not have been, because QEMU does not offer `mwait` to the guest at all, and GatOS idles with `hlt` there. It took writing down "the core sleeps, and the timer wakes it up" right next to "the deepest C-state available" to see that the second sentence can break the first.

### Sleeping

With a calibrated TSC, the rest is mostly arithmetic. Here is `sleep_ms`:

```c
void sleep_ms(uint64_t ms) {
    if (ms == 0) return;

    if (sched_current() != NULL) {
        sched_sleep(ms);
        return;
    }

    if (tsc_tpm > 0) {
        uint64_t target = tsc_read() + (ms * tsc_tpm);
        while (tsc_read() < target) __asm__ volatile("pause");
    } else if (hpet_is_available()) {
        uint64_t q = 1000000000000ULL / hpet_period;
        uint64_t r = 1000000000000ULL % hpet_period;
        uint64_t target = hpet_read_counter() + ms * q + (ms * r) / hpet_period;
        while (hpet_read_counter() < target) __asm__ volatile("pause");
    } else {
        // ... PIT, one millisecond at a time ...
    }
}
```

Once the scheduler is running, sleeping means "let someone else run until I am due". That is `sched_sleep`, and Chapter 14 again. Before that, there is nobody else to run, and we spin. The `pause` instruction is the same hint we used in the spinlocks back in Chapter 6. It tells the CPU this is a spin loop, saving power and playing nicer with hyper-threading.

The TSC path is the obvious one. The HPET path has an odd `q` and `r` in it, where you might expect a plain `ms * 10^12 / hpet_period`. The reason is overflow. `ms * 10^12` overflows 64 bits once `ms` passes about 18.4 million, roughly 5 hours. Nobody sleeps that long at boot, but the fix costs nothing. We split 10^12 by the period into a quotient `q` and a remainder `r`, where `10^12 = q * period + r`. Then `ms * 10^12 / period` becomes `ms * q + ms * r / period`, and neither product gets anywhere near overflowing. The remainder term matters for periods that do not divide 10^12 evenly, like that 69,841,279 fs from real hardware.

`sleep_us` is the same idea at microsecond resolution, minus the scheduler path. Sleeping for a few microseconds is not worth a context switch. It always spins.

### Uptime

Uptime in milliseconds is just TSC ticks since `timer_init`, divided by ticks per millisecond:

```c
uint64_t get_uptime_ms(void) {
    if (tsc_tpm == 0) return 0;
    return (tsc_read() - boot_tsc) / tsc_tpm;
}
```

Nanoseconds hide a trap similar to the HPET's:

```c
uint64_t get_uptime_ns(void) {
    if (tsc_tpm == 0) return 0;
    uint64_t delta = tsc_read() - boot_tsc;
    uint64_t whole = delta / tsc_tpm;
    uint64_t rem = delta % tsc_tpm;
    return whole * 1000000 + (rem * 1000000) / tsc_tpm;
}
```

The direct formula would be `delta * 1,000,000 / tsc_tpm`. At 3.7 GHz, `delta * 1,000,000` overflows once `delta` passes about 1.8 * 10^13 ticks. That is only about 83 minutes of uptime, after which every timestamp in the system would be garbage. Instead, the code splits `delta` into whole milliseconds (safe to multiply) and the leftover ticks. There are always fewer leftover ticks than one millisecond's worth, and their product stays small. The answer is the same, and `whole * 1,000,000` will not overflow for about 580 years of uptime.

## Putting It All Together

Here is this chapter's slice of `kernel_bootstrap()`, right after the heap:

```c
// ACPI and APIC come after memory management since they require dynamic memory for tables and structures
// and they need to be initialized before we can safely enable interrupts
acpi_init(mb);
// ...
apic_init();
// ...

// Timers before scheduler
timer_init();
```

Each step builds on the ones before it. GRUB's Multiboot2 tag from Chapter 4 gives `acpi_init` the RSDP. The physmap from Chapter 5 (or the VMM from Chapter 8, when the physmap is not enough) gives it a way to read the tables. `acpi_find_table("APIC")` then gives `apic_init` the MADT, and with it the I/O APIC's address, the NMI wiring and the interrupt source overrides. The VMM maps the LAPIC and I/O APIC registers as uncacheable device memory, the job `VM_FLAG_DEVICE` was built for.

By the time `apic_init` returns, the PIC has been remapped out of the exception range and masked, the LAPIC is switched on, and every I/O APIC pin is set up and masked, waiting for its driver. Finally, `acpi_find_table("HPET")` gives `timer_init` a reference clock. The calibration measures the TSC and the LAPIC timer against it. From then on, the LAPIC timer (periodic or TSC-deadline) is ready to drive the scheduler.

Chapter 6's EOI finally makes sense too. Every hardware interrupt marks itself "in service" in the LAPIC, and holds back everything of equal or lower priority until the EOI clears it.

The kernel can hear now. In the next chapter, we give it something to listen to. User programs will talk to the kernel through the `syscall` instruction, finally explaining all those stack switches and that `swapgs` from Chapter 9. The keyboard will also become the first device in the system with a real, unmasked I/O APIC pin.
