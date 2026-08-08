/*
 * bootstrap.h - Staged kernel initialization
 *
 * Single entry point that brings the kernel from raw multiboot handoff to
 * fully initialized (interrupts on). kernel_main stays a thin caller that
 * only hosts what runs after boot (userspace launch, interactive loop).
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <arch/x86_64/multiboot2.h>
#include <stdbool.h>

// Total boot stage QEMU_LOG markers (bootstrap + kernel_main)
#define TOTAL_DBG 25

bool kernel_bootstrap(void* mb_info, multiboot_parser_t* mb, bool verbose, const char* version);
