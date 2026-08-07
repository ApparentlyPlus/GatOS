/*
 * font.c - Font Driver
 *
 * Provides support for loading and rendering fonts.
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/font.h>

static psf1_font_t cur_font;

#define QUAD_ORDER 0x00E62B79D184ULL

// Corner radius, in pixels, of the rounded box drawing characters
#define ARC_R 2

static uint8_t ext_glyphs[FONT_EXT_GLYPHS * FONT_MAX_CHARSIZE];
static int ext_h = 16;

/*
 * grid - Fill a gw x gh sub-cell grid; bit (gy*gw + gx) selects a sub-cell
 */
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

/*
 * synth_block - Block Elements U+2580..259F
 */
static void synth_block(uint8_t* g, int h, uint32_t cp) {
    if (cp == 0x2580u) { grid(g, h, 1, 2, 0x1u); return; }
    if (cp <= 0x2588u) {
        uint32_t n = cp - 0x2580u;
        grid(g, h, 1, 8, ((1u << n) - 1u) << (8u - n));
        return;
    }
    if (cp <= 0x258Fu) {
        uint32_t n = 0x2590u - cp;
        grid(g, h, 8, 1, (1u << n) - 1u);
        return;
    }
    if (cp == 0x2590u) { grid(g, h, 2, 1, 0x2u); return; }
    if (cp <= 0x2593u) {
        for (int y = 0; y < h; y++) {
            uint8_t b = 0;
            for (int x = 0; x < 8; x++) {
                int on = (cp == 0x2591u) ? (((x + 2 * (y & 1)) & 3) == 0)
                       : (cp == 0x2592u) ? (((x ^ y) & 1) == 0)
                                         : (((x + 2 * (y & 1)) & 3) != 0);
                if (on) b |= (uint8_t)(0x80u >> x);
            }
            g[y] = b;
        }
        return;
    }
    if (cp == 0x2594u) { grid(g, h, 1, 8, 0x01u); return; }
    if (cp == 0x2595u) { grid(g, h, 8, 1, 0x80u); return; }
    grid(g, h, 2, 2, (uint32_t)((QUAD_ORDER >> (4u * (cp - 0x2596u))) & 0xFu));
}

/*
 * synth_sextant - Sextants U+1FB00..1FB3B.
 */
static void synth_sextant(uint8_t* g, int h, uint32_t cp) {
    uint32_t p = (cp - FONT_SEX_FIRST) + 1u;
    if (p >= 21u) p++;
    if (p >= 42u) p++;
    grid(g, h, 2, 3, p);
}

/* arm weights: 0 none, 1 light, 2 heavy, 3 double */
enum { BX_UP = 0, BX_DN, BX_LF, BX_RT };

/*
 * box_arms - Decompose a Box Drawing codepoint into per-side arm weights.
 */
static void box_arms(uint32_t cp, uint8_t* a, uint8_t* dash, uint8_t* arc, uint8_t* diag) {
    static const uint8_t P[3] = { 1, 3, 3 };
    static const uint8_t O[3] = { 3, 1, 3 };
    uint32_t i, g, v, m;

    a[0] = a[1] = a[2] = a[3] = 0;
    *dash = *arc = *diag = 0;

    if (cp <= 0x2503u) {
        i = cp - 0x2500u;
        if (i >= 2u) a[BX_UP] = a[BX_DN] = (uint8_t)(1u + (i & 1u));
        else a[BX_LF] = a[BX_RT] = (uint8_t)(1u + (i & 1u));
    } else if (cp <= 0x250Bu) {
        i = cp - 0x2504u;
        *dash = (uint8_t)(3u + ((i >> 2) & 1u));
        if ((i >> 1) & 1u) a[BX_UP] = a[BX_DN] = (uint8_t)(1u + (i & 1u));
        else a[BX_LF] = a[BX_RT] = (uint8_t)(1u + (i & 1u));
    } else if (cp <= 0x251Bu) {
        i = cp - 0x250Cu; g = i >> 2; v = i & 3u;
        a[(g & 2u) ? BX_UP : BX_DN] = (uint8_t)(1u + ((v >> 1) & 1u));
        a[(g & 1u) ? BX_LF : BX_RT] = (uint8_t)(1u + (v & 1u));
    } else if (cp <= 0x253Bu) {
        i = cp - 0x251Cu; g = i >> 3; v = i & 7u;
        if (g < 2u) {
            m = (v == 0u) ? 0u : (v == 1u) ? 4u : (v <= 4u) ? v - 1u : v;
            a[BX_UP] = (uint8_t)((m & 1u) ? 2 : 1);
            a[BX_DN] = (uint8_t)((m & 2u) ? 2 : 1);
            a[g ? BX_LF : BX_RT]  = (uint8_t)((m & 4u) ? 2 : 1);
        } else {
            m = ((v & 3u) << 1) | (v >> 2);
            a[(g == 3u) ? BX_UP : BX_DN] = (uint8_t)((m & 1u) ? 2 : 1);
            a[BX_LF] = (uint8_t)((m & 2u) ? 2 : 1);
            a[BX_RT] = (uint8_t)((m & 4u) ? 2 : 1);
        }
    } else if (cp <= 0x254Bu) {
        v = cp - 0x253Cu;
        if (v <= 3u) m = v;
        else if (v <= 6u) m = (v - 3u) << 2;
        else if (v <= 10u) { uint32_t j = v - 7u; m = ((j & 1u) ? 2u : 1u) | ((j & 2u) ? 8u : 4u); }
        else if (v <= 14u) m = 15u - (1u << (14u - v));
        else m = 15u;
        a[BX_LF] = (uint8_t)((m & 1u) ? 2 : 1);
        a[BX_RT] = (uint8_t)((m & 2u) ? 2 : 1);
        a[BX_UP] = (uint8_t)((m & 4u) ? 2 : 1);
        a[BX_DN] = (uint8_t)((m & 8u) ? 2 : 1);
    } else if (cp <= 0x254Fu) {
        i = cp - 0x254Cu; *dash = 2;
        if ((i >> 1) & 1u) a[BX_UP] = a[BX_DN] = (uint8_t)(1u + (i & 1u));
        else a[BX_LF] = a[BX_RT] = (uint8_t)(1u + (i & 1u));
    } else if (cp <= 0x256Cu) {
        i = cp - 0x2550u;
        if (i == 0u) { a[BX_LF] = a[BX_RT] = 3; }
        else if (i == 1u) { a[BX_UP] = a[BX_DN] = 3; }
        else if (i <= 0x0Du) {
            uint32_t j = i - 2u; g = j / 3u; v = j % 3u;
            a[(g & 2u) ? BX_UP : BX_DN] = P[v];
            a[(g & 1u) ? BX_LF : BX_RT] = O[v];
        } else if (i <= 0x19u) {
            uint32_t j = i - 0x0Eu; uint8_t wp, wo;
            g = j / 3u; v = j % 3u;
            wp = (g < 2u) ? P[v] : O[v];
            wo = (g < 2u) ? O[v] : P[v];
            if (g < 2u) { a[BX_UP] = a[BX_DN] = wp; a[g ? BX_LF : BX_RT] = wo; }
            else { a[BX_LF] = a[BX_RT] = wp; a[(g == 3u) ? BX_UP : BX_DN] = wo; }
        } else {
            v = i - 0x1Au;
            a[BX_UP] = a[BX_DN] = P[v];
            a[BX_LF] = a[BX_RT] = O[v];
        }
    } else if (cp <= 0x2570u) {
        i = cp - 0x256Du;
        a[(i < 2u) ? BX_DN : BX_UP] = 1;
        a[(i == 0u || i == 3u) ? BX_RT : BX_LF] = 1;
        *arc = 1;
    } else if (cp <= 0x2573u) {
        *diag = (uint8_t)(cp - 0x2571u + 1u);
    } else if (cp <= 0x257Bu) {
        static const uint8_t dir[4] = { BX_LF, BX_UP, BX_RT, BX_DN };
        i = cp - 0x2574u;
        a[dir[i & 3u]] = (uint8_t)(1u + (i >> 2));
    } else {
        i = cp - 0x257Cu;
        if (i == 0u) { a[BX_LF] = 1; a[BX_RT] = 2; }
        else if (i == 1u) { a[BX_UP] = 1; a[BX_DN] = 2; }
        else if (i == 2u) { a[BX_LF] = 2; a[BX_RT] = 1; }
        else { a[BX_UP] = 2; a[BX_DN] = 1; }
    }
}

/*
 * synth_box - Rasterize one Box Drawing glyph from its arm weights
 */
static void synth_box(uint8_t* g, int h, uint32_t cp) {
    uint8_t a[4], dash, arc, diag;
    int cx = 3, cy = (h - 1) / 2;
    int i, x, y;

    box_arms(cp, a, &dash, &arc, &diag);

    if (diag) {
        for (y = 0; y < h; y++) {
            if (diag & 2u) { x = y * 7 / (h - 1);           g[y] |= (uint8_t)(0x80u >> x); }
            if (diag & 1u) { x = (h - 1 - y) * 7 / (h - 1); g[y] |= (uint8_t)(0x80u >> x); }
        }
        return;
    }

    if (arc) {
        int vd = a[BX_DN] ? 1 : -1;
        int hd = a[BX_RT] ? 1 : -1;
        int qx = cx + (hd > 0 ? 1 : 0);
        int ox = qx + hd * ARC_R;
        int oy = cy + vd * ARC_R;
        int r4 = (2 * ARC_R - 1) * (2 * ARC_R - 1);

        for (y = (vd > 0) ? oy : 0; y <= ((vd > 0) ? h - 1 : oy); y++) {
            if (y < 0 || y >= h) continue;
            g[y] |= (uint8_t)((0x80u >> cx) | (0x80u >> (cx + 1)));
        }
        for (x = (hd > 0) ? ox : 0; x <= ((hd > 0) ? 7 : ox); x++) {
            if (x < 0 || x > 7) continue;
            g[cy] |= (uint8_t)(0x80u >> x);
        }
        for (y = (vd > 0) ? cy : oy; y <= ((vd > 0) ? oy : cy); y++) {
            if (y < 0 || y >= h) continue;
            for (x = (hd > 0) ? qx : ox; x <= ((hd > 0) ? ox : qx); x++) {
                int dx = x - ox, dy = y - oy;
                if (x < 0 || x > 7) continue;
                if (4 * (dx * dx + dy * dy) >= r4) g[y] |= (uint8_t)(0x80u >> x);
            }
        }
        return;
    }

    for (i = 0; i < 2; i++) {
        uint8_t w = a[i ? BX_RT : BX_LF];
        int x0 = i ? cx : 0, x1 = i ? 7 : cx + 1;
        int r0, r1, r;
        if (!w) continue;
        r0 = (w == 3) ? cy - 1 : cy;
        r1 = (w == 2) ? cy + 1 : (w == 3) ? cy + 1 : cy;
        for (r = r0; r <= r1; r++) {
            if (w == 3 && r == cy) continue;
            if (r < 0 || r >= h) continue;
            for (x = x0; x <= x1; x++) {
                if (dash && ((x * 2 * dash / 8) & 1)) continue;
                g[r] |= (uint8_t)(0x80u >> x);
            }
        }
    }

    for (i = 0; i < 2; i++) {
        uint8_t w = a[i ? BX_DN : BX_UP];
        int y0 = i ? cy : 0, y1 = i ? h - 1 : cy + 1;
        int c0, c1, c;
        if (!w) continue;
        c0 = (w == 1) ? cx : cx - 1;
        c1 = cx + 1;
        for (c = c0; c <= c1; c++) {
            if (w == 3 && c == cx) continue;
            if (c < 0 || c > 7) continue;
            for (y = y0; y <= y1; y++) {
                if (y < 0 || y >= h) continue;
                if (dash && ((y * 2 * dash / h) & 1)) continue;
                g[y] |= (uint8_t)(0x80u >> c);
            }
        }
    }
}

/*
 * build_glyphs - Build every synthesized glyph. Runs once, at font_init.
 */
static void build_glyphs(int charsize) {
    uint32_t i;
    ext_h = (charsize > 0 && charsize <= FONT_MAX_CHARSIZE) ? charsize : 16;

    for (i = 0; i < sizeof(ext_glyphs); i++) ext_glyphs[i] = 0;

    for (i = 0; i < FONT_BLK_N; i++)
        synth_block(&ext_glyphs[(FONT_BLK_SLOT - FONT_BASE_GLYPHS + i) * FONT_MAX_CHARSIZE], ext_h, FONT_BLK_FIRST + i);
    for (i = 0; i < FONT_SEX_N; i++)
        synth_sextant(&ext_glyphs[(FONT_SEX_SLOT - FONT_BASE_GLYPHS + i) * FONT_MAX_CHARSIZE], ext_h, FONT_SEX_FIRST + i);
    for (i = 0; i < FONT_BOX_N; i++)
        synth_box(&ext_glyphs[(FONT_BOX_SLOT - FONT_BASE_GLYPHS + i) * FONT_MAX_CHARSIZE], ext_h, FONT_BOX_FIRST + i);
}

/*
 * unicode_to_glyph - Codepoint to glyph slot, CP437 first then synthesized
 */
uint16_t unicode_to_glyph(uint32_t cp) {
    uint8_t base = unicode_to_cp437(cp);
    if (base != 0 || cp == 0) return base;
    if (cp >= FONT_BOX_FIRST && cp <= FONT_BOX_LAST) return (uint16_t)(FONT_BOX_SLOT + (cp - FONT_BOX_FIRST));
    if (cp >= FONT_BLK_FIRST && cp <= FONT_BLK_LAST) return (uint16_t)(FONT_BLK_SLOT + (cp - FONT_BLK_FIRST));
    if (cp >= FONT_SEX_FIRST && cp <= FONT_SEX_LAST) return (uint16_t)(FONT_SEX_SLOT + (cp - FONT_SEX_FIRST));
    return 0;
}

/*
 * font_glyph - Bitmap for a glyph slot
 */
const uint8_t* font_glyph(uint16_t slot) {
    psf1_font_t* f = font_get_current();
    if (!f) return ext_glyphs;                    /* never NULL */
    if (slot < FONT_BASE_GLYPHS)
        return f->glyph_buffer + (uint32_t)slot * f->header->charsize;
    if (slot < FONT_TOTAL_GLYPHS)
        return &ext_glyphs[(uint32_t)(slot - FONT_BASE_GLYPHS) * FONT_MAX_CHARSIZE];
    return f->glyph_buffer;
}


/*
 * font_init - Initialize the font system
 */
void font_init(void) {
    cur_font.header = (const psf1_header_t*)vga_font;
    cur_font.glyph_buffer = vga_font + sizeof(psf1_header_t);
    build_glyphs(cur_font.header->charsize);
}

/*
 * font_get_current - Get a pointer to the current font
 */
psf1_font_t* font_get_current(void) {
    return &cur_font;
}

/*
 * unicode_to_cp437 - Convert a Unicode codepoint to a CP437 glyph index
 */
uint8_t unicode_to_cp437(uint32_t codepoint) {
    if (codepoint < 128) return (uint8_t)codepoint;
    switch (codepoint) {
        case 0x263A: return 1; case 0x263B: return 2; case 0x2665: return 3; case 0x2666: return 4;
        case 0x2663: return 5; case 0x2660: return 6; case 0x2022: return 7; case 0x25D8: return 8;
        case 0x25CB: return 9; case 0x25D9: return 10; case 0x2642: return 11; case 0x2640: return 12;
        case 0x266A: return 13; case 0x266B: return 14; case 0x263C: return 15; case 0x25BA: return 16;
        case 0x25C4: return 17; case 0x2195: return 18; case 0x203C: return 19; case 0x00B6: return 20;
        case 0x00A7: return 21; case 0x25AC: return 22; case 0x21A8: return 23; case 0x2191: return 24;
        case 0x2193: return 25; case 0x2192: return 26; case 0x2190: return 27; case 0x221F: return 28;
        case 0x2194: return 29; case 0x25B2: return 30; case 0x25BC: return 31; 
        
        case 0x00C7: return 128; case 0x00FC: return 129; case 0x00E9: return 130; case 0x00E2: return 131;
        case 0x00E4: return 132; case 0x00E0: return 133; case 0x00E5: return 134; case 0x00E7: return 135;
        case 0x00EA: return 136; case 0x00EB: return 137; case 0x00E8: return 138; case 0x00EF: return 139;
        case 0x00EE: return 140; case 0x00EC: return 141; case 0x00C4: return 142; case 0x00C5: return 143;
        case 0x00C9: return 144; case 0x00E6: return 145; case 0x00C6: return 146; case 0x00F4: return 147;
        case 0x00F6: return 148; case 0x00F2: return 149; case 0x00FB: return 150; case 0x00F9: return 151;
        case 0x00FF: return 152; case 0x00D6: return 153; case 0x00DC: return 154; case 0x00A2: return 155;
        case 0x00A3: return 156; case 0x00A5: return 157; case 0x20A7: return 158; case 0x0192: return 159;
        case 0x00E1: return 160; case 0x00ED: return 161; case 0x00F3: return 162; case 0x00FA: return 163;
        case 0x00F1: return 164; case 0x00D1: return 165; case 0x00AA: return 166; case 0x00BA: return 167;
        case 0x00BF: return 168; case 0x2310: return 169; case 0x00AC: return 170; case 0x00BD: return 171;
        case 0x00BC: return 172; case 0x00A1: return 173; case 0x00AB: return 174; case 0x00BB: return 175; 
        
        case 0x2591: return 176; case 0x2592: return 177; case 0x2593: return 178; case 0x2502: return 179;
        case 0x2524: return 180; case 0x2561: return 181; case 0x2562: return 182; case 0x2556: return 183;
        case 0x2555: return 184; case 0x2563: return 185; case 0x2551: return 186; case 0x2557: return 187;
        case 0x255D: return 188; case 0x255C: return 189; case 0x255B: return 190; case 0x2510: return 191;
        case 0x2514: return 192; case 0x2534: return 193; case 0x252C: return 194; case 0x251C: return 195;
        case 0x2500: return 196; case 0x253C: return 197; case 0x255E: return 198; case 0x255F: return 199;
        case 0x255A: return 200; case 0x2554: return 201; case 0x2569: return 202; case 0x2566: return 203;
        case 0x2560: return 204; case 0x2550: return 205; case 0x256C: return 206; case 0x2567: return 207;
        case 0x2568: return 208; case 0x2564: return 209; case 0x2565: return 210; case 0x2559: return 211;
        case 0x2558: return 212; case 0x2552: return 213; case 0x2553: return 214; case 0x256B: return 215;
        case 0x256A: return 216; case 0x2518: return 217; case 0x250C: return 218; 
        
        case 0x2588: return 219; case 0x2584: return 220; case 0x258C: return 221; case 0x2590: return 222;
        case 0x2580: return 223; case 0x03B1: return 224; case 0x00DF: return 225; case 0x0393: return 226;
        case 0x03C0: return 227; case 0x03A3: return 228; case 0x03C3: return 229; case 0x00B5: return 230;
        case 0x03C4: return 231; case 0x03A6: return 232; case 0x0398: return 233; case 0x03A9: return 234;
        case 0x03B4: return 235; case 0x221E: return 236; case 0x03C6: return 237; case 0x03B5: return 238;
        case 0x2229: return 239; case 0x2261: return 240; case 0x00B1: return 241; case 0x2265: return 242;
        case 0x2264: return 243; case 0x2320: return 244; case 0x2321: return 245; case 0x00F7: return 246;
        case 0x2248: return 247; case 0x00B0: return 248; case 0x2219: return 249; case 0x00B7: return 250;
        case 0x221A: return 251; case 0x207F: return 252; case 0x00B2: return 253; case 0x25A0: return 254;
        case 0x00A0: return 255;
        default: return 0;
    }
}