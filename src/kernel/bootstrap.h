/*
 * bootstrap.h - Staged kernel initialization
 *
 * One entry point: raw multiboot handoff to fully initialized, interrupts on, honoring the
 * GATA_CAP_* gates. Shared by kernel_main and appa's emitted boot preamble.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <arch/x86_64/multiboot2.h>
#include <stdbool.h>

bool kernel_bootstrap(void* mb_info, multiboot_parser_t* mb, bool verbose);
