/*
 * ldisc.c - Line Discipline Implementation
 * 
 * This file implements the line discipline logic for TTY input handling.
 * The line discipline operates in canonical mode, buffering input until a newline is received.
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/ldisc.h>
#include <kernel/drivers/tty.h>
#include <libc/string.h>

/*
 * ldisc_init - Resets the line discipline state.
 */
void ldisc_init(ldisc_t* ld) {
    memset(ld->line_buffer, 0, LDISC_LINE_MAX);
    ld->pos = 0;
    ld->echo = true;
}

/*
 * ldisc_input - Processes a character through the line discipline.
 * Handled characters are eventually pushed to the TTY read buffer.
 */
void ldisc_input(tty_t* tty, char c) {
    ldisc_t* ld = &tty->ldisc;

    // Enforce Canonical Mode Logic
    if (c == '\b') {
        if (ld->pos > 0) {
            ld->pos--;
            if (ld->echo && tty->console) {
                con_putc(tty->console, '\b');
            }
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        c = '\n';
        if (ld->echo && tty->console) {
            con_putc(tty->console, '\n');
        }
        
        // Commit line to TTY buffer
        for (uint32_t i = 0; i < ld->pos; i++) {
            tty_push_char_raw(tty, ld->line_buffer[i]);
        }
        tty_push_char_raw(tty, '\n');
        
        ld->pos = 0;
        return;
    }

    // Normal character in canonical mode
    if (ld->pos < LDISC_LINE_MAX - 1) {
        ld->line_buffer[ld->pos++] = c;
        if (ld->echo && tty->console) {
            con_putc(tty->console, c);
        }
    }
}
