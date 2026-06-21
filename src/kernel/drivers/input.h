/*
 * input.h - System Input Hub Interface
 *
 * This module provides a centralized entry point for all hardware input
 * events. It handles system-wide hotkeys and routes input to the
 * active terminal.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <kernel/drivers/keyboard.h>

void input_init(void);
void input_handle_key(key_event_t event);

// Static, allocation-free getchar for builds with no scheduler/TTY
// (GATA_CAP_INPUT without GATA_CAP_THREADS - see kernel/caps.h). Populated
// directly from the keyboard IRQ handler via input_handle_key; returns -1
// if nothing is buffered. Hotkeys (dashboard toggle, ALT+Tab, ALT+F4) don't
// exist in this mode since they all act on the TTY/dashboard subsystem.
int input_getchar(void);
