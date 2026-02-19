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
    psf1_header_t* header;
    void* glyph_buffer;
} psf1_font_t;

// Standard VGA 8x16 font embedded in the kernel
extern uint8_t g_vga_font_data[];

// Helper to initialize the font structure
void font_init(void);
psf1_font_t* font_get_current(void);

// Maps a Unicode codepoint to a CP437 glyph index.
// Returns 0 (NUL) if no mapping exists, though usually 0 is a valid glyph too.
// We might return a specific replacement index (like 0 or 0x3F '?') if not found.
uint8_t unicode_to_cp437(uint32_t codepoint);