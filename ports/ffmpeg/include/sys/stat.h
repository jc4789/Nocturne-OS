#pragma once
/* FFmpeg-internal C header glue, not a new Nocturne filesystem ABI. */
#include "nocturne.h"
#include <stdint.h>
#include <sys/types.h>
struct stat { int64_t st_size, st_mtime; uint32_t st_mode; };
static inline int ffmpeg_fstat(int fd, struct stat *out) {
    struct n_stat native;
    int r = fstat(fd, &native);
    if (!r) { out->st_size = (int64_t)native.size; out->st_mtime = native.mtime; out->st_mode = native.type; }
    return r;
}
static inline int ffmpeg_stat(const char *path, struct stat *out) {
    struct n_stat native;
    int r = stat(path, &native);
    if (!r) { out->st_size = (int64_t)native.size; out->st_mtime = native.mtime; out->st_mode = native.type; }
    return r;
}
#define fstat ffmpeg_fstat
#define stat(path,out) ffmpeg_stat(path,out)
#define S_ISREG(mode) ((mode) == N_FT_FILE)
#define S_ISDIR(mode) ((mode) == N_FT_DIR)
