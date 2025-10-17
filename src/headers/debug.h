/*
 * debug.h - Debugging utilities for GatOS kernel
 *
 * Declares all debugging related functions.
 * 
 * Author: u/ApparentlyPlus
 */

#pragma once

void QEMU_LOG(const char* msg, int total);
void QEMU_GENERIC_LOG(const char* msg);
void QEMU_DUMP_PMT(void);
void DEBUGF(const char* fmt, ...);