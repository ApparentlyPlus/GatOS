/*
 * vga_console.c - Framebuffer console implementation
 *
 * Provides a text console over a high-resolution framebuffer.
 * Implements a Unicode-aware (UTF-8) character renderer using a PSF1 font.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/console.h>
#include <kernel/drivers/font.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/panic.h>
#include <libc/string.h>

// Framebuffer State
static uint8_t* g_fb_addr = NULL; // Virtual address of framebuffer
static uint64_t g_fb_phys = 0;
static uint32_t g_fb_width = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch = 0;
static uint32_t g_fb_bpp = 0;
static size_t g_fb_size = 0;

// Console State
static size_t g_cursor_x = 0; // In pixels (for variable width support later)
static size_t g_cursor_y = 0; // In pixels
static size_t g_font_width = 8;
static size_t g_font_height = 16;
static size_t g_max_cols = 0;
static size_t g_max_rows = 0;

// Spacing
#define PADDING_Y 2

static uint32_t g_fg_color = 0xFFFFFFFF; // Default White
static uint32_t g_bg_color = 0xFF000000; // Default Black

// UTF-8 State Machine
static uint32_t g_utf8_codepoint = 0;
static int g_utf8_bytes_needed = 0;

// Standard VGA Color Palette (ARGB)
static const uint32_t VGA_PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA, 
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF, 
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

/*
 * put_pixel - Draws a single pixel to the framebuffer
 */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb_width || y >= g_fb_height) return; 
    
    size_t offset = (y * g_fb_pitch) + (x * (g_fb_bpp / 8));
    uint8_t* dst = g_fb_addr + offset;

    if (g_fb_bpp == 32) {
        *(uint32_t*)dst = color;
    } else if (g_fb_bpp == 24) {
        dst[0] = color & 0xFF;
        dst[1] = (color >> 8) & 0xFF;
        dst[2] = (color >> 16) & 0xFF;
    }
}

/*
 * draw_glyph - Draws a PSF glyph at specific pixel coordinates
 */
static void draw_glyph(uint8_t* glyph, size_t px, size_t py, uint32_t fg, uint32_t bg) {
    for (size_t y = 0; y < g_font_height; y++) {
        uint8_t row = glyph[y];
        for (size_t x = 0; x < g_font_width; x++) {
            // PSF fonts are usually MSB left. 
            // NOTE: This assumes standard 8-pixel wide PSF1. 
            // If we upgrade to wider fonts, we need to handle stride.
            bool active = (row >> (7 - x)) & 1;
            put_pixel(px + x, py + y, active ? fg : bg);
        }
    }
}

/*
 * get_glyph - Retrieves the glyph pointer for a given codepoint
 */
static uint8_t* get_glyph(uint32_t codepoint) {
    psf1_font_t* font = font_get_current();
    if (!font) return NULL;
    
    uint8_t index = unicode_to_cp437(codepoint);
    
    // If index is 0 (NUL), check if the original codepoint was NUL.
    // If it was not NUL, it means mapping failed.
    if (index == 0 && codepoint != 0) {
        // Fallback to a block or question mark. 
        // In CP437, 0xDB is a full block (█), 0x3F is '?'
        index = 0x3F; 
    }
    
    return (uint8_t*)font->glyph_buffer + (index * font->header->charsize);
}

/*
 * console_clear - Clears the screen to background color
 */
void console_clear(void) {
    if (!g_fb_addr) return;
    
    // If background is black (0), we can optimize with memset
    if (g_bg_color == 0) {
        memset(g_fb_addr, 0, g_fb_height * g_fb_pitch);
        g_cursor_x = 0;
        g_cursor_y = 0;
        return;
    }

    for (uint32_t y = 0; y < g_fb_height; y++) {
        for (uint32_t x = 0; x < g_fb_width; x++) {
            put_pixel(x, y, g_bg_color);
        }
    }
    
    g_cursor_x = 0;
    g_cursor_y = 0;
}

/*
 * scroll_screen - Scrolls the screen up by one line height
 */
static void scroll_screen(void) {
    if (!g_fb_addr) return;

    size_t line_height = g_font_height + PADDING_Y;
    size_t line_height_bytes = line_height * g_fb_pitch;
    size_t screen_size_bytes = g_fb_height * g_fb_pitch;
    size_t copy_size = screen_size_bytes - line_height_bytes;

    memmove(g_fb_addr, g_fb_addr + line_height_bytes, copy_size);

    // Clear the new area at the bottom
    // Calculate pointer to the start of the cleared area
    
    // memset safe if 0
    if (g_bg_color == 0) {
        memset(g_fb_addr + copy_size, 0, line_height_bytes);
    } else {
        // Manual clear for colored background
        // We can't easily iterate x/y here without calculating row pointers manually
        // or just calling put_pixel for the bottom area.
        size_t start_y = g_fb_height - line_height;
        for (size_t y = start_y; y < g_fb_height; y++) {
             for (size_t x = 0; x < g_fb_width; x++) {
                 put_pixel(x, y, g_bg_color);
             }
        }
    }
}

/*
 * console_init - Initialize the framebuffer console
 */
void console_init(multiboot_parser_t* parser) {
    font_init();
    
    multiboot_framebuffer_t* fb = multiboot_get_framebuffer(parser);
    if (!fb) {
        panic("No framebuffer found!");
    }

    g_fb_phys = fb->addr;
    g_fb_width = fb->width;
    g_fb_height = fb->height;
    g_fb_pitch = fb->pitch;
    g_fb_bpp = fb->bpp;
    g_fb_size = g_fb_height * g_fb_pitch;

    size_t map_size = align_up(g_fb_size, PAGE_SIZE);
    
    vmm_t* kernel_vmm = vmm_kernel_get();
    if (!kernel_vmm) panic("Console init before VMM!");

    void* virt_addr = NULL;
    vmm_status_t status = vmm_alloc(kernel_vmm, map_size, VM_FLAG_MMIO | VM_FLAG_WRITE, (void*)g_fb_phys, &virt_addr);
    
    if (status != VMM_OK) panic("Failed to map framebuffer!");
    
    g_fb_addr = (uint8_t*)virt_addr;

    // Font setup
    psf1_font_t* font = font_get_current();
    g_font_width = 8; // Fixed for PSF1
    g_font_height = font->header->charsize;

    g_max_cols = g_fb_width / g_font_width;
    g_max_rows = g_fb_height / (g_font_height + PADDING_Y);

    console_clear();
}

/*
 * handle_codepoint - Draws a fully decoded unicode character
 */
static void handle_codepoint(uint32_t cp) {
    size_t line_height = g_font_height + PADDING_Y;

    if (cp == '\n') {
        g_cursor_x = 0;
        g_cursor_y += line_height;
    } else if (cp == '\r') {
        g_cursor_x = 0;
    } else if (cp == '\b') {
        if (g_cursor_x >= g_font_width) g_cursor_x -= g_font_width;
    } else if (cp == '\t') {
        g_cursor_x += g_font_width * 4; // Tab = 4 spaces
    } else {
        uint8_t* glyph = get_glyph(cp);
        if (glyph) {
            draw_glyph(glyph, g_cursor_x, g_cursor_y, g_fg_color, g_bg_color);
        }
        g_cursor_x += g_font_width;
    }
    
    // Wrap
    if (g_cursor_x >= g_fb_width) {
        g_cursor_x = 0;
        g_cursor_y += line_height;
    }
    
    // Scroll
    if (g_cursor_y + line_height > g_fb_height) {
        scroll_screen();
        g_cursor_y -= line_height;
    }
}

/*
 * console_print_char - Processes a byte of the stream (UTF-8 decoder)
 */
void console_print_char(char character) {
    uint8_t byte = (uint8_t)character;

    if (g_utf8_bytes_needed == 0) {
        if ((byte & 0x80) == 0) {
            // 1 byte (ASCII)
            handle_codepoint(byte);
        } else if ((byte & 0xE0) == 0xC0) {
            // 2 bytes
            g_utf8_bytes_needed = 1;
            g_utf8_codepoint = byte & 0x1F;
        } else if ((byte & 0xF0) == 0xE0) {
            // 3 bytes
            g_utf8_bytes_needed = 2;
            g_utf8_codepoint = byte & 0x0F;
        } else if ((byte & 0xF8) == 0xF0) {
            // 4 bytes
            g_utf8_bytes_needed = 3;
            g_utf8_codepoint = byte & 0x07;
        } else {
            // Invalid start byte, ignore or print replacement
            handle_codepoint(0xFFFD);
        }
    } else {
        // Continuation byte (10xxxxxx)
        if ((byte & 0xC0) == 0x80) {
            g_utf8_codepoint = (g_utf8_codepoint << 6) | (byte & 0x3F);
            g_utf8_bytes_needed--;
            if (g_utf8_bytes_needed == 0) {
                handle_codepoint(g_utf8_codepoint);
            }
        } else {
            // Invalid continuation, reset
            g_utf8_bytes_needed = 0;
            handle_codepoint(0xFFFD);
        }
    }
}

/*
 * console_set_color - Sets the foreground and background colors
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    if (foreground < 16) g_fg_color = VGA_PALETTE[foreground];
    if (background < 16) g_bg_color = VGA_PALETTE[background];
}

/*
 * console_get_width - Retrieves console width in characters
 */
size_t console_get_width() {
    return g_max_cols;
}

/*
 * console_get_height - Retrieves console height in characters
 */
size_t console_get_height() {
    return g_max_rows;
}