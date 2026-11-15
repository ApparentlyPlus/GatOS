/*
 * debug.h - Debugging utilities for GatOS kernel
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

void QEMU_LOG(const char* msg, int total);
void QEMU_GENERIC_LOG(const char* msg);
#include <stdbool.h>

void LOGF(const char* fmt, ...);
void LOGH(const char* fmt, ...);
bool log_resume(void);
void log_indent(int levels);