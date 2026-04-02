/*
 * power.h - Kernel Power Management
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

// System control
void reboot(void);
void power_off(void);

// RAPL energy sampling
void power_rapl_init(void);
bool power_rapl_available(void);
uint32_t power_avg_watts(void);
