/* The services of Mesa's util/os_file.c that the code a program or a driver image links uses:
   the GL state tracker, driconf and llvmpipe. Apart from the GL frontend (nocturne_gl.c), which a
   driver image does not link. */
#include "util/os_file.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

/* A whole file in a buffer the caller frees, followed by a NUL (driconf reads the program file
   with it to hash it). */
char *os_read_file(const char *filename, size_t *size) {
    void *data = NULL;
    size_t length = 0;
    if (n_document_load(filename, &data, &length) < 0) return NULL;
    char *copy = length < SIZE_MAX ? malloc(length + 1) : NULL;
    if (copy) {
        if (length) memcpy(copy, data, length);
        copy[length] = 0;
        if (size) *size = length;
    }
    if (data) n_memory_release(data, length);
    return copy;
}

/* A new folder, 0, or -1 when it cannot be made (it may already be there); Nocturne has no use for
   the permissions in mode. llvmpipe's GALLIVM_DEBUG=symbols writes each shader's NIR into one. */
int os_mkdir(const char *pathname, int mode) {
    (void)mode;
    return n_folder_create(pathname) < 0 ? -1 : 0;
}

/* Shader capture (MESA_SHADER_CAPTURE_PATH) writes each program to a new
   file and never replaces an existing one. Plain C stdio: a name that
   already opens is taken. */
FILE *os_file_create_unique(const char *filename, int filemode) {
    (void)filemode;
    FILE *existing = fopen(filename, "rb");
    if (existing) {
        fclose(existing);
        return NULL;
    }
    return fopen(filename, "w");
}
