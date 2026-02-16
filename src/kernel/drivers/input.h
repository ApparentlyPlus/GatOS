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
