#pragma once
#include <stdint.h>
#include <stdbool.h>

#define IA32_APIC_BASE          0x1B
#define APIC_GLOBAL_ENABLE_BIT (1ULL << 11)

uint64_t get_lapic_address(bool mapped);