/* The start of a driver image (SYS_IMAGE_MAP, common/abi.h): the image's own C library comes up in
   a process that already runs. The kernel only maps an image, so here it puts itself in order: its
   addresses, its thread-local block, its constructors. Linked only into an image, whose link names
   this function as its entry (user/image.map). */
#include <stddef.h>
#include <stdint.h>
#include "abi.h"

#define HIDDEN __attribute__((visibility("hidden")))

struct image_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct image_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct image_dyn {
    int64_t tag;
    uint64_t value;
};
struct image_rela {
    uint64_t offset, info;
    int64_t addend;
};
enum { DT_RELA = 7, DT_RELASZ = 8, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27 };
enum { PT_TLS = 7 };
enum { R_X86_64_NONE = 0, R_X86_64_RELATIVE = 8, R_X86_64_DTPMOD64 = 16, R_X86_64_DTPOFF64 = 17 };

/* The linker's. The image is linked at address 0, so the header's address is its base. */
extern const struct image_ehdr __ehdr_start HIDDEN;
extern const struct image_dyn _DYNAMIC[] HIDDEN;
/* What the image gives its loader: the driver's part names it. */
extern const char __image_exports[] HIDDEN;

void __libc_image_init(const struct n_start *start, const char *tls_image, const char *tls_image_end, size_t tls_memsz,
                       size_t tls_align);

/* NULL when the image was linked otherwise than this code knows: every symbol is local to it
   (user/image.map), so only these relocations exist. There is one thread-local module per copy of
   the libc, and __tls_get_addr (user/libc/sys.c) ignores its number. */
__attribute__((visibility("default"))) const void *__libc_image_start(const struct n_start *start) {
    uintptr_t base = (uintptr_t)&__ehdr_start;
    const struct image_rela *rela = NULL;
    size_t rela_bytes = 0, init_bytes = 0;
    void (*const *init)(void) = NULL;
    for (const struct image_dyn *d = _DYNAMIC; d->tag; d++) {
        if (d->tag == DT_RELA) rela = (const struct image_rela *)(base + d->value);
        else if (d->tag == DT_RELASZ) rela_bytes = d->value;
        else if (d->tag == DT_INIT_ARRAY) init = (void (*const *)(void))(base + d->value);
        else if (d->tag == DT_INIT_ARRAYSZ) init_bytes = d->value;
    }
    for (size_t i = 0; i < rela_bytes / sizeof *rela; i++) {
        uint64_t *at = (uint64_t *)(base + rela[i].offset);
        uint32_t type = (uint32_t)rela[i].info;
        if (rela[i].info >> 32) return NULL;
        if (type == R_X86_64_RELATIVE) *at = base + (uint64_t)rela[i].addend;
        else if (type == R_X86_64_DTPMOD64) *at = 1;
        else if (type == R_X86_64_DTPOFF64) *at = (uint64_t)rela[i].addend;
        else if (type != R_X86_64_NONE) return NULL;
    }
    const char *tls_image = NULL;
    size_t tls_file = 0, tls_memsz = 0, tls_align = 0;
    const struct image_phdr *ph = (const struct image_phdr *)(base + __ehdr_start.phoff);
    for (unsigned i = 0; i < __ehdr_start.phnum; i++)
        if (ph[i].type == PT_TLS) {
            tls_image = (const char *)(base + ph[i].vaddr);
            tls_file = ph[i].filesz;
            tls_memsz = ph[i].memsz;
            tls_align = ph[i].align;
        }
    __libc_image_init(start, tls_image, tls_image + tls_file, tls_memsz, tls_align);
    for (size_t i = 0; i < init_bytes / sizeof *init; i++) init[i]();
    return __image_exports;
}
