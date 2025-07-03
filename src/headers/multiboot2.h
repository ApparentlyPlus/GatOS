#pragma once
#include <stdint.h>

extern char __bss_end;

typedef struct {
	uint32_t type;
	uint32_t size;
} multiboot_tag;

typedef struct {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
} mem_map_tag;

typedef struct {
	uint64_t addr;
	uint64_t len;
	uint32_t type;
	uint32_t reserved;
} mem_map_entry;

typedef struct {
	uint64_t available;
	uint64_t reserved;
	uint64_t acpi_reclaimable;
	uint64_t acpi_nvs;
	uint64_t bad_ram;
} mem_summary;

#define MULTIBOOT_TAG_TYPE_END            0
#define MULTIBOOT_TAG_TYPE_MMAP           6
#define MULTIBOOT_MEMORY_AVAILABLE        1


uint64_t multiboot_detect_heap(void* mb_info);
void multiboot_print_memory_map(void* mb_info);
mem_summary multiboot_get_memory_summary(void* mb_info);

