/*
 * pci.h - PCI/PCIe Subsystem
 *
 * Legacy I/O config space access, device enumeration, BAR decoding, and MSI.
 * 
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PCI_CFG_ADDR        0xCF8
#define PCI_CFG_DATA        0xCFC

#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_CAP_PTR         0x34
#define PCI_INT_LINE        0x3C
#define PCI_INT_PIN         0x3D

#define PCI_CMD_IO          (1u << 0)
#define PCI_CMD_MEM         (1u << 1)
#define PCI_CMD_BUS_MASTER  (1u << 2)
#define PCI_CMD_INT_DISABLE (1u << 10)

#define PCI_STAT_CAP_LIST   (1u << 4)

#define PCI_CAP_MSI         0x05
#define PCI_CAP_MSIX        0x11

#define PCI_MSI_CTRL        0x02
#define PCI_MSI_ADDR_LO     0x04
#define PCI_MSI_ADDR_HI     0x08
#define PCI_MSI_DATA_32     0x08
#define PCI_MSI_DATA_64     0x0C
#define PCI_MSI_EN          (1u << 0)
#define PCI_MSI_64BIT       (1u << 7)

#define PCI_CLASS_USB       0x0C
#define PCI_SUBCLASS_USB    0x03
#define PCI_PROGIF_UHCI     0x00
#define PCI_PROGIF_OHCI     0x10
#define PCI_PROGIF_EHCI     0x20
#define PCI_PROGIF_XHCI     0x30

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if;
    uint64_t bar0_phys;
    uint32_t bar0_size;
    uint8_t int_line;
    uint8_t msi_cap;
} pci_dev_t;

void pci_init(void);
int pci_get_xhci_controllers(pci_dev_t *out_arr, int max_out);

uint8_t pci_read8(pci_dev_t *d, uint8_t off);
uint16_t pci_read16(pci_dev_t *d, uint8_t off);
uint32_t pci_read32(pci_dev_t *d, uint8_t off);
void pci_write8(pci_dev_t *d, uint8_t off, uint8_t v);
void pci_write16(pci_dev_t *d, uint8_t off, uint16_t v);
void pci_write32(pci_dev_t *d, uint8_t off, uint32_t v);

void pci_enable(pci_dev_t *d);
bool pci_cfg_msi(pci_dev_t *d, uint8_t vec, uint32_t lapic_id);
