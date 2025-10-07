/*
 * ACPI.h - ACPI (Advanced Configuration and Power Interface) related definitions.
 *
 * Author: u/ApparentlyPlus
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <multiboot2.h>

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

/* ACPI SDT header */
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

/* RSDT and XSDT structures */
typedef struct {
    ACPISDTHeader sdt_header; // signature "RSDT"
    uint32_t sdt_addresses[];
} __attribute__ ((packed)) RSDT;

typedef struct {
    ACPISDTHeader sdt_header; // signature "XSDT"
    uint64_t sdt_addresses[];
} __attribute__ ((packed)) XSDT;


bool acpi_validate_rsdp(RSDP2Descriptor* rsdp);
RSDP2Descriptor* acpi_find_rsdp(multiboot_parser_t* parser);
void* acpi_find_root_sdt(RSDP2Descriptor* rsdp);
ACPISDTHeader* acpi_get_nth_sdt_from_root(void* root_sdt, size_t index);


bool acpi_init(multiboot_parser_t* parser);
RSDP2Descriptor* acpi_get_rsdp(void);
void* acpi_get_root_sdt(void);
bool acpi_is_xsdt_supported(void);

#endif
