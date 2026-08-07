/*
 * vgafont.S - Embeds the VGA 8x16 CP437 font in the kernel image
 *
 * The .incbin path is relative to the build's working directory, which is the
 * repository root (run.py invokes the compiler without changing directory).
 
 * Author: u/ApparentlyPlus
 */

.section .rodata
.globl vga_font
.balign 8

vga_font:
    .incbin "assets/vgafont.psf"
    .size vga_font, . - vga_font
