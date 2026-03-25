/*
 * console.c - Framebuffer console implementation
 *
 * This is the lowest level of the output stack, solely responsible for 
 * rendering characters to the framebuffer. It provides a simple API for drawing
 * characters, managing the cursor, setting colors and controlling a sticky header area.
 * 
 * TTY and higher-level console abstractions are built on top of this.
 * 
 * For panic specifically, the console can be used directly through the con_crash_*
 * API without relying on the rest of the console/TTY subsystem, which may be in an 
 * inconsistent state during a panic.
 * 
 * Author: u/ApparentlyPlus
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
static uint8_t* fb = NULL;
static uint64_t fb_phys = 0;
static uint32_t fb_w = 0;
static uint32_t fb_h = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_bpp = 0;
static size_t fb_sz = 0;

static size_t fw = 8;
static size_t fh = 16;
static size_t cols = 0;
static size_t rows = 0;

#define PADDING_Y 2

static const uint32_t VGA_PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

// Panic cursor/color (fb dims/addr shared with regular console via fb_*)
static uint32_t ccx = 0;
static uint32_t ccy = 0;
static uint8_t cfg = CONSOLE_COLOR_WHITE;
static uint8_t cbg = CONSOLE_COLOR_RED;
static char     cbuf[2048];

#pragma region Hardware Drawing

/*
 * put_pixel - Writes one pixel at (x, y) to the framebuffer
 */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_w || y >= fb_h) return;
    uint8_t* dst = fb + y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) *(uint32_t*)dst = color;
    else { dst[0] = color & 0xFF; dst[1] = (color >> 8) & 0xFF; dst[2] = (color >> 16) & 0xFF; }
}

/*
 * draw_glyph - Renders a PSF1 glyph at pixel coordinates (px, py)
 */
static void draw_glyph(uint8_t* glyph, size_t px, size_t py, uint32_t fg, uint32_t bg) {
    for (size_t y = 0; y < fh; y++) {
        uint8_t row = glyph[y];
        for (size_t x = 0; x < fw; x++)
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
    if (!fb) return;
    extern tty_t* active_tty;
    if (!active_tty || active_tty->console != con) return;
    size_t px = con->cx * fw;
    size_t py = con->cy * (fh + PADDING_Y);
    if (on) {
        uint32_t color = VGA_PALETTE[con->fg];
        for (size_t y = 0; y < fh; y++)
            for (size_t x = 0; x < fw; x++)
                put_pixel(px + x, py + y, color);
    } else {
        console_char_t c = con->buffer[con->cy * con->width + con->cx];
        draw_glyph(get_glyph(c.codepoint), px, py, VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
    }
}

#pragma region Instance Internals

/*
 * refresh_locked - Redraws the full console to the framebuffer (lock must be held)
 */
static void refresh_locked(console_t* con) {
    if (!fb) return;
    extern tty_t* active_tty;
    if (!active_tty || active_tty->console != con) return;
    for (size_t y = 0; y < con->height; y++)
        for (size_t x = 0; x < con->width; x++) {
            console_char_t c = con->buffer[y * con->width + x];
            draw_glyph(get_glyph(c.codepoint), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
        }
    if (con->on) render_cursor(con, true);
}

/*
 * scroll - Scrolls the content area (below header) up by one line
 * Uses a pixel-level memmove on the framebuffer instead of a full redraw.
 */
static void scroll(console_t* con) {
    if (con->on) render_cursor(con, false);
    size_t first = con->header_rows;
    size_t rows = con->height - first;
    if (!rows) return;

    if (rows > 1)
        kmemmove(con->buffer + first * con->width, con->buffer + (first + 1) * con->width, (rows - 1) * con->width * sizeof(console_char_t));
    for (size_t x = 0; x < con->width; x++) {
        size_t idx = (con->height - 1) * con->width + x;
        con->buffer[idx] = (console_char_t){ ' ', con->fg, con->bg };
    }
    con->cy--;
    if (con->cy < first) con->cy = first;

    if (fb) {
        extern tty_t* active_tty;
        if (active_tty && active_tty->console == con) {
            uint32_t row_h = (uint32_t)(fh + PADDING_Y);
            size_t first_px = first * row_h;
            size_t src_off = (first_px + row_h) * fb_pitch;
            size_t dst_off = first_px * fb_pitch;
            size_t move_bytes = (rows - 1) * row_h * fb_pitch;

            if (move_bytes > 0)
                kmemmove(fb + dst_off, fb + src_off, move_bytes);

            uint32_t bg_color = VGA_PALETTE[con->bg];
            size_t last_row_off = (con->height - 1) * row_h * fb_pitch;
            size_t last_row_len = row_h * fb_pitch;
            if (fb_bpp == 32) {
                uint32_t* p = (uint32_t*)(fb + last_row_off);
                for (size_t i = 0; i < last_row_len / 4; i++) p[i] = bg_color;
            } else {
                for (uint32_t y = 0; y < row_h; y++)
                    for (uint32_t x = 0; x < fb_w; x++)
                        put_pixel(x, (uint32_t)((con->height - 1) * row_h) + y, bg_color);
            }
        }
    }
}

/*
 * emit_cp - Renders a Unicode codepoint or control character to the console
 */
static void emit_cp(console_t* con, uint32_t cp) {
    extern tty_t* active_tty;
    bool active = (active_tty && active_tty->console == con);
    if (active && con->on) render_cursor(con, false);

    if (cp == '\n')      { con->cx = 0; con->cy++; }
    else if (cp == '\r') { con->cx = 0; }
    else if (cp == '\b') {
        if (con->cx > 0) con->cx--;
        size_t idx = con->cy * con->width + con->cx;
        con->buffer[idx].codepoint = ' ';
        if (active) draw_glyph(get_glyph(' '), con->cx * fw, con->cy * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
    } else if (cp == '\t') {
        con->cx = (con->cx + 4) & ~3;
    } else {
        if (con->cx >= con->width) { con->cx = 0; con->cy++; }
        if (con->cy >= con->height) scroll(con);
        size_t idx = con->cy * con->width + con->cx;
        con->buffer[idx] = (console_char_t){ cp, con->fg, con->bg };
        if (active) draw_glyph(get_glyph(cp), con->cx * fw, con->cy * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
        con->cx++;
    }
    if (con->cy >= con->height) scroll(con);
    if (active && con->on) render_cursor(con, true);
}

#pragma region Instance API

/*
 * con_init - Initializes a console instance (allocates backbuffer, resets state)
 */
bool con_init(console_t* con) {
    con->width = cols; con->height = rows;
    con->cx = 0; con->cy = 0;
    con->fg = CONSOLE_COLOR_WHITE; con->bg = CONSOLE_COLOR_BLACK;
    con->u8n = 0; con->u8cp = 0;
    con->ansi_st = 0; con->reent = 0;
    con->on = true; con->header_rows = 0;
    con->buffer = kmalloc(con->width * con->height * sizeof(console_char_t));
    if (!con->buffer) return false;
    spinlock_init(&con->lock, "console_lock");
    con_clear(con, CONSOLE_COLOR_BLACK);
    return true;
}

/*
 * con_clear - Clears the content area (below sticky header) and resets the cursor
 */
void con_clear(console_t* con, uint8_t background) {
    bool flags = spinlock_acquire(&con->lock);
    con->bg = background & 0xF;
    for (size_t y = con->header_rows; y < con->height; y++)
        for (size_t x = 0; x < con->width; x++) {
            size_t i = y * con->width + x;
            con->buffer[i] = (console_char_t){ ' ', con->fg, con->bg };
        }
    con->cx = 0; con->cy = con->header_rows;
    extern tty_t* active_tty;
    if (active_tty && active_tty->console == con) {
        uint32_t bg_color = VGA_PALETTE[con->bg];
        size_t start_py = con->header_rows * (fh + PADDING_Y);
        if (fb_bpp == 32) {
            size_t off = start_py * fb_pitch / 4;
            uint32_t* p = (uint32_t*)fb + off;
            for (size_t i = 0; i < (fb_sz / 4) - off; i++) p[i] = bg_color;
        } else {
            for (uint32_t y = (uint32_t)start_py; y < fb_h; y++)
                for (uint32_t x = 0; x < fb_w; x++)
                    put_pixel(x, y, bg_color);
        }
        if (con->on) render_cursor(con, true);
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

    if (con->ansi_st == 1) {
        if (byte == '[') con->ansi_st = 2;
        else { con->ansi_st = 0; emit_cp(con, '\x1b'); emit_cp(con, byte); }
        spinlock_release(&con->lock, flags); return;
    } else if (con->ansi_st == 2) {
        if      (byte == 'H') { con->cx = 0; con->cy = con->header_rows; con->ansi_st = 0; }
        else if (byte == '2') { con->ansi_st = 3; }
        else                  { con->ansi_st = 0; }
        spinlock_release(&con->lock, flags); return;
    } else if (con->ansi_st == 3) {
        if (byte == 'J') {
            con->ansi_st = 0;
            extern tty_t* active_tty;
            bool active = (active_tty && active_tty->console == con);
            if (active && con->on) render_cursor(con, false);
            for (size_t y = con->header_rows; y < con->height; y++)
                for (size_t x = 0; x < con->width; x++) {
                    size_t i = y * con->width + x;
                    if (con->buffer[i].codepoint != ' ' || con->buffer[i].bg != con->bg) {
                        con->buffer[i] = (console_char_t){ ' ', con->fg, con->bg };
                        if (active) draw_glyph(get_glyph(' '), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
                    }
                }
            con->cx = 0; con->cy = con->header_rows;
            if (active && con->on) render_cursor(con, true);
        } else { con->ansi_st = 0; }
        spinlock_release(&con->lock, flags); return;
    } else if (byte == '\x1b') {
        con->ansi_st = 1;
        spinlock_release(&con->lock, flags); return;
    }

    if (con->u8n == 0) {
        if      ((byte & 0x80) == 0x00) emit_cp(con, byte);
        else if ((byte & 0xE0) == 0xC0) { con->u8n = 1; con->u8cp = byte & 0x1F; }
        else if ((byte & 0xF0) == 0xE0) { con->u8n = 2; con->u8cp = byte & 0x0F; }
        else if ((byte & 0xF8) == 0xF0) { con->u8n = 3; con->u8cp = byte & 0x07; }
    } else {
        if ((byte & 0xC0) == 0x80) {
            con->u8cp = (con->u8cp << 6) | (byte & 0x3F);
            if (--con->u8n == 0) emit_cp(con, con->u8cp);
        } else { con->u8n = 0; emit_cp(con, 0xFFFD); }
    }
    spinlock_release(&con->lock, flags);
}

/*
 * con_refresh - Redraws the entire console to the framebuffer
 */
void con_refresh(console_t* con) {
    if (!fb) return;
    bool flags = spinlock_acquire(&con->lock);
    refresh_locked(con);
    spinlock_release(&con->lock, flags);
}

/*
 * con_set_color - Updates the foreground and background color for a console instance
 */
void con_set_color(console_t* con, uint8_t foreground, uint8_t background) {
    bool flags = spinlock_acquire(&con->lock);
    con->fg = foreground & 0xF; con->bg = background & 0xF;
    spinlock_release(&con->lock, flags);
}

/*
 * con_enable_cursor - Enables or disables the block cursor for a console instance
 */
void con_enable_cursor(console_t* con, bool enabled) {
    bool flags = spinlock_acquire(&con->lock);
    if (con->on && !enabled)  render_cursor(con, false);
    else if (!con->on && enabled) render_cursor(con, true);
    con->on = enabled;
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
    if (con->cy < rows) { con->cy = rows; con->cx = 0; }
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
    extern tty_t* active_tty;
    if (active_tty && active_tty->console == con) {
        size_t py = row * (fh + PADDING_Y);
        for (size_t x = 0; x < con->width; x++) {
            console_char_t ch = con->buffer[row * con->width + x];
            draw_glyph(get_glyph(ch.codepoint), x * fw, py, VGA_PALETTE[ch.fg], VGA_PALETTE[ch.bg]);
        }
    }
    spinlock_release(&con->lock, flags);
}

#pragma region Crash Console

/*
 * con_crash_width - Returns the width of the crash console in characters
 */
size_t con_crash_width(void){ 
    return cols; 
}

/*
 * crash_pix - Writes one pixel directly to fb (panic path, no locks)
 */
static inline void crash_pix(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb || x >= fb_w || y >= fb_h) return;
    uint8_t* dst = fb + y * fb_pitch + x * (fb_bpp / 8);
    if (fb_bpp == 32) *(uint32_t*)dst = color;
    else { dst[0] = color & 0xFF; dst[1] = (color >> 8) & 0xFF; dst[2] = (color >> 16) & 0xFF; }
}

/*
 * crash_scroll - Pixel-level scroll of the crash view by one text row
 */
static void crash_scroll(void) {
    uint32_t row_h = (uint32_t)fh + PADDING_Y;
    size_t row_bytes = row_h * fb_pitch;
    size_t total = (size_t)fb_h * fb_pitch;
    kmemmove(fb, fb + row_bytes, total - row_bytes);
    uint32_t bg = VGA_PALETTE[cbg];
    uint8_t* last = fb + total - row_bytes;
    if (fb_bpp == 32) {
        uint32_t* p = (uint32_t*)last;
        for (size_t i = 0; i < row_bytes / 4; i++) p[i] = bg;
    } else {
        for (uint32_t y = 0; y < row_h; y++)
            for (uint32_t x = 0; x < fb_w; x++)
                crash_pix(x, fb_h - row_h + y, bg);
    }
    if (ccy > 0) ccy = (uint32_t)rows - 1;
}

/*
 * crash_emit - Renders one ASCII byte directly to the framebuffer
 */
static void crash_emit(uint8_t c) {
    uint32_t row_h = (uint32_t)fh + PADDING_Y;
    if      (c == '\n') { ccx = 0; ccy++; }
    else if (c == '\r') { ccx = 0; }
    else if (c == '\t') { ccx = (ccx + 4) & ~3u; }
    else {
        if (ccx >= (uint32_t)cols) { ccx = 0; ccy++; }
        if (ccy >= (uint32_t)rows) crash_scroll();
        uint32_t px = ccx * 8;
        uint32_t py = ccy * row_h;
        uint8_t* glyph = get_glyph(c);
        if (glyph)
            for (uint32_t y = 0; y < (uint32_t)fh; y++) {
                uint8_t bits = glyph[y];
                for (uint32_t x = 0; x < 8; x++)
                    crash_pix(px + x, py + y, ((bits >> (7 - x)) & 1) ? VGA_PALETTE[cfg] : VGA_PALETTE[cbg]);
            }
        ccx++;
    }
    if (ccy >= (uint32_t)rows) crash_scroll();
}

/*
 * con_crash_clear - Fills the framebuffer with bg color and resets the panic cursor
 */
void con_crash_clear(uint8_t bg) {
    if (!fb) return;
    cbg = bg & 0xF; ccx = 0; ccy = 0;
    uint32_t color = VGA_PALETTE[cbg];
    size_t total = (size_t)fb_h * fb_pitch;
    if (fb_bpp == 32) {
        uint32_t* p = (uint32_t*)fb;
        for (size_t i = 0; i < total / 4; i++) p[i] = color;
    } else {
        for (uint32_t y = 0; y < fb_h; y++)
            for (uint32_t x = 0; x < fb_w; x++)
                crash_pix(x, y, color);
    }
}

/*
 * con_crash_puts - Outputs a null-terminated string directly to the framebuffer
 */
void con_crash_puts(const char* s) {
    if (!fb || !s) return;
    while (*s) crash_emit((uint8_t)*s++);
}

/*
 * con_crash_printf - Formatted direct output to the framebuffer
 */
void con_crash_printf(const char* fmt, ...) {
    if (!fb) return;
    va_list args;
    va_start(args, fmt);
    kvsnprintf(cbuf, sizeof(cbuf), fmt, args);
    va_end(args);
    con_crash_puts(cbuf);
}

#pragma region Globals

/*
 * console_init - Probes multiboot framebuffer info and initializes all console state
 */
void console_init(multiboot_parser_t* parser) {
    font_init();
    multiboot_framebuffer_t* mbfb = multiboot_get_framebuffer(parser);
    if (!mbfb) return;
    fb_phys = mbfb->addr;
    fb_w = mbfb->width;  fb_h = mbfb->height;
    fb_pitch = mbfb->pitch;  fb_bpp = mbfb->bpp;
    fb_sz = fb_h * fb_pitch;
    fb = (uint8_t*)PHYSMAP_P2V(fb_phys);
    fh = font_get_current()->header->charsize;
    cols = fb_w / fw;
    rows = fb_h / (fh + PADDING_Y);
    kmemset(fb, 0, fb_sz);
}

/*
 * console_print_char - Global accessor: prints a character to the active TTY
 */
void console_print_char(char character) {
    extern tty_t* active_tty;
    if (active_tty && active_tty->console)
        con_putc(active_tty->console, character);
}

/*
 * console_set_color - Global accessor: sets colors on the active TTY
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    extern tty_t* active_tty;
    if (active_tty && active_tty->console)
        con_set_color(active_tty->console, foreground, background);
}

/*
 * console_enable_cursor - Global accessor: toggles the cursor on the active TTY
 */
void console_enable_cursor(bool enabled) {
    extern tty_t* active_tty;
    if (active_tty && active_tty->console)
        con_enable_cursor(active_tty->console, enabled);
}

/*
 * console_clear - Global accessor: clears the active TTY's display
 */
void console_clear(uint8_t background) {
    extern tty_t* active_tty;
    if (active_tty && active_tty->console)
        con_clear(active_tty->console, background);
}

size_t console_get_width()  { return cols; }
size_t console_get_height() { return rows; }
