/*
 * bitops.h - Common bitwise utility functions
 *
 * Provides shared bit manipulation helpers as static inlines
 * to avoid duplicate definitions across translation units.
 *
 * Author: ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * is_pow2_u64 - check if x is a power of two
 */
static inline bool is_pow2_u64(uint64_t x) {
    return x && ((x & (x - 1)) == 0);
}
