# Chapter 13: The Console and TTYs

Here is a fun thing to think about. Every screenshot, every log line and every panic in this book so far was drawn by code we never actually looked at. We have been printing to the screen since Chapter 6. All we know about it is `put_pixel` and a ten line `draw_glyph`.

This chapter is where we open it up. It is a lighter one than the last two, I promise. There is no spec to wrestle with and no hardware that lies to us. Most of the code is plain C operating on plain memory. Still, there is a surprising amount of thought behind "put text on a screen", and a few of the bugs I found in it were genuinely sneaky.

There are two layers to it. The **console** draws text on the screen, with a few more tricks than `draw_glyph` lets on. The **TTY** sits on top of it and decides whose text it is, and TTYs are also what Alt+Tab has been switching between all along.

>[!NOTE]
>The code for this chapter lives in [`console.c`](/src/kernel/drivers/console.c), [`font.c`](/src/kernel/drivers/font.c) and [`tty.c`](/src/kernel/drivers/tty.c), along with their headers and [`ldisc.h`](/src/kernel/drivers/ldisc.h).

## Where We Left the Screen

Everything here sits on top of what Chapter 6 built. Here is a quick recap. `console_init` gets the framebuffer from GRUB through Multiboot2 and reaches it through the physmap. `put_pixel` writes one pixel. `draw_glyph` draws a character by walking its bitmap row by row, painting the foreground color wherever a bit is set and the background wherever it is not.

The crash console sits on the same three things (the framebuffer pointer, the font and `draw_glyph`) and nothing else. It stays that way. Everything in this chapter is about the *other* console, the one regular output goes through. None of it is allowed to leak into the panic path.

## The Text Stack

Before we dig into any code, I want to do what we did for memory back in Chapter 7, and build a mental model of the whole thing first. Text handling has the same problem memory management has. People throw around words like terminal, console, TTY, line discipline and framebuffer, often as if they meant the same thing, and it is never quite clear which piece does what, or why there are so many of them.

So here is the full picture for GatOS. Every keypress and every character of output passes through a stack of components, each with one job:

1. **The input hub** (from Chapter 11), which takes key events from every keyboard
2. **The TTY**, which belongs to one program and connects its input to its output
3. **The line discipline**, which lives inside a TTY and decides what happens to each typed key
4. **The console**, which turns a stream of bytes into a grid of characters
5. **The font and the framebuffer**, which turn characters into pixels on the screen

Input goes down this list from the keyboard to a program, and output goes from a program down to the screen. Let's take them one at a time, and then follow a single keypress all the way through.

### What does a TTY do?

A TTY is a terminal, in the old sense of the word: a keyboard and a screen that belong to one conversation with the computer. The name comes from the teletypewriter, an electric typewriter wired to a computer, with a keyboard going in and paper coming out. Unix modeled its terminals on them, and the name stuck around long after the paper went away.

In GatOS, a TTY ties together three things. It has an **input buffer**, where typed characters wait until a program reads them. It has a **console**, its own private screen, where everything written to it ends up. And it has a **line discipline**, the rules for how typing turns into input.

The important thing to understand is that there are many TTYs, and every program has its own. The kernel has one, created at boot. Every process gets a new one when it starts. When a program prints, the text goes to *its* TTY, and when it reads, it reads from *its* TTY, no matter what is on screen at the time.

>[!NOTE]
>Keep in mind that there are **many** TTYs, but only **one** of them is on screen at any moment. That one is called the **active** TTY, and Alt+Tab is how you pick it.

### What does the console do?

A console is a screen's worth of character cells, 240 × 60 on a 1920x1080 display, plus a cursor. It takes a stream of bytes and works out what they mean. Most bytes are text. It decodes them (UTF-8) and writes them at the cursor. Some are commands, like "newline", "move the cursor to row 5" or "switch to red", and those it carries out.

Every TTY has its own console, and every console keeps its own copy of what its screen looks like, in RAM. Only the active TTY's console is allowed to draw on the real screen. All the others keep updating their copy in the dark, and the moment you switch to one, its copy gets drawn. That is why switching to a program that printed something while you were away shows you its output, as if it had been there all along.

### What does the line discipline do?

The line discipline sits between the keyboard and a TTY's input buffer, and decides what each key turns into. Most of the time, it gathers a whole line before handing anything over, so that you can fix typos with backspace before the program sees them. It also **echoes** what you type, by writing it to the TTY's console. Without echo, typing would work, but you would be typing blind.

Programs that want every key the moment it is pressed (a game, for example) can switch their TTY's line discipline to **raw** mode, where keys go straight through, one at a time.

### What do the font and the framebuffer do?

These are the bottom of the stack, and Chapter 6 already built most of them. The framebuffer is the screen itself, as an array of pixels in memory. The font holds a small bitmap for every character. Drawing a character means copying its bitmap into the framebuffer, in two colors.

### Following a Keypress

With all the pieces named, let's follow the letter `a` from the keyboard to a program, and back to the screen. Say you are in a program that asked "What's your name?" and is waiting for an answer:

```
        ┌──────────────────────────┐
        │  Keyboard (PS/2 or USB)  │
        └────────────┬─────────────┘
                     │  key event: "A pressed"
                     ▼
        ┌──────────────────────────┐  hotkey?  ┌─────────────────────┐
        │        Input hub         ├──────────►│ handled right here  │
        └────────────┬─────────────┘           │ (Alt+Tab, Alt+F4)   │
                     │ everything else         └─────────────────────┘
                     ▼
  ┌──────────── Active TTY (the program on screen) ────────────┐
  │                                                            │
  │   ┌─────────────────┐   echo 'a'   ┌──────────────────┐    │
  │   │ Line discipline ├─────────────►│     Console      ├────┼───► Screen
  │   └────────┬────────┘              └──────────────────┘    │
  │            │ on Enter: "alice\n"                           │
  │            ▼                                               │
  │   ┌─────────────────┐                                      │
  │   │  Input buffer   │                                      │
  │   └────────┬────────┘                                      │
  └────────────┼───────────────────────────────────────────────┘
               │
               ▼
     the program's read() returns "alice\n"
```

1. The keyboard driver turns the hardware's signal into a key event and gives it to the input hub. This part is Chapter 11 (or Chapter 12, for USB keyboards).
2. The input hub checks for hotkeys first. Alt+Tab, for example, never reaches any program. Everything else goes to exactly one place, the **active** TTY.
3. The active TTY's line discipline adds `a` to the line it is building, and echoes it to the TTY's console. The console draws it on screen.
4. Nothing reaches the program yet. Only when you press Enter does the line discipline move the whole line into the TTY's input buffer.
5. The program, which has been asleep inside `read()` the whole time, wakes up, takes the line from the input buffer and carries on.

The key detail is in step 2. Input only ever goes to the active TTY, so a program only gets keys while you are looking at it. A program in the background that tries to read just sleeps until you switch to it and type.

Output is simpler, and goes the other way. When a program prints, the text goes to its own TTY's console, and the console updates its copy of the screen. If that TTY is the active one, the change shows up right away. If not, it waits for you to switch over.

>[!IMPORTANT]
>Why bother with all these layers? Take any one away and something breaks. Without TTYs, every program would print over every other program on a single screen, and every key would go to whoever happened to be reading. Without the line discipline, every program would have to implement backspace and echo itself. Without the console's private copy of the screen, switching programs would leave you looking at garbage. Each layer exists because the one below it cannot do its job.

In the rest of this chapter, we build this stack from the bottom up, starting with fonts and ending with TTYs and their line discipline.

## Fonts

Chapter 6 waved PSF away as "glyphs copied and pasted from google". That is still more or less what it is, but now that we are drawing every character on screen with it, it deserves a closer look.

**PSF** (PC Screen Font) is the font format of the Linux text console. GatOS uses version 1, about as simple as a font format gets. It starts with a 4-byte header:

```c
typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
} __attribute__((packed)) psf1_header_t;
```

The magic bytes are `0x36 0x04`. Bit 0 of `mode` says whether the font holds 256 or 512 glyphs. The other bits say whether a Unicode mapping table follows the glyphs. `charsize` is the number of bytes per glyph. Every PSF1 glyph is exactly 8 pixels wide. One byte holds one row of pixels, which makes `charsize` the glyph's height as well.

Right after the header come the glyphs, back to back. Glyph `n` starts at byte `4 + n * charsize`. That is the entire format. GatOS embeds a standard 8x16 VGA font ([`vgafont.psf`](/src/kernel/drivers/vgafont.psf)) straight into the kernel image through `vgafont.S`. All `font_init` has to do is point at it:

```c
void font_init(void) {
    cur_font.header = (const psf1_header_t*)vga_font;
    cur_font.glyph_buffer = vga_font + sizeof(psf1_header_t);
    build_glyphs(cur_font.header->charsize);
}
```

On a 1920x1080 screen, 8 pixel wide glyphs give us 240 columns. Each row of text is 16 pixels of glyph plus 2 pixels of padding (`PADDING_Y` keeps the lines from touching), and 1080 pixels give us 60 rows.

### Drawing Faster

The old `draw_glyph` called `put_pixel` once per pixel. For a full screen of text, that is 240 × 60 × 128 = 1,843,200 calls, each one recomputing the address and checking the color depth. The real `draw_glyph` keeps that loop for odd framebuffers, but adds a fast path for the 32 bits per pixel case that every modern machine uses:

```c
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
```

There is a lot packed into those few lines. Let's take them apart, starting with what the framebuffer looks like in memory. It is one long array of pixels, stored row after row. At 32 bits per pixel, every pixel is a `uint32_t` holding its color as `0x00RRGGBB`. A row of 1920 pixels is 7,680 bytes long. The **pitch** is the distance in bytes from the start of one row to the start of the next. It can be a bit more than the row itself, since the hardware is allowed to pad rows:

```
fb ─► [px 0][px 1][px 2] ... [px 1919][padding]   row 0
      [px 0][px 1][px 2] ... [px 1919][padding]   row 1, starts at fb + pitch
      [px 0][px 1][px 2] ... [px 1919][padding]   row 2, starts at fb + 2 * pitch
```

A glyph lands in a small 8×16 window of this array. The fast path computes the address of the window's top left pixel once, and every step down to the next row is just "add the pitch". The old loop recomputed the full address for every single pixel.

Now for one row of a glyph. Each glyph byte is one row of pixels, one bit per pixel, with bit 7 on the left. Take `0x18`, which is `00011000` in binary. That is one row of the vertical bar `│`, two lit pixels in the middle:

```
bits:     0  0  0  1  1  0  0  0
pixels:  bg bg bg fg fg bg bg bg
```

`colors[0]` holds the background and `colors[1]` the foreground, so `colors[(bits >> 7) & 1]` is the color of the leftmost pixel, and `colors[(bits >> 6) & 1]` the one next to it. There is no `if` anywhere. The bit itself picks the color.

The last trick is how the pixels are written. Two neighboring pixels take 8 bytes, the size of one `uint64_t`. We glue two colors together and store both at once. The second pixel goes in the upper half because x86 is little endian. The upper half of a value lands at the higher address, and that is where the right pixel lives. One glyph row becomes four stores instead of eight, and a whole glyph 64 instead of 128.

Why bother halving the stores? Remember that the framebuffer is write-combining (Chapter 5). Writes to it skip the cache, and the CPU collects them in a small buffer that gets sent out in one burst when it fills up. Wider stores fill that buffer in fewer instructions, and the old loop paid for a function call, a bounds check and a color depth check on top of every pixel.

The fast path is only taken when the whole glyph fits on screen. A glyph that would hang off the right or bottom edge goes through the slow, clipped loop instead.

## Missing Glyphs

The VGA font has 256 glyphs, laid out in the order of **code page 437**, the character set of the original IBM PC. It has the ASCII range, a handful of accented letters, some Greek, a set of box drawing lines and those famous smiley faces at the bottom.

Text inside GatOS is Unicode, though (more on that soon). Every character has to be mapped from its Unicode **codepoint** to one of those 256 glyphs. For ASCII, the two agree. For everything else, `unicode_to_cp437` is one big `switch`:

```c
uint8_t unicode_to_cp437(uint32_t codepoint) {
    if (codepoint < 128) return (uint8_t)codepoint;
    switch (codepoint) {
        case 0x263A: return 1; case 0x263B: return 2; case 0x2665: return 3; case 0x2666: return 4;
        // ...
        case 0x00C7: return 128; case 0x00FC: return 129; case 0x00E9: return 130; case 0x00E2: return 131;
        // ...
        default: return 0;
    }
}
```

This turns `é` (U+00E9) into glyph 130 and `☺` (U+263A) into glyph 1. Anything without a glyph returns 0, and the console draws a `?` in its place.

That left a gap I kept running into. Unicode has a lot of characters for drawing in a terminal (block elements like `▄` and `▟`, box drawing lines like `┍` and `╭`). CP437 only has a few dozen of them. Programs that draw progress bars, graphs or rounded boxes look broken without the rest.

Instead of shipping a bigger font, GatOS generates the missing ones in code when it boots. `build_glyphs` creates three extra ranges, stored after the 256 font glyphs:

| Range | Characters | Count |
|---|---|---|
| U+2580 to U+259F | Block elements (`▀ ▄ █ ▌ ▐ ░ ▒ ▓ ▖ ▟` ...) | 32 |
| U+1FB00 to U+1FB3B | Sextants (2×3 blocks) | 60 |
| U+2500 to U+257F | Box drawing (`─ │ ┍ ╋ ╭ ╱` ...) | 128 |

`unicode_to_glyph` checks CP437 first. A character the font already has always wins. A generated glyph is only used when CP437 has nothing.

### Blocks from a Grid

Most block elements are some combination of filled sub-cells. `▀` is the top half, `▖` is the bottom left quarter, and `▟` is three quarters of the cell. We can build the whole family with one helper that splits the 8×16 cell into a grid and fills whichever sub-cells a bitmask asks for:

```c
static void grid(uint8_t* g, int h, int gw, int gh, uint32_t mask) {
    for (int gy = 0; gy < gh; gy++) {
        int y0 = gy * h / gh, y1 = (gy + 1) * h / gh;
        for (int gx = 0; gx < gw; gx++) {
            if (!((mask >> (gy * gw + gx)) & 1u)) continue;
            int x0 = gx * 8 / gw, x1 = (gx + 1) * 8 / gw;
            uint8_t bits = 0;
            for (int x = x0; x < x1; x++) bits |= (uint8_t)(0x80u >> x);
            for (int y = y0; y < y1; y++) g[y] |= bits;
        }
    }
}
```

Bit `gy * gw + gx` of the mask stands for the sub-cell in column `gx` and row `gy`. For a 2×2 grid on our 8×16 cell, that numbering looks like this:

```
          gx = 0       gx = 1
        (cols 0-3)   (cols 4-7)
       ┌──────────┬──────────┐
gy = 0 │  bit 0   │  bit 1   │  rows 0-7
       ├──────────┼──────────┤
gy = 1 │  bit 2   │  bit 3   │  rows 8-15
       └──────────┴──────────┘
```

Let's run `▖` (the bottom left quarter) through it. Its mask is `0b0100`, only bit 2. For `gy = 1, gx = 0`, the helper works out that the sub-cell covers rows 8 to 15 and columns 0 to 3. Columns 0 to 3 are the leftmost four bits of a row byte, `0xF0`, so rows 8 to 15 of the glyph become `0xF0`, and rows 0 to 7 stay `0x00`. That is the whole glyph. The row boundaries are computed from the font height (`gy * h / gh`) rather than hardcoded. The same code works for a 14 pixel font.

With that, each block element is a grid shape and a mask. The eighths (`▁` to `█`) use a 1×8 grid with the bottom `n` rows set. The vertical eighths (`▏` to `▉`) use an 8×1 grid with the left `n` columns set. The ten quadrant characters at the end of the range use a 2×2 grid, with their masks packed into a single 64-bit constant, one nibble each:

```c
#define QUAD_ORDER 0x00E62B79D184ULL
// ...
grid(g, h, 2, 2, (uint32_t)((QUAD_ORDER >> (4u * (cp - 0x2596u))) & 0xFu));
```

Let's decode `▟` (U+259F, the bottom right three quarters). It is the tenth quadrant. We shift by `4 × 9 = 36` bits and take the nibble, which is `0xE`, or `1110` in binary. Bits 1, 2 and 3 are set, which gives the top right, bottom left and bottom right quarters. That is `▟`.

Sextants work the same way on a 2×3 grid. Unicode numbers them by treating the six cells as a 6-bit number and skipping the four patterns that already exist elsewhere (empty, left half, right half and full). The code only has to count past those to get the mask.

### Lines from Arms

Box drawing characters are a different shape of problem. Every one of them is some set of lines leaving the center of the cell, going up, down, left or right. Each of those lines is either missing, light, heavy or double. `┍` is a light line going down and a heavy one going right. `╋` is heavy in all four directions.

`box_arms` decodes a codepoint into four "arm weights" (plus flags for dashed lines, rounded corners and diagonals), mostly by doing arithmetic on its offset within the block, since Unicode lays these out in regular groups. `synth_box` then draws each arm from the center to the edge, with a thickness that depends on its weight.

Here is what comes out for `┍`, printed one glyph row per line (`#` is a lit pixel). The light arm going down is 2 pixels wide, the same as the VGA font's own `│`. The heavy arm going right is 2 rows thick, where a light one would be a single row:

```
row  0-6  ........
row  7    ...#####   ← heavy arm, right
row  8    ...#####
row  9-15 ...##...   ← light arm, down
```

Both arms start at the same center column. That is what makes neighboring box characters join into one continuous line on screen.

Once again, the code is mostly bookkeeping, and the idea is the important part. A few hundred lines of C replace 220 hand-drawn glyphs that would otherwise have to be redrawn for every font height.

>[!NOTE]
>These glyphs are easy to get subtly wrong, with an arm one pixel off center or a corner that does not meet its line. While writing this chapter, I compiled `font.c` on my machine and printed every generated glyph as ASCII art to check them one by one. They all came out right, which was a nice change of pace.

## The Console

With glyphs sorted out, we can look at the console itself. A **console** is a grid of character cells, plus a cursor and the colors to write with:

```c
typedef struct {
    size_t cx;
    size_t cy;
    uint8_t fg;
    uint8_t bg;

    // Cursor State (Static Block)
    bool on;

    // UTF8 State
    uint32_t u8cp;
    int u8n;
    uint8_t u8lead;

    // ANSI State (0 = text, 1 = after ESC, 2 = inside ESC[ ... )
    uint8_t ansi_st;
    uint16_t ansi_p[8];
    uint8_t ansi_n;
    bool ansi_priv; // ESC[? sequences

    // Backbuffer
    console_char_t* buffer;
    size_t width;
    size_t height;

    // Sticky header, rows [0, header_rows) are reserved and never scrolled
    size_t header_rows;

    spinlock_t lock;
    int reent;

    // Dirty tracking
    uint8_t* dirty;

    // When true, we skip draw_glyph
    bool defer_render;
} console_t;
```

We will meet most of these fields on the way. `cx` and `cy` are the cursor, in cells. `fg` and `bg` are the current colors, as indexes into the classic 16-color VGA palette. `on` says whether the cursor is visible. The UTF-8 and ANSI fields hold the state of two small parsers that live inside the console. We will get to both.

The most important field is `buffer`, the **backbuffer**. It is an array of `width × height` cells, one per character on screen:

```c
typedef struct {
    uint32_t codepoint;
    uint8_t fg;
    uint8_t bg;
} console_char_t;
```

The backbuffer is the console's own record of what is on screen, as characters rather than pixels. To see why we need one, think about scrolling without it. Every pixel on screen would have to move up by one text row, 18 pixels. That means reading back the whole screen from the framebuffer (1920 × 1080 pixels, a little over 8 MB) and writing it again one row higher, for every single line of output.

Writing that much is bad enough. Reading it is far worse, since a write-combining framebuffer is not cached, and every read has to go all the way out to the graphics hardware and wait for the answer. This is the same reason the crash console from Chapter 6 refuses to scroll at all.

The backbuffer turns that into a much smaller problem. To scroll, we move characters around in RAM (about 115 KB of them), and then only redraw the glyphs that actually changed. The console never reads the framebuffer at all. It only ever writes to it, keeping the truth in RAM.

The backbuffer also makes having many consoles possible. Every TTY has its own console, but only one of them is on screen at any moment. The rest keep writing into their own backbuffers. When the user switches, we redraw the screen from the new one.

>[!NOTE]
>A console's backbuffer is 115,200 bytes at 1920x1080 (240 × 60 cells of 8 bytes each). That is why consoles only exist once the heap does, while the crash console has no backbuffer at all.

### Writing a Character

Every character that reaches the console eventually goes through `put_cell`, which writes one cell:

```c
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
```

The early return is one of the most effective lines in the console. The donut redraws its entire frame many times a second by moving the cursor home and printing every character again. Most of those characters are the same as last frame. With that check, they cost a comparison instead of a glyph draw.

If the cell did change, we update it and mark it dirty (we will get to what that means in a moment). Then, if this console is the one on screen and we are not in the middle of a batch, we draw the glyph right away.

`emit_cp` sits on top of `put_cell`. It handles the cursor and the few control characters a console understands:

```c
static void emit_cp(console_t* con, uint32_t cp) {
    // ...
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
```

**What's happening here:**
* A newline moves to the start of the next row (GatOS treats `\n` as a full new line, the way most terminals are configured to). A carriage return only moves to the start of the current row.
* Backspace moves back one cell and erases it. We will come back to why it erases (and to the second branch) in the line discipline section.
* A tab moves to the next multiple of 4.
* Any other control character is ignored. It used to be drawn. Since CP437 has a picture for every byte, ringing the bell printed a `•`. The pictures are still reachable through their real codepoints (`☺` is U+263A), just not through raw control bytes.
* Everything else is a printable character. If the cursor is past the last column, it wraps to the next row first. That is a little unusual, and it matters for the backspace fix later.

Notice that wrapping happens *before* printing, not after. After printing in the last column, `cx` equals `width`, one past the edge. The cursor stays there until the next character shows up. If that character is a newline, there was no reason to wrap at all, and printing exactly 240 characters followed by `\n` takes one row instead of two.

### Scrolling

When the cursor falls off the bottom row, everything has to move up by one. Thanks to the backbuffer, that is a `kmemmove` of all rows but the first, plus clearing the last row:

```c
if (rows > 1){
    kmemmove(con->buffer + first * con->width, con->buffer + (first + 1) * con->width, (rows - 1) * con->width * sizeof(console_char_t));
}
```

`first` is `header_rows`, since the sticky header at the top of the screen never scrolls (more on that at the end of the chapter).

Updating the screen is the expensive part. The naive way is to redraw every cell, which for 240 × 60 cells means 14,400 glyphs per line of output. `scroll` does something smarter.

Before moving the backbuffer, it compares each cell with the cell right below it (the one about to replace it) and only redraws the cells where the two differ. A screen of log lines has a lot of identical spaces on the right hand side. None of those get touched.

## Drawing Less

Drawing every character the moment it arrives is fine for a keypress. It is terrible for a program printing 10 KiB of text, where most of the characters are going to scroll off the screen before anyone could read them anyway. The console handles that case with two ideas that work together, a dirty bitmap and deferred rendering.

### The Dirty Bitmap

`dirty` has one bit per cell. `DIRTY_SET` sets a cell's bit whenever its contents change. A set bit means "the screen no longer shows what the backbuffer says here", and a clear bit means "the screen is up to date". At 240 × 60 = 14,400 cells, the whole bitmap is only 1,800 bytes.

Finding a cell's bit is the same arithmetic as finding the cell itself. The cell at column 5 of row 2 is cell number `2 × 240 + 5 = 485`. Reading the bitmap as 64-bit words, that cell is bit `485 % 64 = 37` of word `485 / 64 = 7`.

`flush_display` is what brings the screen back in line. It walks the bitmap, draws every dirty cell and clears its bit. Doing that one bit at a time would be slow. Instead, it reads the bitmap 64 bits at a time and skips 64 clean cells with a single compare:

```c
for (size_t w = 0; w < words; w++) {
    // Take the word and clear it in one go. An echo from an interrupt may set a bit in it at any moment
    uint64_t bits = __atomic_exchange_n(&dw[w], 0, __ATOMIC_RELAXED);
    if (!bits) continue;

    // Scan each bit in the word, which corresponds to a cell in the backbuffer
    while (bits) {
        size_t idx = w * 64 + (size_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        // ...
    }
}
```

`__builtin_ctzll` counts the trailing zeros of a word. That is the same thing as the index of its lowest set bit, and it compiles down to a single `tzcnt` or `bsf` instruction. `bits &= bits - 1` then clears that lowest bit. Subtracting 1 flips the lowest set bit to 0 and every zero below it to 1, so the AND keeps everything above it and wipes the rest.

Let's trace a word where cells 2, 3 and 5 are dirty, `bits = 0b101100`:

1. `ctz(0b101100) = 2`, so cell 2 gets drawn. `0b101100 & 0b101011 = 0b101000`.
2. `ctz(0b101000) = 3`, so cell 3 gets drawn. `0b101000 & 0b100111 = 0b100000`.
3. `ctz(0b100000) = 5`, so cell 5 gets drawn. `0b100000 & 0b011111 = 0`, and the loop ends.

Three cells, three iterations. The 61 clean cells in the same word cost nothing, and a word with no dirty cells at all is skipped by the `if (!bits) continue` before the loop even starts. (Ignore the `__atomic_exchange_n` for now, it is the subject of a bug story in a moment.)

On top of that, dirty cells that sit next to each other on the same row get grouped into **runs** of up to 16 cells. `draw_glyph_run` draws a whole run in one pass. It looks up all the glyphs and colors first, then fills in pixel row 0 of every glyph in the run, then pixel row 1, and so on. That way, the framebuffer gets written in long, continuous stretches, row by row. Write-combining likes that order best.

### Deferred Rendering

The second idea is to stop drawing altogether while a big write is in progress. `con_write_batch` is how the TTY layer writes a buffer of text:

```c
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
    // ...
    bool flags = spinlock_acquire(&con->lock);
    while (i < end) {
        _con_process_byte(con, (uint8_t)buf[i++]);
    }
    spinlock_release(&con->lock, flags);
}
```

With `defer_render` set, `put_cell` and `scroll` only update the backbuffer and the dirty bitmap. They never touch the framebuffer. Once the whole batch is in, one `flush_display` draws the result.

Some numbers make the difference obvious. Say a program prints 200 lines of 50 characters on a full screen. Drawing as we go, that is 10,000 glyphs for the characters themselves, plus 200 scrolls, each of which redraws every cell that changed, easily a few thousand glyphs per scroll on a screen full of text. Deferred, all of that happens in RAM, and the flush at the end draws at most one screen's worth of cells, 14,400 glyphs, and usually far fewer, since a lot of the screen is blank. Only the final state of each cell is ever drawn.

The text goes in 64 bytes at a time, and we let go of the lock between chunks. The console's lock is a spinlock, so interrupts are off for as long as it is held. A keyboard interrupt that had to wait for a whole 10 KiB write would be a noticeable lag.

There is one subtle thing in the scrolling code for this mode. When scrolling while deferred, a cell that changes has to be marked dirty, but a cell that is *already* dirty must be left alone. Its old contents were never drawn. The screen still shows whatever was there before, and comparing against the backbuffer would compare against the wrong thing.

### The Lost Echo

Between the chunks of a batch, interrupts are on again. The keyboard handler can run. Typing echoes characters to the console through `con_putc`. During a batch, `defer_render` is set. `con_putc` only writes the backbuffer and marks the cell dirty, trusting the batch's final flush to draw it.

That trust was misplaced. There were two ways for a typed character to go missing. The first one was in how `flush_display` used to take a word of the bitmap:

```c
uint64_t bits = dw[w];
if (!bits) continue;
dw[w] = 0;
```

The flush runs without holding the lock, since a full screen redraw can take a while. Here is how a keypress could get lost, step by step:

1. The flush reads word 7 of the bitmap. Say cells 448 and 450 are dirty.
2. A key interrupt arrives. The echo writes the typed character into cell 455, and sets bit 7 of the same word.
3. The interrupt returns, and the flush continues with `dw[w] = 0`, clearing bit 7 along with the others.
4. The flush draws cells 448 and 450, the only ones it read. Cell 455 is never drawn, and its dirty bit is gone.

The second way was timing. If the key echoed *after* the flush had already gone past its word, the bit was set correctly, but nothing was left to draw it, since the batch was finished. Either way, the character sat in the backbuffer, invisible, until something else happened to redraw that cell.

We fix the first problem by taking the word and clearing it with a single instruction, `__atomic_exchange_n` (an `xchg`). An interrupt either lands before the swap (and its bit is part of this flush) or after it (and its bit stays set for the next one). The second is fixed in `con_write_batch`. It now keeps flushing until the bitmap is clean, and only then turns direct drawing back on:

```c
while (true) {
    flush_display(con);
    bool flags = spinlock_acquire(&con->lock);
    bool done = !any_dirty(con) || !active_tty || active_tty->console != con;
    if (done) con->defer_render = false;
    spinlock_release(&con->lock, flags);
    if (done) break;
}
```

The check and the switch back to direct drawing happen under the lock, which means a keypress cannot sneak in between them. Any echo after this point draws itself directly.

>[!NOTE]
>The same race could leave a ghost cursor. If an echo moved the cursor during a batch, the block drawn at its old position was never erased. `con_putc` now marks the cell under the cursor dirty when it cannot erase the cursor itself. The next flush then paints over it.

## UTF-8

So far, we have been talking about codepoints, but what arrives at the console is a stream of bytes. One byte can only hold 256 different values, while Unicode has room for over a million characters. Some kind of encoding has to map one onto the other.

The simplest option would be 4 bytes per character, enough for any codepoint. That makes every plain English text four times bigger, though, and breaks every program that expects ASCII to look like ASCII. GatOS, like pretty much everything else these days, uses **UTF-8** instead. It keeps ASCII exactly as it is and only spends more bytes on the characters that need them.

UTF-8 encodes every Unicode codepoint in one to four bytes. ASCII stays exactly as it is, a single byte below `0x80`. Anything bigger is split into a lead byte (whose top bits say how many bytes follow) and continuation bytes, each carrying 6 bits of the codepoint:

| Codepoints | Bytes | Layout |
|---|---|---|
| U+0000 to U+007F | 1 | `0xxxxxxx` |
| U+0080 to U+07FF | 2 | `110xxxxx 10xxxxxx` |
| U+0800 to U+FFFF | 3 | `1110xxxx 10xxxxxx 10xxxxxx` |
| U+10000 to U+10FFFF | 4 | `11110xxx 10xxxxxx 10xxxxxx 10xxxxxx` |

Let's decode `é`, which arrives as the two bytes `C3 A9`:

```
   0xC3 = 110 00011          0xA9 = 10 101001
          └┬┘ └─┬─┘                 └┘ └──┬─┘
     "2 bytes"  5 bits      "continue"   6 bits

   codepoint = 00011 101001 = 0xE9 = U+00E9 = é
```

The `110` at the top of the first byte says "this character takes 2 bytes", leaving 5 bits for data. The `10` at the top of the second byte says "I am a continuation", leaving 6 bits. Glued together, the 11 data bits are `00011101001`, or `0xE9`, which is `é`. Then `unicode_to_cp437` turns it into glyph 130.

A nice property falls out of this layout. A continuation byte always starts with `10`, and a lead byte never does. A decoder that starts reading in the middle of a character can tell immediately, and skip ahead to the next lead byte.

The bytes of one character can be split between two writes (a program could print `C3` in one call and `A9` in the next), so the decoder cannot just look at a buffer. It is a small state machine inside the console.

The decoder remembers the codepoint built so far (`u8cp`), how many continuation bytes are still expected (`u8n`) and the lead byte (`u8lead`). A lead byte starts a sequence and every continuation adds 6 bits. When the count hits zero, the codepoint is done.

The interesting part of the decoder is what it refuses:

```c
uint32_t min = (con->u8lead < 0xE0u) ? 0x80u
             : (con->u8lead < 0xF0u) ? 0x800u : 0x10000u;
if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFD;
```

**What's happening here:**
* A codepoint that could have been encoded in fewer bytes is called **overlong**. `/` can be written as `2F`, but `C0 AF` decodes to the same value. Accepting both sounds harmless, but it has been a real security hole in the past, for example in web servers that checked a path for `../` before decoding it. The rule is that every codepoint has exactly one valid encoding. The decoder checks that the result really needed as many bytes as it used.
* Nothing past U+10FFFF exists in Unicode.
* U+D800 to U+DFFF are **surrogates**. They are reserved for UTF-16 and never valid on their own.

Anything rejected becomes U+FFFD, the official "replacement character". CP437 does not have it, and the console draws a `?`. A sequence cut short by a byte that is not a continuation emits U+FFFD, then starts over with that byte. One broken character cannot swallow the one after it.

## Escape Sequences

Programs do not just print text. They also want to move the cursor, clear the screen and use colors. In the terminal world, all of that is done by printing special byte sequences, called **ANSI escape sequences** (they come from the ECMA-48 standard, and the VT100 terminal made them popular).

Most of them have the same shape. They start with the ESC character (`0x1B`) and `[`, together called the **Control Sequence Introducer** (CSI). Then come zero or more numeric parameters separated by `;`. One final letter says what to do:

```
ESC [ 3 1 ; 4 4 m
└┬┘ └┬┘ └─┬─┘   └ final byte: m, "set colors"
 └────┴─ CSI     parameters: 31 (red text) and 44 (blue background)
```

A few more examples. `ESC[H` moves the cursor to the top left corner, while `ESC[5;10H` moves it to row 5, column 10. `ESC[2J` clears the screen. `ESC[K` clears from the cursor to the end of the line.

### The Old Parser

Until this chapter, the console understood exactly two sequences, `ESC[H` and `ESC[2J`. Its parser had a state for "after ESC", one for "after ESC[" and one for "after ESC[2". When it got anything it did not expect, it gave up and went back to printing text, right in the middle of the sequence.

This was fine for GatOS itself, since the donut only ever uses `ESC[H`. Any other program would print garbage, though. `ESC[31m` (red text) printed `1m`, since the parser gave up at the `3`. `ESC[?25l` (hide the cursor) printed `25l`. Gata programs set their colors through system calls when they run on GatOS and never ran into it. Anything else would have.

### A Real Parser

The new parser is still a state machine, but it understands the general shape of a sequence rather than a fixed list of them. Let's feed it `ESC[31;44m` one byte at a time:

| Byte | State | What happens | Parameters |
|---|---|---|---|
| `ESC` | text | Start of an escape, go to state 1 | |
| `[` | 1 | It is a CSI, go to state 2 and clear the parameters | |
| `3` | 2 | A digit, start the first parameter | 3 |
| `1` | 2 | A digit, `3 × 10 + 1` | 31 |
| `;` | 2 | Start the next parameter | 31, 0 |
| `4` | 2 | A digit | 31, 4 |
| `4` | 2 | A digit, `4 × 10 + 4` | 31, 44 |
| `m` | 2 | A final byte. Run `csi('m')`, back to text | |

The old parser, given the same bytes, went to its "after ESC[" state on the `[`, then saw `3`, which was neither `H` nor `2`. It gave up and went back to text mode, having swallowed the `3`, and printed `1;44m` on the screen.

Here is state 2 in code:

```c
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
```

The standard says the final byte is anything from `0x40` to `0x7E`, covering every letter plus a few symbols. Knowing that is enough to find the end of any sequence, even one we do not support. An unknown sequence now disappears completely instead of leaking its tail onto the screen.

`csi` then does the actual work. It understands cursor positioning (`H`), moving the cursor (`A` to `D`), erasing part of the screen (`J`) or the line (`K`), colors (`m`), and showing or hiding the cursor (`ESC[?25h` and `ESC[?25l`). Everything is clamped to the screen. Row numbers count from below the sticky header, which keeps programs from drawing over it by accident.

Colors deserve a quick note, since they have a small trap of their own:

```c
// ANSI orders its colors black, red, green, yellow, blue, magenta, cyan, white. VGA swaps red and blue
static const uint8_t vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
```

ANSI's color 1 is red, while VGA's color 1 is blue. So `ESC[31m` (ANSI red) has to become VGA color 4. Codes 90 to 97 are the bright versions. VGA gets those by setting bit 3. A few programs use the 256-color (`38;5;n`) or true color (`38;2;r;g;b`) forms. A 16-color console cannot show those, so the parser skips their arguments. Otherwise, the `31` in `38;5;31` would be taken as "red".

>[!NOTE]
>There is one more small change. An ESC followed by anything other than `[` used to print a `←` (the CP437 picture for ESC) followed by the byte. Now the ESC is dropped and the byte is printed normally. A lone Esc echoed in raw mode can no longer swallow the next key.

There are six new tests for the console, covering colors, unknown sequences, cursor movement, control characters and the two backspace fixes. All six fail on the old code.

## TTYs

With the console done, we can go back up the stack. A console knows how to put text on a screen, but it has no idea who is typing, who is reading, or which program the text belongs to. As we saw at the start of the chapter, that is the TTY's job. Here is the structure that holds one:

```c
typedef struct tty {
    char buffer[TTY_BUFFER_SIZE];
    volatile uint32_t head;      // Write index (volatile: polled outside the lock)
    volatile uint32_t tail;      // Read index (volatile: polled outside the lock)

    spinlock_t lock;
    struct thread* wait_head;  // threads blocked waiting for input

    tty_output_t own_out;
    tty_output_t* out;

    // Line Discipline
    ldisc_t ldisc;

    // Hardware Association
    console_t* console;

    // Linked List for dynamic management
    struct tty* next;
    struct tty* prev;

    bool hidden; // if true, excluded from Alt+Tab cycle

} tty_t;
```

The kernel's TTY is created in bootstrap, right after the heap and the timers, and every process creates its own when it starts. (Processes themselves are next chapter's job, but their TTY side is all here.)

### The Input Buffer

Keys arrive in an interrupt handler, while programs read them whenever they get around to it. In between, they wait in `buffer`, a 4 KiB **ring buffer**. `head` is where the next byte goes in. `tail` is where the next byte comes out. Both wrap around at the end, and the buffer is empty when they are equal:

```c
void tty_push_char_raw(tty_t* tty, char c) {
    if (!tty) return;
    bool flags = spinlock_acquire(&tty->lock);

    uint32_t next_idx = (tty->head + 1) % TTY_BUFFER_SIZE;
    if (next_idx != tty->tail) {
        tty->buffer[tty->head] = c;
        tty->head = next_idx;
        tty_wake(tty);
    }

    spinlock_release(&tty->lock, flags);
}
```

If `head` is about to run into `tail`, the buffer is full and the byte is dropped. This is why the buffer can only ever hold 4095 bytes, not 4096.

To see why, shrink the buffer down to 4 slots. It starts empty, with `head = tail = 0`. We push `a`, `b` and `c`, and `head` moves to 3:

```
 slot:   0   1   2   3
         a   b   c   .
         ▲           ▲
       tail        head      3 bytes waiting
```

A reader takes `a` and `b`, so `tail` moves to 2. Then we push `d` into slot 3, and `head` wraps around to 0. That is the "ring" part, the buffer has no end, it just keeps going around:

```
 slot:   0   1   2   3
         .   .   c   d
         ▲       ▲
       head    tail          2 bytes waiting (c, d)
```

Now say we allowed a fourth byte in. We push `e` and `f` into slots 0 and 1, and `head` moves to 2, right on top of `tail`. But `head == tail` is exactly how we recognize an *empty* buffer. A full buffer and an empty one would look the same. So we stop one slot short, and a 4 slot buffer holds at most 3 bytes.

### Waiting for Input

A program that reads from an empty TTY should not spin, burning the CPU on a loop while the user thinks. Instead, it goes to sleep until a key arrives. `tty_block` puts the current thread on the TTY's wait list and gives up the CPU:

```c
static void tty_block(tty_t* tty) {
    if (!sched_active()) return;
    thread_t* t = sched_current();
    if (!t) return;

    bool iflag = intr_save();
    if (tty->head != tty->tail) {
        // Data arrived between caller's check and here, no need to block.
        intr_restore(iflag);
        return;
    }
    t->state = T_BLOCKED;
    t->rnext = tty->wait_head;
    tty->wait_head = t;
    intr_restore(iflag);
    sched_yield();
}
```

The caller checked that the buffer was empty before calling this. Why check again? Here is what could happen without the second check:

1. A program calls `tty_read`. It sees `head == tail`, so the buffer is empty, and calls `tty_block`.
2. Before `tty_block` turns interrupts off, the user presses a key. The interrupt handler pushes it into the buffer and calls `tty_wake`. Nobody is on the wait list yet, so it wakes nobody.
3. `tty_block` puts the thread on the wait list and goes to sleep.

The key is sitting in the buffer, and the wakeup that was meant for it has already come and gone.

The thread would sleep until the *next* key, which, if the user is waiting for a reaction, might never come. This is called a **lost wakeup**, and the cure is always the same. The final check and going to sleep have to happen with interrupts off, so nothing can happen in between. That is what the second check in `tty_block` is for. With interrupts off, step 2 can no longer squeeze in between the check and the sleep. It either happens before (and the check sees the key) or after (and the wakeup finds the thread on the list).

On the other side, `tty_push_char_raw` calls `tty_wake`, handing every waiting thread back to the scheduler. They wake up, find data in the buffer and read it.

### Many TTYs

All TTYs live in a circular, doubly linked list. `tty_create` adds new ones at the end. `active_tty` points at the one on screen. Switching is short:

```c
void tty_switch(tty_t* tty) {
    if (!tty || active_tty == tty) return;

    active_tty = tty;

    if (tty->console) {
        con_refresh(tty->console);
    }
}
```

`con_refresh` marks every cell of the new console dirty and flushes it, drawing the whole backbuffer to the screen. The old console needs nothing. Everything that checks whether a console is active now says "no" for it, and it goes on writing into its backbuffer in the dark.

The input hub's Alt+Tab calls `tty_cycle`. It walks the list to the next TTY that is not hidden:

```c
tty_t* next = active_tty->next;
while (next != active_tty && next->hidden)
    next = next->next;
if (next != active_tty && !next->hidden)
    tty_switch(next);
```

Hidden TTYs exist for threads that need a TTY (every process has one) but have nothing to show. The xHCI hotplug thread from last chapter is one of them. The dashboard is another, reachable only through its own hotkey.

Alt+F4 calls `tty_destroy` on the active TTY, unless it is the kernel's. That kills every process attached to the TTY and wakes anything still waiting on it (so those threads can be cleaned up). The TTY then leaves the list, and the screen goes back to the first TTY (always the kernel's).

## The Line Discipline

Between the keyboard and the input buffer sits one more piece, the **line discipline**. Its job is to decide what happens to each key before a program gets it. It has two modes.

In **canonical** mode (the default), the line discipline collects a whole line before handing it over. Characters go into a separate line buffer while being echoed to the screen. Backspace removes the last one, both from the line buffer and from the screen.

Only when Enter is pressed does the line, newline and all, move into the TTY's input buffer, where a program can read it. That is why a program reading a line never sees your typos, only the line you finally sent.

In **raw** mode, every byte goes straight into the input buffer. Backspace is just another byte. Programs that react to single keys (a game, an editor) switch to raw mode through a system call. Arrow keys only produce their `ESC [ A` style sequences in raw mode, since a line editor has nothing useful to do with them.

Let's trace a typo in canonical mode. The user wants `cat`, types `cst`, notices, presses backspace twice and types `at`, then Enter:

| Key | Line buffer | On screen | Input buffer |
|---|---|---|---|
| `c` `s` `t` | `cst` | `cst` | (empty) |
| backspace | `cs` | `cs` | (empty) |
| backspace | `c` | `c` | (empty) |
| `a` `t` | `cat` | `cat` | (empty) |
| Enter | (empty) | `cat`, new line | `cat\n` |

The program reading from the TTY never sees the `s` or the `t`. It just gets `cat\n`, all at once.

The whole thing is one function:

```c
void ldisc_input(tty_t* tty, char c) {
    ldisc_t* ld = &tty->ldisc;

    // No editing in raw mode, the reader gets the byte as is
    if (ld->raw) {
        if (ld->echo && tty->console) con_putc(tty->console, c);
        tty_push_char_raw(tty, c);
        return;
    }

    if (c == '\b') {
        if (ld->pos > 0) {
            ld->pos--;
            if (ld->echo && tty->console) con_putc(tty->console, '\b');
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        if (ld->echo && tty->console) con_putc(tty->console, '\n');
        for (uint32_t i = 0; i < ld->pos; i++) tty_push_char_raw(tty, ld->line_buffer[i]);
        tty_push_char_raw(tty, '\n');
        ld->pos = 0;
        return;
    }

    if (ld->pos < LDISC_LINE_MAX - 1) {
        ld->line_buffer[ld->pos++] = c;
        if (ld->echo && tty->console) con_putc(tty->console, c);
    }
}
```

This is where the console's backspace earns its odd behavior. On a real terminal, `\b` only moves the cursor back. Erasing a character takes three bytes, `\b \b` (back, overwrite with a space, back again). GatOS made `\b` erase on its own, keeping the echo down to one byte.

### Backspacing Over a Wrap

Before this chapter, backspace only ever moved left within the current row. That is fine for short lines. But say you type a line that is longer than the screen is wide. The text wraps onto the next row. The line discipline still sees it as one line.

Now press backspace until you have erased everything on the second row. The cursor reaches column 0. The next backspace removes a character from the line buffer, while the console, unable to move left, just erases column 0 again. From then on, the screen and the line disagree about what you have typed. Every further backspace makes it worse.

The fix is in `emit_cp`, which now steps back to the last column of the previous row when backspace hits column 0:

```c
if (con->cx > 0) con->cx--;
else if (con->cy > con->header_rows) { con->cy--; con->cx = con->width - 1; }
put_cell(con, con->cx, con->cy, ' ', active);
```

It works because of where the console wraps. A typed line always fills its rows completely before wrapping. Whenever the cursor is at column 0 with the line discipline still holding characters, the previous one is guaranteed to be in the last column of the row above.

>[!NOTE]
>Backspace used to erase the cell on screen with the current colors, while keeping the old colors in the backbuffer. Erasing a red character left a cell that looked fine until the next redraw, then turned red again. `put_cell` now writes both.

## Who Gets to Write

The last problem is output. Every process writes to its TTY whenever it likes, and a process can have several threads writing at once.

Say thread A prints `Hello from A\n` and thread B prints `Greetings from B\n` at the same moment. `kprintf` does not print a string in one go. It formats it into chunks and writes them out one after the other. If the scheduler switches threads in the middle, the two outputs get spliced together, and the screen could easily show something like `Hello frGreetings from B` followed by `om A`. We want two whole lines, in either order, but never mixed.

GatOS gives each TTY an output owner. `tty_lock_output` makes the calling thread the owner. Anybody else who wants to write in the meantime blocks until the owner lets go:

```c
while (true) {
    bool flags = spinlock_acquire(&o->lock);
    if (!o->owner || o->owner == me) {
        o->owner = me;
        o->depth++;
        spinlock_release(&o->lock, flags);
        return true;
    }
    me->state = T_BLOCKED;
    me->rnext = o->wait;
    o->wait = me;
    spinlock_release(&o->lock, flags);
    sched_yield();
}
```

The owner can take the lock again (`depth` counts how many times), which lets a function that holds the output call another one that also asks for it. `kprintf` takes it once for the whole call. One `printf` always comes out in one piece, even though it writes its output in 256 byte chunks.

Interrupt handlers are the exception, because they cannot block. If an interrupt handler wants to print while some thread owns the output, `tty_write` copies its text into `held`, a small 512 byte buffer on the side. The owner writes it out when it lets go. Anything past 512 bytes is dropped, which is fine for the occasional line from an interrupt.

This is also where `kprintf` gets its destination. Back in Chapter 4, it wrote straight to the serial port. Today, it writes to the calling thread's own TTY when the scheduler is running and the thread belongs to a process. Otherwise, it writes to the active TTY:

```c
static tty_t* _out_tty(void) {
    if (sched_active()) {
        thread_t* current = sched_current();
        if (current && current->process && current->process->tty) {
            return current->process->tty;
        }
    }
    return active_tty;
}
```

This is why a background process that prints does not take over the screen. Its text goes into its own console's backbuffer and shows up the next time someone Alt+Tabs to it.

## The Sticky Header

Every process TTY has a status line at the top, showing the process's name, PID and how many of its threads are alive. It stays put while everything below it scrolls. That is the **sticky header**, the reason for the `header_rows` field we have been running into all chapter.

`con_header_init` reserves the top rows. Everything else respects them. `scroll` starts moving rows at `header_rows` instead of 0. Clearing the screen only clears below it, and cursor positioning counts rows from just below it. `con_header_write` writes a centered line of text into one header row and redraws it on the spot. The process code calls it whenever a process's state changes:

```c
ksnprintf(hdr, sizeof(hdr), "Process: %s  |  PID: %u  |  Threads: %zu  |  State: %s", proc->name, proc->pid, alive, state);

tty_header_write(proc->tty, 1, hdr, CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
```

Each process TTY reserves three rows, using the middle one for this line and the other two as spacing.

## Putting It All Together

The console and TTYs come up in two separate places in `kernel_bootstrap()`:

```c
// We need panic to work right about now
// if we panic before this, something went catastrophically wrong
console_init(mb);
// ...

// TTYs and output finally online
tty_t* k_tty = tty_create();
if (!k_tty) panic("Failed to create kernel TTY!");

// Default to Kernel TTY
active_tty = k_tty;
kernel_tty = k_tty; // Protect this from ALT+F4
```

`console_init` runs early, right after the physmap, since the crash console needs the framebuffer and the font from the very start. It allocates nothing. The first real console only appears when `tty_create` builds the kernel's TTY. That has to wait for the heap, because the backbuffer and the dirty bitmap are heap allocations. From there, every process creates its own TTY the same way.

Each piece builds on one before it. The physmap gives us the framebuffer, and the PAT makes writing to it fast, as long as we never read it back. The heap gives every console a backbuffer, and we never have to. The dirty bitmap and deferred rendering keep the framebuffer writes down to the cells that actually changed.

The UTF-8 decoder and the escape sequence parser turn a stream of bytes into characters and commands. The font, generated glyphs included, turns characters into pixels. On top of all of that, the TTY gives each program its own screen, its own input and its own rules for both, with Alt+Tab to move between them.

Every program in this chapter had a TTY of its own, and all of them ran at the same time on a single core. How that works is the last big piece of the kernel. In the next chapter, we build the scheduler, the processes and threads it runs, and the context switch that makes many programs look like they run at once on a single core.
