/* NOSfs, the persistent volume behind the namespace (fs/nos.c, docs/nosfs.md §5–7, §9). */
#pragma once
#include <stdbool.h>

/* Find the volume on Nocturne's own disks, or make one where there is room, and attach the
   namespace to it (after the disk drivers, before init). false: there is none, the namespace
   lives in RAM only. */
bool nos_attach(void);
