/*
 * ldisc.h - Line Discipline Header
 *
 * Canonical mode, input is buffered until a newline.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LDISC_LINE_MAX 1024

// Forward declaration to avoid circular dependency
typedef struct tty tty_t;

typedef struct {
    char line_buffer[LDISC_LINE_MAX];
    uint32_t pos;
    bool echo;
} ldisc_t;

void ldisc_init(ldisc_t* ld);
void ldisc_input(tty_t* tty, char c);
