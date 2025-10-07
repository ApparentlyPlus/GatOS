/*
 * multiboot2.h - Multiboot 2 specification definitions
 *
 * Defines structures and constants for parsing Multiboot 2 boot information.
 * Includes memory management structures for available memory tracking.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

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

#define MEASUREMENT_UNIT_BYTES                 1
#define MEASUREMENT_UNIT_KB                    1024
#define MEASUREMENT_UNIT_MB                    1024*1024
#define MEASUREMENT_UNIT_GB                    1024*1024*1024

// Maximum number of memory ranges we can store
#define MAX_MEMORY_RANGES 64

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
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    multiboot_module_t module;
} __attribute__((packed)) multiboot_module_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t num;
    uint32_t entsize;
    uint32_t shndx;
    uint32_t addr;
    uint32_t reserved;
} __attribute__((packed)) multiboot_elf_sections_t;

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
    char string[];
} __attribute__((packed)) multiboot_string_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[];
} __attribute__((packed)) multiboot_acpi_t;

// Memory range structure for available memory
typedef struct memory_range {
    uintptr_t start;
    uintptr_t end;
    struct memory_range* next;
} memory_range_t;

// Main multiboot2 parser structure
typedef struct {
    // Copied multiboot2 data (all in higher half)
    multiboot_info_t* info;
    const char* bootloader_name;
    const char* command_line;
    multiboot_memory_map_t* memory_map;
    size_t memory_map_length;
    
    // Available memory ranges (excluding kernel)
    memory_range_t ranges[MAX_MEMORY_RANGES];
    memory_range_t* available_memory_head;
    size_t available_memory_count;
    
    // Copy buffer management
    uint8_t* data_buffer;
    size_t buffer_size;
    size_t buffer_used;
    
    // State
    int initialized;
} multiboot_parser_t;

// External kernel symbols
extern uintptr_t KPHYS_START;
extern uintptr_t KPHYS_END;

// Core functions
void multiboot_init(multiboot_parser_t* parser, void* mb_info, uint8_t* buffer, size_t buffer_size);

// Information accessors
const char* multiboot_get_bootloader_name(multiboot_parser_t* parser);
const char* multiboot_get_command_line(multiboot_parser_t* parser);
uint64_t multiboot_get_total_RAM(multiboot_parser_t* parser, int measurementUnit);
uint64_t multiboot_get_highest_physical_address(multiboot_parser_t* parser);

// Memory management
memory_range_t* multiboot_get_available_memory(multiboot_parser_t* parser);
size_t multiboot_get_available_memory_count(multiboot_parser_t* parser);
int multiboot_get_memory_region(multiboot_parser_t* parser, size_t index, 
                               uintptr_t* start, uintptr_t* end, uint32_t* type);

// Module access
int multiboot_get_module_count(multiboot_parser_t* parser);
multiboot_module_t* multiboot_get_module(multiboot_parser_t* parser, int index);

// Hardware information
multiboot_framebuffer_t* multiboot_get_framebuffer(multiboot_parser_t* parser);
multiboot_elf_sections_t* multiboot_get_elf_sections(multiboot_parser_t* parser);
multiboot_acpi_t* multiboot_get_acpi_rsdp(multiboot_parser_t* parser);

// Utilities
void multiboot_get_kernel_range(uintptr_t* start, uintptr_t* end);
int multiboot_is_page_used(multiboot_parser_t* parser, uintptr_t start, size_t page_size);

// Debug output
void multiboot_dump_info(multiboot_parser_t* parser);
void multiboot_dump_memory_map(multiboot_parser_t* parser);