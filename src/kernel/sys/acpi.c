/*
 * acpi.c - ACPI (Advanced Configuration and Power Interface) related functions.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/multiboot2.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/acpi.h>
#include <kernel/debug.h>
#include <libc/string.h>

static RSDP2Descriptor* g_rsdp = NULL;
static void* g_root_sdt = NULL;
static bool g_xsdt_supported = false;

/*
 * acpi_validate_rsdp - Validate the RSDP structure by checking its checksum
 */
bool acpi_validate_rsdp(RSDP2Descriptor* rsdp) {
    if (!rsdp)
        return false;

    size_t length = (rsdp->Revision < 2) ? sizeof(RSDPDescriptor) : rsdp->Length;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += ((uint8_t*)rsdp)[i];
    return sum == 0;
}

/*
 * acpi_find_rsdp - Locate and validate the RSDP structure from multiboot info
 */
RSDP2Descriptor* acpi_find_rsdp(multiboot_parser_t* parser) {
    if (!parser || !parser->initialized || !parser->info)
        return NULL;

    multiboot_acpi_t* acpi_tag = multiboot_get_acpi_rsdp(parser);
    if (!acpi_tag)
        return NULL;

    if (acpi_tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW) {
        RSDP2Descriptor* rsdp2 = (RSDP2Descriptor*)acpi_tag->rsdp;
        return acpi_validate_rsdp(rsdp2) ? rsdp2 : NULL;
    } else if (acpi_tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD) {
        RSDPDescriptor* rsdp1 = (RSDPDescriptor*)acpi_tag->rsdp;
        static RSDP2Descriptor rsdp2;

        /* Convert RSDP 1.0 → RSDP 2.0 */
        memcpy(rsdp2.Signature, rsdp1->Signature, 8);
        rsdp2.Checksum = rsdp1->Checksum;
        memcpy(rsdp2.OEMID, rsdp1->OEMID, 6);
        rsdp2.Revision = rsdp1->Revision;
        rsdp2.RsdtAddress = rsdp1->RsdtAddress;

        /* Fill missing fields */
        rsdp2.Length = sizeof(RSDP2Descriptor);
        rsdp2.XsdtAddress = 0;
        rsdp2.ExtendedChecksum = 0;
        memset(rsdp2.Reserved, 0, sizeof(rsdp2.Reserved));

        return acpi_validate_rsdp((RSDP2Descriptor*)rsdp1) ? &rsdp2 : NULL;
    }

    return NULL;
}

/*
 * acpi_find_root_sdt - Locate the Root System Description Table (RSDT or XSDT)
 */
void* acpi_find_root_sdt(RSDP2Descriptor* rsdp) {
    if (!rsdp)
        return NULL;

    if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
        /* ACPI 2.0+ → use XSDT */
        g_xsdt_supported = true;
        return (void*)(uintptr_t)rsdp->XsdtAddress;
    } else {
        /* ACPI 1.0 → use RSDT */
        g_xsdt_supported = false;
        return (void*)(uintptr_t)rsdp->RsdtAddress;
    }
}

/*
 * acpi_get_nth_sdt_from_root - Get the N-th SDT from the root SDT (RSDT or XSDT)
 */
ACPISDTHeader* acpi_get_nth_sdt_from_root(void* root_sdt, size_t index) {
    if (!root_sdt)
        return NULL;

    if (g_xsdt_supported)
        return (ACPISDTHeader*)(uintptr_t)((XSDT*)root_sdt)->sdt_addresses[index];
    else
        return (ACPISDTHeader*)(uintptr_t)((RSDT*)root_sdt)->sdt_addresses[index];
}

/*
 * acpi_init - Initialize ACPI by locating and validating the RSDP and root SDT
 */
bool acpi_init(multiboot_parser_t* parser) {
    g_rsdp = acpi_find_rsdp(parser);
    if (!g_rsdp) {
        panic("Failed to find valid RSDP.\n");
        return false;
    }

    g_root_sdt = acpi_find_root_sdt(g_rsdp);
    if (!g_root_sdt) {
        panic("Failed to locate Root SDT (RSDT/XSDT).\n");
        return false;
    }

    return true;
}

/*
 * acpi_get_rsdp - Get the cached RSDP pointer
 */
RSDP2Descriptor* acpi_get_rsdp(void) {
    return g_rsdp;
}

/*
 * acpi_get_root_sdt - Get the cached root SDT pointer (RSDT or XSDT)
 */
void* acpi_get_root_sdt(void) {
    return g_root_sdt;
}

/*
 * acpi_is_xsdt_supported - Check if XSDT is supported (ACPI 2.0+)
 */
bool acpi_is_xsdt_supported(void) {
    return g_xsdt_supported;
}
