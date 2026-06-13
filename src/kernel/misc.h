/*
 * misc.h - Miscellaneous kernel utilities
 *
 * Contains function declarations for kernel banner printing, 
 * position verification, and integer conversion utilities.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>

void print_banner(char* KERNEL_VERSION);
void print_test_banner(char* KERNEL_VERSION);
void check_kpos();
uintptr_t get_rip();