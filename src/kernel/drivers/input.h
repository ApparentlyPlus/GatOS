/*
 * input.h - System Input Hub Interface
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <kernel/drivers/keyboard.h>

void input_init(void);
void input_handle_key(key_event_t event);
