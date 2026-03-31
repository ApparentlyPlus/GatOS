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
    if (!fb || !glyph) return;

    // If the framebuffer isn't 32bpp we have to do per pixel writes anyway, so just do a simple loop
    // This is purely for backwards cokmpatibility with ancient hardware
    bool clipped = (px + fw > fb_w) || (py + fh > fb_h);
    if (fb_bpp != 32 || clipped) {
        for (size_t y = 0; y < fh; y++) {
            uint8_t row = glyph[y];
            for (size_t x = 0; x < fw; x++)
                put_pixel(px + x, py + y, ((row >> (7 - x)) & 1) ? fg : bg);
        }
        return;
    }

    // Fast path for 32bpp framebuffers is to write an entire glyph row as 8 pixels in a tight loop
    uint32_t* row_ptr = (uint32_t*)(fb + py * fb_pitch + px * 4);
    size_t pitch_u32 = fb_pitch / 4;
    uint32_t colors[2] = { bg, fg };
    for (size_t y = 0; y < fh; y++) {
        uint8_t bits = glyph[y];
        row_ptr[0] = colors[(bits >> 7) & 1];
        row_ptr[1] = colors[(bits >> 6) & 1];
        row_ptr[2] = colors[(bits >> 5) & 1];
        row_ptr[3] = colors[(bits >> 4) & 1];
        row_ptr[4] = colors[(bits >> 3) & 1];
        row_ptr[5] = colors[(bits >> 2) & 1];
        row_ptr[6] = colors[(bits >> 1) & 1];
        row_ptr[7] = colors[(bits >> 0) & 1];
        row_ptr += pitch_u32;
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

    // Checks
    if (!active_tty || active_tty->console != con) return;
    if (con->cx >= con->width || con->cy >= con->height) return;

    // Calculate pixel coordinates and render a solid block for the cursor background color
    size_t px = con->cx * fw;
    size_t py = con->cy * (fh + PADDING_Y);
    
    if (on) {
        // If the framebuffer is 32bpp and the glyph fits entirely onscreen
        // we can do a single memset for the entire cursor block
        bool clipped = (px + fw > fb_w) || (py + fh > fb_h);
        if (fb_bpp == 32 && !clipped) {
            uint32_t color = VGA_PALETTE[con->fg];
            uint32_t* row_ptr = (uint32_t*)(fb + py * fb_pitch + px * 4);
            size_t pitch_u32 = fb_pitch / 4;
            for (size_t y = 0; y < fh; y++) {
                row_ptr[0] = color; row_ptr[1] = color;
                row_ptr[2] = color; row_ptr[3] = color;
                row_ptr[4] = color; row_ptr[5] = color;
                row_ptr[6] = color; row_ptr[7] = color;
                row_ptr += pitch_u32;
            }
        } else {
            // fallback, much slower
            uint32_t color = VGA_PALETTE[con->fg];
            for (size_t y = 0; y < fh; y++)
                for (size_t x = 0; x < fw; x++)
                    put_pixel(px + x, py + y, color);
        }
    } else {
        // when erasing the cursor, we have to redraw the underlying glyph from the backbuffer
        console_char_t c = con->buffer[con->cy * con->width + con->cx];
        draw_glyph(get_glyph(c.codepoint), px, py, VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
    }
}

#pragma region Instance Internals

// Dirty tracking helpers
#define DIRTY_SET(con, i)   if ((con)->dirty) (con)->dirty[(i)>>3] |=  (1u << ((i) & 7))
#define DIRTY_CLR(con, i)   if ((con)->dirty) (con)->dirty[(i)>>3] &= ~(1u << ((i) & 7))
#define DIRTY_TST(con, i)   ((con)->dirty && ((con)->dirty[(i)>>3] & (1u << ((i) & 7))))
#define DIRTY_SET_ALL(con)  if ((con)->dirty) kmemset((con)->dirty, 0xFF, ((con)->width * (con)->height + 7) / 8)
#define DIRTY_CLR_ALL(con)  if ((con)->dirty) kmemset((con)->dirty, 0x00, ((con)->width * (con)->height + 7) / 8)

/*
 * flush_display - Flushes dirty character cells to the framebuffer
 * Assumption: No SMP
 */
static void flush_display(console_t* con) {
    if (!fb) return;
    extern tty_t* active_tty;

    // Only flush if this console is active
    if (!active_tty || active_tty->console != con) return;

    // If we don't have dirty tracking, just redraw everything in the backbuffer
    if (!con->dirty) {
        for (size_t y = 0; y < con->height; y++) {
            for (size_t x = 0; x < con->width; x++) {
                size_t idx = y * con->width + x;
                console_char_t c = con->buffer[idx];
                draw_glyph(get_glyph(c.codepoint), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
            }
        }
        if (con->on) render_cursor(con, true);
        return;
    }

    // With dirty tracking, we can skip cells that haven't changed since the last flush
    for (size_t y = 0; y < con->height; y++) {
        for (size_t x = 0; x < con->width; x++) {
            size_t idx = y * con->width + x;
            // skipskipskip
            if (!DIRTY_TST(con, idx)) continue;
            console_char_t c = con->buffer[idx];
            draw_glyph(get_glyph(c.codepoint), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
            DIRTY_CLR(con, idx);
        }
    }

    // After flushing, ensure the cursor is drawn on top of any recently changed cells
    if (con->on) render_cursor(con, true);
}

/*
 * scroll - Scrolls the content area (below header) up by one line
 */
static void scroll(console_t* con) {
    // Only erase the cursor from the framebuffer if we're rendering directly
    // In deferred mode the framebuffer is not touched until flush_display
    if (!con->defer_render && con->on) render_cursor(con, false);

    size_t first = con->header_rows;
    size_t rows = con->height - first;
    if (!rows) return;

    // Move all rows up by one in the backbuffer, then clear the last row
    if (rows > 1){
        kmemmove(con->buffer + first * con->width, con->buffer + (first + 1) * con->width, (rows - 1) * con->width * sizeof(console_char_t));
    }
    
    // Clear the last row in the backbuffer and update cursor position
    for (size_t x = 0; x < con->width; x++) {
        size_t idx = (con->height - 1) * con->width + x;
        con->buffer[idx] = (console_char_t){ ' ', con->fg, con->bg };
    }

    // Move cursor up one line, but not into the header area
    con->cy--;
    if (con->cy < first) con->cy = first;

    if (fb) {
        extern tty_t* active_tty;
        // If we have a framebuffer and this console is active
        // we can do an efficient pixel level scroll using memmove and a single fill for the new line
        if (active_tty && active_tty->console == con) {
            if (!con->defer_render) {
                uint32_t row_h = (uint32_t)(fh + PADDING_Y);
                size_t first_px = first * row_h;
                size_t src_off = (first_px + row_h) * fb_pitch;
                size_t dst_off = first_px * fb_pitch;
                size_t move_bytes = (rows - 1) * row_h * fb_pitch;

                if (move_bytes > 0)
                    kmemmove(fb + dst_off, fb + src_off, move_bytes);

                // Clear the last line
                uint32_t bg_color = VGA_PALETTE[con->bg];
                size_t last_row_off = (con->height - 1) * row_h * fb_pitch;
                size_t last_row_len = row_h * fb_pitch;

                // If the framebuffer is 32bpp we can do a single memset for the entire row, otherwise we have to loop
                if (fb_bpp == 32) {
                    uint32_t* p = (uint32_t*)(fb + last_row_off);
                    for (size_t i = 0; i < last_row_len / 4; i++) p[i] = bg_color;
                } else {
                    for (uint32_t y = 0; y < row_h; y++)
                        for (uint32_t x = 0; x < fb_w; x++)
                            put_pixel(x, (uint32_t)((con->height - 1) * row_h) + y, bg_color);
                }
            } else {
                // Deferred mode here
                // Mark all content cells dirty so flush_display redraws from the scrolled backbuffer
                DIRTY_SET_ALL(con);
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

    if (cp == '\n')      { con->cx = 0; con->cy++; }
    else if (cp == '\r') { con->cx = 0; }
    else if (cp == '\b') {
        if (con->cx > 0) con->cx--;
        size_t idx = con->cy * con->width + con->cx;
        con->buffer[idx].codepoint = ' ';
        // If this console is active and we're not deferring rendering
        // we can immediately erase the character on the framebuffer
        DIRTY_SET(con, idx);
        if (active && !con->defer_render) draw_glyph(get_glyph(' '), con->cx * fw, con->cy * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
    } else if (cp == '\t') {
        con->cx = (con->cx + 4) & ~3;
    } else {
        if (con->cx >= con->width) { con->cx = 0; con->cy++; }
        if (con->cy >= con->height) scroll(con);
        size_t idx = con->cy * con->width + con->cx;
        con->buffer[idx] = (console_char_t){ cp, con->fg, con->bg };
        DIRTY_SET(con, idx);
        if (active && !con->defer_render) draw_glyph(get_glyph(cp), con->cx * fw, con->cy * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
        con->cx++;
    }
    if (con->cy >= con->height) scroll(con);
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
    con->defer_render = false;
    con->buffer = kmalloc(con->width * con->height * sizeof(console_char_t));
    if (!con->buffer) return false;
    size_t dirty_bytes = (con->width * con->height + 7) / 8;
    con->dirty = kmalloc(dirty_bytes);
    if (con->dirty) kmemset(con->dirty, 0, dirty_bytes);
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

    // reset dirty tracking
    DIRTY_SET_ALL(con);
    extern tty_t* active_tty;

    // If this console is active and we're not deferring rendering, we can also clear the framebuffer immediately
    if (active_tty && active_tty->console == con && !con->defer_render) {
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

        // Since we've directly updated the framebuffer, we can clear all dirty flags to avoid redundant redraws until the next change
        DIRTY_CLR_ALL(con);
    }
    spinlock_release(&con->lock, flags);
}

/*
 * _con_process_byte - Process one raw byte through the UTF-8 decoder and ANSI state machine
 */
static void _con_process_byte(console_t* con, uint8_t byte) {
    // This function is lit
    if (con->ansi_st == 1) {
        if (byte == '[') con->ansi_st = 2;
        else { con->ansi_st = 0; emit_cp(con, '\x1b'); emit_cp(con, byte); }
        return;
    } else if (con->ansi_st == 2) {
        if      (byte == 'H') { con->cx = 0; con->cy = con->header_rows; con->ansi_st = 0; }
        else if (byte == '2') { con->ansi_st = 3; }
        else                  { con->ansi_st = 0; }
        return;
    } else if (con->ansi_st == 3) {
        if (byte == 'J') {
            con->ansi_st = 0;
            extern tty_t* active_tty;
            bool active = (active_tty && active_tty->console == con);
            for (size_t y = con->header_rows; y < con->height; y++)
                for (size_t x = 0; x < con->width; x++) {
                    size_t i = y * con->width + x;
                    if (con->buffer[i].codepoint != ' ' || con->buffer[i].bg != con->bg) {
                        con->buffer[i] = (console_char_t){ ' ', con->fg, con->bg };
                        DIRTY_SET(con, i);
                        if (active && !con->defer_render)
                            draw_glyph(get_glyph(' '), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
                    }
                }
            con->cx = 0; con->cy = con->header_rows;
        } else { con->ansi_st = 0; }
        return;
    } else if (byte == '\x1b') {
        con->ansi_st = 1;
        return;
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
}

/*
 * con_putc - Outputs one byte to a console instance
 */
void con_putc(console_t* con, char character) {
    bool flags;
    if (!spinlock_try_acquire(&con->lock, &flags)) return;
    extern tty_t* active_tty;
    bool active = (active_tty && active_tty->console == con);
    bool draw = active && !con->defer_render;
    if (draw && con->on) render_cursor(con, false);
    _con_process_byte(con, (uint8_t)character);
    if (draw && con->on) render_cursor(con, true);
    spinlock_release(&con->lock, flags);
}

/*
 * con_write_batch - Double-buffered batch write
 */
void con_write_batch(console_t* con, const char* buf, size_t count) {
    if (!con || !buf || !count) return;
    extern tty_t* active_tty;

    bool active = (active_tty && active_tty->console == con);
    size_t i = 0;
    const size_t chunk_len = 64;

    // If this console is active, we can optimize the batch write by deferring 
    // rendering until the end of the batch
    if (active) {
        bool flags = spinlock_acquire(&con->lock);
        if (con->on) render_cursor(con, false);
        con->defer_render = true;
        spinlock_release(&con->lock, flags);
    }

    // Process the input in chunks to avoid holding the lock for too long at once, which keeps IRQs responsive
    while (i < count) {
        size_t end = i + chunk_len;
        if (end > count) end = count;

        bool flags = spinlock_acquire(&con->lock);
        while (i < end) {
            _con_process_byte(con, (uint8_t)buf[i++]);
        }
        spinlock_release(&con->lock, flags);
    }

    // After the batch is done if this console is active 
    // we need to flush the backbuffer to the framebuffer and enable direct rendering
    if (active) {
        flush_display(con);
        bool flags = spinlock_acquire(&con->lock);
        con->defer_render = false;
        spinlock_release(&con->lock, flags);
    }
}

/*
 * con_refresh - Redraws the entire console to the framebuffer
 */
void con_refresh(console_t* con) {
    if (!fb) return;
    bool flags = spinlock_acquire(&con->lock);
    DIRTY_SET_ALL(con);
    spinlock_release(&con->lock, flags);
    flush_display(con);
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
    for (size_t x = 0; x < con->width; x++) {
        con->buffer[row * con->width + x] = (console_char_t){ ' ', fg, bg };
        DIRTY_SET(con, row * con->width + x);
    }
    for (size_t c = 0; c < len && (pad + c) < con->width; c++) {
        con->buffer[row * con->width + pad + c] = (console_char_t){ (uint8_t)text[c], fg, bg };
        DIRTY_SET(con, row * con->width + pad + c);
    }
    extern tty_t* active_tty;
    if (active_tty && active_tty->console == con && !con->defer_render) {
        size_t py = row * (fh + PADDING_Y);
        for (size_t x = 0; x < con->width; x++) {
            console_char_t ch = con->buffer[row * con->width + x];
            draw_glyph(get_glyph(ch.codepoint), x * fw, py, VGA_PALETTE[ch.fg], VGA_PALETTE[ch.bg]);
            DIRTY_CLR(con, row * con->width + x);
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
