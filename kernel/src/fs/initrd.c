/* Unpack a USTAR archive (the Limine module) into the root ramfs. */
#include "fs/vfs.h"

static uint64_t octal(const char *s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0');
    return v;
}

void initrd_load(void *data, uint64_t size) {
    uint8_t *p = data, *end = p + size;
    int files = 0, dirs = 0;
    while (p + 512 <= end) {
        char *h = (char *)p;
        if (!h[0]) break;
        char path[PATH_MAX_LEN];
        char name[101], prefix[156];
        memcpy(name, h, 100);
        name[100] = 0;
        memcpy(prefix, h + 345, 155);
        prefix[155] = 0;
        if (prefix[0]) ksnprintf(path, sizeof path, "/%s/%s", prefix, name);
        else ksnprintf(path, sizeof path, "/%s", name);
        size_t l = strlen(path);
        while (l > 1 && path[l - 1] == '/') path[--l] = 0;
        uint64_t fsize = octal(h + 124, 12);
        char type = h[156];
        if (type == '5') {
            vfs_mkdir(path);
            dirs++;
        } else if (type == '0' || type == 0) {
            struct vnode *v = vfs_lookup(path);
            if (!v) {
                struct file *f;
                if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) {
                    vfs_write(f, p + 512, fsize);
                    vfs_close(f);
                    files++;
                }
            }
            struct vnode *n = vfs_lookup(path);
            if (n) n->mode = (uint32_t)octal(h + 100, 8);
        }
        p += 512 + ALIGN_UP(fsize, 512);
    }
    kprintf("initrd: unpacked %d files, %d directories\n", files, dirs);
}
