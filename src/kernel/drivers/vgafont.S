/*
 * vgafont.S - Embeds the VGA 8x16 CP437 font in the kernel image
 *
 * Author: u/ApparentlyPlus
 */

#include <kernel/caps.h>

#ifdef GATA_CAP_FRAMEBUFFER

.section .rodata
.globl vga_font
.balign 8

vga_font:
    .incbin "kernel/drivers/vgafont.psf"
    .size vga_font, . - vga_font

#endif
