/* stb_truetype (public domain, third_party/img), used by font.c. Compiled without warnings. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* FreeType has no filesystem port in Nocturne. Only memory faces are opened;
 * use our existing allocator, with signed-size checks at its native boundary. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include FT_ERRORS_H
static void *nft_alloc(FT_Memory memory, long size) {
    (void)memory; return size > 0 ? malloc((size_t)size) : NULL;
}
static void nft_free(FT_Memory memory, void *block) {
    (void)memory; free(block);
}
static void *nft_realloc(FT_Memory memory, long old_size, long size, void *block) {
    (void)memory; (void)old_size;
    if (size <= 0) { free(block); return NULL; }
    return realloc(block, (size_t)size);
}
FT_Memory FT_New_Memory(void) {
    FT_Memory memory = malloc(sizeof *memory);
    if (memory) {
        memory->user = NULL; memory->alloc = nft_alloc;
        memory->free = nft_free; memory->realloc = nft_realloc;
    }
    return memory;
}
void FT_Done_Memory(FT_Memory memory) { free(memory); }
FT_Error FT_Stream_Open(FT_Stream stream, const char *path) {
    (void)stream; (void)path; return FT_Err_Cannot_Open_Resource;
}
