#pragma once
// libc++ needs a complete conversion-state type for its stream-position
// declarations even with wide characters/localization disabled. No conversion
// API is supplied by this profile; this is its native, zero-initializable ABI.
typedef struct {
    unsigned int __count;
    unsigned int __value;
} mbstate_t;
