/*
 * debug.h - Debugging utilities for GatOS kernel
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdbool.h>

void QEMU_LOG(const char* msg, ...);
void LOGF(const char* fmt, ...);
void LOGH(const char* fmt, ...);
bool log_resume(void);
void log_indent(int levels);
