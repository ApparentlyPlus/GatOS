/*
 * debug.h - Debugging utilities for GatOS kernel
 *
 * Provides the DEBUG macro for formatted serial output with automatic 
 * counter tracking. Includes file/line information when enabled.
 *
 * Author: u/ApparentlyPlus
 */

static int DBG_COUNTER = 0;

#define DEBUG(str, total) do { \
    char _debug_buf[100]; \
    char *_debug_ptr = _debug_buf; \
    \
    *_debug_ptr++ = '['; \
    _debug_ptr += int_to_str(++DBG_COUNTER, _debug_ptr); \
    *_debug_ptr++ = '/'; \
    _debug_ptr += int_to_str((total), _debug_ptr); \
    *_debug_ptr++ = ']'; \
    *_debug_ptr++ = ' '; \
    \
    const char *_debug_str = (str); \
    while (*_debug_str) *_debug_ptr++ = *_debug_str++; \
    \
    *_debug_ptr++ = '\n'; \
    *_debug_ptr = '\0'; \
    \
    serial_write(_debug_buf); \
} while (0)
