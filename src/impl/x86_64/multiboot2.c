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
#include "print.h"
#include <stddef.h>

static uintptr_t align_up(uintptr_t val, uintptr_t align) {
	return (val + align - 1) & ~(align - 1);
}

void multiboot2_parser_init(multiboot2_parser_t* parser, void* mb_info) {
	parser->mb_info = mb_info;
}

static multiboot_tag_t* find_tag(multiboot2_parser_t* parser, uint32_t wanted_type) {
	uintptr_t addr = (uintptr_t)parser->mb_info + 8;

	while (1) {
		multiboot_tag_t* tag = (multiboot_tag_t*)addr;
		if (tag->type == MULTIBOOT_TAG_TYPE_END)
			break;

		if (tag->type == wanted_type)
			return tag;

		addr = align_up(addr + tag->size, 8);
	}
	return NULL;
}

const char* mb2_get_cmdline(multiboot2_parser_t* p) {
	multiboot_tag_string_t* tag = (multiboot_tag_string_t*)find_tag(p, MULTIBOOT_TAG_TYPE_CMDLINE);
	return tag ? tag->cmdline : NULL;
}

const char* mb2_get_bootloader_name(multiboot2_parser_t* p) {
	multiboot_tag_string_t* tag = (multiboot_tag_string_t*)find_tag(p, MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME);
	return tag ? tag->cmdline : NULL;
}

int mb2_get_module_count(multiboot2_parser_t* p) {
	int count = 0;
	uintptr_t addr = (uintptr_t)p->mb_info + 8;

	while (1) {
		multiboot_tag_t* tag = (multiboot_tag_t*)addr;
		if (tag->type == MULTIBOOT_TAG_TYPE_END)
			break;
		if (tag->type == MULTIBOOT_TAG_TYPE_MODULE)
			count++;
		addr = align_up(addr + tag->size, 8);
	}
	return count;
}

multiboot_module_t* mb2_get_module(multiboot2_parser_t* p, int index) {
	int count = 0;
	uintptr_t addr = (uintptr_t)p->mb_info + 8;

	while (1) {
		multiboot_tag_t* tag = (multiboot_tag_t*)addr;
		if (tag->type == MULTIBOOT_TAG_TYPE_END)
			break;
		if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
			if (count == index)
				return (multiboot_module_t*)(tag + 1);
			count++;
		}
		addr = align_up(addr + tag->size, 8);
	}
	return NULL;
}

multiboot_tag_mmap_t* mb2_get_memory_map(multiboot2_parser_t* p) {
	return (multiboot_tag_mmap_t*)find_tag(p, MULTIBOOT_TAG_TYPE_MMAP);
}

multiboot_tag_framebuffer_t* mb2_get_framebuffer_info(multiboot2_parser_t* p) {
	return (multiboot_tag_framebuffer_t*)find_tag(p, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
}

multiboot_tag_elf_sections_t* mb2_get_elf_sections(multiboot2_parser_t* p) {
	return (multiboot_tag_elf_sections_t*)find_tag(p, MULTIBOOT_TAG_TYPE_ELF_SECTIONS);
}

void* mb2_get_acpi_rsdp(multiboot2_parser_t* p) {
	multiboot_tag_t* tag = find_tag(p, MULTIBOOT_TAG_TYPE_ACPI_NEW);
	if (!tag)
		tag = find_tag(p, MULTIBOOT_TAG_TYPE_ACPI_OLD);
	return tag ? ((uint8_t*)tag + sizeof(multiboot_tag_t)) : NULL;
}

void mb2_dump(multiboot2_parser_t* p) {
	//const char* cmdline = mb2_get_cmdline(p);
	//print_str("[MB2] Command line: "); print_str(cmdline ? cmdline : "(none)"); print_str("\n");
	const char* bootloader = mb2_get_bootloader_name(p);
	print_str("[MB2] Bootloader: "); print_str(bootloader ? bootloader : "(unknown)"); print_str("\n");

	int mods = mb2_get_module_count(p);
	for (int i = 0; i < mods; i++) {
		multiboot_module_t* mod = mb2_get_module(p, i);
		print_str("[MB2] Module: 0x"); print_hex32(mod->mod_start);
		print_str(" - 0x"); print_hex32(mod->mod_end);
		print_str(" | String: "); print_str((char*)(uintptr_t)mod->string); print_str("\n");
	}

	// Framebuffer
	multiboot_tag_framebuffer_t* fb = mb2_get_framebuffer_info(p);
	if (fb) {
		print_str("[MB2] Framebuffer: ");
		print_int(fb->width); print_str("x");
		print_int(fb->height); print_str(" @ ");
		print_int(fb->bpp); print_str("bpp\n");
	}
}

void mb2_dump_memory_map(multiboot2_parser_t* p) {
	multiboot_tag_mmap_t* mmap_tag = mb2_get_memory_map(p);

	if (!mmap_tag) {
		print_str("[MB2] No memory map found.\n");
		return;
	}

	print_str("[MB2] Memory Map:\n");

	uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
	uintptr_t ptr = (uintptr_t)mmap_tag + sizeof(multiboot_tag_mmap_t);

	while (ptr < mmap_end) {
		multiboot_mmap_entry_t* entry = (multiboot_mmap_entry_t*)ptr;

		print_str("  [Region] Start: ");
		print_hex64(entry->addr);
		print_str(" | End: ");
		print_hex64(entry->addr + entry->len);
		print_str(" | Size: ");
		print_int((int)(entry->len / 1024));
		print_str(" KiB | Type: ");

		switch (entry->type) {
			case MULTIBOOT_MEMORY_AVAILABLE:
				print_str("Available\n");
				break;
			case MULTIBOOT_MEMORY_RESERVED:
				print_str("Reserved\n");
				break;
			case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE:
				print_str("ACPI Reclaimable\n");
				break;
			case MULTIBOOT_MEMORY_NVS:
				print_str("ACPI NVS\n");
				break;
			case MULTIBOOT_MEMORY_BADRAM:
				print_str("Bad RAM\n");
				break;
			default:
				print_str("Unknown\n");
				break;
		}

		ptr += mmap_tag->entry_size;
	}
}

