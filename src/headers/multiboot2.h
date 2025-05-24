#pragma once
#include <stdint.h>

extern char __bss_end;

uint64_t multiboot_detect_heap(void* multiboot_info_addr);