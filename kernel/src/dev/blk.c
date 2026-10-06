#include "kernel.h"
#include "dev/blk.h"

#define MAX_BLK 16
static struct blkdev *devs[MAX_BLK];
static int ndevs;

void blk_register(struct blkdev *d) {
    if (ndevs == MAX_BLK) return;
    d->index = ndevs;
    devs[ndevs++] = d;
    kprintf("blk: %s: %s, %lu MiB\n", d->name, d->model, d->sectors / 2048);
}

int blk_count(void) { return ndevs; }

struct blkdev *blk_get(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }
