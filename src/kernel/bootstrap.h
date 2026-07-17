/*
 * bootstrap.h - Staged kernel initialization
 *
 * Single entry point that brings the kernel from raw multiboot handoff to
 * fully initialized (interrupts on), honoring the GATA_CAP_* capability
 * gates. Both the template's kernel_main and appa's emitted boot preamble
 * call this, so the init sequence can never drift between them.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <arch/x86_64/multiboot2.h>
#include <stdbool.h>

bool kernel_bootstrap(void* mb_info, multiboot_parser_t* mb, bool verbose);
