/*
 * vgafont.S - Embeds the VGA 8x16 CP437 font in the kernel image
 *
 * The bitmaps live next to this file as a real PSF1 file rather than a wall of
 * hex in font.c, so they can be edited with a font editor.
 *
 * Author: u/ApparentlyPlus
 */

.section .rodata
.globl vga_font
.balign 8

vga_font:
    .incbin "kernel/drivers/vgafont.psf"
    .size vga_font, . - vga_font
