#pragma once
#include <stdint.h>

// Common tag header
typedef struct {
	uint32_t type;
	uint32_t size;
} multiboot_tag_t;

// Memory map tag
typedef struct {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
	// After tag, we have the entries
} multiboot_tag_mmap_t;

typedef struct {
	uint64_t addr;
	uint64_t len;
	uint32_t type;
	uint32_t reserved;
} multiboot_mmap_entry_t;

// Modules
typedef struct {
	uint32_t mod_start;
	uint32_t mod_end;
	uint32_t string;
	uint32_t reserved;
} multiboot_module_t;

// ELF
typedef struct {
	uint32_t type;
	uint32_t size;
	uint32_t num;
	uint32_t entsize;
	uint32_t shndx;
	uint32_t addr;
	uint32_t reserved;
} multiboot_tag_elf_sections_t;

// Framebuffer
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
} multiboot_tag_framebuffer_t;

// ACPI
typedef struct {
	uint32_t type;
	uint32_t size;
	uint8_t rsdp[0]; // variable size
} multiboot_tag_acpi_t;

// Command line
typedef struct {
	uint32_t type;
	uint32_t size;
	char cmdline[0];
} multiboot_tag_string_t;

#define MULTIBOOT_MEMORY_AVAILABLE          1
#define MULTIBOOT_MEMORY_RESERVED           2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE   3
#define MULTIBOOT_MEMORY_NVS                4
#define MULTIBOOT_MEMORY_BADRAM             5

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


typedef struct {
	void* mb_info;
} multiboot2_parser_t;


void multiboot2_parser_init(multiboot2_parser_t* parser, void* mb_info);

const char* mb2_get_cmdline(multiboot2_parser_t* p);
const char* mb2_get_bootloader_name(multiboot2_parser_t* p);

int mb2_get_module_count(multiboot2_parser_t* p);
multiboot_module_t* mb2_get_module(multiboot2_parser_t* p, int index);

multiboot_tag_mmap_t* mb2_get_memory_map(multiboot2_parser_t* p);
multiboot_tag_framebuffer_t* mb2_get_framebuffer_info(multiboot2_parser_t* p);
multiboot_tag_elf_sections_t* mb2_get_elf_sections(multiboot2_parser_t* p);
void* mb2_get_acpi_rsdp(multiboot2_parser_t* p);

void mb2_dump(multiboot2_parser_t* p);
void mb2_dump_memory_map(multiboot2_parser_t* p);
