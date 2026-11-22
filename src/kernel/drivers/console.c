/*
 * console.c - Framebuffer console implementation
 *
 * Lowest level of the output stack, TTY and friends sit on top of it.
 *
 * The con_crash_* API works without the rest of the console/TTY, which may be in an
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
static char cbuf[2048];

/*
 * glyph_index - Map a codepoint onto its glyph slot (CP437 or synthesized)
 */
static inline uint16_t glyph_index(uint32_t cp) {
    uint16_t idx = unicode_to_glyph(cp);
    if (idx == 0 && cp != 0) idx = 0x3F;
    return idx;
}

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
 * fill_rect - Paints a w x h pixel rectangle at (px, py) one color, clipped to the screen.
 * At 32bpp pixels go out in pairs, as single 64-bit stores
 */
static void fill_rect(uint32_t px, uint32_t py, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb || px >= fb_w || py >= fb_h) return;
    if (w > fb_w - px) w = fb_w - px;
    if (h > fb_h - py) h = fb_h - py;

    if (fb_bpp != 32) {
        for (uint32_t y = py; y < py + h; y++)
            for (uint32_t x = px; x < px + w; x++)
                put_pixel(x, y, color);
        return;
    }

    uint64_t c2 = (uint64_t)color | ((uint64_t)color << 32);
    for (uint32_t y = py; y < py + h; y++) {
        uint32_t* row = (uint32_t*)(fb + (size_t)y * fb_pitch) + px;
        uint32_t x = 0;
        if (((uintptr_t)row & 7) && w) row[x++] = color;
        for (; x + 1 < w; x += 2) *(uint64_t*)&row[x] = c2;
        if (x < w) row[x] = color;
    }
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

    // Fast path for 32bpp framebuffers
    uint32_t* row_ptr = (uint32_t*)(fb + py * fb_pitch + px * 4);
    size_t pitch_u32 = fb_pitch / 4;
    uint32_t colors[2] = { bg, fg };

    // Each glyph row is 8 pixels wide, so we can write it as 4 pairs of 32-bit pixels (64 bits total)
    for (size_t y = 0; y < fh; y++) {
        uint8_t bits = glyph[y];
        uint64_t* p = (uint64_t*)row_ptr;
        p[0] = (uint64_t)colors[(bits >> 7) & 1] | ((uint64_t)colors[(bits >> 6) & 1] << 32);
        p[1] = (uint64_t)colors[(bits >> 5) & 1] | ((uint64_t)colors[(bits >> 4) & 1] << 32);
        p[2] = (uint64_t)colors[(bits >> 3) & 1] | ((uint64_t)colors[(bits >> 2) & 1] << 32);
        p[3] = (uint64_t)colors[(bits >> 1) & 1] | ((uint64_t)colors[(bits >> 0) & 1] << 32);
        row_ptr += pitch_u32;
    }
}

/*
 * get_glyph - Returns the PSF1 glyph data for the given codepoint
 */
static uint8_t* get_glyph(uint32_t cp) {
    return (uint8_t*)font_glyph(glyph_index(cp));
}

/*
 * render_cursor - Draws (on=true) or erases (on=false) the block cursor
 */
static void render_cursor(console_t* con, bool on) {
    if (!fb) return;
    extern tty_t* volatile active_tty;

    // Checks
    if (!active_tty || active_tty->console != con) return;
    if (con->cx >= con->width || con->cy >= con->height) return;

    // Calculate pixel coordinates and render a solid block for the cursor background color
    size_t px = con->cx * fw;
    size_t py = con->cy * (fh + PADDING_Y);
    
    if (on) {
        fill_rect(px, py, fw, fh, VGA_PALETTE[con->fg]);
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

// Max glyph run length for flush_display
#define GLYPH_RUN_MAX 16

/*
 * draw_glyph_run - Renders count consecutive cells of row y in one pass
 */
static void draw_glyph_run(console_t* con, size_t y, size_t x0, size_t count) {
    size_t py = y * (fh + PADDING_Y);
    size_t px = x0 * fw;

    // Anything but a fully onscreen 32bpp run goes through the per-cell path
    if (fb_bpp != 32 || px + count * fw > fb_w || py + fh > fb_h) {
        for (size_t i = 0; i < count; i++) {
            console_char_t c = con->buffer[y * con->width + x0 + i];
            draw_glyph(get_glyph(c.codepoint), (x0 + i) * fw, py, VGA_PALETTE[c.fg], VGA_PALETTE[c.bg]);
        }
        return;
    }

    // Resolve each cell once, not once per glyph row
    uint8_t* glyphs[GLYPH_RUN_MAX];
    uint32_t fgc[GLYPH_RUN_MAX];
    uint32_t bgc[GLYPH_RUN_MAX];

    // Gather glyphs and colors for the run
    for (size_t i = 0; i < count; i++) {
        console_char_t c = con->buffer[y * con->width + x0 + i];
        glyphs[i] = get_glyph(c.codepoint);
        fgc[i] = VGA_PALETTE[c.fg];
        bgc[i] = VGA_PALETTE[c.bg];
    }

    uint32_t* row_ptr = (uint32_t*)(fb + py * fb_pitch + px * 4);
    size_t pitch_u32 = fb_pitch / 4;

    // Each glyph row is 8 pixels wide, so we can write it as 4 pairs of 32bit pixels (64 bits total)
    for (size_t gy = 0; gy < fh; gy++) {
        uint64_t* p = (uint64_t*)row_ptr;
        for (size_t i = 0; i < count; i++) {
            uint8_t bits = glyphs[i] ? glyphs[i][gy] : 0;
            uint32_t colors[2] = { bgc[i], fgc[i] };
            p[0] = (uint64_t)colors[(bits >> 7) & 1] | ((uint64_t)colors[(bits >> 6) & 1] << 32);
            p[1] = (uint64_t)colors[(bits >> 5) & 1] | ((uint64_t)colors[(bits >> 4) & 1] << 32);
            p[2] = (uint64_t)colors[(bits >> 3) & 1] | ((uint64_t)colors[(bits >> 2) & 1] << 32);
            p[3] = (uint64_t)colors[(bits >> 1) & 1] | ((uint64_t)colors[(bits >> 0) & 1] << 32);
            p += 4;
        }
        row_ptr += pitch_u32;
    }
}

/*
 * any_dirty - Whether some cell still waits to be drawn
 */
static bool any_dirty(console_t* con) {
    if (!con->dirty) return false;
    uint64_t* dw = (uint64_t*)con->dirty;
    size_t words = (con->width * con->height + 63) / 64;
    for (size_t w = 0; w < words; w++) if (dw[w]) return true;
    return false;
}

/*
 * flush_display - Push dirty cells to the framebuffer (assumes no SMP)
 */
static void flush_display(console_t* con) {
    if (!fb) return;
    extern tty_t* volatile active_tty;

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

    // With dirty tracking, scan the bitmap a word at a time and skip
    // clean runs of 64 cells with a single compare
    size_t cells = con->width * con->height;
    uint64_t* dw = (uint64_t*)con->dirty;
    size_t words = (cells + 63) / 64;
    size_t run_y = 0, run_x = 0, run_n = 0;

    // Scan each word of the dirty bitmap
    for (size_t w = 0; w < words; w++) {
        // Take the word and clear it in one go. An echo from an interrupt may set a bit in it at any moment
        uint64_t bits = __atomic_exchange_n(&dw[w], 0, __ATOMIC_RELAXED);
        if (!bits) continue;

        // Scan each bit in the word, which corresponds to a cell in the backbuffer
        while (bits) {
            size_t idx = w * 64 + (size_t)__builtin_ctzll(bits);
            bits &= bits - 1;
            if (idx >= cells) break;
            size_t x = idx % con->width;
            size_t y = idx / con->width;

            // Extend the current run when this cell simply follows it
            if (run_n && y == run_y && x == run_x + run_n && run_n < GLYPH_RUN_MAX) {
                run_n++;
                continue;
            }
            if (run_n) draw_glyph_run(con, run_y, run_x, run_n);
            run_y = y; run_x = x; run_n = 1;
        }
    }
    if (run_n) draw_glyph_run(con, run_y, run_x, run_n);

    // After flushing, draw the cursor on top of any recently changed cells
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

    extern tty_t* volatile active_tty;
    bool active = fb && active_tty && active_tty->console == con;

    // Here we are in immediate mode, so we can scroll the framebuffer directly
    if (active && !con->defer_render) {
        console_char_t blank = (console_char_t){ ' ', con->fg, con->bg };
        for (size_t y = first; y < con->height; y++) {
            for (size_t x = 0; x < con->width; x++) {
                size_t idx = y * con->width + x;
                console_char_t nc = (y + 1 < con->height) ? con->buffer[idx + con->width] : blank;
                console_char_t oc = con->buffer[idx];
                DIRTY_CLR(con, idx);
                if (nc.codepoint == oc.codepoint && nc.fg == oc.fg && nc.bg == oc.bg) continue;
                draw_glyph(get_glyph(nc.codepoint), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[nc.fg], VGA_PALETTE[nc.bg]);
            }
        }
    }

    // Here we are in deferred mode, so the framebuffer is not touched until flush_display.
    // But the backbuffer still has to match the framebuffer.
    if (active && con->defer_render && con->dirty) {
        console_char_t blank = (console_char_t){ ' ', con->fg, con->bg };
        for (size_t y = first; y < con->height; y++) {
            for (size_t x = 0; x < con->width; x++) {
                size_t idx = y * con->width + x;

                // Already stale on screen, so it is redrawn regardless
                if (DIRTY_TST(con, idx)) continue;
                console_char_t nc = (y + 1 < con->height) ? con->buffer[idx + con->width] : blank;
                console_char_t oc = con->buffer[idx];
                if (nc.codepoint != oc.codepoint || nc.fg != oc.fg || nc.bg != oc.bg)
                    DIRTY_SET(con, idx);
            }
        }
    }

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
}

/*
 * put_cell - Writes one cell in the current colors, and draws it right away unless rendering is deferred
 */
static void put_cell(console_t* con, size_t x, size_t y, uint32_t cp, bool active) {
    size_t idx = y * con->width + x;
    console_char_t* cell = &con->buffer[idx];

    // Rewriting a cell with what it already holds is free, so a program that
    // redraws the whole screen from ESC[H only repaints the cells that changed
    if (cell->codepoint == cp && cell->fg == con->fg && cell->bg == con->bg) return;
    *cell = (console_char_t){ cp, con->fg, con->bg };
    DIRTY_SET(con, idx);
    if (active && !con->defer_render)
        draw_glyph(get_glyph(cp), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[con->fg], VGA_PALETTE[con->bg]);
}

/*
 * emit_cp - Renders a Unicode codepoint or control character to the console
 */
static void emit_cp(console_t* con, uint32_t cp) {
    extern tty_t* volatile active_tty;
    bool active = (active_tty && active_tty->console == con);

    if (cp == '\n') { con->cx = 0; con->cy++; }
    else if (cp == '\r') { con->cx = 0; }
    else if (cp == '\b') {
        // Backspace erases, and steps back over a wrap so editing a long typed line works
        if (con->cx > 0) con->cx--;
        else if (con->cy > con->header_rows) { con->cy--; con->cx = con->width - 1; }
        put_cell(con, con->cx, con->cy, ' ', active);
    } else if (cp == '\t') {
        con->cx = (con->cx + 4) & ~3;
    } else if (cp < 0x20 || cp == 0x7F) {
        // Bell, NUL and friends have nothing to draw (CP437's pictures for them live at U+263A and up)
        return;
    } else {
        if (con->cx >= con->width) { con->cx = 0; con->cy++; }
        if (con->cy >= con->height) scroll(con);
        put_cell(con, con->cx, con->cy, cp, active);
        con->cx++;
    }
    if (con->cy >= con->height) scroll(con);
}

/*
 * erase - Blanks cells [from, to) in the current colors
 */
static void erase(console_t* con, size_t from, size_t to, bool active) {
    for (size_t i = from; i < to; i++) put_cell(con, i % con->width, i / con->width, ' ', active);
}

/*
 * sgr - ESC[...m, the color sequences
 */
static void sgr(console_t* con) {
    // ANSI orders its colors black, red, green, yellow, blue, magenta, cyan, white. VGA swaps red and blue
    static const uint8_t vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    if (!con->ansi_n) con->ansi_n = 1; // ESC[m is ESC[0m

    for (size_t i = 0; i < con->ansi_n; i++) {
        uint16_t v = con->ansi_p[i];
        if (v == 0) { con->fg = CONSOLE_COLOR_WHITE; con->bg = CONSOLE_COLOR_BLACK; }
        else if (v == 1) con->fg |= 8; // bold, which a 16 color console can only show as bright
        else if (v == 22) con->fg &= 7;
        else if (v >= 30 && v <= 37) con->fg = vga[v - 30];
        else if (v >= 90 && v <= 97) con->fg = vga[v - 90] | 8;
        else if (v == 39) con->fg = CONSOLE_COLOR_WHITE;
        else if (v >= 40 && v <= 47) con->bg = vga[v - 40];
        else if (v >= 100 && v <= 107) con->bg = vga[v - 100] | 8;
        else if (v == 49) con->bg = CONSOLE_COLOR_BLACK;
        else if (v == 38 || v == 48) {
            // 256 color (5;n) and true color (2;r;g;b) forms, which we can't show. Skip their arguments
            if (i + 1 < con->ansi_n) i += (con->ansi_p[i + 1] == 5) ? 2 : (con->ansi_p[i + 1] == 2) ? 4 : 0;
        }
    }
}

/*
 * csi - Runs one complete ESC[ sequence, fin is its final letter
 */
static void csi(console_t* con, uint8_t fin) {
    extern tty_t* volatile active_tty;
    bool active = (active_tty && active_tty->console == con);
    uint16_t* p = con->ansi_p;
    size_t n = con->ansi_n;
    size_t cnt = (n && p[0]) ? p[0] : 1; // a missing or zero count means 1
    size_t top = con->header_rows;

    // ESC[?25h and ESC[?25l show and hide the cursor, every other private mode is ignored
    if (con->ansi_priv) {
        if (n && p[0] == 25 && (fin == 'h' || fin == 'l')) con->on = (fin == 'h');
        return;
    }

    // The cursor can sit one past the last column, waiting to wrap. Erases count from the last column
    size_t cx = (con->cx < con->width) ? con->cx : con->width - 1;
    size_t row = con->cy * con->width;
    uint16_t mode = n ? p[0] : 0;

    switch (fin) {
        case 'H': case 'f':
            // Rows count from the top of the scrolling area, so the sticky header stays out of reach
            con->cy = top + ((n && p[0]) ? p[0] : 1) - 1;
            con->cx = ((n > 1 && p[1]) ? p[1] : 1) - 1;
            break;
        case 'A': con->cy = (con->cy >= top + cnt) ? con->cy - cnt : top; break;
        case 'B': con->cy += cnt; break;
        case 'C': con->cx = cx + cnt; break;
        case 'D': con->cx = (cx >= cnt) ? cx - cnt : 0; break;
        case 'J':
            if (mode == 0) erase(con, row + cx, con->width * con->height, active);
            else if (mode == 1) erase(con, top * con->width, row + cx + 1, active);
            else erase(con, top * con->width, con->width * con->height, active);
            return;
        case 'K':
            if (mode == 0) erase(con, row + cx, row + con->width, active);
            else if (mode == 1) erase(con, row, row + cx + 1, active);
            else erase(con, row, row + con->width, active);
            return;
        case 'm': sgr(con); return;
        default: return;
    }

    if (con->cy >= con->height) con->cy = con->height - 1;
    if (con->cx >= con->width) con->cx = con->width - 1;
}

#pragma region Instance API

/*
 * con_init - Initializes a console instance (allocates backbuffer, resets state)
 */
bool con_init(console_t* con) {
    con->width = cols; con->height = rows;
    con->cx = 0; con->cy = 0;
    con->fg = CONSOLE_COLOR_WHITE; con->bg = CONSOLE_COLOR_BLACK;
    con->u8n = 0; con->u8cp = 0; con->u8lead = 0;
    con->ansi_st = 0; con->reent = 0;
    con->on = true; con->header_rows = 0;
    con->defer_render = false;
    con->buffer = kmalloc(con->width * con->height * sizeof(console_char_t));
    if (!con->buffer) return false;
    size_t dirty_bytes = ((con->width * con->height + 63) / 64) * 8;
    con->dirty = kmalloc(dirty_bytes);
    spinlock_init(&con->lock, "console_lock");
    con_clear(con, CONSOLE_COLOR_BLACK);
    return true;
}

/*
 * con_destroy - Frees what con_init allocated (the console struct itself is the caller's)
 */
void con_destroy(console_t* con) {
    kfree(con->buffer);
    kfree(con->dirty);
    con->buffer = NULL;
    con->dirty = NULL;
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
    extern tty_t* volatile active_tty;

    // If this console is active and we're not deferring rendering, we can also clear the framebuffer immediately
    if (active_tty && active_tty->console == con && !con->defer_render) {
        uint32_t start_py = con->header_rows * (fh + PADDING_Y);
        fill_rect(0, start_py, fb_w, fb_h - start_py, VGA_PALETTE[con->bg]);
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
        con->ansi_st = 0;
        if (byte == '[') {
            con->ansi_st = 2;
            con->ansi_n = 0;
            con->ansi_priv = false;
            kmemset(con->ansi_p, 0, sizeof(con->ansi_p));
            return;
        }
    } else if (con->ansi_st == 2) {
        if (byte >= '0' && byte <= '9') {
            if (!con->ansi_n) con->ansi_n = 1;
            uint16_t* v = &con->ansi_p[con->ansi_n - 1];
            if (*v < 10000) *v = (uint16_t)(*v * 10 + (byte - '0'));
            return;
        }
        if (byte == ';') {
            if (!con->ansi_n) con->ansi_n = 1; // ESC[;5H has an empty first parameter
            if (con->ansi_n < 8) con->ansi_p[con->ansi_n++] = 0;
            return;
        }
        if (byte == '?') { con->ansi_priv = true; return; }
        if (byte >= 0x40 && byte <= 0x7E) { con->ansi_st = 0; csi(con, byte); return; }
        if (byte >= 0x20 && byte < 0x40) return; // other parameter and intermediate bytes, ignored

        // A control character or a stray byte cuts the sequence short, and is handled as itself
        con->ansi_st = 0;
    }
    if (byte == '\x1b') {
        con->ansi_st = 1;
        return;
    }

// utf8 decoding state machine
utf8_restart:
    if (con->u8n == 0) {
        if ((byte & 0x80u) == 0x00u) { emit_cp(con, byte); return; }
        if ((byte & 0xE0u) == 0xC0u) {
            if (byte < 0xC2u) { emit_cp(con, 0xFFFD); return; } // C0/C1 are always overlong
            con->u8n = 1; con->u8cp = byte & 0x1Fu;
        } else if ((byte & 0xF0u) == 0xE0u) {
            con->u8n = 2; con->u8cp = byte & 0x0Fu;
        } else if ((byte & 0xF8u) == 0xF0u && byte <= 0xF4u) {
            con->u8n = 3; con->u8cp = byte & 0x07u;
        } else {
            emit_cp(con, 0xFFFD); return; // 80..BF stray, F5..FF invalid
        }
        con->u8lead = byte;
        return;
    }

    if ((byte & 0xC0u) != 0x80u) { // sequence cut short
        con->u8n = 0;
        emit_cp(con, 0xFFFD);
        goto utf8_restart;
    }

    con->u8cp = (con->u8cp << 6) | (byte & 0x3Fu);
    if (--con->u8n == 0) {
        uint32_t cp = con->u8cp;
        uint32_t min = (con->u8lead < 0xE0u) ? 0x80u
                     : (con->u8lead < 0xF0u) ? 0x800u : 0x10000u;
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFD;
        emit_cp(con, cp);
    }
}

/*
 * con_putc - Outputs one byte to a console instance
 */
void con_putc(console_t* con, char character) {
    bool flags;
    if (!spinlock_try_acquire(&con->lock, &flags)) return;
    extern tty_t* volatile active_tty;
    bool active = (active_tty && active_tty->console == con);
    bool draw = active && !con->defer_render;
    if (draw && con->on) render_cursor(con, false);

    // Mid batch the cursor block isn't ours to erase, so make the next flush repaint that cell
    if (active && !draw && con->on && con->cx < con->width && con->cy < con->height)
        DIRTY_SET(con, con->cy * con->width + con->cx);
    _con_process_byte(con, (uint8_t)character);
    if (draw && con->on) render_cursor(con, true);
    spinlock_release(&con->lock, flags);
}

/*
 * con_write_batch - Double-buffered batch write
 */
void con_write_batch(console_t* con, const char* buf, size_t count) {
    if (!con || !buf || !count) return;
    extern tty_t* volatile active_tty;

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
        // A key echoed from an interrupt during the flush only marks its cell, so flush until
        // nothing is left. Only then is it safe to go back to drawing directly
        while (true) {
            flush_display(con);
            bool flags = spinlock_acquire(&con->lock);
            bool done = !any_dirty(con) || !active_tty || active_tty->console != con;
            if (done) con->defer_render = false;
            spinlock_release(&con->lock, flags);
            if (done) break;
        }
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
    if (con->on && !enabled) render_cursor(con, false);
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
    extern tty_t* volatile active_tty;
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

size_t con_crash_width(void){ 
    return cols; 
}

/*
 * crash_cell - Draw one codepoint at cell (x, y) in the panic colors (panic path, no locks)
 */
static void crash_cell(uint32_t cp, uint32_t x, uint32_t y) {
    if (x >= (uint32_t)cols || y >= (uint32_t)rows) return;
    draw_glyph(get_glyph(cp), x * fw, y * (fh + PADDING_Y), VGA_PALETTE[cfg], VGA_PALETTE[cbg]);
}

/*
 * crash_emit - Render one codepoint straight to the framebuffer
 * A panic screen fits on one screen, so instead of scrolling, a full screen starts over on a clean one
 */
static void crash_emit(uint32_t cp) {
    if (cp == '\n') { ccx = 0; ccy++; return; }
    if (cp == '\r') { ccx = 0; return; }
    if (cp == '\t') { ccx = (ccx + 4) & ~3u; return; }
    if (cp == '\b') {
        if (ccx > 0) crash_cell(' ', --ccx, ccy);
        return;
    }

    if (ccx >= (uint32_t)cols) { ccx = 0; ccy++; }
    if (ccy >= (uint32_t)rows) con_crash_clear(cbg);
    crash_cell(cp, ccx++, ccy);
}

/*
 * con_crash_clear - Fills the framebuffer with bg color and resets the panic cursor
 */
void con_crash_clear(uint8_t bg) {
    if (!fb) return;
    cbg = bg & 0xF; ccx = 0; ccy = 0;
    fill_rect(0, 0, fb_w, fb_h, VGA_PALETTE[cbg]);
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
    multiboot_framebuffer_t* mbfb = multiboot_framebuffer(parser);
    if (!mbfb) return;
    fb_phys = mbfb->addr;
    fb_w = mbfb->width; fb_h = mbfb->height;
    fb_pitch = mbfb->pitch; fb_bpp = mbfb->bpp;
    fb_sz = fb_h * fb_pitch;
    fb = (uint8_t*)PHYSMAP_P2V(fb_phys);
    fh = font_get_current()->header->charsize;
    cols = fb_w / fw;
    rows = fb_h / (fh + PADDING_Y);
    kmemset(fb, 0, fb_sz);
}

/*
 * console_set_color - Global accessor: sets colors on the active TTY
 */
void console_set_color(uint8_t foreground, uint8_t background) {
    extern tty_t* volatile active_tty;
    if (active_tty && active_tty->console)
        con_set_color(active_tty->console, foreground, background);
}

/*
 * console_enable_cursor - Global accessor: toggles the cursor on the active TTY
 */
void console_enable_cursor(bool enabled) {
    extern tty_t* volatile active_tty;
    if (active_tty && active_tty->console)
        con_enable_cursor(active_tty->console, enabled);
}

/*
 * console_clear - Global accessor: clears the active TTY's display
 */
void console_clear(uint8_t background) {
    extern tty_t* volatile active_tty;
    if (active_tty && active_tty->console)
        con_clear(active_tty->console, background);
}

size_t console_get_width() { return cols; }
size_t console_get_height() { return rows; }
