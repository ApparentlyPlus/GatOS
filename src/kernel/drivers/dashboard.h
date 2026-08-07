/*
 * dashboard.h - Live Kernel Dashboard
 *
 * Author: u/ApparentlyPlus
 */

#pragma once
#include <kernel/caps.h>
#include <stdbool.h>

#if defined(GATA_CAP_THREADS) && defined(GATA_CAP_FRAMEBUFFER)

void dash_init(void);
void dash_toggle(void);
bool dash_active(void);

#else

static inline void dash_init(void)   { }
static inline void dash_toggle(void) { }
static inline bool dash_active(void) { return false; }

#endif
