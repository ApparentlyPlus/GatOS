/*
 * pci.c - PCI/PCIe Subsystem Implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/pci.h>
#include <arch/x86_64/cpu/io.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>

#pragma region I dont even know bro

static void cfg_sel(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    uint32_t addr = 0x80000000 | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC);
    outl(PCI_CFG_ADDR, addr);
}

static uint8_t r8(uint8_t b, uint8_t d, uint8_t f, uint8_t o) {
    cfg_sel(b, d, f, o);
    return inb(PCI_CFG_DATA + (o & 3));
}

static uint16_t r16(uint8_t b, uint8_t d, uint8_t f, uint8_t o) {
    cfg_sel(b, d, f, o);
    return inw(PCI_CFG_DATA + (o & 2));
}

static uint32_t r32(uint8_t b, uint8_t d, uint8_t f, uint8_t o) {
    cfg_sel(b, d, f, o);
    return inl(PCI_CFG_DATA);
}

static void w16(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint16_t v) {
    cfg_sel(b, d, f, o);
    outw(PCI_CFG_DATA + (o & 2), v);
}

static void w32(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v) {
    cfg_sel(b, d, f, o);
    outl(PCI_CFG_DATA, v);
}

uint16_t pci_read16(pci_dev_t *d, uint8_t o) { return r16(d->bus, d->dev, d->func, o); }
uint32_t pci_read32(pci_dev_t *d, uint8_t o) { return r32(d->bus, d->dev, d->func, o); }
void pci_write16(pci_dev_t *d, uint8_t o, uint16_t v) { w16(d->bus, d->dev, d->func, o, v); }
void pci_write32(pci_dev_t *d, uint8_t o, uint32_t v) { w32(d->bus, d->dev, d->func, o, v); }

#pragma region Actual code

/*
 * decode_bar - Decodes a PCI BAR (Base Address Register)
 */
static bool decode_bar(uint8_t b, uint8_t d, uint8_t f, uint8_t idx, uint64_t *phys, uint64_t *sz) {
    uint8_t off = PCI_BAR0 + idx * 4;
    uint32_t lo = r32(b, d, f, off);

    if (lo & 1) return false;

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
    return true;
}

/*
 * find_cap - Searches the PCI capability list for a given capability ID
 */
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

/*
 * pci_init - Just logs lol
 */
void pci_init(void) {
    LOGF("[PCI] Subsystem initialized\n");
}

/*
 * pci_find_xhci - Finds all xHCI USB controllers (takes a moment)
 */
int pci_find_xhci(pci_dev_t *out_arr, int max_out) {
    int found = 0;
    for (int b = 0; b < 256; b++) {
        for (int d = 0; d < 32; d++) {
            if (r16(b, d, 0, PCI_VENDOR_ID) == 0xFFFF) continue;

            int max_f = (r8(b, d, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
            for (int f = 0; f < max_f; f++) {
                if (r16(b, d, f, PCI_VENDOR_ID) == 0xFFFF) continue;

                if (r8(b, d, f, PCI_CLASS) != PCI_CLASS_USB ||
                    r8(b, d, f, PCI_SUBCLASS) != PCI_SUBCLASS_USB ||
                    r8(b, d, f, PCI_PROG_IF) != PCI_PROGIF_XHCI) continue;

                if (found >= max_out) {
                    LOGF("[PCI] Too many xHCI controllers, skipping remaining\n");
                    return found;
                }

                pci_dev_t *out = &out_arr[found];
                if (!decode_bar(b, d, f, 0, &out->bar0_phys, &out->bar0_size)) {
                    LOGF("[PCI] xHCI BAR0 decode failed\n");
                    continue;
                }

                out->bus = b;
                out->dev = d;
                out->func = f;
                out->vendor_id = r16(b, d, f, PCI_VENDOR_ID);
                out->device_id = r16(b, d, f, PCI_DEVICE_ID);
                out->class_code = PCI_CLASS_USB;
                out->subclass = PCI_SUBCLASS_USB;
                out->prog_if = PCI_PROGIF_XHCI;
                out->int_line = r8(b, d, f, PCI_INT_LINE);
                out->msi_cap = find_cap(b, d, f, PCI_CAP_MSI);
                out->msix_cap = find_cap(b, d, f, PCI_CAP_MSIX);

                LOGF("[PCI] xHCI at %02x:%02x.%x BAR0=0x%lx (%lu B) MSI=0x%02x MSI-X=0x%02x\n",
                     b, d, f, out->bar0_phys, out->bar0_size, out->msi_cap, out->msix_cap);
                found++;
            }
        }
    }
    return found;
}

/*
 * pci_decode_bar - Address and size of one of a device's memory BARs
 */
bool pci_decode_bar(pci_dev_t *d, uint8_t idx, uint64_t *phys, uint64_t *sz) {
    return decode_bar(d->bus, d->dev, d->func, idx, phys, sz);
}

/*
 * pci_enable - Enables a PCI device
 */
void pci_enable(pci_dev_t *d) {
    uint16_t cmd = pci_read16(d, PCI_COMMAND);
    cmd |= (PCI_CMD_MEM | PCI_CMD_BUS_MASTER | PCI_CMD_INT_DISABLE);
    cmd &= ~PCI_CMD_IO;
    pci_write16(d, PCI_COMMAND, cmd);
}

/*
 * cfg_msix - Points entry 0 of the MSI-X table at the LAPIC, used when there is no plain MSI
 */
static bool cfg_msix(pci_dev_t *d, uint8_t vec, uint32_t addr) {
    uint8_t c = d->msix_cap;
    uint32_t tbl = pci_read32(d, c + PCI_MSIX_TABLE);
    uint8_t bir = tbl & 7;
    if (bir > 5) return false;

    // The table lives inside one of the device's own BARs
    uint8_t off = PCI_BAR0 + bir * 4;
    uint32_t lo = pci_read32(d, off);
    if (lo & 1) return false;
    uint64_t bar = lo & 0xFFFFFFF0;
    if (((lo >> 1) & 3) == 2) bar |= (uint64_t)pci_read32(d, off + 4) << 32;

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

    LOGF("[PCI] MSI-X cfg: vec=0x%02x\n", vec);
    return true;
}

bool pci_cfg_msi(pci_dev_t *d, uint8_t vec, uint32_t lapic_id) {
    uint32_t addr = 0xFEE00000 | ((lapic_id & 0xFF) << 12);
    if (!d->msi_cap) return d->msix_cap && cfg_msix(d, vec, addr);
    uint8_t c = d->msi_cap;

    uint16_t ctrl = pci_read16(d, c + PCI_MSI_CTRL);

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

    LOGF("[PCI] MSI cfg: vec=0x%02x lapic=%u\n", vec, lapic_id);
    return true;
}
