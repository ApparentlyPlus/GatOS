/*
 * multiboot2.c - Multiboot2 information parsing for GatOS kernel
 *
 * This file contains code to parse the Multiboot2 information structure passed
 * by the bootloader at kernel entry. It extracts memory map information and
 * detects the available heap size by examining memory regions available after
 * the kernel's BSS segment.
 *
 * Author: u/ApparentlyPlus
 */

#include "multiboot2.h"
#include <stdint.h>
#include <stddef.h>

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

#define MULTIBOOT_TAG_TYPE_END            0
#define MULTIBOOT_TAG_TYPE_MMAP           6
#define MULTIBOOT_MEMORY_AVAILABLE        1

/*
 * multiboot_detect_heap - Determines available heap memory size from Multiboot2 info
 * @mb_info: Pointer to the Multiboot2 info structure passed by the bootloader
 *
 * Parses the Multiboot2 tags to find the memory map and calculates the largest
 * available memory region after the kernel's BSS end, suitable for use as heap.
 *
 * Returns the size in bytes of the detected heap memory, or 0 if none found.
 */
uint64_t multiboot_detect_heap(void* mb_info) {
	uintptr_t addr = (uintptr_t)mb_info;
	addr += 8; // Skip total size and reserved fields at start

	mem_map_tag* mmap_tag = NULL;

	// Iterate over all tags until the end tag is reached
	while (1) {
		multiboot_tag* tag = (multiboot_tag*)addr;

		if (tag->type == MULTIBOOT_TAG_TYPE_END)
			break;

		if (tag->type == MULTIBOOT_TAG_TYPE_MMAP)
			mmap_tag = (mem_map_tag*)tag;

		// Advance to next tag, aligned to 8 bytes
		addr += (tag->size + 7) & ~7;
	}

	// No memory map found
	if (!mmap_tag)
		return 0;

	uint64_t heap_top = 0;

	// Calculate end address of memory map entries
	uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;

	// Iterate each memory map entry
	for (uintptr_t ptr = (uintptr_t)mmap_tag + sizeof(mem_map_tag); ptr < mmap_end; ptr += mmap_tag->entry_size) {
		mem_map_entry* entry = (mem_map_entry*)ptr;

		if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
			uint64_t region_end = entry->addr + entry->len;

			// Check if the region contains or extends beyond kernel BSS end
			if (entry->addr <= (uintptr_t)&__bss_end && region_end > (uintptr_t)&__bss_end) {
				uint64_t available = region_end - (uintptr_t)&__bss_end;

				if (available > heap_top)
					heap_top = available;
			}
		}
	}

	return heap_top;
}
