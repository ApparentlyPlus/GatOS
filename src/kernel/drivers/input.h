/*
 * input.h - Input Hub Header
 * 
 * This file declares the system input hub interface that handles keyboard events and routes them to the appropriate TTY.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <kernel/drivers/keyboard.h>

void input_init(void);
void input_handle_key(key_event_t event);
