# Chapter 12: PCI and USB xHCI

Quick question before we start. What is the keyboard you are typing on right now plugged into?

On a laptop, the honest answer is "I have no idea". On a desktop, it is almost certainly a USB port. Either way, it is probably not the round purple PS/2 connector we spent half of the last chapter talking to. So how did GatOS read your keys in QEMU, or on any real machine you might have booted it on?

The firmware lied to us. On purpose. Most PC firmware ships with something called **USB legacy support**. The firmware drives the USB keyboard itself, behind the operating system's back, while faking the PS/2 controller's ports `0x60` and `0x64` so that old software (DOS, bootloaders and hobby kernels like ours) keeps working.

The fake works well enough for typing, but it was never meant to last. It is slow, it breaks in odd ways once an OS starts reconfiguring hardware, and some newer machines do not offer it at all.

The real way to read a USB keyboard is to talk to the USB host controller directly. This chapter is about doing just that, and it is a long way down.

Before we can talk to the controller, we have to find it. Finding it means learning about **PCI**, the bus almost every device in a PC hangs off. Once we have the controller, we need just enough USB to ask a keyboard what it is, before its reports can make their way into the input hub from last chapter.

I will be honest with you about the second half. xHCI, the USB 3 host controller interface, is specified in a document that is over 700 pages long. A lot of GatOS's driver is that document turned into C, field by field. Walking through every bit of every structure would make for a very long and very boring chapter (and you would forget it all by the next one).

So for xHCI, we will focus on the ideas that make it tick (rings, doorbells and contexts) and on the path a single keypress takes. The rest is a matter of looking things up in the spec. The code is there whenever you need it.

>[!NOTE]
>The code for this chapter lives in [`pci.c`](/src/kernel/drivers/pci.c), [`xhci.c`](/src/kernel/drivers/xhci.c) and [`usb.h`](/src/kernel/drivers/usb.h), along with their headers.

## What is PCI?

Every device we have talked to so far sat at an address somebody agreed on decades ago. The PS/2 controller lives at ports `0x60` and `0x64`, the PIT at `0x40` to `0x43`, the serial port at `0x3F8`. Since these addresses are hardwired into the design of the PC, the code that uses them just hardcodes them too.

That approach falls apart quickly once you allow people to plug in their own hardware. Two network cards from two different vendors cannot both claim `0x300`, and a graphics card with 16 MiB of video memory needs somewhere to put all of that memory in the physical address space without stepping on anything else. Someone has to hand out addresses, and someone has to tell the OS what is actually in the machine.

**PCI** (Peripheral Component Interconnect) solved this in the early 90s. It is still how a PC organizes its devices today. Its modern form, PCI Express, swapped the shared parallel bus for fast serial links while keeping the software side almost the same, which is why everything in this section applies to both.

Your GPU, NVMe drive, network card and USB controllers are all PCI devices. So are a bunch of things inside the chipset that do not even look like separate devices.

The key idea is that every PCI device carries a small block of registers describing itself, called its **configuration space**. It says who made the device, what kind of device it is, how much memory it wants, and how it can interrupt the CPU.

The firmware reads all of these at boot, picks an address for every device and writes those addresses back. By the time GatOS starts, the machine is already set up. All we have to do is go and read what the firmware decided.

### Addressing a Device

PCI devices are found by their position, not by name. Every device sits at a **bus** number (0 to 255), a **device** number on that bus (0 to 31) and a **function** number within that device (0 to 7). A function is one logical device. A single physical chip can expose several of them. Older Intel chipsets, for example, put a few USB controllers on one chip as functions 0, 1 and 2 of the same device.

This triple is usually written as `bus:device.function`, which is how the logs print it:

```
[PCI] xHCI at 00:04.0 BAR0=0xfebf0000 (16384 B) MSI=0x00 MSI-X=0x90
```

That is bus 0, device 4, function 0. This is where QEMU put the xHCI controller on my setup (the exact spot depends on what else you add).

### Configuration Space

The original way to read configuration space goes through two I/O ports. We write the address of the register we want to port `0xCF8`, then read or write the register itself through port `0xCFC`. In PCI terms, these are `CONFIG_ADDRESS` and `CONFIG_DATA`.

The address we write to `0xCF8` packs the whole location into one 32-bit value:

```
 31   30-24     23-16    15-11    10-8      7-0
+----+--------+--------+--------+--------+----------+
| EN | (zero) |  bus   | device |  func  | register |
+----+--------+--------+--------+--------+----------+
```

Bit 31 is the enable bit, saying "this is a configuration access", while the low byte is the offset of the register inside the device's configuration space. Since the data port is always 32 bits wide, its bottom two bits are dropped. The whole read path in GatOS is these few lines:

```c
static void cfg_sel(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    uint32_t addr = 0x80000000 | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC);
    outl(PCI_CFG_ADDR, addr);
}

static uint16_t r16(uint8_t b, uint8_t d, uint8_t f, uint8_t o) {
    cfg_sel(b, d, f, o);
    return inw(PCI_CFG_DATA + (o & 2));
}
```

`r16` is the interesting one. Since `cfg_sel` always selects a whole aligned 32-bit register, a 16-bit read at offset `0x06` has to pick the upper half of the register at `0x04`. We do that by reading from `0xCFC + 2` instead of `0xCFC`. The 8-bit and 32-bit versions work the same way, and the `pci_read16` style wrappers on top of them just pull the bus, device and function out of a `pci_dev_t` before calling them.

>[!NOTE]
>PCI Express added a faster way in, called ECAM, where all of configuration space is memory mapped through a region described by the ACPI `MCFG` table. It also grows each function's space from 256 bytes to 4 KiB. GatOS sticks with the ports, since everything we need sits in the first 256 bytes.

### The Header

The first 64 bytes of every function's configuration space follow a standard layout, called the header. These are the fields GatOS cares about:

| Offset | Size | Field | What it is |
|---|---|---|---|
| `0x00` | 2 | Vendor ID | Who made it. `0xFFFF` means nothing is there |
| `0x02` | 2 | Device ID | Which product it is |
| `0x04` | 2 | Command | Switches for memory access, I/O access, DMA and interrupts |
| `0x06` | 2 | Status | Bit 4 says the device has a capability list |
| `0x09` | 1 | Prog IF | The exact programming interface |
| `0x0A` | 1 | Subclass | The kind of device, within the class |
| `0x0B` | 1 | Class | The broad category |
| `0x0E` | 1 | Header type | Bit 7 means "more than one function" |
| `0x10` | 4 each | BAR0 to BAR5 | Where the device's registers live |
| `0x34` | 1 | Capabilities pointer | Start of the capability list |

The three class bytes are what let a generic driver find its hardware without knowing about any specific product. A USB controller has class `0x0C` (serial bus controller) and subclass `0x03` (USB). The programming interface then says which generation it speaks: `0x00` for UHCI, `0x10` for OHCI, `0x20` for EHCI and `0x30` for xHCI. A driver that understands xHCI can drive any xHCI controller from any vendor, since they all promise to behave the same way.

### Scanning the Bus

With all of that in place, finding every xHCI controller in the machine is a brute force search. We try every bus, every device and every function, keeping the ones that match:

```c
for (int b = 0; b < 256; b++) {
    for (int d = 0; d < 32; d++) {
        if (r16(b, d, 0, PCI_VENDOR_ID) == 0xFFFF) continue;

        int max_f = (r8(b, d, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
        for (int f = 0; f < max_f; f++) {
            if (r16(b, d, f, PCI_VENDOR_ID) == 0xFFFF) continue;

            if (r8(b, d, f, PCI_CLASS) != PCI_CLASS_USB ||
                r8(b, d, f, PCI_SUBCLASS) != PCI_SUBCLASS_USB ||
                r8(b, d, f, PCI_PROG_IF) != PCI_PROGIF_XHCI) continue;

            // ... decode BAR0, find the MSI capabilities, store it
        }
    }
}
```

**What's happening here:**
* A slot with no device behind it reads back all ones, since nobody drives the bus. This is why a vendor ID of `0xFFFF` means "empty" (and why no real vendor will ever be given that ID).
* We only look at functions 1 to 7 when function 0 sets the multi-function bit in its header type. Some single-function devices ignore the function number entirely and answer as the same device eight times. Without this check, we would find every one of them eight times over.
* For every match, we decode its first BAR and look for its interrupt capabilities, then write it all into a `pci_dev_t` for the xHCI driver.

That is 256 × 32 = 8192 probes of function 0, every one of them two port accesses. The comment above the function says it "takes a moment". It does. A smarter scan would start at bus 0 and only follow the buses that PCI-to-PCI bridges actually lead to, but this one is simple and it only runs once.

## BARs

Finding the controller is the easy part. To actually use it, we need to know where its registers are. This is what the six **Base Address Registers** (BARs) are for.

Each BAR describes one block of addresses the device answers to. A BAR can be an I/O BAR (a range of ports) or a memory BAR (a range of physical addresses). The low bits of the register say which:

| Bits | Memory BAR meaning |
|---|---|
| 0 | `0` for memory, `1` for I/O |
| 2-1 | `00` for a 32-bit address, `10` for a 64-bit address |
| 3 | Prefetchable |
| 31-4 | The address itself |

A 64-bit BAR needs more than 32 bits of address, so it eats the next BAR slot too, using it for the upper half. The xHCI controller's registers are always in a memory BAR (BAR0). On QEMU, it is a 64-bit one.

### How Big is It?

The address is easy to read, since it is right there in the register. The size is the interesting part, because no register stores it. Instead, PCI uses a rather clever trick.

The firmware can only place a BAR at an address that is a multiple of its size, which means the low bits of the address are always zero. The device takes advantage of this by hardwiring those bits, so whatever we write to them, they read back as zero.

To find the size, we write all ones to the BAR and read it back. The bits that refuse to become ones are the bits covered by the size.

Let's make this concrete with the xHCI controller on QEMU, whose BAR0 sits at `0xFEBF0000`. After writing `0xFFFFFFFF`, it reads back `0xFFFFC004`. We first mask off the four type bits to get `0xFFFFC000`, then invert it to get `0x00003FFF`, then add one to get `0x4000`. That is 16 KiB, the "16384 B" in the log line from earlier.

Once we know the size, we write the original address back. The BAR is as good as new. In GatOS, this is `decode_bar`:

```c
uint8_t type = (lo >> 1) & 3;
uint32_t hi = (type == 2) ? r32(b, d, f, off + 4) : 0;

// Stop the device decoding while the BAR holds all ones, or it claims a bogus range
uint16_t cmd = r16(b, d, f, PCI_COMMAND);
w16(b, d, f, PCI_COMMAND, cmd & ~(PCI_CMD_IO | PCI_CMD_MEM));

w32(b, d, f, off, 0xFFFFFFFF);
uint32_t sz_lo = r32(b, d, f, off);
w32(b, d, f, off, lo);

uint32_t sz_hi = 0xFFFFFFFF;
if (type == 2) {
    w32(b, d, f, off + 4, 0xFFFFFFFF);
    sz_hi = r32(b, d, f, off + 4);
    w32(b, d, f, off + 4, hi);
}

w16(b, d, f, PCI_COMMAND, cmd);

if (!sz_lo || sz_lo == 0xFFFFFFFF) return false;

*sz = ~(((uint64_t)sz_hi << 32) | (sz_lo & 0xFFFFFFF0)) + 1;
*phys = ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0);
```

For a 64-bit BAR, the upper half goes through the same process before the two halves are glued together and inverted. A 32-bit BAR has no upper half. We pretend it read back as all ones, since that makes the inverted upper bits zero.

>[!NOTE]
>While writing this section, I found that the upper half of a 64-bit BAR was never sized. Any BAR larger than 4 GiB would have come out with the wrong size.

The second bug I found here is more interesting, and it is the reason for the `PCI_COMMAND` lines. For a short moment, the BAR holds `0xFFFFFFF0` as its address. If the device still has memory decoding turned on during that moment, it will answer to that address range. That range overlaps whatever really lives at the top of the 32-bit space (the LAPIC and the firmware's flash, among other things).

In practice, nobody touches those addresses in the few microseconds the sizing takes. Linux still disables decoding before sizing a BAR, though, and GatOS now does too. The test kernel sizes the VGA card's BAR on QEMU and checks that both the BAR and the command register come back exactly as they were.

### Mapping It

A memory BAR is just a range of physical addresses with device registers behind them, the same kind of MMIO we used for the LAPIC in Chapter 10. We map it the same way, too:

```c
uint64_t ms = align_up(pci->bar0_size, PAGE_SIZE);
if (ms < PAGE_SIZE) ms = PAGE_SIZE;
if (vmm_alloc(NULL, ms, VM_FLAG_WRITE | VM_FLAG_FOREIGN | VM_FLAG_DEVICE, (void *)pci->bar0_phys, (void **)&hc->cap) != VMM_OK) {
    // ...
}
```

`VM_FLAG_FOREIGN` tells the VMM that this physical memory is not RAM and must never be given back to the PMM. `VM_FLAG_DEVICE` makes the mapping uncacheable, because every read and write has to actually reach the controller.

Before any of that, `pci_enable` flips three switches in the command register:

```c
void pci_enable(pci_dev_t *d) {
    uint16_t cmd = pci_read16(d, PCI_COMMAND);
    cmd |= (PCI_CMD_MEM | PCI_CMD_BUS_MASTER | PCI_CMD_INT_DISABLE);
    cmd &= ~PCI_CMD_IO;
    pci_write16(d, PCI_COMMAND, cmd);
}
```

Memory space has to be on for the BAR to answer at all. Interrupt disable turns off the old style of PCI interrupt (we will replace it with something better in a moment). Bus master is the new one. It deserves a proper introduction.

>[!IMPORTANT]
>What is DMA?
>
>**Direct Memory Access** (DMA) means a device reading and writing RAM on its own, without the CPU copying each byte. We hand the device a physical address, it does the transfer, and it tells us when it is done. A device that is allowed to do this is called a **bus master**. Without the bus master bit in its command register, a device cannot touch RAM at all.

Almost everything the xHCI controller does goes through DMA. Keep this in mind for the rest of the chapter. Whenever we give the controller an address, it has to be a **physical** address, since the device has no idea our page tables even exist.

## Capabilities and MSI

The last thing we need from configuration space is a way for the controller to interrupt us.

The traditional answer is a dedicated wire. Every PCI device has up to four interrupt pins (INTA to INTD), which end up on the I/O APIC pins we met in Chapter 10. The trouble is figuring out which pin.

These lines are shared between devices, swizzled across slots and routed differently on every motherboard (sometimes even between two revisions of the same one). The only complete description of the wiring lives in ACPI, inside a method called `_PRT`. A method, in ACPI terms, is bytecode (called AML) that the OS has to *run*. GatOS has no AML interpreter.

Thankfully, there is a much better answer.

### Message Signaled Interrupts

In Chapter 10, we mapped the LAPIC's registers at the physical address `0xFEE00000`. With **Message Signaled Interrupts** (MSI), a device interrupts the CPU by doing a DMA write to that address range. The chipset recognizes writes there as interrupt messages. Instead of storing anything in memory, it delivers them straight to a LAPIC, with the address picking the core and the data picking the vector.

No wires. No sharing. No routing tables. The I/O APIC is not involved at all. Every device gets its own vector, and since each write is exactly one interrupt, an MSI behaves like an edge triggered one.

To set this up, the OS writes the address and the data into the device's MSI registers. Those registers are not in the standard header, though. They live in a **capability**.

### The Capability List

The 64-byte header is fixed, while PCI kept growing long after it was designed. Anything newer than the header (MSI, power management, PCI Express itself) is described by a capability structure, and these capabilities sit in the rest of configuration space as a linked list that any driver can walk.

Bit 4 of the status register says whether the list exists. If it does, the byte at offset `0x34` points to the first entry. Each entry starts with a one-byte ID saying what it is, followed by a one-byte pointer to the next entry, with `0` ending the list:

```c
static uint8_t find_cap(uint8_t b, uint8_t d, uint8_t f, uint8_t id) {
    if (!(r16(b, d, f, PCI_STATUS) & PCI_STAT_CAP_LIST)) return 0;

    uint8_t ptr = r8(b, d, f, PCI_CAP_PTR);
    for (int i = 0; ptr && i < 48; i++) {
        ptr &= 0xFC;
        if (r8(b, d, f, ptr) == id) return ptr;
        ptr = r8(b, d, f, ptr + 1);
    }
    return 0;
}
```

The `i < 48` is there in case a broken device makes the list loop back on itself, since the 192 bytes past the header can hold at most 48 entries of 4 bytes. Anything longer has to be a loop.

MSI is capability `0x05`. Setting it up takes three writes:

```c
uint32_t addr = 0xFEE00000 | ((lapic_id & 0xFF) << 12);
// ...
if (ctrl & PCI_MSI_64BIT) {
    pci_write32(d, c + PCI_MSI_ADDR_LO, addr);
    pci_write32(d, c + PCI_MSI_ADDR_HI, 0);
    pci_write16(d, c + PCI_MSI_DATA_64, vec);
} else {
    pci_write32(d, c + PCI_MSI_ADDR_LO, addr);
    pci_write16(d, c + PCI_MSI_DATA_32, vec);
}

ctrl = (ctrl & ~0x70) | PCI_MSI_EN;
pci_write16(d, c + PCI_MSI_CTRL, ctrl);
```

The address is the LAPIC's base with the target core's APIC ID in bits 12 to 19. The data is just the vector number. The capability comes in a 32-bit and a 64-bit flavor (the control register says which), with the data register at a different offset in each. Finally, clearing bits 4 to 6 asks for a single vector, and bit 0 turns MSI on.

GatOS uses vector `0x60` for the first xHCI controller. It used to be 50, inside the range the I/O APIC hands out (`32 + GSI`, where GSI 18 would land on 50 as well). Nothing ever collided, since every I/O APIC pin is masked except the keyboard's. Still, there is no reason to share a neighborhood with them.

### MSI-X

When I first booted GatOS with a USB keyboard on QEMU's default xHCI controller, it did not type a single character.

The controller was found, the keyboard was enumerated, and the logs looked perfect. But the log line above says `MSI=0x00`. QEMU's `qemu-xhci` does not offer plain MSI by default. It only offers **MSI-X**, capability `0x11`. GatOS only knew about MSI, which made `pci_cfg_msi` quietly return `false`. Since `pci_enable` had already turned the old interrupt pins off, the controller had no way left to tell us anything.

MSI-X is MSI's bigger sibling, made for devices that want many vectors (think a network card with one queue per core). The idea is the same, a write to the LAPIC's address. What changes is where the address and data live.

Instead of sitting in configuration space, they sit in a **table** inside one of the device's own BARs. Each vector gets a 16-byte entry with the address (low and high), the data, and a control word whose bit 0 masks the entry.

The capability only says where to find that table, as a BAR number (the BIR, in the low 3 bits) plus an offset into it. `cfg_msix` follows that pointer, maps the page and fills in entry 0:

```c
uint64_t phys = bar + (tbl & ~7u);
void *page;
if (vmm_alloc(NULL, PAGE_SIZE, VM_FLAG_WRITE | VM_FLAG_FOREIGN | VM_FLAG_DEVICE,
              (void *)(phys & ~(uint64_t)(PAGE_SIZE - 1)), &page) != VMM_OK) return false;
volatile uint32_t *ent = (volatile uint32_t *)((uint8_t *)page + (phys & (PAGE_SIZE - 1)));

// Mask the whole function while entry 0 is half written
uint16_t ctrl = pci_read16(d, c + PCI_MSIX_CTRL);
pci_write16(d, c + PCI_MSIX_CTRL, ctrl | PCI_MSIX_EN | PCI_MSIX_FMASK);
ent[0] = addr;
ent[1] = 0;
ent[2] = vec;
ent[3] = 0; // unmask entry 0
pci_write16(d, c + PCI_MSIX_CTRL, (ctrl | PCI_MSIX_EN) & ~PCI_MSIX_FMASK);
```

The order matters. Every entry starts out masked. We also set the function mask bit while we write, because the device must never fire an interrupt using an address that is only half written. Once the entry is complete, we unmask it and drop the function mask.

`pci_cfg_msi` now prefers plain MSI and falls back to MSI-X when it is missing. With that one fallback, the USB keyboard types on QEMU out of the box.

## USB in Five Minutes

Before we touch the controller, we need a picture of what it controls. USB is its own little world with its own vocabulary, and the xHCI spec assumes you already know all of it before you open page one.

A USB system is a tree. At the root sits the **host controller**, the only thing on the bus allowed to start a conversation. Devices plug into its **root hub** ports, or into **hubs** (devices whose only job is to provide more ports). Devices never talk to each other. They never speak unless spoken to. Even a keyboard, which looks like it "sends" keypresses, is really being asked "anything new?" over and over by the host.

Every device is given a small **address** when it is plugged in. Inside a device, communication happens through **endpoints**. An endpoint is a numbered mailbox (0 to 15), with a direction (IN means device to host, OUT means host to device) and a transfer type. Two of those types matter to us:

* **Control** transfers are structured request and response exchanges. Every device has a control endpoint 0. This is how the host asks a device about itself and configures it.
* **Interrupt** transfers move small amounts of data on a schedule. The host polls the endpoint every so many milliseconds, and the device answers with data or with "nothing new". A keyboard's keypresses come through one of these.

>[!NOTE]
>Despite the name, USB interrupt transfers have nothing to do with CPU interrupts. The device cannot interrupt anyone. It just gets polled on a fixed schedule, often enough that it never has to wait long for its turn.

### Descriptors

A device describes itself through **descriptors**, small binary structures that the host requests with control transfers. They form a hierarchy. The **device descriptor** says who made the device and what class it belongs to. Under it, a **configuration descriptor** describes one way of running the device. A configuration contains **interfaces**, each one a separate function of the device (a keyboard with a built-in touchpad might have two). Finally, every interface lists its **endpoints**.

Every descriptor starts with the same two bytes, its length and its type. This lets us walk a list of them without understanding every one. The configuration descriptor is special, since requesting it returns the configuration followed by all of its interfaces and endpoints, packed back to back in one buffer. We will walk such a buffer later.

### Control Transfers

A control transfer happens in up to three stages. It always starts with a **setup** stage, an 8-byte packet that describes the request:

```c
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;
```

`bmRequestType` packs the direction, who defined the request (the USB spec, a device class, or the vendor) and who it is aimed at (the device, an interface, an endpoint, or "other", which hubs use for their ports).

`bRequest` is the request itself, such as `GET_DESCRIPTOR` or `SET_CONFIGURATION`. `wValue` and `wIndex` are request-specific parameters. `wLength` is how many bytes the optional **data** stage moves. The last stage is the **status** stage, where the side that did not send the data confirms that everything went fine.

So "give me your device descriptor" is a setup packet of `80 06 00 01 00 00 12 00`, a data stage where 18 bytes come back, and a status stage. Keep that shape in mind. It shows up again later.

### HID and the Boot Protocol

Keyboards and mice belong to the **HID** class (Human Interface Device), class code 3. HID is extremely flexible. A HID device sends a *report descriptor*, a little program describing the exact layout of its reports, which the host is supposed to parse in order to figure out which bit of a report means what. Parsing these is a chapter of its own.

Luckily, the HID spec also defines a **boot protocol**, made for BIOSes that cannot afford a full HID parser (and, as it turns out, for hobby kernels that cannot either). A keyboard that supports it marks its interface with subclass 1 (boot) and protocol 1 (keyboard). When asked to switch to boot protocol, it sends a fixed 8-byte report that never changes layout. This is the protocol GatOS speaks, and it lets us skip report descriptors entirely.

## What is xHCI?

USB has gone through four generations of host controller interfaces. UHCI and OHCI were the two competing USB 1 designs, while EHCI handled USB 2's high speed. A machine from the late 2000s often had all of them at once, with ports switched between controllers depending on what was plugged in.

**xHCI** (eXtensible Host Controller Interface) replaced the lot. One xHCI controller handles every USB speed, from 1.5 Mbit/s keyboards to 20 Gbit/s drives. Any machine made in the last decade has one.

The design idea behind xHCI is that the CPU should do as little as possible. Older controllers had the OS build and walk the bus schedules itself, frame by frame. With xHCI, the OS describes what it wants in memory and pokes a register to say "go look", and the controller does the rest on its own through DMA, without any help from the CPU. Pretty much the entire driver comes down to three mechanisms:

1. **Rings**, circular buffers in RAM where we queue work and where the controller queues its answers.
2. **Doorbells**, registers we write to tell the controller there is something new on a ring.
3. **Contexts**, structures in RAM where the controller keeps what it knows about each device and endpoint.

Everything else in the spec builds on these three.

### The Registers

BAR0 holds four groups of registers, one after the other:

* The **capability registers** are read-only. They describe what this particular controller supports: how many device slots and ports it has, how big its contexts are, and where the other groups start.
* The **operational registers** are where we run the controller: start and stop it, reset it, give it our data structures, and watch the state of each root hub port.
* The **runtime registers** hold the interrupt settings, including the pointer to how far we have read the event ring.
* The **doorbell array** is 256 32-bit registers, one per device slot plus one for the controller itself.

Only the capability registers sit at a fixed spot (the start of BAR0). Their first byte says how long they are (the operational registers start right after). Two more of them hold the offsets of the runtime registers and the doorbells. The first thing `xhci_init` does after mapping BAR0 is follow those pointers:

```c
hc->op = hc->cap + cr8(hc, XHCI_CAPLEN);
hc->slots = HCS1_SLOTS(cr32(hc, XHCI_HCSPARAMS1));
hc->ports = HCS1_PORTS(cr32(hc, XHCI_HCSPARAMS1));
uint32_t hcc1 = cr32(hc, XHCI_HCCPARAMS1);
hc->ctx_sz = HCC1_CSZ(hcc1) ? 64 : 32;
hc->ac64 = (hcc1 & 1) != 0;
hc->rt = hc->cap + (cr32(hc, XHCI_RTSOFF) & ~0x1F);
hc->db = (uint32_t *)(hc->cap + (cr32(hc, XHCI_DBOFF) & ~3));
```

From here on, the driver reads and writes these through a handful of one-line helpers (`or32` reads an operational register, `iw32` writes an interrupter register, and so on). They are all `volatile` pointer accesses, the same as the LAPIC's in Chapter 10.

## Rings

Everything that goes between the driver and the controller travels in units of 16 bytes, called **TRBs** (Transfer Request Blocks):

```c
typedef struct {
    uint32_t dw0, dw1, dw2, ctrl;
} __attribute__((packed)) trb_t;
```

The meaning of the first three words depends on the TRB's type, stored in bits 10 to 15 of `ctrl`. Usually, `dw0` and `dw1` hold a 64-bit physical address, with a length in `dw2`. A "Normal" TRB says "move this many bytes to or from this buffer", an "Enable Slot" TRB is a command for the controller, and so on. Bit 0 of `ctrl` is special in every TRB. It is the most important bit in this chapter.

TRBs are queued on **rings**. A ring is an array of TRBs in RAM, with one side producing TRBs and the other side consuming them. When the producer reaches the end of a ring, it wraps around to the start and reuses the slots the consumer has already finished with.

GatOS has three kinds of rings. The **command ring** is where we queue commands for the controller itself ("give me a device slot", "set up this endpoint"). Each endpoint has its own **transfer ring**, where we queue data transfers. Finally, the **event ring** goes the other way. This is where the controller reports what happened.

### Who Owns a TRB?

There is a problem here. It is a good one to think about before reading on.

On a normal circular buffer, the producer and consumer agree on where the valid entries are by sharing two indices, a head and a tail. Our consumer is a piece of hardware, though. It reads the ring from RAM through DMA, and it has no access to our variables. So how does the controller know which TRBs on the ring are new work, and which ones are leftovers it already processed on its previous trip around?

Checking whether a TRB is zeroed would not work, since somebody would have to clear every slot after use, and that somebody would be the controller writing into our ring. It cannot use an index register either, since then every single TRB would cost us an MMIO write.

The answer is bit 0 of `ctrl`, called the **cycle bit**. The producer keeps a single bit of state, its *producer cycle state*, starting at 1. Every TRB it writes gets that value in its cycle bit.

The consumer keeps its own copy of the same bit, and treats a TRB as valid only for as long as the TRB's cycle bit matches its own. The moment it reads a TRB whose bit does not match, it knows it has caught up with the producer. It stops there.

The trick is in what happens at the end of the ring. Each time the producer wraps around, it flips its cycle state. On the second lap it writes TRBs with a cycle bit of 0, then 1 again on the third, and so on.

The TRBs left over from the previous lap all carry the *old* value, so they never look valid to a consumer that is already on the new lap. This way, a single bit keeps both sides in agreement on where the valid data ends, without any shared counters or clearing.

### Link TRBs

The hardware has no idea how long our ring is, so the producer has to mark where the ring ends with a **Link TRB** in its last slot. It holds the physical address of the ring's start, and the consumer jumps there when it reaches it. One bit in a Link TRB, *Toggle Cycle*, tells the consumer to flip its own cycle state as it jumps, keeping it in step with the producer.

`ring_init` sets one up in the last slot:

```c
static void ring_init(xhci_hc_t *hc, ring_t *r, uint32_t cnt) {
    r->trbs = dma_alloc(hc, cnt * sizeof(trb_t), &r->phys);
    r->enq = r->deq = 0;
    r->cyc = 1;
    trb_t *link = &r->trbs[cnt - 1];
    link->dw0 = (uint32_t)r->phys;
    link->dw1 = (uint32_t)(r->phys >> 32);
    link->dw2 = 0;
    link->ctrl = TRB_TYPE(TRB_LINK) | TRB_TC | r->cyc;
}
```

`dma_alloc` gets zeroed, physically contiguous pages from the PMM and returns them through the physmap, keeping the physical address for the controller. Since the memory is zeroed, every TRB starts with a cycle bit of 0, which a consumer starting at 1 reads as "not yet valid".

Queuing a TRB is `enq`:

```c
static void enq(ring_t *r, uint32_t cnt, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t c) {
    trb_t *t = &r->trbs[r->enq];
    t->dw0 = d0;
    t->dw1 = d1;
    t->dw2 = d2;
    __asm__ volatile("" ::: "memory");
    t->ctrl = c | (r->cyc ? TRB_C : 0);
    if (++r->enq == cnt - 1) {
        trb_t *l = &r->trbs[cnt - 1];
        l->ctrl = TRB_TYPE(TRB_LINK) | TRB_TC | (r->cyc ? TRB_C : 0);
        r->cyc ^= 1;
        r->enq = 0;
    }
}
```

**What's happening here:**
* We fill in the three data words first and the `ctrl` word last, since writing `ctrl` is what flips the cycle bit and hands the TRB over. The compiler barrier in between stops the compiler from reordering the stores. (x86 keeps stores in order on its own, so no fence instruction is needed.)
* When the next slot would be the Link TRB, we give the Link TRB the current cycle bit (handing it over too), flip our cycle state and go back to slot 0.

Let's trace a tiny ring of 4 TRBs, with slots 0 to 2 for work and slot 3 for the Link:

1. At the start, every slot reads cycle 0. Our cycle state is 1, and the controller's is also 1. Nothing is valid.
2. We queue A, B and C into slots 0, 1 and 2, each with cycle 1. After C, `enq` gives the Link TRB cycle 1, flips our state to 0 and wraps to slot 0.
3. The controller processes A, B and C, then reaches the Link TRB. Its cycle bit is 1, which matches. The controller jumps back to slot 0 and, because of Toggle Cycle, flips its own state to 0.
4. At slot 0 it finds A, with cycle 1. That does not match its state of 0, so it stops. A is old news.
5. We queue D into slot 0 with cycle 0. Now it matches, and the controller picks it up.

### Doorbells

The cycle bit tells the controller *which* TRBs are valid, but the controller does not sit there rereading every ring in memory all day. Once it runs out of work on a ring, it goes idle. Writing to a **doorbell** is how we wake it up again.

Doorbell 0 belongs to the controller. Ringing it with `0` means "check the command ring". Every other doorbell belongs to the device slot with the same number, with the value written saying which of that device's endpoints has new work:

```c
static inline void dbw (xhci_hc_t *hc, uint32_t s, uint32_t v) {
    __asm__ volatile("" ::: "memory"); // every TRB store must land before the doorbell
    *(volatile uint32_t *)(hc->db + s) = v;
}
```

>[!NOTE]
>That compiler barrier is new. `volatile` only orders accesses against other `volatile` accesses, while the TRB stores in `enq` are plain ones. Without the barrier, the compiler may sink them below the doorbell write. The controller could then go looking before the TRB was there.

### The Event Ring

The event ring is the same idea in reverse. The controller is the producer and we are the consumer. The cycle bit works the same way. Every command completion, every finished transfer and every port change shows up here as an event TRB.

There are two differences. The first is that the controller finds the event ring through a small table called the **Event Ring Segment Table** (ERST), since a big event ring can be split into several pieces, called segments. GatOS uses one segment of 64 TRBs, so its table has one entry.

The second is that the event ring has no Link TRB, since the controller already knows the segment's size from the table and wraps around on its own when it reaches the end.

Reading an event is `deq`:

```c
static trb_t *deq(xhci_hc_t *hc) {
    trb_t *t = &hc->evt.trbs[hc->evt.deq];
    __asm__ volatile("" ::: "memory");
    if ((t->ctrl & TRB_C) != (uint32_t)hc->evt.cyc) return NULL;
    if (++hc->evt.deq >= ERING_SZ) {
        hc->evt.deq = 0;
        hc->evt.cyc ^= 1;
    }
    return t;
}
```

On this ring, the controller is the one producing TRBs. It has to know how far we have read, otherwise it would wrap around and overwrite events we have not looked at yet. So once we are done with a batch, `flush_erdp` writes our position into the **Event Ring Dequeue Pointer** register (ERDP). This is the one ring where we do report an index back, and even then, only once per batch rather than once per event.

## Contexts

Rings carry the work, but the controller also needs to remember things about every device: what speed it runs at, which port it is on, what endpoints it has, and where each endpoint's transfer ring is. All of that lives in **contexts**, structures in RAM that the controller reads and updates through DMA.

When a device shows up, the controller assigns it a **slot**, numbered from 1. From then on, slots are how we refer to devices. For every slot, the controller keeps a **device context**, made of one **slot context** describing the device as a whole, followed by up to 31 **endpoint contexts**. A context is either 32 or 64 bytes, depending on the controller (that is the `ctx_sz` we read from the capability registers).

The controller finds each device context through the **Device Context Base Address Array** (DCBAA), an array of 64-bit physical pointers indexed by slot number. We allocate the array and give its address to the controller. Entry `n` gets filled in whenever slot `n` is set up.

The endpoint contexts are indexed in a slightly odd way, called the **Device Context Index** (DCI). Endpoint 0 is DCI 1. For every other endpoint, the DCI is twice the endpoint number, plus one for IN endpoints. A keyboard's interrupt endpoint at address `0x81` (endpoint 1, IN) is therefore DCI 3. The DCI is also what we write to a device's doorbell, which is why ringing endpoint 0 is `dbw(hc, slot, 1)`.

### Input and Output

Here is the part that confuses everyone at first. The device context belongs to the controller. We are not supposed to write into it. To change anything, we fill in a separate structure, an **input context**, and pass its address to a command. The controller reads it, checks it, then copies what it accepts into its own device context (which the spec, from its own point of view, calls the *output* context).

An input context is laid out like a device context with one extra entry at the front, the **input control context**. Its main field is a bitmask of "add" flags, one per entry, saying which of the following contexts we actually want the controller to look at. Bit 0 is the slot context, bit 1 is endpoint 0 and bit `n` is DCI `n`. GatOS reaches them with three tiny helpers:

```c
static inline ctrl_ctx_t *get_ctrl(void *c) { return c; }
static inline slot_ctx_t *get_slot(xhci_hc_t *hc, void *c) { return (void *)((uint8_t *)c + hc->ctx_sz); }
static inline ep_ctx_t *get_ep(xhci_hc_t *hc, void *c, uint8_t i) { return (void *)((uint8_t *)c + hc->ctx_sz * (2 + i)); }
```

So the conversation with the controller is always the same. We fill in an input context, queue a command on the command ring pointing at it, ring doorbell 0, and wait for a completion event. Only the command and the add flags change.

>[!NOTE]
>This split exists so the controller can refuse a request without leaving a half-applied mess behind. If a command fails, the device context is left exactly as it was.

## Bringing the Controller Up

We now know enough to read `xhci_init`. Once the registers are found, it has to take the controller away from the firmware, reset it, give it its data structures and start it.

### Taking It from the Firmware

This is where the lie from the start of this chapter ends. The firmware has been running this controller since power on, pretending your USB keyboard is a PS/2 one, and we have to politely ask it to stop.

xHCI has a list of **extended capabilities** inside BAR0, working just like PCI's capability list. One of them is **USB Legacy Support**. It holds two semaphore bits, one owned by the BIOS and one owned by the OS. Setting the OS bit makes the firmware's code run, finish what it was doing and clear the BIOS bit:

```c
volatile uint32_t *reg = (volatile uint32_t *)cap;
*reg |= LEG_OS_SEM;
for (int j = 0; j < 1000; j++) {
    if (!(*reg & LEG_BIOS_SEM)) break;
    sleep_ms(1);
}
if (*reg & LEG_BIOS_SEM) {
    LOGF("[XHCI] BIOS never let go, taking the controller anyway\n");
    *reg &= ~LEG_BIOS_SEM;
}

// Turn off every SMI source (bits 0, 4, 13-15) and ack the pending ones (29-31)
volatile uint32_t *ctl = (volatile uint32_t *)(cap + 4);
*ctl = (*ctl & ~((1u << 0) | (1u << 4) | (7u << 13))) | (7u << 29);
```

The firmware's emulation runs in **System Management Mode** (SMM), a special CPU mode that is invisible to the OS. When certain hardware events trigger a System Management Interrupt (SMI), the CPU drops whatever it was doing (our kernel included), runs firmware code and comes back as if nothing happened. The second register controls which xHCI events trigger one. We turn all of them off.

The old code only cleared bits 0 to 4. That left three SMI sources armed (on changes to the semaphores, on writes to the PCI command register and on writes to the BARs). It also never acknowledged pending SMI events, and it simply moved on when a buggy BIOS refused to let go. Linux takes the controller anyway in that case and says so in the log. GatOS now does the same.

>[!NOTE]
>The four Intel-only lines at the top of `bios_handoff` are about the same history. On some Intel chipsets, every port is wired to both an EHCI and an xHCI controller, starting out routed to EHCI. Those writes reroute every port to xHCI, following what Linux does.

### Reset and Setup

The rest of the bring-up is mostly writing addresses into registers, in the order the spec asks for:

```c
bios_handoff(hc, pci);
if (!reset_hc(hc)) {
    // ...
}

hc->dcbaa = dma_alloc(hc, align_up((hc->slots + 1) * 8, 64), &hc->dcbaa_phys);
uint32_t scratches = HCS2_SCRATCH(cr32(hc, XHCI_HCSPARAMS2));
if (scratches) {
    uint64_t spa;
    uint64_t *spa_buf = dma_alloc(hc, scratches * 8, &spa);
    for (uint32_t j = 0; j < scratches; j++) dma_alloc(hc, PAGE_SIZE, &spa_buf[j]);
    hc->dcbaa[0] = spa;
}

ow32(hc, XHCI_CONFIG, hc->slots & 0xFF);
ow64(hc, XHCI_DCBAAP_LO, hc->dcbaa_phys);
ring_init(hc, &hc->cmd, RING_SZ);
ow64(hc, XHCI_CRCR_LO, (hc->cmd.phys & ~0x3F) | hc->cmd.cyc);
evt_init(hc);

start_hc(hc);
```

**What's happening here:**
* `reset_hc` clears the Run/Stop bit, waits for the controller to report itself halted, then sets the reset bit and waits for it to clear. The controller also has a "Controller Not Ready" bit. Nothing else may be written until that one is clear too.
* The DCBAA has one entry per slot, plus entry 0. Slot numbers start at 1, leaving entry 0 free. The spec reuses it for the **scratchpad**, a set of pages some controllers ask the OS for, to use as their own private memory. They are never touched by the OS. We just allocate as many as the controller asks for and hand them over.
* `XHCI_CONFIG` says how many slots we will use (all of them). `DCBAAP` gets the array's physical address.
* The command ring's address goes into `CRCR`, together with our starting cycle bit.
* `evt_init` builds the event ring and its one-entry ERST, then writes them into interrupter 0's registers and enables interrupts on it. The ERST's address goes in last, since writing it is what switches the ring on. (The old code wrote it before the dequeue pointer, which is the wrong order in the spec's own list of steps.)
* `start_hc` sets Run/Stop along with the interrupt enable bit, then waits for the controller to leave the halted state.

From this point on, the controller is running, watching its ports and waiting for us to ring a doorbell.

## Enumerating a Device

A device that was just plugged in has no address, no slot and no configuration, and the only thing it answers to is endpoint 0 at the default address 0. Turning it into a working keyboard is called **enumeration**. This is the job of `enum_dev`, the longest function in the driver by far, and we will walk through its steps without dwelling on every single field it fills in.

### Resetting the Port

Each root hub port has a `PORTSC` register (port status and control). Bit 0, Current Connect Status, says whether something is plugged in. When `xhci_init` finds a connected port, it first **resets** it:

```c
static bool reset_port(xhci_hc_t *hc, uint8_t p) {
    uint32_t sc = or32(hc, XHCI_PORTSC(p));
    uint32_t safe_sc = sc & 0x0E00C200;
    ow32(hc, XHCI_PORTSC(p), safe_sc | PORT_RW1C);
    ow32(hc, XHCI_PORTSC(p), safe_sc | PORT_PR);
    // ... wait for Port Reset Change
}
```

The reset puts the device into its default state and makes the port negotiate a speed with it, which the controller then reports in bits 10 to 13 of `PORTSC`. This speed matters for almost every step after this one.

The odd-looking mask is there because `PORTSC` is a minefield. Some bits are plain read/write. Some are "write 1 to clear", where writing back a 1 we just read would clear a status change we never handled. One of them, bit 1, *disables the port* when we write a 1 to it, while reading as 1 on every working port.

This is why we never write back what we read. We keep only the few bits that are safe to preserve (port power, the indicator bits and the wake enables), then add the bits we mean to set.

### Getting a Slot and an Address

Next, we ask the controller for a slot with an **Enable Slot** command, and its completion event carries the slot number it picked. We allocate a page for the device's output context and put its physical address into the DCBAA at that slot, then set up a transfer ring for endpoint 0.

The **Address Device** command comes next. It takes an input context with two add flags set (`c->add = 3`, the slot context and endpoint 0):

```c
slot_ctx_t *sc = get_slot(hc, in);
sc->dw0 = SLOT_SPD(spd) | SLOT_CTX_ENT(1) | (route_string & 0xFFFFF);
sc->dw1 = SLOT_PORT(root_hub_port);
// ...
ep_ctx_t *e0 = get_ep(hc, in, 0);
uint16_t mps = get_mps(spd);
e0->dw1 = EP_CERR(3) | EP_TYPE(EP_CTRL) | EP_PKT(mps);
e0->deq_lo = (uint32_t)s->ep0.phys | EP_DCS;
e0->deq_hi = (uint32_t)(s->ep0.phys >> 32);
```

The slot context says how fast the device is and which root port it hangs off, plus a route string (more on that in the hubs section). The endpoint context says this is a control endpoint, how big its packets are and where its transfer ring starts, with the ring's starting cycle bit (`EP_DCS`) tucked into bit 0 of that address since ring addresses are always aligned anyway.

When the controller runs Address Device, it picks a USB address on its own and sends the device a `SET_ADDRESS` request. On older controllers, the OS had to do that part itself. From here on, we never see the USB address at all. We only ever talk about the slot.

The command has a flag, BSR (Block Set Address Request), that tells the controller to skip the `SET_ADDRESS`. It exists for a few ancient devices that want their descriptor read before they get an address, and it leaves the slot in a halfway state the spec calls Default. GatOS used to set it for every USB 3 device, under a comment about hubs. A slot in the Default state refuses Configure Endpoint, so no USB 3 keyboard could ever have finished enumerating. I only caught this with the spec open next to the code. A USB 3 storage device on QEMU now reaches the Addressed state like everything else.

The maximum packet size of endpoint 0 is a guess at this point. Low, high and SuperSpeed devices only allow one size each (8, 64 and 512 bytes), but a full speed device can pick 8, 16, 32 or 64, and `get_mps` guesses 64. So the very first thing we ask for is just the first 8 bytes of the device descriptor, whose last byte, `bMaxPacketSize0`, holds the real value. If the guess was wrong, an **Evaluate Context** command fixes endpoint 0's context, and only then do we read all 18 bytes.

The old code asked for all 18 bytes right away. A full speed device with 8-byte packets would send its first 8 bytes and stop, since the controller took a packet shorter than 64 bytes to mean "that is all". The request still reported 18 bytes, though, and the rest of the buffer was whatever happened to be in it.

### A Control Transfer in TRBs

That first request is also our first control transfer. Its three stages become three TRBs:

```c
uint32_t trt = req->wLength ? (in ? 3 << 16 : 2 << 16) : 0;
prepare_wait(hc, TRB_EV_XFER, s->id, 1);
enq(&s->ep0, RING_SZ, d0, d1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
if (req->wLength) enq(&s->ep0, RING_SZ, (uint32_t)buf, (uint32_t)(buf >> 32), req->wLength, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
enq(&s->ep0, RING_SZ, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in && req->wLength ? 0 : TRB_DIR_IN));
dbw(hc, s->id, 1);
```

**What's happening here:**
* The **Setup Stage** TRB carries the 8-byte setup packet itself, in `dw0` and `dw1` instead of a pointer. That is what `TRB_IDT` (Immediate Data) means. The `trt` field tells the controller whether a data stage follows (and in which direction).
* The **Data Stage** TRB, when there is one, points at a DMA buffer for the data.
* The **Status Stage** TRB goes the opposite way from the data. With no data stage, it is always IN. Its `TRB_IOC` (Interrupt On Completion) bit asks the controller to post an event once the whole transfer is done.
* Ringing the device's doorbell with `1` means "endpoint 0 has work".

The buffer is `hc->scratch`, a single page per controller that every descriptor request reads into. (It has nothing to do with the scratchpad from earlier, despite the name.)

### Finding the Keyboard

With endpoint 0 working, we read the configuration descriptor, which takes two requests since we cannot know its full length in advance. The first one asks for only the first 9 bytes, whose `wTotalLength` says how long the whole thing is. The second one asks for all of it.

`parse_cfg` then walks the buffer, one descriptor at a time, using the length byte at the start of each one to find the next. It remembers whether the last interface it saw was a boot keyboard. The first interrupt IN endpoint it finds after one is the endpoint we want:

```c
} else if (buf[p + 1] == USB_DESC_INTERFACE) {
    usb_interface_desc_t *id = (void *)(buf + p);
    is_kbd = id->bInterfaceClass == USB_CLASS_HID && id->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
             id->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD;
    iface = id->bInterfaceNumber;
} else if (buf[p + 1] == USB_DESC_ENDPOINT) {
    usb_endpoint_desc_t *ed = (void *)(buf + p);
    if (is_kbd && (ed->bEndpointAddress & USB_EP_DIR_IN) && (ed->bmAttributes & 3) == USB_EP_TYPE_INTERRUPT) {
        // ... remember the endpoint
        return true;
    }
}
```

The subclass check is new. The HID spec only defines the protocol field for boot interfaces. A non-boot interface that happened to have a 1 there would have been taken for a keyboard, only to refuse our request to switch to boot protocol.

### Configuring the Endpoint

Once the keyboard's interface and endpoint are known, three things are left. A **Configure Endpoint** command tells the controller about the interrupt endpoint and gives it its own transfer ring. Then we send `SET_CONFIGURATION` to switch the device on. The order matters, since the spec says that sending `SET_CONFIGURATION` after a failed Configure Endpoint leads to undefined behavior (GatOS used to do it the other way around). Finally, two HID requests go to the keyboard's interface. `SET_PROTOCOL` switches it to the boot protocol. `SET_IDLE` with a rate of 0 tells it to only send a report when something changes.

The endpoint context for the interrupt endpoint has one field worth a closer look, the polling interval. The endpoint descriptor's `bInterval` is in milliseconds for full and low speed devices, but in powers of two of 125 µs "microframes" for faster ones, while xHCI always wants a power of two of microframes. `enum_dev` converts the first kind:

```c
if (spd == SPD_FS || spd == SPD_LS) {
    uint8_t b = s->ep_ival;
    if (b == 0) b = 1;
    uint8_t msb = 0;
    while (b > 1) { msb++; b >>= 1; }
    iv = msb + 3;
} else {
    iv = s->ep_ival ? s->ep_ival - 1 : 0;
}
```

A full speed keyboard asking for 10 ms gives `msb = 3` and `iv = 6`. 2⁶ microframes is 8 ms. Rounding down is fine here, since polling a little faster than asked never hurts. The `+ 3` is the conversion itself, because one millisecond is 8 = 2³ microframes.

The device is now fully set up. To actually receive a report, we queue a Normal TRB on the interrupt endpoint's ring, pointing at an 8-byte buffer, and ring its doorbell:

```c
static void arm_int(xhci_hc_t *hc, xhci_slot_t *s) {
    enq(&s->intr, RING_SZ, (uint32_t)s->hid_phys, (uint32_t)(s->hid_phys >> 32), 8, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_INTR(0));
    dbw(hc, s->id, s->ep_idx);
}
```

The controller will now poll the keyboard on its own, every 8 ms, for as long as it takes. The moment a key goes down, the report lands in our buffer and a transfer event lands on the event ring. Every time we handle one, we call `arm_int` again to queue the next.

## Waiting for the Controller

Every command and control transfer above ends with waiting for an event. GatOS waits in two different ways, depending on when it is asked.

`xhci_init` runs during bootstrap, well before the scheduler exists (and before interrupts are even on). At that point, `wait_ev` just polls the event ring in a loop with `wait_ev_poll`, until the event it wants shows up or a timeout passes. Hotplug happens later, though, in a thread (we will meet threads properly in Chapter 14). Spinning there would waste the CPU on a few milliseconds of USB traffic.

Once the scheduler is running, the waiting thread marks itself blocked and yields instead, and the MSI handler wakes it back up when the event it was waiting for arrives.

The handoff between the two sides goes through a small **completion** structure. One exists for commands and one for transfers:

```c
typedef struct {
    volatile int  done;
    trb_t         result;
    thread_t     *waiter;
    uint8_t       slot, ep; // which transfer we are waiting on (0, 0 for commands)
} xhci_completion_t;
```

`prepare_wait` clears it right before the doorbell is rung, while the interrupt handler's `proc_evts` fills it in. If the event arrives before the thread gets around to blocking, `done` is already set. In that case, `wait_ev` never sleeps at all. Clearing it *before* ringing the doorbell is what makes that safe. Clearing it after could wipe out an event that had already arrived.

### Whose Event Is It?

The `slot` and `ep` fields are new. They fix a bug that only shows up when two things happen at once.

Originally, a control transfer accepted the next transfer event that nobody else claimed, from any device. Usually that was the right one, since the driver only runs one control transfer at a time. But it is not the only thing producing transfer events. Pressing Caps Lock makes the interrupt handler send a `SET_REPORT` to the keyboard to update its LEDs.

The LED update is a control transfer too, ending in an event of its own. If you pressed Caps Lock while a second keyboard was being enumerated, the LED update's event could complete the *other* device's request. The enumeration would then carry on reading a descriptor that had not arrived yet.

The polling path had the same problem with hubs, whose status reports are transfer events too. Now every control transfer records which slot and endpoint it is waiting on. Only an event from that slot's endpoint 0 completes it:

```c
// Only the transfer someone is actually waiting on, LED updates complete into the void
if (!handled && slot_id == hc->xfer_comp.slot && ev_ep == hc->xfer_comp.ep)
    complete(&hc->xfer_comp, ev);
```

### The Interrupt Handler

The MSI handler itself is short. It drains the event ring and sorts each event by type:

* A **Command Completion** event fills in the command completion and wakes whoever is waiting on it.
* A **Transfer** event from a keyboard's interrupt endpoint is a fresh report. We decode it, then call `arm_int` to queue the next one. From a hub's interrupt endpoint, it means a port changed. We set a bit for the hotplug thread and wake it. If either one carries an error code, the endpoint has halted, and the hotplug thread gets to fix it (more on that below). Anything else is a control transfer, which goes through the check above.
* A **Port Status Change** event means something was plugged in or pulled out of a root port. We set the port's bit and wake the hotplug thread.

Once the ring is empty, we write the new dequeue pointer to `ERDP` and acknowledge the interrupt in the status register and in the interrupter's `IMAN` register. The end of `xhci_init` runs the same drain by hand, with a comment from my past self:

```c
// Author's Note: This is quite critical.
// Drain any stale events left from enumeration
// If IMAN IP is still set the controller won't
// generate a new MSI for future events, blocking hotplug forever
```

The controller only sends a new MSI when its Interrupt Pending bit goes from clear to set. Enumeration at boot leaves events sitting on the ring with that bit already set. If we never acknowledge it, the controller considers us "still busy" with the first interrupt, forever.

### When an Endpoint Halts

Not every transfer succeeds. A device can answer a request it does not support with a **STALL**, a cable can glitch in the middle of a packet, and a device that is being unplugged fails everything it is sent. When a transfer fails like that, the controller **halts** the endpoint. It marks the endpoint context as Halted, ignores its doorbell, and leaves every TRB after the failed one sitting on the ring.

The driver used to have no answer to this at all. One stalled request on endpoint 0 (a keyboard that does not support `SET_IDLE`, say, and some really do not) and every later control transfer to that device would simply time out. That includes the LED updates.

Getting an endpoint going again takes two commands. **Reset Endpoint** takes it out of the Halted state, and **Set TR Dequeue Pointer** tells the controller where on the ring to pick up again. We point it at our own enqueue pointer, which skips the failed TD along with anything queued behind it:

```c
static bool recover_ep(xhci_hc_t *hc, xhci_slot_t *s, uint8_t dci, ring_t *r) {
    if (!ep_halted(hc, s, dci)) return true;
    if (!cmd_ep(hc, 0, 0, TRB_RESET_EP, s->id, dci)) return false;

    // The failed TD (and anything queued behind it) is dead, so restart at our enqueue pointer
    uint64_t deq = r->phys + r->enq * sizeof(trb_t);
    return cmd_ep(hc, (uint32_t)deq | r->cyc, (uint32_t)(deq >> 32), TRB_SET_DEQ, s->id, dci);
}
```

`ep_halted` reads the endpoint's state from the controller's output context, which is one of the few times we ever look inside it. Like the ring pointers in the endpoint context, the new dequeue pointer carries the cycle bit in bit 0, so the controller knows which lap the ring is on.

`ctrl_xfer` calls this whenever a control transfer fails. To test it, I made enumeration send QEMU's keyboard a vendor request it does not understand. The request stalled and endpoint 0 recovered, and enumeration carried on to a keyboard that typed. With the recovery commented out, the very next request timed out.

Interrupt endpoints are trickier, because their events arrive in the interrupt handler, which cannot wait for commands. The handler only marks the slot with `needs_reset` and wakes the hotplug thread, which runs `recover_ep` on the interrupt endpoint, sends the device a `CLEAR_FEATURE(ENDPOINT_HALT)` and arms it again.

>[!NOTE]
>`CLEAR_FEATURE(ENDPOINT_HALT)` matters even when the device did not stall. Reset Endpoint zeroes the controller's data toggle (the bit USB 2 uses to spot repeated packets), and clearing the halt zeroes the device's toggle to match.

A device that is being unplugged fails every transfer until the port event arrives, which would turn this into an endless loop. After three resets in a row without a single good report, the thread gives up on the device and leaves it to the disconnect handling.

## From Reports to Keypresses

A boot keyboard report is 8 bytes:

```c
typedef struct {
    uint8_t modifiers; // Bit flags: LCTRL LSHIFT LALT LGUI RCTRL RSHIFT RALT RGUI
    uint8_t reserved;
    uint8_t keycodes[6];
} __attribute__((packed)) usb_hid_boot_report_t;
```

There is a big difference from PS/2 here. A PS/2 keyboard sends *events* ("A went down", "A went up"). A USB keyboard sends its *state*, meaning which modifiers are held and up to six other keys that are currently down. Holding Shift and A gives `02 00 04 00 00 00 00 00`, since `0x04` is the HID usage code for A. Letting go of both gives eight zeros.

The input hub from last chapter wants events. We have to turn states into events by comparing every report with the previous one. That is `usb_kbd_feed`, which, like `keyboard_feed` for PS/2, is a pure function with no I/O in it:

```c
int usb_kbd_feed(uint8_t prev[8], const uint8_t r[8], uint8_t *locks, key_event_t *out, int max) {
    // ...
    // Too many keys at once, every slot reads 0x01 (ErrorRollOver). Keep the last real state
    if (r[2] == 0x01) return 0;

    uint8_t nmod = 0, c = r[0] ^ prev[0];
    for (int i = 0; i < 8; i++) if (r[0] & (1 << i)) nmod |= mmap[i];

    for (int i = 0; i < 8 && n < max; i++) {
        if (c & (1 << i)) out[n++] = (key_event_t){ mods[i], (r[0] & (1 << i)) != 0, nmod, *locks };
    }

    // Keys in the old report but not the new one went up
    // ...
    // And keys in the new report but not the old one went down
    // ...
    kmemcpy(prev, r, 8);
    return n;
}
```

**What's happening here:**
* XORing the two modifier bytes gives exactly the modifiers that changed. Each one becomes a press or a release of its own key, such as `KEY_LEFT_SHIFT`.
* A key that was in the previous report but is missing from this one has been released. A key that is in this report but was missing from the previous one has just been pressed. Where it sits among the six slots does not matter.
* Usage codes go through `kmap`, a table from HID usages to the same `keycode_t` values the PS/2 driver produces. From here on, the input hub cannot tell which keyboard a key came from.
* Lock keys toggle their lock bit on the press. A USB keyboard has no typematic repeat (holding a key just keeps it in every report). The auto-repeat problem from the PS/2 driver never comes up here.

The first check deals with **rollover**. When more keys are held than the report has room for, the keyboard fills all six slots with `0x01`, ErrorRollOver, meaning "I can't tell you right now". The old code treated that like any other report.

Since none of the held keys appeared in it, every one of them was "released", then "pressed" again on the next real report. Mashing seven keys produced a burst of phantom releases and repeats. The HID spec says to ignore such a report and keep the last good state. This is what the early return does.

The interrupt handler feeds the events to `input_handle_key`, then updates the LEDs if a lock changed:

```c
// HID wants bit 0 = Num, bit 1 = Caps, bit 2 = Scroll
uint8_t hid_leds = ((usb_locks & LOCK_NUM) ? 1 : 0) | ((usb_locks & LOCK_CAPS) ? 2 : 0) | ((usb_locks & LOCK_SCROLL) ? 4 : 0);
```

>[!NOTE]
>Compare this with the PS/2 LED byte from the last chapter, where Scroll Lock is bit 0 and Caps Lock is bit 2. Same three lights, opposite order.

`update_leds` sends that byte as a `SET_REPORT` control transfer, built from the same three TRBs as any other. Since it runs inside the interrupt handler, it cannot wait for the result, and that result is the event the completion check above now throws away.

## Hubs and Hotplug

So far, every device has been plugged straight into a root port. Hubs add a layer. They are also where two of this chapter's bugs were hiding.

### Route Strings

When the controller sends something to a device behind a hub, it has to know the path. xHCI encodes the path in the slot context as a **route string**, 20 bits holding one 4-bit hub port number per tier, starting from the hub nearest the root. A device on port 2 of a hub that sits on root port 5 has a root port of 5 and a route string of `0x2`.

If that device were another hub with a keyboard on its port 4, the keyboard would get `0x42`. A device plugged straight into a root port has a route string of 0.

Building a child's route string means writing the port number into the first empty nibble:

```c
uint32_t xhci_route_child(uint32_t route, uint8_t port) {
    for (int shift = 0; shift < 20; shift += 4) {
        if (!(route & (0xFu << shift)))
            return route | ((uint32_t)(port > 15 ? 15 : port) << shift);
    }
    return route; // five hubs deep, USB does not allow more
}
```

Ports past 15 do not fit in a nibble. Linux caps them at 15, and so do we. Hub ports are numbered from 1, which leaves a nibble of 0 free to mean "end of route".

This is where the first of the two hub bugs was. The old code only built a route string for children of USB 3 hubs. A keyboard behind any USB 2 or USB 1 hub got route string 0, and the controller thought it was plugged into the root port, where the hub itself was. Address Device then failed.

I found it the way I find most of these, by reading the code to explain it and getting stuck on a sentence that did not make sense. Booting QEMU with a keyboard behind a hub (`-device usb-hub,bus=xhci.0,port=1 -device usb-kbd,bus=xhci.0,port=1.2`) confirmed it with a Transaction Error.

The hub's own slot context was also missing its **Hub** flag and its port count, both of which the controller needs before it can address anything behind the hub. `enum_dev` now sends both in a Configure Endpoint command, before it enumerates the hub's ports.

### Talking to a Hub

A hub is a USB device like any other, with a class of its own (9), and it gets enumerated the same way up to its configuration.

After that come a few class requests of its own. A USB 3 hub first gets `SET_HUB_DEPTH`, which tells it how many hubs sit above it, so it knows which nibble of a route string belongs to it. Then we read the hub descriptor (for the number of ports), send `SET_FEATURE(PORT_POWER)` to each port, then ask every port for its status with `GET_STATUS`. Hub ports are reset with `SET_FEATURE(PORT_RESET)` instead of a `PORTSC` write, and their status tells us the device's speed. Then we recurse into `enum_dev` with the child's route string.

Hubs are the one place where `SET_CONFIGURATION` still goes first. The spec wants the hub fields (the **Hub** flag, the port count and, for high speed hubs, the TT think time) in the very first Configure Endpoint, and the port count comes from the hub descriptor, which is a class request we only send to a configured hub. So a hub gets configured, then described, then handed to the controller.

Every hub port also has **change bits** in its status, such as "connection changed" and "reset finished", each with its own feature selector (and USB 3 hubs moved a few of them around). They stay set until the OS clears them with `CLEAR_FEATURE`. As long as any of them is set, the hub keeps reporting that port on its own interrupt endpoint.

That was the second hub bug. The boot path never cleared them. As soon as the hub's interrupt endpoint was armed, the hub reported its ports as changed. The hotplug thread saw "connection changed" with a device present, and did what it should do for a new device.

The thread reset the port (knocking the working keyboard off its address) and enumerated it again into a second slot. Every port status read now goes through `hub_port_status`, which clears whatever change bits it saw.

>[!NOTE]
>For full and low speed devices behind a USB 2 hub, there is one more twist. The hub talks high speed upstream, translating for the slow device with its **Transaction Translator** (TT). The device's slot context names the hub's slot and port as its TT. This is what `tt_slot` and `tt_port` are for.

### Hotplug

Plugging a keyboard in after boot shows up as a Port Status Change event (for a root port) or a hub status report (for a hub port). Neither is something we want to handle inside an interrupt handler, since enumeration takes dozens of commands and transfers, each one waiting on an event that the very same handler has to deliver.

Instead, the handler only sets a bit (in `pending_ports` or `pending_hub_slots`) and wakes the **hotplug worker**. That is a kernel thread that `xhci_hotplug_init` starts once the scheduler is running. The worker takes the pending bits and handles each port, then blocks again when there is nothing left:

```c
bool iflag = intr_save();
if (!any_pending() && hotplug_thread) {
    worker_idle = true;
    hotplug_thread->state = T_BLOCKED;
    intr_restore(iflag);
    sched_yield();
    worker_idle = false;
} else {
    intr_restore(iflag); // more work arrived while we were processing, loop again
}
```

The check and the switch to blocked happen with interrupts off. Otherwise, an interrupt could set a pending bit right after we checked. We would then go to sleep on top of work.

For a root port, the worker acknowledges the change bits, then looks at the connection bit. For a new connection, it waits 100 ms for the connector to settle (the USB spec calls this debouncing, since contacts bounce for a moment when they touch), then resets and enumerates the device like at boot.

For a disconnection, it disables every slot hanging off that port and frees their memory. Hub ports work the same way. A disconnect there tears down every slot whose route string starts with that port, which means a hub unplugged with a keyboard behind it takes the keyboard along with it.

## Putting It All Together

Here is the part of `bootstrap.c` this chapter covers:

```c
// PCI and USB for external keyboards
pci_init();
if (xhci_init()) {
    // ...
}

// Enable multitasking and userspace
process_init();
sched_init();
xhci_hotplug_init();
```

`pci_init` only prints a line, since the scan happens inside `xhci_init` when it looks for controllers. For every controller found, `xhci_init` enables it on the bus, maps BAR0, takes it from the firmware, resets it, builds its rings and contexts and starts it.

`xhci_init` then enumerates whatever is already plugged in, by polling, since there is no scheduler yet and interrupts are still off. Only after that does it register the interrupt handler, point the controller's MSI (or MSI-X) at the LAPIC, drain the events left over from enumeration and arm every keyboard's interrupt endpoint.

`xhci_hotplug_init` has to wait until after `sched_init`, since the worker is a thread. From then on, the worker enumerates anything plugged in later, with waits that block instead of spinning.

Each step builds on the one before. PCI's configuration space tells us where the controller is and gives us the BAR. The VMM maps it as uncacheable device memory, the same way Chapter 10 mapped the LAPIC. The PMM hands out the physically contiguous pages every ring and context lives in, while the physmap lets the driver write to them as the controller reads them through DMA.

MSI lets the controller reach the LAPIC directly, without any of the I/O APIC routing we cannot do without an AML interpreter. Finally, the input hub from Chapter 11 takes the events. From there on, a key from a USB keyboard and a key from a PS/2 keyboard are the same thing.

We now have two keyboards, text input, system calls and a way to route every keypress. What we still lack is a decent way to *show* things. The console has been quietly drawing every log line in this book, but we have never looked at how.

The next chapter opens it up, along with the TTYs behind Alt+Tab, the line discipline that turns keypresses into lines, and the escape sequences that let a program paint the screen.
