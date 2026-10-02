/*
 * input.h - System Input Hub Interface
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <kernel/drivers/keyboard.h>

void input_init(void);
void input_handle_key(key_event_t event);

// Static, allocation free getchar for builds with no scheduler/TTY (INPUT without THREADS, see
// caps.h). Filled from the keyboard IRQ handler via input_handle_key, -1 if empty. No hotkeys here,
// they all act on the TTY/dashboard.
int input_getchar(void);
