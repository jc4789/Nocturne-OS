#pragma once
#include_next <errno.h>
/* Additional C-library error tokens used inside FFmpeg only. Native syscall
 * errors retain Nocturne's values; these do not extend the kernel ABI. */
#define EDOM 33
#define ENXIO 6
#define EACCES 13
#define EXDEV 18
#define ENODEV 19
#define ENFILE 23
#define ENOTTY 25
#define EFBIG 27
#define EMLINK 31
#define EDEADLK 35
#define ENOLCK 37
#define ENODATA 61
#define EOVERFLOW 75
#define EILSEQ 84
#define ENOTSUP 95
#define EOPNOTSUPP ENOTSUP
