/* Host (Windows/UCRT) shims so Nocturne's libc-style sources compile in a harness. Harness-only. */
#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
