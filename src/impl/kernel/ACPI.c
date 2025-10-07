/*
 * ACPI.c - ACPI (Advanced Configuration and Power Interface) related functions.
 *
 * Author: u/ApparentlyPlus
 */

#include <multiboot2.h>
#include <libc/string.h>
#include <sys/ACPI.h>
#include <sys/panic.h>
#include <debug.h>

static RSDP2Descriptor* g_rsdp = nullptr;
static void* g_root_sdt = nullptr;
static bool is_xsdt_supported = false;

/*
 * acpi_init - Initializes the ACPI subsystem.
 * Detects RSDP, validates it, determines whether XSDT is supported,
 * and loads the root SDT for later use.
 */
bool acpi_init(multiboot_parser_t* parser) {
    g_rsdp = getRSDP(parser);
    if (!g_rsdp) {
        panic("Failed to find valid RSDP.\n");
        return false;
    }

    g_root_sdt = getRootSDT(g_rsdp);

    if (!g_root_sdt) {
        panic("Failed to locate Root SDT (RSDT/XSDT).\n");
        return false;
    }

    return true;
}

/*
 * validate_RSDP - Validates the RSDP structure by checking its checksum.
 */
bool validate_RSDP(RSDP2Descriptor* rsdp) {
    if (!rsdp)
        return false;

    size_t len = (rsdp->Revision < 2) ? sizeof(RSDPDescriptor) : rsdp->Length;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += ((uint8_t*)rsdp)[i];
    return sum == 0;
}

/*
 * getRSDP - Retrieves the RSDP (Root System Description Pointer) from the multiboot2 parser.
 * Supports both ACPI 1.0 and ACPI 2.0+ by converting ACPI 1.0 RSDP to ACPI 2.0 format.
 */
RSDP2Descriptor* getRSDP(multiboot_parser_t* parser) {

    if (!parser || !parser->initialized || !parser->info) {
        return nullptr;
    }

    multiboot_acpi_t* acpi_tag = multiboot_get_acpi_rsdp(parser);
    if (!acpi_tag)
        return nullptr;

    if (acpi_tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW) {
        RSDP2Descriptor* rsdp2 = (RSDP2Descriptor*)acpi_tag->rsdp;
        return validate_RSDP(rsdp2) ? rsdp2 : nullptr;
    } 
    else if (acpi_tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD) {
        RSDPDescriptor* rsdp1 = (RSDPDescriptor*)acpi_tag->rsdp;
        static RSDP2Descriptor rsdp2;

        // Convert RSDP 1.0 → RSDP 2.0 format
        memcpy(rsdp2.Signature, rsdp1->Signature, 8);
        rsdp2.Checksum = rsdp1->Checksum;
        memcpy(rsdp2.OEMID, rsdp1->OEMID, 6);
        rsdp2.Revision = rsdp1->Revision;
        rsdp2.RsdtAddress = rsdp1->RsdtAddress;

        // Fill missing fields
        rsdp2.Length = sizeof(RSDP2Descriptor);
        rsdp2.XSDTAddress = 0;
        rsdp2.ExtendedChecksum = 0;
        memset(rsdp2.Reserved, 0, sizeof(rsdp2.Reserved));

        // Validate using only the 1.0 portion
        return validate_RSDP((RSDP2Descriptor*)rsdp1) ? &rsdp2 : nullptr;
    }

    return nullptr;
}

/*
 * getRootSDT - Chooses between RSDT and XSDT depending on ACPI revision.
 * Returns pointer to the root SDT header (either RSDT or XSDT).
 */
void* getRootSDT(RSDP2Descriptor* rsdp) {
    if (!rsdp)
        return nullptr;

    if (rsdp->Revision >= 2 && rsdp->XSDTAddress != 0) {
        is_xsdt_supported = true;

        // ACPI 2.0+ → use XSDT (64-bit pointer)
        return (void*)(uintptr_t)rsdp->XSDTAddress;
    } else {
        is_xsdt_supported = false;

        // ACPI 1.0 → use RSDT (32-bit pointer)
        return (void*)(uintptr_t)rsdp->RsdtAddress;
    }
}

/*
 * acpi_is_xsdt_supported - Returns whether the system supports XSDT (ACPI 2.0+).
 */
bool acpi_is_xsdt_supported() {
    return is_xsdt_supported;
}

/*
 * get_nth_sdt - Retrieves the N-th SDT (System Description Table) from the root SDT.
 * Supports both RSDT and XSDT based on system capabilities.
 */
ACPISDTHeader* get_nth_sdt(void* root_sdt, size_t n) {
    ACPISDTHeader* header = (ACPISDTHeader*)(is_xsdt_supported ? 
        ((XSDT*)root_sdt)->sdtAddresses[n] : ((RSDT*)root_sdt)->sdtAddresses[n]);

    return (ACPISDTHeader*)header;
}


/*
 * acpi_get_rsdp - Returns pointer to the current RSDP.
 */
RSDP2Descriptor* acpi_get_rsdp() {
    return g_rsdp;
}

/*
 * acpi_get_root_sdt - Returns pointer to the current Root SDT (RSDT/XSDT).
 */
void* acpi_get_root_sdt() {
    return g_root_sdt;
}