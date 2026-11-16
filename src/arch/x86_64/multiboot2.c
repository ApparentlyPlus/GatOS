/*
 * multiboot2.c - Clean Multiboot2 parser for GatOS kernel
 *
 * Author: u/ApparentlyPlus
 */

#include <arch/x86_64/memory/layout.h>
#include <arch/x86_64/multiboot2.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <stdbool.h>
#include <stddef.h>

static multiboot_tag_t* get_next_tag(multiboot_tag_t* tag) {
    return (multiboot_tag_t*)((uintptr_t)tag + align_up(tag->size, 8));
}

/*
 * find_tag - Locates specific multiboot tag type
 */
static multiboot_tag_t* find_tag(multiboot_parser_t* parser, uint32_t type) {
    if (!parser->initialized) return NULL;

    multiboot_tag_t* tag = (multiboot_tag_t*)((uintptr_t)parser->info + sizeof(multiboot_info_t));

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == type) {
            return tag;
        }
        tag = get_next_tag(tag);
    }
    return NULL;
}

/*
 * multiboot_init - Copies the multiboot2 struct into buffer (higher half) and indexes the memory map
 * Everything in the struct, strings included, sits inside total_size, so one copy is enough
 */
void multiboot_init(multiboot_parser_t* parser, void* mb_info, uint8_t* buffer, size_t buffer_size) {
    kmemset(parser, 0, sizeof(multiboot_parser_t));

    size_t size = ((multiboot_info_t*)mb_info)->total_size;
    if (size > buffer_size) {
        LOGF("[MB2] Error: Buffer too small (need %d, have %d)\n", (int)size, (int)buffer_size);
        return;
    }

    kmemcpy(buffer, mb_info, size);
    parser->info = (multiboot_info_t*)buffer;
    parser->initialized = 1;

    multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)find_tag(parser, MULTIBOOT_TAG_TYPE_MMAP);
    if (mmap) {
        parser->memory_map = mmap;
        parser->memory_map_length = (mmap->size - sizeof(multiboot_memory_map_t)) / mmap->entry_size;
    }

    #ifndef TEST_BUILD
    LOGF("[MB2] Initialization complete (used %d of %d bytes)\n", (int)size, (int)buffer_size);
    #endif
}

int multiboot_mem_region(multiboot_parser_t* parser, size_t index,
                               uintptr_t* start, uintptr_t* end, uint32_t* type) {
    if (!parser->memory_map || index >= parser->memory_map_length) {
        return 1; // Error
    }

    multiboot_memory_entry_t* entry = &parser->memory_map->entries[0];
    entry = (multiboot_memory_entry_t*)((uintptr_t)entry + (index * parser->memory_map->entry_size));

    *start = (uintptr_t)entry->addr;
    *end = (uintptr_t)(entry->addr + entry->len);
    *type = entry->type;

    return 0; // Success
}

/*
 * multiboot_total_ram - End of the highest available region, in measurementUnit
 */
uint64_t multiboot_total_ram(multiboot_parser_t* parser, int measurementUnit) {
    uint64_t highest = 0;

    for (size_t i = 0; i < parser->memory_map_length; i++) {
        uintptr_t start, end;
        uint32_t type;
        if (multiboot_mem_region(parser, i, &start, &end, &type) == 0 &&
            type == MULTIBOOT_MEMORY_AVAILABLE && end > highest)
            highest = end;
    }

    return highest / measurementUnit;
}

/*
 * multiboot_is_avail - True if [start, end) sits entirely inside one available region
 */
bool multiboot_is_avail(multiboot_parser_t* parser, uint64_t start, uint64_t end) {
    for (size_t i = 0; i < parser->memory_map_length; i++) {
        uintptr_t rs, re;
        uint32_t type;
        if (multiboot_mem_region(parser, i, &rs, &re, &type) == 0 &&
            type == MULTIBOOT_MEMORY_AVAILABLE && start >= rs && end <= re)
            return true;
    }
    return false;
}

/*
 * multiboot_hits_avail - True if [start, end) touches any available region
 */
bool multiboot_hits_avail(multiboot_parser_t* parser, uint64_t start, uint64_t end) {
    for (size_t i = 0; i < parser->memory_map_length; i++) {
        uintptr_t rs, re;
        uint32_t type;
        if (multiboot_mem_region(parser, i, &rs, &re, &type) == 0 &&
            type == MULTIBOOT_MEMORY_AVAILABLE && start < re && rs < end)
            return true;
    }
    return false;
}

/*
 * multiboot_framebuffer - Returns framebuffer information if available
 */
multiboot_framebuffer_t* multiboot_framebuffer(multiboot_parser_t* parser) {
    return (multiboot_framebuffer_t*)find_tag(parser, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
}

multiboot_acpi_t* multiboot_acpi_rsdp(multiboot_parser_t* parser) {
    multiboot_tag_t* tag = find_tag(parser, MULTIBOOT_TAG_TYPE_ACPI_NEW);
    if (!tag) {
        tag = find_tag(parser, MULTIBOOT_TAG_TYPE_ACPI_OLD);
    }
    return (multiboot_acpi_t*)tag;
}

/*
 * multiboot_dump_mmap - Logs the firmware memory map
 */
void multiboot_dump_mmap(multiboot_parser_t* parser) {
    static const char* types[] = { "?", "Available", "Reserved", "ACPI Reclaimable", "ACPI NVS", "Bad RAM" };

    LOGF("[MB2] Memory map (%d regions):\n", (int)parser->memory_map_length);
    for (size_t i = 0; i < parser->memory_map_length; i++) {
        uintptr_t start, end;
        uint32_t type;
        if (multiboot_mem_region(parser, i, &start, &end, &type) != 0) continue;
        LOGF("  0x%016lx - 0x%016lx  %s\n", start, end, type <= 5 ? types[type] : "?");
    }
}
