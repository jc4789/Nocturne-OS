/* Unpack a USTAR archive (the Limine module) into the root ramfs. The files' bytes stay in the
   archive and are read there (ramfs_borrow); a page of it goes back to the allocator once no file
   reads any of its bytes any more (they were changed, removed or replaced). */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"

#define BLOCK 512 /* USTAR's unit: every file's bytes start on one */

static uint64_t base, npages; /* physical, page aligned */
static uint64_t *given_back;   /* 1 bit per archive page: handed to the allocator */
static uint8_t *lent;          /* per archive page, 1 bit per block a file still reads there */

static bool page_given(uint64_t i) { return given_back[i / 64] & (1ULL << (i % 64)); }

static void give_back(uint64_t i) {
    if (lent[i] || page_given(i)) return;
    given_back[i / 64] |= 1ULL << (i % 64);
    pmm_release_range(base + i * PAGE_SIZE, PAGE_SIZE);
}

/* blocks [first, end) of the archive; set or clear */
static void mark(uint64_t first, uint64_t end, bool on) {
    for (uint64_t b = first; b < end; b++) {
        uint8_t bit = (uint8_t)(1u << (b % (PAGE_SIZE / BLOCK)));
        uint64_t i = b / (PAGE_SIZE / BLOCK);
        lent[i] = on ? lent[i] | bit : lent[i] & ~bit;
    }
}

void initrd_unborrow(const uint8_t *data, uint64_t size) {
    if (!given_back || !size) return;
    uint64_t at = hhdm_virt_to_phys(data);
    if (at < base || at >= base + npages * PAGE_SIZE) return;
    /* A file's bytes start on a block and its last block's padding is its own: the range covers
       whole blocks, rounded out at the end only. */
    uint64_t first = ALIGN_UP(at - base, BLOCK) / BLOCK;
    uint64_t end = MIN(ALIGN_UP(at - base + size, BLOCK), npages * PAGE_SIZE) / BLOCK;
    if (first >= end) return;
    mark(first, end, false);
    uint64_t per = PAGE_SIZE / BLOCK;
    for (uint64_t i = first / per; i <= (end - 1) / per; i++) give_back(i);
}

bool initrd_range(uint64_t *phys, uint64_t *pages) {
    *phys = base;
    *pages = npages;
    return given_back != NULL;
}

bool initrd_page_pristine(uint64_t pfn) {
    uint64_t i = pfn - base / PAGE_SIZE;
    return given_back && pfn >= base / PAGE_SIZE && i < npages && !page_given(i);
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
    lent = kzalloc(npages);
    if (!given_back || !lent) { /* no room to track blocks: every file gets a copy */
        kfree(given_back);
        kfree(lent);
        given_back = NULL;
        lent = NULL;
    }
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
                    uint64_t at = hhdm_virt_to_phys(p + 512) - base;
                    if (n && given_back && ramfs_borrow(n, p + 512, fsize))
                        mark(at / BLOCK, MIN(ALIGN_UP(at + fsize, BLOCK), npages * PAGE_SIZE) / BLOCK, true);
                    else if (n) vfs_write_whole(path, p + 512, fsize);
                    files++;
                }
            }
            struct vnode *n = vfs_lookup(path);
            if (n) n->mode = (uint32_t)octal(h + 100, 8);
        }
        uint64_t padded = ALIGN_UP(fsize, 512);
        p += 512 + (padded > avail ? avail : padded); /* a short final block stays inside the archive */
    }
    /* headers, folders and empty files are read once, here */
    if (given_back)
        for (uint64_t i = 0; i < npages; i++) give_back(i);
    kprintf("initrd: unpacked %d files, %d directories\n", files, dirs);
}
