/*
 * console.c - Framebuffer console implementation
 */

#include <kernel/drivers/console.h>
#include <kernel/drivers/font.h>
#include <kernel/drivers/tty.h>
#include <arch/x86_64/memory/paging.h>
#include <kernel/memory/heap.h>
#include <klibc/string.h>
#include <klibc/stdio.h>
#include <stdarg.h>

#pragma region Statistics

// Framebuffer hardware
static uint8_t* g_fb_addr   = NULL;
static uint64_t g_fb_phys   = 0;
static uint32_t g_fb_width  = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_pitch  = 0;
static uint32_t g_fb_bpp    = 0;
static size_t   g_fb_size   = 0;

static size_t g_font_width  = 8;
static size_t g_font_height = 16;
static size_t g_max_cols    = 0;
static size_t g_max_rows    = 0;

#define PADDING_Y 2

static const uint32_t VGA_PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

// Panic cursor/color (fb dims/addr shared with regular console via g_fb_*)
static uint32_t g_crash_cx  = 0;
static uint32_t g_crash_cy  = 0;
static uint8_t  g_crash_fg  = CONSOLE_COLOR_WHITE;
static uint8_t  g_crash_bg  = CONSOLE_COLOR_RED;
static char     g_crash_buf[2048];

#pragma region Hardware Drawing

/*
 * put_pixel - Writes one pixel at (x, y) to the framebuffer
 */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb_width || y >= g_fb_height) return;
    uint8_t* dst = g_fb_addr + y * g_fb_pitch + x * (g_fb_bpp / 8);
    if (g_fb_bpp == 32) *(uint32_t*)dst = color;
    else { dst[0] = color & 0xFF; dst[1] = (color >> 8) & 0xFF; dst[2] = (color >> 16) & 0xFF; }
}

/*
 * draw_glyph - Renders a PSF1 glyph at pixel coordinates (px, py)
 */
static void draw_glyph(uint8_t* glyph, size_t px, size_t py, uint32_t fg, uint32_t bg) {
    for (size_t y = 0; y < g_font_height; y++) {
        uint8_t row = glyph[y];
        for (size_t x = 0; x < g_font_width; x++)
            put_pixel(px + x, py + y, ((row >> (7 - x)) & 1) ? fg : bg);
    }
}

/*
 * get_glyph - Returns a pointer to the PSF1 glyph data for the given codepoint
 */
static uint8_t* get_glyph(uint32_t cp) {
    psf1_font_t* font = font_get_current();
    if (!font) return NULL;
    uint8_t idx = unicode_to_cp437(cp);
    if (idx == 0 && cp != 0) idx = 0x3F;
    return (uint8_t*)font->glyph_buffer + idx * font->header->charsize;
}

/*
 * render_cursor - Draws (on=true) or erases (on=false) the block cursor
 */
static void render_cursor(console_t* con, bool on) {
    if (!g_fb_addr) return;
    extern tty_t* g_active_tty;
    if (!g_active_tty || g_active_tty->console != con) return;
    size_t px = con->cursor_x * g_font_width;
    size_t py = con->cursor_y * (g_font_height + PADDING_Y);
    if (on) {
        uint32_t color = VGA_PALETTE[con->fg_color];
        for (size_t y = 0; y < g_font_height; y++)
            for (size_t x = 0; x < g_font_width; x++)
                put_pixel(px + x, py + y, color);
    } else {
        console_char_t c = con->buffer[con->cursor_y * con->width + con->cursor_x];
        draw_glyph(get_glyph(c.codepoint), px, py, VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
    }
}

#pragma region Instance Internals

/*
 * refresh_locked - Redraws the full console to the framebuffer (lock must be held)
 */
static void refresh_locked(console_t* con) {
    if (!g_fb_addr) return;
    extern tty_t* g_active_tty;
    if (!g_active_tty || g_active_tty->console != con) return;
    for (size_t y = 0; y < con->height; y++)
        for (size_t x = 0; x < con->width; x++) {
            console_char_t c = con->buffer[y * con->width + x];
            draw_glyph(get_glyph(c.codepoint), x * g_font_width, y * (g_font_height + PADDING_Y),
                       VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
        }
    if (con->cursor_enabled) render_cursor(con, true);
}

/*
 * scroll - Scrolls the content area (below header) up by one line
 */
static void scroll(console_t* con) {
    if (con->cursor_enabled) render_cursor(con, false);
    size_t first = con->header_rows;
    size_t rows  = con->height - first;
    if (!rows) return;
    if (rows > 1)
        kmemmove(con->buffer + first * con->width,
                 con->buffer + (first + 1) * con->width,
                 (rows - 1) * con->width * sizeof(console_char_t));
    for (size_t x = 0; x < con->width; x++) {
        size_t idx = (con->height - 1) * con->width + x;
        con->buffer[idx] = (console_char_t){ ' ', con->fg_color, con->bg_color };
    }
    con->cursor_y--;
    if (con->cursor_y < first) con->cursor_y = first;
    refresh_locked(con);
}

/*
 * emit_cp - Renders a Unicode codepoint or control character to the console
 */
static void emit_cp(console_t* con, uint32_t cp) {
    extern tty_t* g_active_tty;
    bool active = (g_active_tty && g_active_tty->console == con);
    if (active && con->cursor_enabled) render_cursor(con, false);

    if (cp == '\n')      { con->cursor_x = 0; con->cursor_y++; }
    else if (cp == '\r') { con->cursor_x = 0; }
    else if (cp == '\b') {
        if (con->cursor_x > 0) con->cursor_x--;
        size_t idx = con->cursor_y * con->width + con->cursor_x;
        con->buffer[idx].codepoint = ' ';
        if (active) draw_glyph(get_glyph(' '), con->cursor_x * g_font_width,
                               con->cursor_y * (g_font_height + PADDING_Y),
                               VGA_PALETTE[con->fg_color], VGA_PALETTE[con->bg_color]);
    } else if (cp == '\t') {
        con->cursor_x = (con->cursor_x + 4) & ~3;
    } else {
        if (con->cursor_x >= con->width) { con->cursor_x = 0; con->cursor_y++; }
        if (con->cursor_y >= con->height) scroll(con);
        size_t idx = con->cursor_y * con->width + con->cursor_x;
        con->buffer[idx] = (console_char_t){ cp, con->fg_color, con->bg_color };
        if (active) draw_glyph(get_glyph(cp), con->cursor_x * g_font_width,
                               con->cursor_y * (g_font_height + PADDING_Y),
                               VGA_PALETTE[con->fg_color], VGA_PALETTE[con->bg_color]);
        con->cursor_x++;
    }
    if (con->cursor_y >= con->height) scroll(con);
    if (active && con->cursor_enabled) render_cursor(con, true);
}

#pragma region Instance API

/*
 * con_init - Initializes a console instance (allocates backbuffer, resets state)
 */
bool con_init(console_t* con) {
    con->width = g_max_cols; con->height = g_max_rows;
    con->cursor_x = 0; con->cursor_y = 0;
    con->fg_color = CONSOLE_COLOR_WHITE; con->bg_color = CONSOLE_COLOR_BLACK;
    con->utf8_bytes_needed = 0; con->utf8_codepoint = 0;
    con->ansi_state = 0; con->reentrancy_count = 0;
    con->cursor_enabled = true; con->header_rows = 0;
    spinlock_init(&con->lock, "console_lock");
    con->buffer = (console_char_t*)kmalloc(con->width * con->height * sizeof(console_char_t));
    if (!con->buffer) return false;
    con_clear(con, CONSOLE_COLOR_BLACK);
    return true;
}

/*
 * con_clear - Clears the content area (below sticky header) and resets the cursor
 */
void con_clear(console_t* con, uint8_t background) {
    bool flags = spinlock_acquire(&con->lock);
    con->bg_color = background & 0xF;
    for (size_t y = con->header_rows; y < con->height; y++)
        for (size_t x = 0; x < con->width; x++) {
            size_t i = y * con->width + x;
            con->buffer[i] = (console_char_t){ ' ', con->fg_color, con->bg_color };
        }
    con->cursor_x = 0; con->cursor_y = con->header_rows;
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console == con) {
        uint32_t bg_color = VGA_PALETTE[con->bg_color];
        size_t start_py  = con->header_rows * (g_font_height + PADDING_Y);
        if (g_fb_bpp == 32) {
            size_t off = start_py * g_fb_pitch / 4;
            uint32_t* p = (uint32_t*)g_fb_addr + off;
            for (size_t i = 0; i < (g_fb_size / 4) - off; i++) p[i] = bg_color;
        } else {
            for (uint32_t y = (uint32_t)start_py; y < g_fb_height; y++)
                for (uint32_t x = 0; x < g_fb_width; x++)
                    put_pixel(x, y, bg_color);
        }
        if (con->cursor_enabled) render_cursor(con, true);
    }
    spinlock_release(&con->lock, flags);
}

/*
 * con_putc - Outputs one byte to a console instance (handles UTF-8 and ANSI escapes)
 */
void con_putc(console_t* con, char character) {
    bool flags;
    if (!spinlock_try_acquire(&con->lock, &flags)) return;

    uint8_t byte = (uint8_t)character;

    if (con->ansi_state == 1) {
        if (byte == '[') con->ansi_state = 2;
        else { con->ansi_state = 0; emit_cp(con, '\x1b'); emit_cp(con, byte); }
        spinlock_release(&con->lock, flags); return;
    } else if (con->ansi_state == 2) {
        if      (byte == 'H') { con->cursor_x = 0; con->cursor_y = con->header_rows; con->ansi_state = 0; }
        else if (byte == '2') { con->ansi_state = 3; }
        else                  { con->ansi_state = 0; }
        spinlock_release(&con->lock, flags); return;
    } else if (con->ansi_state == 3) {
        if (byte == 'J') {
            con->ansi_state = 0;
            extern tty_t* g_active_tty;
            bool active = (g_active_tty && g_active_tty->console == con);
            if (active && con->cursor_enabled) render_cursor(con, false);
            for (size_t y = con->header_rows; y < con->height; y++)
                for (size_t x = 0; x < con->width; x++) {
                    size_t i = y * con->width + x;
                    if (con->buffer[i].codepoint != ' ' || con->buffer[i].bg != con->bg_color) {
                        con->buffer[i] = (console_char_t){ ' ', con->fg_color, con->bg_color };
                        if (active) draw_glyph(get_glyph(' '), x * g_font_width,
                                               y * (g_font_height + PADDING_Y),
                                               VGA_PALETTE[con->fg_color], VGA_PALETTE[con->bg_color]);
                    }
                }
            con->cursor_x = 0; con->cursor_y = con->header_rows;
            if (active && con->cursor_enabled) render_cursor(con, true);
        } else { con->ansi_state = 0; }
        spinlock_release(&con->lock, flags); return;
    } else if (byte == '\x1b') {
        con->ansi_state = 1;
        spinlock_release(&con->lock, flags); return;
    }

    if (con->utf8_bytes_needed == 0) {
        if      ((byte & 0x80) == 0x00) emit_cp(con, byte);
        else if ((byte & 0xE0) == 0xC0) { con->utf8_bytes_needed = 1; con->utf8_codepoint = byte & 0x1F; }
        else if ((byte & 0xF0) == 0xE0) { con->utf8_bytes_needed = 2; con->utf8_codepoint = byte & 0x0F; }
        else if ((byte & 0xF8) == 0xF0) { con->utf8_bytes_needed = 3; con->utf8_codepoint = byte & 0x07; }
    } else {
        if ((byte & 0xC0) == 0x80) {
            con->utf8_codepoint = (con->utf8_codepoint << 6) | (byte & 0x3F);
            if (--con->utf8_bytes_needed == 0) emit_cp(con, con->utf8_codepoint);
        } else { con->utf8_bytes_needed = 0; emit_cp(con, 0xFFFD); }
    }
    spinlock_release(&con->lock, flags);
}

/*
 * con_refresh - Redraws the entire console to the framebuffer
 */
void con_refresh(console_t* con) {
    if (!g_fb_addr) return;
    bool flags = spinlock_acquire(&con->lock);
    refresh_locked(con);
    spinlock_release(&con->lock, flags);
}

/*
 * con_set_color - Updates the foreground and background color for a console instance
 */
void con_set_color(console_t* con, uint8_t foreground, uint8_t background) {
    bool flags = spinlock_acquire(&con->lock);
    con->fg_color = foreground & 0xF; con->bg_color = background & 0xF;
    spinlock_release(&con->lock, flags);
}

/*
 * con_enable_cursor - Enables or disables the block cursor for a console instance
 */
void con_enable_cursor(console_t* con, bool enabled) {
    bool flags = spinlock_acquire(&con->lock);
    if (con->cursor_enabled && !enabled)  render_cursor(con, false);
    else if (!con->cursor_enabled && enabled) render_cursor(con, true);
    con->cursor_enabled = enabled;
    spinlock_release(&con->lock, flags);
}

/*
 * con_header_init - Reserves the top N rows as a sticky header (never scrolled)
 */
void con_header_init(console_t* con, size_t rows) {
    if (!con || rows >= con->height) return;
    bool flags = spinlock_acquire(&con->lock);
    con->header_rows = rows;
    for (size_t y = 0; y < rows; y++)
        for (size_t x = 0; x < con->width; x++) {
            size_t i = y * con->width + x;
            con->buffer[i] = (console_char_t){ ' ', CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK };
        }
    if (con->cursor_y < rows) { con->cursor_y = rows; con->cursor_x = 0; }
    spinlock_release(&con->lock, flags);
}

/*
 * con_header_write - Writes centred text into a sticky header row and redraws it
 */
void con_header_write(console_t* con, size_t row, const char* text, uint8_t fg, uint8_t bg) {
    if (!con || row >= con->header_rows || !text) return;
    bool flags = spinlock_acquire(&con->lock);
    size_t len = kstrlen(text);
    size_t pad = (len < con->width) ? (con->width - len) / 2 : 0;
    for (size_t x = 0; x < con->width; x++)
        con->buffer[row * con->width + x] = (console_char_t){ ' ', fg, bg };
    for (size_t c = 0; c < len && (pad + c) < con->width; c++)
        con->buffer[row * con->width + pad + c] = (console_char_t){ (uint8_t)text[c], fg, bg };
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console == con) {
        size_t py = row * (g_font_height + PADDING_Y);
        for (size_t x = 0; x < con->width; x++) {
            console_char_t ch = con->buffer[row * con->width + x];
            draw_glyph(get_glyph(ch.codepoint), x * g_font_width, py,
                       VGA_PALETTE[ch.fg], VGA_PALETTE[ch.bg]);
        }
    }
    spinlock_release(&con->lock, flags);
}

#pragma region Crash Console

/*
 * con_crash_width - Returns the width of the crash console in characters
 */
size_t con_crash_width(void){ 
    return g_max_cols; 
}

/*
 * crash_pix - Writes one pixel directly to g_fb_addr (panic path, no locks)
 */
static inline void crash_pix(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_fb_addr || x >= g_fb_width || y >= g_fb_height) return;
    uint8_t* dst = g_fb_addr + y * g_fb_pitch + x * (g_fb_bpp / 8);
    if (g_fb_bpp == 32) *(uint32_t*)dst = color;
    else { dst[0] = color & 0xFF; dst[1] = (color >> 8) & 0xFF; dst[2] = (color >> 16) & 0xFF; }
}

/*
 * crash_scroll - Pixel-level scroll of the crash view by one text row
 */
static void crash_scroll(void) {
    uint32_t row_h   = (uint32_t)g_font_height + PADDING_Y;
    size_t row_bytes = row_h * g_fb_pitch;
    size_t total     = (size_t)g_fb_height * g_fb_pitch;
    kmemmove(g_fb_addr, g_fb_addr + row_bytes, total - row_bytes);
    uint32_t bg   = VGA_PALETTE[g_crash_bg];
    uint8_t* last = g_fb_addr + total - row_bytes;
    if (g_fb_bpp == 32) {
        uint32_t* p = (uint32_t*)last;
        for (size_t i = 0; i < row_bytes / 4; i++) p[i] = bg;
    } else {
        for (uint32_t y = 0; y < row_h; y++)
            for (uint32_t x = 0; x < g_fb_width; x++)
                crash_pix(x, g_fb_height - row_h + y, bg);
    }
    if (g_crash_cy > 0) g_crash_cy = (uint32_t)g_max_rows - 1;
}

/*
 * crash_emit - Renders one ASCII byte directly to the framebuffer
 */
static void crash_emit(uint8_t c) {
    uint32_t row_h = (uint32_t)g_font_height + PADDING_Y;
    if      (c == '\n') { g_crash_cx = 0; g_crash_cy++; }
    else if (c == '\r') { g_crash_cx = 0; }
    else if (c == '\t') { g_crash_cx = (g_crash_cx + 4) & ~3u; }
    else {
        if (g_crash_cx >= (uint32_t)g_max_cols) { g_crash_cx = 0; g_crash_cy++; }
        if (g_crash_cy >= (uint32_t)g_max_rows) crash_scroll();
        uint32_t px = g_crash_cx * 8;
        uint32_t py = g_crash_cy * row_h;
        uint8_t* glyph = get_glyph(c);
        if (glyph)
            for (uint32_t y = 0; y < (uint32_t)g_font_height; y++) {
                uint8_t bits = glyph[y];
                for (uint32_t x = 0; x < 8; x++)
                    crash_pix(px + x, py + y,
                        ((bits >> (7 - x)) & 1) ? VGA_PALETTE[g_crash_fg] : VGA_PALETTE[g_crash_bg]);
            }
        g_crash_cx++;
    }
    if (g_crash_cy >= (uint32_t)g_max_rows) crash_scroll();
}

/*
 * con_crash_clear - Fills the framebuffer with bg color and resets the panic cursor
 */
void con_crash_clear(uint8_t bg) {
    if (!g_fb_addr) return;
    g_crash_bg = bg & 0xF; g_crash_cx = 0; g_crash_cy = 0;
    uint32_t color = VGA_PALETTE[g_crash_bg];
    size_t total = (size_t)g_fb_height * g_fb_pitch;
    if (g_fb_bpp == 32) {
        uint32_t* p = (uint32_t*)g_fb_addr;
        for (size_t i = 0; i < total / 4; i++) p[i] = color;
    } else {
        for (uint32_t y = 0; y < g_fb_height; y++)
            for (uint32_t x = 0; x < g_fb_width; x++)
                crash_pix(x, y, color);
    }
}

/*
 * con_crash_puts - Outputs a null-terminated string directly to the framebuffer
 */
void con_crash_puts(const char* s) {
    if (!g_fb_addr || !s) return;
    while (*s) crash_emit((uint8_t)*s++);
}

/*
 * con_crash_printf - Formatted direct output to the framebuffer
 */
void con_crash_printf(const char* fmt, ...) {
    if (!g_fb_addr) return;
    va_list args;
    va_start(args, fmt);
    kvsnprintf(g_crash_buf, sizeof(g_crash_buf), fmt, args);
    va_end(args);
    con_crash_puts(g_crash_buf);
}

#pragma region Globals

/*
 * console_init - Probes multiboot framebuffer info and initializes all console state
 */
void console_init(multiboot_parser_t* parser) {
    font_init();
    multiboot_framebuffer_t* fb = multiboot_get_framebuffer(parser);
    if (!fb) return;
    g_fb_phys   = fb->addr;
    g_fb_width  = fb->width;  g_fb_height = fb->height;
    g_fb_pitch  = fb->pitch;  g_fb_bpp    = fb->bpp;
    g_fb_size   = g_fb_height * g_fb_pitch;
    g_fb_addr   = (uint8_t*)PHYSMAP_P2V(g_fb_phys);
    g_font_height = font_get_current()->header->charsize;
    g_max_cols = g_fb_width / g_font_width;
    g_max_rows = g_fb_height / (g_font_height + PADDING_Y);
    kmemset(g_fb_addr, 0, g_fb_size);
}

/*
 * console_print_char - Global accessor: prints a character to the active TTY
 */
void console_print_char(char character) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console)
        con_putc(g_active_tty->console, character);
}

/*
 * console_set_color - Global accessor: sets colors on the active TTY
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console)
        con_set_color(g_active_tty->console, foreground, background);
}

/*
 * console_enable_cursor - Global accessor: toggles the cursor on the active TTY
 */
void console_enable_cursor(bool enabled) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console)
        con_enable_cursor(g_active_tty->console, enabled);
}

/*
 * console_clear - Global accessor: clears the active TTY's display
 */
void console_clear(uint8_t background) {
    extern tty_t* g_active_tty;
    if (g_active_tty && g_active_tty->console)
        con_clear(g_active_tty->console, background);
}

size_t console_get_width()  { return g_max_cols; }
size_t console_get_height() { return g_max_rows; }
