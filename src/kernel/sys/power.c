/*
 * power.c - Kernel Power Management
 *
 * This file implements system reboot, shutdown, and RAPL power measurement.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/cpu/interrupts.h>
#include <arch/x86_64/cpu/io.h>
#include <arch/x86_64/cpu/cpu.h>
#include <arch/x86_64/cpu/msr.h>
#include <kernel/sys/power.h>
#include <kernel/sys/timers.h>
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

// rapl decls
typedef enum {
    RAPL_NONE = 0,
    RAPL_INTEL,
    RAPL_AMD,
} rapl_vendor_t;

// globals
static rapl_vendor_t rapl_vendor = RAPL_NONE;
static uint32_t rapl_esu = 0;
static uint32_t rapl_prev = 0;
static uint64_t rapl_prev_ms = 0;
static bool rapl_primed = false;

/*
 * power_rapl_init - Detect and initialise RAPL energy counters
 */
void power_rapl_init(void) {
    const char *vendor = cpu_get_info()->vendor;
    bool is_intel = (kstrcmp(vendor, "GenuineIntel") == 0);
    bool is_amd = (kstrcmp(vendor, "AuthenticAMD") == 0);

    if (is_intel) {
        uint32_t a, b, c, d;
        cpuid(6, 0, &a, &b, &c, &d);
        if (!(a & (1u << 3))) {
            LOGF("[POWER] RAPL not advertised by CPUID.06H (Intel).\n");
            return;
        }
        uint64_t unit_msr = read_msr(MSR_RAPL_POWER_UNIT);
        rapl_esu = (uint32_t)((unit_msr >> 8) & 0x1F);
        rapl_vendor = RAPL_INTEL;
        LOGF("[POWER] Intel RAPL ready. ESU=%u\n", rapl_esu);

    } else if (is_amd) {
        uint64_t unit_msr = read_msr(MSR_AMD_ENERGY_UNIT);
        rapl_esu = (uint32_t)((unit_msr >> 8) & 0x1F);
        rapl_vendor = RAPL_AMD;
        LOGF("[POWER] AMD RAPL ready. ESU=%u\n", rapl_esu);

    } else {
        LOGF("[POWER] RAPL not supported (vendor=%s).\n", vendor);
    }
}

/*
 * power_rapl_available - Returns true if RAPL was initialised
 */
bool power_rapl_available(void) {
    return rapl_vendor != RAPL_NONE;
}

/*
 * power_avg_watts - Average package power since last call
 */
uint32_t power_avg_watts(void) {
    if (rapl_vendor == RAPL_NONE) return 0;

    uint32_t msr_addr = (rapl_vendor == RAPL_AMD) ? MSR_AMD_PKG_ENERGY : MSR_PKG_ENERGY_STATUS;

    uint32_t raw = (uint32_t)(read_msr(msr_addr) & 0xFFFFFFFF);
    uint64_t now_ms = get_uptime_ms();

    if (!rapl_primed) {
        rapl_prev = raw;
        rapl_prev_ms = now_ms;
        rapl_primed = true;
        return 0;
    }

    uint32_t delta_raw = raw - rapl_prev;
    uint64_t delta_ms = now_ms - rapl_prev_ms;

    rapl_prev = raw;
    rapl_prev_ms = now_ms;

    if (delta_ms == 0 || rapl_esu == 0 || rapl_esu >= 32) return 0;

    uint64_t denom = delta_ms * ((uint64_t)1u << rapl_esu);
    if (denom == 0) return 0;

    uint64_t watts_x10 = ((uint64_t)delta_raw * 10000ULL) / denom;
    if (watts_x10 > 9999) watts_x10 = 9999;
    return (uint32_t)watts_x10;
}
