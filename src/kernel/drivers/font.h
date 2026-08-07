/* font.h - Font Driver
 *
 * Provides support for loading and rendering fonts.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
} __attribute__((packed)) psf1_header_t;

typedef struct {
    const psf1_header_t* header;
    const uint8_t* glyph_buffer;
} psf1_font_t;

// Standard VGA 8x16 font embedded in the kernel
extern const uint8_t vga_font[];

// Helper to initialize the font structure
void font_init(void);
psf1_font_t* font_get_current(void);

#define FONT_BASE_GLYPHS 256
#define FONT_MAX_CHARSIZE  16
#define FONT_BLK_FIRST 0x2580u
#define FONT_BLK_LAST  0x259Fu
#define FONT_SEX_FIRST 0x1FB00u
#define FONT_SEX_LAST  0x1FB3Bu
#define FONT_BOX_FIRST 0x2500u
#define FONT_BOX_LAST  0x257Fu

#define FONT_BLK_N (FONT_BLK_LAST - FONT_BLK_FIRST + 1)
#define FONT_SEX_N (FONT_SEX_LAST - FONT_SEX_FIRST + 1)
#define FONT_BOX_N (FONT_BOX_LAST - FONT_BOX_FIRST + 1)

#define FONT_BLK_SLOT FONT_BASE_GLYPHS
#define FONT_SEX_SLOT (FONT_BLK_SLOT + FONT_BLK_N)
#define FONT_BOX_SLOT (FONT_SEX_SLOT + FONT_SEX_N)
#define FONT_EXT_GLYPHS (FONT_BLK_N + FONT_SEX_N + FONT_BOX_N)
#define FONT_TOTAL_GLYPHS (FONT_BASE_GLYPHS + FONT_EXT_GLYPHS)

uint16_t unicode_to_glyph(uint32_t codepoint);
const uint8_t* font_glyph(uint16_t slot);
uint8_t unicode_to_cp437(uint32_t codepoint);