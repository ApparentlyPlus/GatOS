/*
 * multiboot2.h - Multiboot 2 specification definitions
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Multiboot2 magic number
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

// Memory types
#define MULTIBOOT_MEMORY_AVAILABLE          1
#define MULTIBOOT_MEMORY_RESERVED           2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE   3
#define MULTIBOOT_MEMORY_NVS                4
#define MULTIBOOT_MEMORY_BADRAM             5

// Tag types
#define MULTIBOOT_TAG_TYPE_END                 0
#define MULTIBOOT_TAG_TYPE_CMDLINE             1
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME    2
#define MULTIBOOT_TAG_TYPE_MODULE              3
#define MULTIBOOT_TAG_TYPE_BASIC_MEMINFO       4
#define MULTIBOOT_TAG_TYPE_BOOTDEV             5
#define MULTIBOOT_TAG_TYPE_MMAP                6
#define MULTIBOOT_TAG_TYPE_VBE                 7
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER         8
#define MULTIBOOT_TAG_TYPE_ELF_SECTIONS        9
#define MULTIBOOT_TAG_TYPE_APM                 10
#define MULTIBOOT_TAG_TYPE_EFI32               11
#define MULTIBOOT_TAG_TYPE_EFI64               12
#define MULTIBOOT_TAG_TYPE_SMBIOS              13
#define MULTIBOOT_TAG_TYPE_ACPI_OLD            14
#define MULTIBOOT_TAG_TYPE_ACPI_NEW            15
#define MULTIBOOT_TAG_TYPE_NETWORK             16
#define MULTIBOOT_TAG_TYPE_EFI_MMAP            17
#define MULTIBOOT_TAG_TYPE_EFI_BS              18
#define MULTIBOOT_TAG_TYPE_EFI32_IH            19
#define MULTIBOOT_TAG_TYPE_EFI64_IH            20
#define MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR      21


// Multiboot2 structures (packed for bootloader compatibility)
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) multiboot_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot_tag_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot_memory_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    multiboot_memory_entry_t entries[];
} __attribute__((packed)) multiboot_memory_map_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type_info;
    uint8_t  reserved[2];
} __attribute__((packed)) multiboot_framebuffer_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[];
} __attribute__((packed)) multiboot_acpi_t;

// Main multiboot2 parser structure
typedef struct {
    // Copied multiboot2 data (all in higher half)
    multiboot_info_t* info;
    multiboot_memory_map_t* memory_map;
    size_t memory_map_length;
    int initialized;
} multiboot_parser_t;

// Core functions
void multiboot_init(multiboot_parser_t* parser, void* mb_info, uint8_t* buffer, size_t buffer_size);

// Memory map
uint64_t multiboot_total_ram(multiboot_parser_t* parser, int measurementUnit);
int multiboot_mem_region(multiboot_parser_t* parser, size_t index, 
                               uintptr_t* start, uintptr_t* end, uint32_t* type);
bool multiboot_is_avail(multiboot_parser_t* parser, uint64_t start, uint64_t end);
bool multiboot_hits_avail(multiboot_parser_t* parser, uint64_t start, uint64_t end);

// Hardware information
multiboot_framebuffer_t* multiboot_framebuffer(multiboot_parser_t* parser);
multiboot_acpi_t* multiboot_acpi_rsdp(multiboot_parser_t* parser);

// Debug output
void multiboot_dump_mmap(multiboot_parser_t* parser);
