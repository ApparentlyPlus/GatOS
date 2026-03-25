/*
 * acpi.c - ACPI (Advanced Configuration and Power Interface) related functions.
 *
 * This implementation handles locating the RSDP, finding the Root SDT (RSDT/XSDT),
 * and iterating through ACPI tables. It uses the VMM to safe virtual memory mapping
 * for ACPI tables, ensuring they are mapped into the dynamic kernel region.
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/multiboot2.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/sys/panic.h>
#include <kernel/sys/acpi.h>
#include <kernel/memory/vmm.h>
#include <kernel/debug.h>
#include <klibc/string.h>

static RSDP2Descriptor* rsdp = NULL;
static uint64_t rsdt_phys = 0; // Physical address of RSDT/XSDT
static void* rsdt_virt = NULL; // Mapped virtual address
static bool xsdt_ok = false;

/*
 * acpi_map_phys - Map a physical address to a virtual one using vmm_alloc
 * This ensures the address is mapped in a safe, non-conflicting region of kernel memory.
 */
static void* acpi_map_phys(uint64_t phys_addr, size_t size) {
    if (phys_addr == 0) return NULL;

    void* virt_addr = NULL;

    uint64_t page_offset = phys_addr & (PAGE_SIZE - 1);
    uint64_t base_phys = phys_addr - page_offset;
    size_t map_size = align_up(size + page_offset, PAGE_SIZE);

    // VM_FLAG_MMIO ensures we treat this as device memory
    // MMIO dude, I hate that we need the vmm for this dammit
    vmm_status_t status = vmm_alloc(NULL, map_size, VM_FLAG_WRITE | VM_FLAG_MMIO, (void*)base_phys, &virt_addr);

    if (status != VMM_OK) {
        LOGF("[ACPI ERROR] Failed to map physical address 0x%lx (Status: %d)\n", phys_addr, status);
        return NULL;
    }

    return (void*)((uintptr_t)virt_addr + page_offset);
}

/*
 * acpi_unmap_phys - Unmap a previously mapped physical region
 */
static void acpi_unmap_phys(void* virt) {
    if (!virt) return;

    void* base_virt = (void*)((uintptr_t)virt & ~(PAGE_SIZE - 1));

    vmm_free(NULL, base_virt);
}

/*
 * acpi_validate_rsdp - Validate the RSDP structure by checking its checksum
 */
bool acpi_validate_rsdp(RSDP2Descriptor* rsdp) {
    if (!rsdp) return false;

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

        kmemcpy(rsdp2.Signature, rsdp1->Signature, 8);
        rsdp2.Checksum = rsdp1->Checksum;
        kmemcpy(rsdp2.OEMID, rsdp1->OEMID, 6);
        rsdp2.Revision = rsdp1->Revision;
        rsdp2.RsdtAddress = rsdp1->RsdtAddress;
        rsdp2.Length = sizeof(RSDP2Descriptor);
        rsdp2.XsdtAddress = 0;
        rsdp2.ExtendedChecksum = 0;
        kmemset(rsdp2.Reserved, 0, sizeof(rsdp2.Reserved));

        return acpi_validate_rsdp((RSDP2Descriptor*)rsdp1) ? &rsdp2 : NULL;
    }

    return NULL;
}

/*
 * acpi_init - Initialize ACPI by locating and validating the RSDP and root SDT
 */
bool acpi_init(multiboot_parser_t* parser) {
    rsdp = acpi_find_rsdp(parser);
    if (!rsdp) {
        panic("Failed to find valid RSDP.\n");
        return false;
    }

    if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
        xsdt_ok = true;
        rsdt_phys = rsdp->XsdtAddress;
    } else {
        xsdt_ok = false;
        rsdt_phys = (uint64_t)rsdp->RsdtAddress;
    }

    ACPISDTHeader* header = (ACPISDTHeader*)acpi_map_phys(rsdt_phys, sizeof(ACPISDTHeader));
    if (!header) {
        panic("Failed to map Root SDT Header.");
    }

    uint32_t total_length = header->Length;
    acpi_unmap_phys(header);

    rsdt_virt = acpi_map_phys(rsdt_phys, total_length);
    if (!rsdt_virt) {
        panic("Failed to map full Root SDT.");
    }

    LOGF("[ACPI] Root SDT mapped at 0x%p (Phys: 0x%lx)\n", rsdt_virt, rsdt_phys);
    return true;
}

/*
 * acpi_find_table - Find a specific ACPI table by its signature (e.g., "APIC", "HPET")
 */
void* acpi_find_table(const char* signature) {
    if (!rsdt_virt) return NULL;

    ACPISDTHeader* root_header = (ACPISDTHeader*)rsdt_virt;
    size_t entries_count = 0;

    if (xsdt_ok) {
        entries_count = (root_header->Length - sizeof(ACPISDTHeader)) / sizeof(uint64_t);
    } else {
        entries_count = (root_header->Length - sizeof(ACPISDTHeader)) / sizeof(uint32_t);
    }

    // Iterate through each entry in the RSDT/XSDT and look for the matching signature
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
            return acpi_map_phys(table_phys, length);
        }
    }

    return NULL;
}

// Getters
RSDP2Descriptor* acpi_get_rsdp(void) { return rsdp; }
void* acpi_get_root_sdt(void) { return rsdt_virt; }
bool acpi_is_xsdt_supported(void) { return xsdt_ok; }