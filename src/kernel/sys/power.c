/*
 * power.c - Kernel Power Management
 *
 * This file implements system reboot and shutdown functionality using ACPI
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/cpu/io.h>
#include <kernel/sys/power.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <klibc/string.h>

#define SLP_EN (1 << 13)

/*
 * power_get_s5 - Parse _S5_ from the DSDT to get the sleep type values for ACPI shutdown
 */
static bool power_get_s5(uint16_t* slp_typa, uint16_t* slp_typb) {
    acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_find_table("FACP");
    if (!fadt) return false;

    uint64_t dsdt_phys = (fadt->header.Length >= 148 && fadt->x_dsdt != 0) ? fadt->x_dsdt : (uint64_t)fadt->dsdt;

    // Map the DSDT and search for the _S5_ sleep state package
    ACPISDTHeader* dsdt_header = (ACPISDTHeader*)acpi_map_phys(dsdt_phys, sizeof(ACPISDTHeader));
    if (!dsdt_header) {
        acpi_unmap_phys(fadt);
        return false;
    }

    uint32_t dsdt_len = dsdt_header->Length;
    acpi_unmap_phys(dsdt_header);

    uint8_t* dsdt = (uint8_t*)acpi_map_phys(dsdt_phys, dsdt_len);
    if (!dsdt) {
        acpi_unmap_phys(fadt);
        return false;
    }

    uint8_t* ptr = dsdt + sizeof(ACPISDTHeader);
    uint8_t* end = dsdt + dsdt_len;
    bool found = false;

    while (ptr < end - 4) {
        if (kmemcmp(ptr, "_S5_", 4) == 0) { found = true; break; }
        ptr++;
    }

    if (!found) goto cleanup;

    ptr += 4;
    if (*ptr != 0x12) goto cleanup; // PackageOp
    ptr++;

    // Skip PkgLength
    if ((*ptr & 0xC0) == 0) ptr += 1;
    else ptr += ((*ptr & 0xC0) >> 6) + 1;

    uint8_t num_elements = *ptr++;
    if (num_elements == 0) goto cleanup;

    if (*ptr == 0x0A) ptr++; // BytePrefix
    *slp_typa = (uint16_t)(*ptr++) << 10;

    if (num_elements > 1) {
        if (*ptr == 0x0A) ptr++;
        *slp_typb = (uint16_t)(*ptr) << 10;
    } else {
        *slp_typb = 0;
    }

    acpi_unmap_phys(dsdt);
    acpi_unmap_phys(fadt);
    return true;

cleanup:
    acpi_unmap_phys(dsdt);
    acpi_unmap_phys(fadt);
    return false;
}

/*
 * reboot - Attempt to reboot the system using multiple methods (ACPI, PS/2 controller, triple fault)
 */
void reboot(void) {
    LOGF("[POWER] Initiating system reboot...\n");

    // let's not get preemted
    intr_off();

    // Port 0xCF9, PIIX4/ICH9 reset register
    // supposedly works on QEMU but I don't see it
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();

    // ACPI FADT reset register, the most robust method
    // this usually works on real hardware
    acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_find_table("FACP");
    if (fadt && fadt->header.Revision >= 2 && (fadt->flags & (1 << 10))) {
        if (fadt->reset_reg.address_space == 1) // I/O space
            outb((uint16_t)fadt->reset_reg.address, fadt->reset_value);
    }

    // PS/2 controller reset, works on some hardware but is generally unreliable
    for (int i = 0; i < 0x10000; i++) {
        if (!(inb(0x64) & 0x02)) break;
        io_wait();
    }
    outb(0x64, 0xFE);
    io_wait();

    // Triple fault, last resort
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(idtr));

    for(;;);
}

/*
 * power_off - Attempt to power off the system using ACPI
 */
void power_off(void) {
    LOGF("[POWER] Initiating system shutdown...\n");

    uint16_t slp_typa, slp_typb;
    if (!power_get_s5(&slp_typa, &slp_typb)) {
        LOGF("[POWER] Shutdown failed: _S5 not found.\n");
        return;
    }

    acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_find_table("FACP");
    if (!fadt) return;

    // Enable ACPI mode if not already active
    if (fadt->smi_cmd && fadt->acpi_enable) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
        for (int i = 0; i < 1000; i++) {
            if (inw((uint16_t)fadt->pm1a_cnt_blk) & 1) break;
            io_wait();
        }
    }

    outw((uint16_t)fadt->pm1a_cnt_blk, slp_typa | SLP_EN);
    if (fadt->pm1b_cnt_blk)
        outw((uint16_t)fadt->pm1b_cnt_blk, slp_typb | SLP_EN);

    LOGF("[POWER] Shutdown failed.\n");
    for(;;);
}
