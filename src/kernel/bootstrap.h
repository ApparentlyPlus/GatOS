/*
 * bootstrap.h - Staged kernel initialization
 *
 * One entry point: raw multiboot handoff to fully initialized, interrupts on.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <arch/x86_64/multiboot2.h>
#include <stdbool.h>

// Total boot stage QEMU_LOG markers (bootstrap + kernel_main)
#define TOTAL_DBG 25

bool kernel_bootstrap(void* mb_info, multiboot_parser_t* mb, bool verbose, const char* version);
