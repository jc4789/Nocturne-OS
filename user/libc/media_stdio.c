/* single-owner/BSP の４stdio呼出だけをnative IOへ接続。
 * FF原文/config/devfs/fallbackは不変。通常stdio/globalmallocは変更しない。 */
#include "media_stdio_private.h"
#include "media_alloc_private.h"
#include <stdint.h>
#include <errno.h>
#if !defined(NMEDIA_STDIO_BACKEND_READ) || !defined(NMEDIA_STDIO_BACKEND_CLOSE)
#include <unistd.h>
#endif
#ifndef NMEDIA_STDIO_BACKEND_READ
#define NMEDIA_STDIO_BACKEND_READ read
#endif
#ifndef NMEDIA_STDIO_BACKEND_CLOSE
#define NMEDIA_STDIO_BACKEND_CLOSE close
#endif

struct media_stream { struct media_stream *next; int fd; };
static struct media_stream *media_streams;
static struct media_stream **stream_link(FILE *token) {
    struct media_stream **link = &media_streams;
    while (*link) {
        if ((FILE *)(void *)*link == token) return link;
        link = &(*link)->next;
    }
    return NULL; /* foreign/dangling tokenを一度もdereferenceしない */
}
static FILE *failed_open(int fd, int error) {
    NMEDIA_STDIO_BACKEND_CLOSE(fd);
    errno = error; /* close失敗で元の拒否理由を失わない */
    return NULL;
}
FILE *nmedia_ff_fdopen(int fd, const char *mode) {
    if (fd < 0) { errno = EBADF; return NULL; }
    for (struct media_stream *s = media_streams; s; s = s->next)
        if (s->fd == fd) { errno = EBUSY; return NULL; }
    if (!mode || mode[0] != 'r' || (mode[1] && !(mode[1] == 'b' && !mode[2])))
        return failed_open(fd, EINVAL);
    struct media_stream *s = nmedia_ff_malloc(sizeof *s);
    if (!s) return failed_open(fd, ENOMEM);
    s->fd = fd; s->next = media_streams; media_streams = s;
    return (FILE *)(void *)s;
}
int nmedia_ff_setvbuf(FILE *token, char *buf, int mode, size_t size) {
    (void)size;
    if (!stream_link(token) || buf || mode != _IONBF) { errno = EINVAL; return -1; }
    return 0; /* native直接readなのでstdio bufferは不要・追加heapなし */
}
size_t nmedia_ff_fread(void *buf, size_t size, size_t count, FILE *token) {
    struct media_stream **link = stream_link(token);
    if (!link) { errno = EINVAL; return 0; }
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) { errno = EINVAL; return 0; }
    size_t total = size * count, got = 0;
    if (!buf || total > UINTPTR_MAX - (uintptr_t)buf) { errno = EINVAL; return 0; }
    while (got < total) {
        size_t request = total - got;
        if (request > 4096) request = 4096;
        int64_t n = NMEDIA_STDIO_BACKEND_READ((*link)->fd, (unsigned char *)buf + got, request);
        if (n <= 0) break; /* EOF/部分read/negativeはnative fread同様short count */
        if ((uint64_t)n > request) { errno = EIO; break; }
        got += (size_t)n;
    }
    return got / size;
}
int nmedia_ff_fclose(FILE *token) {
    struct media_stream **link = stream_link(token);
    if (!link) { errno = EINVAL; return EOF; }
    struct media_stream *s = *link;
    *link = s->next;
    int result = NMEDIA_STDIO_BACKEND_CLOSE(s->fd), saved_errno = errno;
    nmedia_ff_free(s);
    if (result < 0) errno = saved_errno;
    return result < 0 ? EOF : 0;
}
