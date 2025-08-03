static int DBG_COUNTER = 0;

#define DEBUG(str, total) do { \
    char _debug_buf[64]; \
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
