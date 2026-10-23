#pragma once
/* Hibernation: Nocturne's own suspend to disk, on every machine whose C: is on a disk (no firmware
   S4).

   The power thread (dev/pm.c) takes the machine down as for S3, but the "sleep" is a snapshot of
   RAM: every page in use, less the pages of zeros (only listed), is compressed into memory set
   aside for it. The machine then comes back as from S3, the image goes to C:\Nocturne\Hibernate
   and the power goes off. That raw document (fs/store.h) is placed at its full size and committed
   before the snapshot, and filled by raw writes to its blocks afterwards: on disk nothing but the
   image changes after the snapshot.

   On the next boot, once C: is mounted and before anything is written to it, hibernate_restore
   reads the image back. Pages that the
   booting kernel does not use are written in place; the others wait in free pages, and the last
   step copies them over the booting kernel from a page below 1 MiB (arch/wake.c) and enters the
   image through its wake page, where wake_sleep returns true and the power thread resumes the
   devices as after S3. The image is marked used before that, so a failed restore boots normally
   the next time. "noresume" on the command line skips it (and drops the image).

   The image holds all of RAM, unencrypted: whatever an app had in memory is on the disk until the
   image is restored or dropped. */
#include <stdbool.h>

const char *hibernate_blocker(void); /* NULL when the machine could hibernate (devices aside) */
/* Before the snapshot, with everything running: the image at its size, the memory for it.
   False with *why set when there is not enough of either. */
bool hibernate_prepare(const char **why);
/* wake_sleep's enter(): the snapshot. Interrupts off, APs halted, devices stopped. */
void hibernate_snapshot(void);
/* After the snapshot, the devices back, every disk held by the caller (blk_hold): the image to disk.
   NULL when it is there and valid, else why not. */
const char *hibernate_write(void);
/* Back from a restore (restored) or after a hibernation that failed: the memory and the image go. */
void hibernate_cleanup(bool restored);
/* Boot, after store_init and before store_start: returns only when there is no image to restore or
   it cannot be restored (it is then removed). */
void hibernate_restore(void);
