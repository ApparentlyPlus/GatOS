/*
 * ACPI.h - ACPI (Advanced Configuration and Power Interface) related definitions.
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>

typedef	struct {
	char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
} __attribute__ ((packed)) RSDPDescriptor;

typedef struct  {
    char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
    uint32_t Length;
    uint64_t XSDTAddress;
    uint8_t ExtendedChecksum;
    uint8_t Reserved[3];
} __attribute__ ((packed)) RSDP2Descriptor;

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

typedef struct {
    ACPISDTHeader sdtHeader; //signature "RSDT"
    uint32_t sdtAddresses[];
} __attribute__ ((packed)) RSDT;

typedef struct {
    ACPISDTHeader sdtHeader; //signature "XSDT"
    uint64_t sdtAddresses[];
} __attribute__ ((packed)) XSDT;

RSDP2Descriptor* getRSDP(multiboot_parser_t* parser);
bool acpi_init(multiboot_parser_t* parser);
RSDP2Descriptor* acpi_get_rsdp();
void* acpi_get_root_sdt();
bool acpi_is_xsdt_supported();
ACPISDTHeader* get_nth_sdt(void* root_sdt, size_t n);

#endif 