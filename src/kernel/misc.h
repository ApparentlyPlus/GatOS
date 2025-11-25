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
void check_kernel_position();
uintptr_t get_rip();
int int_to_str(int num, char *str);