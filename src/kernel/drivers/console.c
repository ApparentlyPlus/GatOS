/*
 * vga_console.c - Framebuffer console implementation
 */

#include <kernel/drivers/console.h>
#include <kernel/drivers/font.h>
#include <kernel/drivers/tty.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/vmm.h>
#include <kernel/memory/heap.h>
#include <kernel/sys/panic.h>
#include <libc/string.h>

// Framebuffer Hardware State
static uint8_t* g_fb_addr = NULL; 
static uint64_t g_fb_phys = 0;
static uint32_t g_fb_width = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch = 0;
static uint32_t g_fb_bpp = 0;
static size_t g_fb_size = 0;

static size_t g_font_width = 8;
static size_t g_font_height = 16;
static size_t g_max_cols = 0;
static size_t g_max_rows = 0;

#define PADDING_Y 2

static const uint32_t VGA_PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA, 
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA, 
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF, 
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

/* --- Hardware Drawing --- */

/*
 * put_pixel - Draws a single pixel to the framebuffer.
 */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb_width || y >= g_fb_height) return; 
    size_t offset = (y * g_fb_pitch) + (x * (g_fb_bpp / 8));
    uint8_t* dst = g_fb_addr + offset;
    if (g_fb_bpp == 32) {
        *(uint32_t*)dst = color;
    } else if (g_fb_bpp == 24) {
        dst[0] = color & 0xFF; dst[1] = (color >> 8) & 0xFF; dst[2] = (color >> 16) & 0xFF;
    }
}

/*
 * draw_glyph - Renders a PSF1 glyph at the specified pixel coordinates.
 */
static void draw_glyph(uint8_t* glyph, size_t px, size_t py, uint32_t fg, uint32_t bg) {
    for (size_t y = 0; y < g_font_height; y++) {
        uint8_t row = glyph[y];
        for (size_t x = 0; x < g_font_width; x++) {
            bool active = (row >> (7 - x)) & 1;
            put_pixel(px + x, py + y, active ? fg : bg);
        }
    }
}

/*
 * get_glyph_ptr - Returns a pointer to the glyph data for a given Unicode codepoint.
 */
static uint8_t* get_glyph_ptr(uint32_t codepoint) {
    psf1_font_t* font = font_get_current();
    if (!font) return NULL;
    uint8_t index = unicode_to_cp437(codepoint);
    if (index == 0 && codepoint != 0) index = 0x3F; 
    return (uint8_t*)font->glyph_buffer + (index * font->header->charsize);
}

/* --- Cursor Rendering --- */

/*
 * con_render_cursor - Draws or erases the console cursor.
 */
static void con_render_cursor(console_t* con, bool on) {
    if (!g_fb_addr) return;
    
    extern tty_t* g_active_tty;
    if (!g_active_tty || g_active_tty->console != con) return;

    size_t px = con->cursor_x * g_font_width;
    size_t py = con->cursor_y * (g_font_height + PADDING_Y);

    if (on) {
        uint32_t color = VGA_PALETTE[con->fg_color];
        for (size_t y = 0; y < g_font_height; y++) {
            for (size_t x = 0; x < g_font_width; x++) {
                put_pixel(px + x, py + y, color);
            }
        }
    } else {
        size_t idx = con->cursor_y * con->width + con->cursor_x;
        console_char_t c = con->buffer[idx];
        draw_glyph(get_glyph_ptr(c.codepoint), px, py, VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
    }
}

/* --- Internal Logic (Assumes Lock Held) --- */

/*
 * console_refresh_locked - Redraws the entire console content to the framebuffer.
 */
static void console_refresh_locked(console_t* con) {
    if (!g_fb_addr) return;
    
    extern tty_t* g_active_tty;
    if (!g_active_tty || g_active_tty->console != con) return;

    for (size_t y = 0; y < con->height; y++) {
        for (size_t x = 0; x < con->width; x++) {
            console_char_t c = con->buffer[y * con->width + x];
            draw_glyph(get_glyph_ptr(c.codepoint), x * g_font_width, y * (g_font_height + PADDING_Y), 
                       VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
        }
    }
    
    if (con->cursor_enabled) {
        con_render_cursor(con, true);
    }
}

/*
 * scroll_inst - Scrolls the console content up by one line.
 */
static void scroll_inst(console_t* con) {
    if (con->cursor_enabled) con_render_cursor(con, false);

    size_t size = (con->height - 1) * con->width;
    memmove(con->buffer, con->buffer + con->width, size * sizeof(console_char_t));
    
    for (size_t x = 0; x < con->width; x++) {
        size_t idx = (con->height - 1) * con->width + x;
        con->buffer[idx].codepoint = ' ';
        con->buffer[idx].fg = con->fg_color;
        con->buffer[idx].bg = con->bg_color;
    }
    con->cursor_y--;

    console_refresh_locked(con);
}

/*
 * handle_cp_inst - Internal handler for rendering characters and control codes.
 */
static void handle_cp_inst(console_t* con, uint32_t cp) {
    extern tty_t* g_active_tty;
    bool is_active = (g_active_tty && g_active_tty->console == con);

    if (is_active && con->cursor_enabled) {
        con_render_cursor(con, false);
    }

    if (cp == '\n') {
        con->cursor_x = 0;
        con->cursor_y++;
    } else if (cp == '\r') {
        con->cursor_x = 0;
    } else if (cp == '\b') {
        if (con->cursor_x > 0) con->cursor_x--;
        size_t idx = con->cursor_y * con->width + con->cursor_x;
        con->buffer[idx].codepoint = ' ';
        if (is_active) {
            draw_glyph(get_glyph_ptr(' '), con->cursor_x * g_font_width, con->cursor_y * (g_font_height + PADDING_Y), 
                       VGA_PALETTE[con->fg_color], VGA_PALETTE[con->bg_color]);
        }
    } else if (cp == '\t') {
        con->cursor_x = (con->cursor_x + 4) & ~3;
    } else {
        if (con->cursor_x >= con->width) {
            con->cursor_x = 0;
            con->cursor_y++;
        }
        if (con->cursor_y >= con->height) {
            scroll_inst(con);
        }

        size_t idx = con->cursor_y * con->width + con->cursor_x;
        con->buffer[idx].codepoint = cp;
        con->buffer[idx].fg = con->fg_color;
        con->buffer[idx].bg = con->bg_color;

        if (is_active) {
            draw_glyph(get_glyph_ptr(cp), con->cursor_x * g_font_width, con->cursor_y * (g_font_height + PADDING_Y), 
                       VGA_PALETTE[con->fg_color], VGA_PALETTE[con->bg_color]);
        }
        con->cursor_x++;
    }

    if (con->cursor_y >= con->height) {
        scroll_inst(con);
    }

    if (is_active && con->cursor_enabled) {
        con_render_cursor(con, true);
    }
}

/* --- Instance Logic --- */

/*
 * con_init - Initializes a console instance.
 */
void con_init(console_t* con) {
    con->width = g_max_cols;
    con->height = g_max_rows;
    con->cursor_x = 0;
    con->cursor_y = 0;
    con->fg_color = CONSOLE_COLOR_WHITE;
    con->bg_color = CONSOLE_COLOR_BLACK;
    con->utf8_bytes_needed = 0;
    con->utf8_codepoint = 0;
    con->reentrancy_count = 0;
    
    con->cursor_enabled = true;
    
    spinlock_init(&con->lock, "console_lock");

    con->buffer = (console_char_t*)kmalloc(con->width * con->height * sizeof(console_char_t));
    if (!con->buffer) panic("Failed to allocate console backbuffer!");

    con_clear(con, CONSOLE_COLOR_BLACK);
}

/*
 * con_clear - Clears the console and fills the framebuffer with the specified background color.
 */
void con_clear(console_t* con, uint8_t background) {
    bool flags = spinlock_acquire(&con->lock);
    
    con->bg_color = background & 0xF;
    for (size_t i = 0; i < con->width * con->height; i++) {
        con->buffer[i].codepoint = ' ';
        con->buffer[i].fg = con->fg_color;
        con->buffer[i].bg = con->bg_color;
    }
    con->cursor_x = 0;
    con->cursor_y = 0;

    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console == con) {
        uint32_t bg_color = VGA_PALETTE[con->bg_color];
        if (g_fb_bpp == 32) {
            uint32_t* fb32 = (uint32_t*)g_fb_addr;
            size_t num_pixels = g_fb_size / 4;
            for (size_t i = 0; i < num_pixels; i++) {
                fb32[i] = bg_color;
            }
        } else {
            // Manual slow clear for other bit depths
            for (uint32_t y = 0; y < g_fb_height; y++) {
                for (uint32_t x = 0; x < g_fb_width; x++) {
                    put_pixel(x, y, bg_color);
                }
            }
        }

        if (con->cursor_enabled) {
            con_render_cursor(con, true);
        }
    }
    
    spinlock_release(&con->lock, flags);
}

/*
 * con_putc - High-level character output for a console instance (handles UTF-8).
 */
void con_putc(console_t* con, char character) {
    bool flags;
    bool locked = false;

    if (spinlock_try_acquire(&con->lock, &flags)) {
        locked = true;
    } else {
        return; 
    }
    
    uint8_t byte = (uint8_t)character;
    if (con->utf8_bytes_needed == 0) {
        if ((byte & 0x80) == 0) handle_cp_inst(con, byte);
        else if ((byte & 0xE0) == 0xC0) { con->utf8_bytes_needed = 1; con->utf8_codepoint = byte & 0x1F; }
        else if ((byte & 0xF0) == 0xE0) { con->utf8_bytes_needed = 2; con->utf8_codepoint = byte & 0x0F; }
        else if ((byte & 0xF8) == 0xF0) { con->utf8_bytes_needed = 3; con->utf8_codepoint = byte & 0x07; }
    } else {
        if ((byte & 0xC0) == 0x80) {
            con->utf8_codepoint = (con->utf8_codepoint << 6) | (byte & 0x3F);
            if (--con->utf8_bytes_needed == 0) handle_cp_inst(con, con->utf8_codepoint);
        } else { con->utf8_bytes_needed = 0; handle_cp_inst(con, 0xFFFD); }
    }
    
    if (locked) spinlock_release(&con->lock, flags);
}

/*
 * con_refresh - Public wrapper to refresh a console's display.
 */
void con_refresh(console_t* con) {
    if (!g_fb_addr) return;
    bool flags = spinlock_acquire(&con->lock);
    console_refresh_locked(con);
    spinlock_release(&con->lock, flags);
}

/*
 * con_set_color - Updates the current drawing colors for a console instance.
 */
void con_set_color(console_t* con, uint8_t foreground, uint8_t background) {
    bool flags = spinlock_acquire(&con->lock);
    con->fg_color = foreground & 0xF;
    con->bg_color = background & 0xF;
    spinlock_release(&con->lock, flags);
}

/*
 * con_set_cursor_enabled - Enables or disables the blinking caret for a console instance.
 */
void con_set_cursor_enabled(console_t* con, bool enabled) {
    bool flags = spinlock_acquire(&con->lock);
    if (con->cursor_enabled && !enabled) {
        con_render_cursor(con, false);
    } else if (!con->cursor_enabled && enabled) {
        con_render_cursor(con, true);
    }
    con->cursor_enabled = enabled;
    spinlock_release(&con->lock, flags);
}

/* --- Global Compatibility --- */

/*
 * console_init - Probes Multiboot information and initializes global framebuffer state.
 */
void console_init(multiboot_parser_t* parser) {
    font_init();
    multiboot_framebuffer_t* fb = multiboot_get_framebuffer(parser);
    if (!fb) panic("No framebuffer!");

    g_fb_phys = fb->addr;
    g_fb_width = fb->width;
    g_fb_height = fb->height;
    g_fb_pitch = fb->pitch;
    g_fb_bpp = fb->bpp;
    g_fb_size = g_fb_height * g_fb_pitch;

    void* virt_addr = NULL;
    vmm_alloc(vmm_kernel_get(), align_up(g_fb_size, PAGE_SIZE), VM_FLAG_MMIO | VM_FLAG_WRITE, (void*)g_fb_phys, &virt_addr);
    g_fb_addr = (uint8_t*)virt_addr;

    g_font_height = font_get_current()->header->charsize;
    g_max_cols = g_fb_width / g_font_width;
    g_max_rows = g_fb_height / (g_font_height + PADDING_Y);

    memset(g_fb_addr, 0, g_fb_size);
}

/*
 * console_print_char - Global accessor to print a character to the active TTY.
 */
void console_print_char(char character) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console) {
        con_putc(g_active_tty->console, character);
    }
}

/*
 * console_set_color - Global accessor to set colors for the active TTY.
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console) {
        con_set_color(g_active_tty->console, foreground, background);
    }
}

/*
 * console_set_cursor_enabled - Global accessor to toggle the cursor for the active TTY.
 */
void console_set_cursor_enabled(bool enabled) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console) {
        con_set_cursor_enabled(g_active_tty->console, enabled);
    }
}

/*
 * console_clear - Global accessor to clear the active TTY's display.
 */
void console_clear(uint8_t background) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console) {
        con_clear(g_active_tty->console, background);
    }
}

/*
 * console_get_width - Returns the console width in columns.
 */
size_t console_get_width() { return g_max_cols; }

/*
 * console_get_height - Returns the console height in rows.
 */
size_t console_get_height() { return g_max_rows; }
