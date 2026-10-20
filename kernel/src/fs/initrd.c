/* Unpack a USTAR archive (the Limine module) into the root ramfs. The files' bytes stay in the
   archive and are read there (ramfs_borrow); a page of it goes back to the allocator once no file
   reads it any more (one was changed, removed or replaced). */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"

static uint64_t base, npages; /* physical, page aligned */
static uint64_t *given_back;   /* 1 bit per archive page: handed to the allocator */

void initrd_unborrow(const uint8_t *data, uint64_t size) {
    if (!given_back || !size) return;
    uint64_t start = ALIGN_UP(hhdm_virt_to_phys(data), PAGE_SIZE);
    uint64_t end = ALIGN_DOWN(hhdm_virt_to_phys(data) + size, PAGE_SIZE);
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        uint64_t i = (p - base) / PAGE_SIZE;
        if (p < base || i >= npages || given_back[i / 64] & (1ULL << (i % 64))) continue;
        given_back[i / 64] |= 1ULL << (i % 64);
        pmm_release_range(p, PAGE_SIZE);
    }
}

bool initrd_range(uint64_t *phys, uint64_t *pages) {
    *phys = base;
    *pages = npages;
    return given_back != NULL;
}

bool initrd_page_pristine(uint64_t pfn) {
    uint64_t i = pfn - base / PAGE_SIZE;
    return given_back && pfn >= base / PAGE_SIZE && i < npages && !(given_back[i / 64] & (1ULL << (i % 64)));
}

static uint64_t octal(const char *s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0');
    return v;
}

void initrd_load(void *data, uint64_t size) {
    uint8_t *p = data, *end = p + size;
    int files = 0, dirs = 0;
    /* Limine loads modules page aligned; the archive's pages stay the allocator's "used" ones */
    base = ALIGN_DOWN(hhdm_virt_to_phys(data), PAGE_SIZE);
    npages = (ALIGN_UP(hhdm_virt_to_phys(data) + size, PAGE_SIZE) - base) / PAGE_SIZE;
    given_back = kzalloc((npages + 63) / 64 * 8);
    while (p + 512 <= end) {
        char *h = (char *)p;
        if (!h[0]) break;
        char path[PATH_MAX_LEN];
        char name[101], prefix[156];
        memcpy(name, h, 100);
        name[100] = 0;
        memcpy(prefix, h + 345, 155);
        prefix[155] = 0;
        /* 155 + 100 + 2 bytes always fit PATH_MAX_LEN, so the join cannot clip */
        if (prefix[0]) ksnprintf(path, sizeof path, "/%s/%s", prefix, name);
        else ksnprintf(path, sizeof path, "/%s", name);
        size_t l = strlen(path);
        while (l > 1 && path[l - 1] == '/') path[--l] = 0;
        uint64_t fsize = octal(h + 124, 12);
        char type = h[156];
        /* The size field is untrusted: an entry whose data runs past the end of
           the archive is truncated, so stop instead of reading beyond it. */
        uint64_t avail = (uint64_t)(end - (p + 512));
        if (fsize > avail) {
            kprintf("initrd: truncated entry %s (%lu bytes, %lu available)\n", path, fsize, avail);
            break;
        }
        if (type == '5') {
            vfs_mkdir(path);
            dirs++;
        } else if (type == '0' || type == 0) {
            struct vnode *v = vfs_lookup(path);
            if (!v) {
                struct file *f;
                if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) {
                    vfs_close(f);
                    struct vnode *n = vfs_lookup(path);
                    if (n && given_back) ramfs_borrow(n, p + 512, fsize);
                    else if (n) vfs_write_whole(path, p + 512, fsize); /* no room to track pages */
                    files++;
                }
            }
            struct vnode *n = vfs_lookup(path);
            if (n) n->mode = (uint32_t)octal(h + 100, 8);
        }
        uint64_t padded = ALIGN_UP(fsize, 512);
        p += 512 + (padded > avail ? avail : padded); /* a short final block stays inside the archive */
    }
    kprintf("initrd: unpacked %d files, %d directories\n", files, dirs);
}
