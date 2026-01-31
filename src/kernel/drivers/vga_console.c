/*
 * vga_console.c - Framebuffer console implementation
 *
 * Provides a text console over a high-resolution framebuffer.
 * Maintains the existing VGA console API for compatibility.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/vga_console.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/drivers/vga_font.h>
#include <kernel/memory/vmm.h>
#include <kernel/sys/panic.h>
#include <libc/string.h>

// Framebuffer State
static uint32_t* g_fb_addr = NULL; // Virtual address of framebuffer
static uint64_t g_fb_phys = 0;
static uint32_t g_fb_width = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch = 0;
static uint32_t g_fb_bpp = 0;
static size_t g_fb_size = 0;

// Console State
static size_t g_cursor_x = 0; // In characters
static size_t g_cursor_y = 0; // In characters
static size_t g_max_cols = 0;
static size_t g_max_rows = 0;

static uint32_t g_fg_color = 0xFFFFFFFF; // Default White
static uint32_t g_bg_color = 0xFF000000; // Default Black

// Standard VGA Color Palette (ARGB)
static const uint32_t VGA_PALETTE[16] = {
    0xFF000000, // Black
    0xFF0000AA, // Blue
    0xFF00AA00, // Green
    0xFF00AAAA, // Cyan
    0xFFAA0000, // Red
    0xFFAA00AA, // Magenta
    0xFFAA5500, // Brown
    0xFFAAAAAA, // Light Gray
    0xFF555555, // Dark Gray
    0xFF5555FF, // Light Blue
    0xFF55FF55, // Light Green
    0xFF55FFFF, // Light Cyan
    0xFFFF5555, // Light Red
    0xFFFF55FF, // Pink
    0xFFFFFF55, // Yellow
    0xFFFFFFFF  // White
};

/*
 * put_pixel - Draws a single pixel to the framebuffer
 */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb_width || y >= g_fb_height) return; 
    
    // Calculate offset in 32-bit words (assuming 32bpp)
    // Pitch is in bytes, so divide by 4 for uint32_t pointer arithmetic
    size_t offset = (y * g_fb_pitch / 4) + x;
    g_fb_addr[offset] = color;
}

/*
 * draw_char_at - Draws a character at specific coordinates
 */
static void draw_char_at(char c, size_t cx, size_t cy, uint32_t fg, uint32_t bg) {
    const uint8_t* glyph = &font_8x16[(uint8_t)c * 16];
    
    uint32_t pix_x = cx * 8;
    uint32_t pix_y = cy * 16;

    for (int y = 0; y < 16; y++) {
        uint8_t row = glyph[y];
        for (int x = 0; x < 8; x++) {
            // Check if bit 7-x is set (font is MSB left)
            bool active = (row >> (7 - x)) & 1;
            put_pixel(pix_x + x, pix_y + y, active ? fg : bg);
        }
    }
}

/*
 * console_clear - Clears the screen to background color
 */
void console_clear(void) {
    if (!g_fb_addr) return;

    // Optimized clear using 32-bit writes
    // dev note: Pitch might include padding, but we can usually treat it as linear
    // for clearing if we are careful. Safer to do row-by-row.
    
    for (uint32_t y = 0; y < g_fb_height; y++) {
        uint32_t* row = (uint32_t*)((uintptr_t)g_fb_addr + (y * g_fb_pitch));
        for (uint32_t x = 0; x < g_fb_width; x++) {
            row[x] = g_bg_color;
        }
    }
    
    g_cursor_x = 0;
    g_cursor_y = 0;
}

/*
 * scroll_screen - Scrolls the screen up by one line
 */
static void scroll_screen(void) {
    if (!g_fb_addr) return;

    size_t line_height_bytes = 16 * g_fb_pitch;
    size_t screen_size_bytes = g_fb_height * g_fb_pitch;
    size_t copy_size = screen_size_bytes - line_height_bytes;

    // Move everything up
    memmove(g_fb_addr, (void*)((uintptr_t)g_fb_addr + line_height_bytes), copy_size);

    // Clear the last line (16 pixel rows)
    void* last_line = (void*)((uintptr_t)g_fb_addr + copy_size);
    
    // We can't use memset efficiently for 32-bit colors unless black/white
    // But since this is a scroll, let's just do it manually for correctness
    for (uint32_t y = 0; y < 16; y++) {
        uint32_t* row = (uint32_t*)((uintptr_t)last_line + (y * g_fb_pitch));
        for (uint32_t x = 0; x < g_fb_width; x++) {
            row[x] = g_bg_color;
        }
    }
}

/*
 * console_init - Initialize the framebuffer console
 */
void console_init(multiboot_parser_t* parser) {
    multiboot_framebuffer_t* fb = multiboot_get_framebuffer(parser);
    if (!fb) {
        panic("No framebuffer found in Multiboot info!");
    }

    g_fb_phys = fb->addr;
    g_fb_width = fb->width;
    g_fb_height = fb->height;
    g_fb_pitch = fb->pitch;
    g_fb_bpp = fb->bpp;
    g_fb_size = g_fb_height * g_fb_pitch;

    // Align size to page boundary
    size_t map_size = align_up(g_fb_size, PAGE_SIZE);
    
    // We must use the VMM to map this MMIO region
    vmm_t* kernel_vmm = vmm_kernel_get();
    if (!kernel_vmm) {
        panic("Console init called before VMM init!");
    }

    void* virt_addr = NULL;
    vmm_status_t status = vmm_alloc(kernel_vmm, map_size, VM_FLAG_MMIO | VM_FLAG_WRITE, (void*)g_fb_phys, &virt_addr);
    
    if (status != VMM_OK) {
        panic("Failed to map framebuffer!");
    }
    
    g_fb_addr = (uint32_t*)virt_addr;

    // Calculate dimensions
    g_max_cols = g_fb_width / 8;
    g_max_rows = g_fb_height / 16;

    console_clear();
}

/*
 * console_print_char - Outputs single character to screen
 */
void console_print_char(char character) {
    if (!g_fb_addr) return;

    if (character == '\n') {
        g_cursor_x = 0;
        g_cursor_y++;
    } else if (character == '\r') {
        g_cursor_x = 0;
    } else if (character == '\b') {
        if (g_cursor_x > 0) g_cursor_x--;
    } else {
        draw_char_at(character, g_cursor_x, g_cursor_y, g_fg_color, g_bg_color);
        g_cursor_x++;
        
        if (g_cursor_x >= g_max_cols) {
            g_cursor_x = 0;
            g_cursor_y++;
        }
    }

    if (g_cursor_y >= g_max_rows) {
        scroll_screen();
        g_cursor_y = g_max_rows - 1;
    }
}

/*
 * console_set_color - Sets foreground/background text colors (VGA 0-15 mapped to RGB)
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    if (foreground < 16) g_fg_color = VGA_PALETTE[foreground];
    if (background < 16) g_bg_color = VGA_PALETTE[background];
}