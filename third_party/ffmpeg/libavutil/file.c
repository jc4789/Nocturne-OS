/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "error.h"
#include "file.h"
#include "internal.h"
#include "log.h"
#include "mem.h"
#include <nocturne.h>

typedef struct FileLogContext {
    const AVClass *class;
    int   log_offset;
    void *log_ctx;
} FileLogContext;

static const AVClass file_log_ctx_class = {
    .class_name                = "FILE",
    .item_name                 = av_default_item_name,
    .option                    = NULL,
    .version                   = LIBAVUTIL_VERSION_INT,
    .log_level_offset_offset   = offsetof(FileLogContext, log_offset),
    .parent_log_context_offset = offsetof(FileLogContext, log_ctx),
};

int av_file_map(const char *filename, uint8_t **bufptr, size_t *size,
                int log_offset, void *log_ctx)
{
    FileLogContext file_log_ctx = { &file_log_ctx_class, log_offset, log_ctx };
    void *data = NULL;
    size_t length = 0;
    /* The document arrives whole in fresh private pages the caller may write,
     * as the old copy-on-write mapping allowed. A failure is -N_ERR_*, and
     * the errno names are those same positive codes. */
    n_result r = n_document_load(filename, &data, &length);

    *bufptr = NULL;
    *size = 0;

    if (r < 0) {
        int err = AVERROR(-(int)r);
        av_log(&file_log_ctx, AV_LOG_ERROR, "Cannot read file '%s': %s\n", filename, n_result_text(r));
        return err;
    }

    *bufptr = data;
    *size = length;
    return 0;
}

void av_file_unmap(uint8_t *bufptr, size_t size)
{
    if (!size || !bufptr)
        return;
    n_memory_release(bufptr, size);
}
