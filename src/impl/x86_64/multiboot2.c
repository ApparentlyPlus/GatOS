/*
 * multiboot2.c - Clean Multiboot2 parser for GatOS kernel
 *
 * This implementation copies all multiboot2 data to higher half memory
 * and provides a clean interface for accessing boot information.
 *
 * Author: u/ApparentlyPlus
 */

#include "multiboot2.h"
#include "print.h"
#include "libc/string.h"
#include <stddef.h>

/*
 * align_up - Aligns address to specified boundary
 */
static uintptr_t align_up(uintptr_t val, uintptr_t align) {
    return (val + align - 1) & ~(align - 1);
}

/*
 * get_next_tag - Advances to next multiboot tag
 */
static multiboot_tag_t* get_next_tag(multiboot_tag_t* tag) {
    uintptr_t addr = (uintptr_t)tag;
    size_t padded_size = align_up(tag->size, 8);
    return (multiboot_tag_t*)(addr + padded_size);
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
 * memory_ranges_overlap - Checks for memory region collisions
 */
static int memory_ranges_overlap(uintptr_t start1, uintptr_t end1, uintptr_t start2, uintptr_t end2) {
    return (start1 < end2) && (start2 < end1);
}

/*
 * add_available_memory_range - Adds memory region to available list
 */
static void add_available_memory_range(multiboot_parser_t* parser, uintptr_t start, uintptr_t end, memory_range_t** prev) {
    if (parser->available_memory_count >= MAX_MEMORY_RANGES) {
        return;
    }
    
    memory_range_t* range = &parser->ranges[parser->available_memory_count];
    range->start = start;
    range->end = end;
    range->next = NULL;
    
    if (*prev) {
        (*prev)->next = range;
    } else {
        parser->available_memory_head = range;
    }
    
    *prev = range;
    parser->available_memory_count++;
}

/*
 * build_available_memory_list - Constructs available memory region list
 */
static void build_available_memory_list(multiboot_parser_t* parser) {
    parser->available_memory_head = NULL;
    parser->available_memory_count = 0;
    
    if (!parser->memory_map || parser->memory_map_length == 0) {
        return;
    }
    
    uintptr_t kernel_start = (uintptr_t)&KPHYS_START;
    uintptr_t kernel_end = (uintptr_t)&KPHYS_END;
    
    memory_range_t* prev = NULL;
    
    for (size_t i = 0; i < parser->memory_map_length && parser->available_memory_count < MAX_MEMORY_RANGES; i++) {
        uintptr_t start, end;
        uint32_t type;
        
        if (multiboot_get_memory_region(parser, i, &start, &end, &type) == 0) {
            if (type == MULTIBOOT_MEMORY_AVAILABLE) {
                // Check if this range overlaps with the kernel
                if (memory_ranges_overlap(start, end, kernel_start, kernel_end)) {
                    // Add memory before kernel (if any)
                    if (start < kernel_start) {
                        uintptr_t before_end = (kernel_start < end) ? kernel_start : end;
                        add_available_memory_range(parser, start, before_end, &prev);
                    }
                    
                    // Add memory after kernel (if any)
                    if (kernel_end < end) {
                        uintptr_t after_start = (kernel_end > start) ? kernel_end : start;
                        add_available_memory_range(parser, after_start, end, &prev);
                    }
                } else {
                    // No overlap with kernel, add the entire range
                    add_available_memory_range(parser, start, end, &prev);
                }
            }
        }
    }
}

/*
 * calculate_required_size - Computes needed buffer size for multiboot data
 */
static size_t calculate_required_size(void* mb_info) {
    multiboot_info_t* info = (multiboot_info_t*)mb_info;
    size_t total_size = info->total_size;

    // Parse tags to find strings and calculate their sizes
    multiboot_tag_t* tag = (multiboot_tag_t*)((uintptr_t)info + sizeof(multiboot_info_t));
    
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
            case MULTIBOOT_TAG_TYPE_CMDLINE: {
                multiboot_string_tag_t* str_tag = (multiboot_string_tag_t*)tag;
                total_size += strlen(str_tag->string) + 1;
                break;
            }
            case MULTIBOOT_TAG_TYPE_MODULE: {
                multiboot_module_tag_t* mod_tag = (multiboot_module_tag_t*)tag;
                if (mod_tag->module.string) {
                    total_size += strlen((char*)(uintptr_t)mod_tag->module.string) + 1;
                }
                break;
            }
        }
        tag = get_next_tag(tag);
    }
    
    return total_size + 64; // Add padding
}

/*
 * copy_multiboot_data - Copies and processes multiboot information
 */
static void copy_multiboot_data(multiboot_parser_t* parser, void* mb_info) {
    multiboot_info_t* src_info = (multiboot_info_t*)mb_info;
    
    // Copy main structure
    size_t struct_size = src_info->total_size;
    memcpy(parser->data_buffer, src_info, struct_size);
    parser->buffer_used = align_up(struct_size, 8);
    
    // Update parser to point to copied structure
    parser->info = (multiboot_info_t*)parser->data_buffer;
    
    // Process tags and fix up pointers
    multiboot_tag_t* tag = (multiboot_tag_t*)((uintptr_t)parser->info + sizeof(multiboot_info_t));
    
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: {
                multiboot_string_tag_t* str_tag = (multiboot_string_tag_t*)tag;
                size_t str_len = strlen(str_tag->string) + 1;
                
                char* new_str = (char*)(parser->data_buffer + parser->buffer_used);
                memcpy(new_str, str_tag->string, str_len);
                parser->buffer_used += align_up(str_len, 8);
                
                parser->bootloader_name = new_str;
                break;
            }
            
            case MULTIBOOT_TAG_TYPE_CMDLINE: {
                multiboot_string_tag_t* str_tag = (multiboot_string_tag_t*)tag;
                size_t str_len = strlen(str_tag->string) + 1;
                
                char* new_str = (char*)(parser->data_buffer + parser->buffer_used);
                memcpy(new_str, str_tag->string, str_len);
                parser->buffer_used += align_up(str_len, 8);
                
                parser->command_line = new_str;
                break;
            }
            
            case MULTIBOOT_TAG_TYPE_MMAP: {
                parser->memory_map = (multiboot_memory_map_t*)tag;
                parser->memory_map_length = (tag->size - sizeof(multiboot_memory_map_t)) / parser->memory_map->entry_size;
                break;
            }
            
            case MULTIBOOT_TAG_TYPE_MODULE: {
                multiboot_module_tag_t* mod_tag = (multiboot_module_tag_t*)tag;
                if (mod_tag->module.string) {
                    char* orig_str = (char*)(uintptr_t)mod_tag->module.string;
                    size_t str_len = strlen(orig_str) + 1;
                    
                    char* new_str = (char*)(parser->data_buffer + parser->buffer_used);
                    memcpy(new_str, orig_str, str_len);
                    parser->buffer_used += align_up(str_len, 8);
                    
                    mod_tag->module.string = (uintptr_t)new_str;
                }
                break;
            }
        }
        tag = get_next_tag(tag);
    }
}

/*
 * multiboot_init - Initializes multiboot parser with boot information
 */
void multiboot_init(multiboot_parser_t* parser, void* mb_info, uint8_t* buffer, size_t buffer_size) {
    // Initialize parser
    memset(parser, 0, sizeof(multiboot_parser_t));
    parser->data_buffer = buffer;
    parser->buffer_size = buffer_size;

    // Check buffer size
    size_t required_size = calculate_required_size(mb_info);

    if (required_size > buffer_size) {
        print("[MB2] Error: Buffer too small (need ");
        print_int((int)required_size);
        print(", have ");
        print_int((int)buffer_size);
        print(")\n");
        return;
    }
    
    // Copy all data to higher half
    copy_multiboot_data(parser, mb_info);
    
    // Build available memory list
    build_available_memory_list(parser);
    
    parser->initialized = 1;
    
    print("[MB2] Initialization complete (used ");
    print_int((int)parser->buffer_used);
    print(" of ");
    print_int((int)buffer_size);
    print(" bytes)\n");
}

/*
 * multiboot_get_bootloader_name - Returns bootloader name string
 */
const char* multiboot_get_bootloader_name(multiboot_parser_t* parser) {
    return parser->bootloader_name;
}

/*
 * multiboot_get_command_line - Returns kernel command line
 */
const char* multiboot_get_command_line(multiboot_parser_t* parser) {
    return parser->command_line;
}

/*
 * multiboot_get_total_RAM - Returns total RAM size
 */
uint64_t multiboot_get_total_RAM(multiboot_parser_t* parser, int measurementUnit) {
    return (multiboot_get_highest_physical_address(parser) - (uint64_t)(uintptr_t)&KPHYS_START)/measurementUnit;
}

/*
 * multiboot_get_highest_physical_address - Returns the highest physical address
 */
uint64_t multiboot_get_highest_physical_address(multiboot_parser_t* parser) {
    if (!parser->memory_map || parser->memory_map_length == 0) {
        return 0;
    }
    
    uint64_t highest_addr = 0;
    
    for (size_t i = 0; i < parser->memory_map_length; i++) {
        uintptr_t start, end;
        uint32_t type;
        
        if (multiboot_get_memory_region(parser, i, &start, &end, &type) == 0) {
            if (end > highest_addr && type == MULTIBOOT_MEMORY_AVAILABLE) {
                highest_addr = end;
            }
        }
    }
    
    return highest_addr;
}

/*
 * multiboot_get_available_memory - Returns linked list of available memory regions
 */
memory_range_t* multiboot_get_available_memory(multiboot_parser_t* parser) {
    return parser->available_memory_head;
}

/*
 * multiboot_get_available_memory_count - Returns available region count
 */
size_t multiboot_get_available_memory_count(multiboot_parser_t* parser) {
    return parser->available_memory_count;
}

/*
 * multiboot_get_memory_region - Retrieves memory region by index
 */
int multiboot_get_memory_region(multiboot_parser_t* parser, size_t index, 
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
 * multiboot_get_module_count - Returns number of loaded modules
 */
int multiboot_get_module_count(multiboot_parser_t* parser) {
    if (!parser->initialized) return 0;
    
    int count = 0;
    multiboot_tag_t* tag = (multiboot_tag_t*)((uintptr_t)parser->info + sizeof(multiboot_info_t));
    
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            count++;
        }
        tag = get_next_tag(tag);
    }
    return count;
}

/*
 * multiboot_get_module - Retrieves module information by index
 */
multiboot_module_t* multiboot_get_module(multiboot_parser_t* parser, int index) {
    if (!parser->initialized) return NULL;
    
    int count = 0;
    multiboot_tag_t* tag = (multiboot_tag_t*)((uintptr_t)parser->info + sizeof(multiboot_info_t));
    
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            if (count == index) {
                multiboot_module_tag_t* module_tag = (multiboot_module_tag_t*)tag;
                return &module_tag->module;
            }
            count++;
        }
        tag = get_next_tag(tag);
    }
    return NULL;
}

/*
 * multiboot_get_framebuffer - Returns framebuffer information if available
 */
multiboot_framebuffer_t* multiboot_get_framebuffer(multiboot_parser_t* parser) {
    return (multiboot_framebuffer_t*)find_tag(parser, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
}

/*
 * multiboot_get_elf_sections - Returns ELF section headers if available
 */
multiboot_elf_sections_t* multiboot_get_elf_sections(multiboot_parser_t* parser) {
    return (multiboot_elf_sections_t*)find_tag(parser, MULTIBOOT_TAG_TYPE_ELF_SECTIONS);
}

/*
 * multiboot_get_acpi_rsdp - Returns ACPI RSDP pointer
 */
void* multiboot_get_acpi_rsdp(multiboot_parser_t* parser) {
    multiboot_tag_t* tag = find_tag(parser, MULTIBOOT_TAG_TYPE_ACPI_NEW);
    if (!tag) {
        tag = find_tag(parser, MULTIBOOT_TAG_TYPE_ACPI_OLD);
    }
    return tag ? ((multiboot_acpi_t*)tag)->rsdp : NULL;
}

/*
 * multiboot_get_kernel_range - Retrieves kernel physical memory range
 */
void multiboot_get_kernel_range(uintptr_t* start, uintptr_t* end) {
    *start = (uintptr_t)&KPHYS_START;
    *end = (uintptr_t)&KPHYS_END;
}

/*
 * multiboot_is_page_used - Checks if physical page is reserved
 */
int multiboot_is_page_used(multiboot_parser_t* parser, uintptr_t start, size_t page_size) {
    if (!parser->initialized) return 0;
    
    uintptr_t page_end = start + page_size;
    
    // Check kernel range
    uintptr_t kernel_start = (uintptr_t)&KPHYS_START;
    uintptr_t kernel_end = (uintptr_t)&KPHYS_END;
    if (memory_ranges_overlap(start, page_end, kernel_start, kernel_end)) {
        return 1;
    }
    
    // Check multiboot structures
    uintptr_t mb_start = (uintptr_t)parser->info;
    uintptr_t mb_end = mb_start + parser->info->total_size;
    if (memory_ranges_overlap(start, page_end, mb_start, mb_end)) {
        return 1;
    }
    
    return 0;
}

/*
 * multiboot_dump_info - Prints multiboot information for debugging
 */
void multiboot_dump_info(multiboot_parser_t* parser) {
    if (!parser->initialized) {
        print("[MB2] Parser not initialized\n");
        return;
    }
    
    print("[MB2] === Multiboot2 Information ===\n");
    
    if (parser->bootloader_name) {
        print("[MB2] Bootloader: ");
        print(parser->bootloader_name);
        print("\n");
    }
    
    if (parser->command_line) {
        print("[MB2] Command line: ");
        print(parser->command_line);
        print("\n");
    }
    
    uintptr_t kernel_start, kernel_end;
    multiboot_get_kernel_range(&kernel_start, &kernel_end);
    print("[MB2] Kernel range: ");
    print_hex64(kernel_start);
    print(" - ");
    print_hex64(kernel_end);
    print(" (");
    print_int((int)((kernel_end - kernel_start) / 1024));
    print(" KiB)\n");
    
    uint64_t total_mem = multiboot_get_total_RAM(parser, MEASUREMENT_UNIT_MB);
    print("[MB2] Total memory: ");
    print_int((int)total_mem);
    print(" MiB\n");
    
    print("[MB2] Available memory ranges: ");
    print_int((int)parser->available_memory_count);
    print("\n");
    
    memory_range_t* range = parser->available_memory_head;
    int i = 0;
    while (range) {
        print("  [");
        print_int(i++);
        print("] ");
        print_hex64(range->start);
        print(" - ");
        print_hex64(range->end);
        print(" (");
        print_int((int)((range->end - range->start) / (1024 * 1024)));
        print(" MiB)\n");
        range = range->next;
    }
    
    int modules = multiboot_get_module_count(parser);
    print("[MB2] Modules: ");
    print_int(modules);
    print("\n");
    
    multiboot_framebuffer_t* fb = multiboot_get_framebuffer(parser);
    if (fb) {
        print("[MB2] Framebuffer: ");
        print_int(fb->width);
        print("x");
        print_int(fb->height);
        print(" @ ");
        print_int(fb->bpp);
        print("bpp\n");
    }
}

/*
 * multiboot_dump_memory_map - Prints memory map for debugging
 */
void multiboot_dump_memory_map(multiboot_parser_t* parser) {
    if (!parser->memory_map) {
        print("[MB2] No memory map found\n");
        return;
    }
    
    print("[MB2] === Memory Map ===\n");
    
    for (size_t i = 0; i < parser->memory_map_length; i++) {
        uintptr_t start, end;
        uint32_t type;
        
        if (multiboot_get_memory_region(parser, i, &start, &end, &type) == 0) {
            print("  [");
            print_int((int)i);
            print("] ");
            print_hex64(start);
            print(" - ");
            print_hex64(end);
            print(" (");
            print_int((int)((end - start) / 1024));
            print(" KiB) - ");
            
            switch (type) {
                case MULTIBOOT_MEMORY_AVAILABLE:
                    print("Available\n");
                    break;
                case MULTIBOOT_MEMORY_RESERVED:
                    print("Reserved\n");
                    break;
                case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE:
                    print("ACPI Reclaimable\n");
                    break;
                case MULTIBOOT_MEMORY_NVS:
                    print("ACPI NVS\n");
                    break;
                case MULTIBOOT_MEMORY_BADRAM:
                    print("Bad RAM\n");
                    break;
                default:
                    print("Unknown (");
                    print_int(type);
                    print(")\n");
                    break;
            }
        }
    }
}