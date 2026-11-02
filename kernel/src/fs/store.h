/* The store: Nocturne's one volume, drive C: (common/store_format.h).

   Everything an app keeps lives here, under one namespace with one on-disk format: the system
   (C:\Nocturne), programs (C:\Programs) and people's files (C:\Users\<name>). A path names an
   entry completely: "C:\Users\a\b.txt". '/' is the same as '\', the drive may be left out
   ("\Users\a" is on C:), "." and ".." are resolved, and names compare without regard to case
   while keeping the case they were given. There is no working folder in the kernel and no
   length limit on a path.

   user: the call is an app's. Its buffers are the app's memory, PRIVATE entries are not there for
   it, and what it may reach is what its identity (struct n_identity) may: fs/store.c's place_allows,
   the one place that decides. N_ERR_DENIED otherwise.

     C:\Nocturne       everyone reads, the system's class writes
     C:\Programs       everyone reads, admin and above write
     C:\Users\<name>   the user who owns it and admin and above, both reading and writing
     C:\ and C:\Users  everyone lists them, admin and above change them
     anything else     admin and above

   Each name has the number of the user it belongs to (store_record's owner): a new name has its
   folder's. What C:\Users\<name> says decides for everything in it. */
#pragma once
#include "kernel.h"

struct blkdev;
struct handle;
struct n_entry;
struct volume;

/* Mounts C: from the disk the system started from, writing nothing to it (a hibernated system may
   still come back to it); panics when there is none. Started from a CD or DVD, C: is the volume the
   boot loader brought into memory: what changes on it lasts until the machine stops. */
void store_init(void);
/* From now on C: changes: its system folders are made ready and its writer starts. */
void store_start(void);
/* C:\Users\<name>, the folder of the person using the machine: the first in C:\Users, "User" made
   when there is none. Apps start in it. Its AppData\Temp is the user's temporary folder, emptied by
   every store_start. Both are "C:\" until then, or when they cannot be made. */
const char *store_user_folder(void);
const char *store_user_temp(void);
/* Everything changed so far is on the medium (power changes, hibernation). */
int store_sync(void);

/* Documents (docs/nocturne-native-abi.md §3), opened for apps (user) and the kernel. */
int store_document_open(const char *path, int access, bool user, struct handle **out);
bool store_document_is(struct handle *h);
int64_t store_document_read(struct handle *h, uint64_t offset, void *buf, size_t n, bool user);
int64_t store_document_write(struct handle *h, uint64_t offset, const void *buf, size_t n, bool user);
int64_t store_document_size(struct handle *h);
/* Its entry as n_entry_info gives it, with no name (an opening has none) and the size it reads. */
int store_document_info(struct handle *h, struct n_entry *out);
/* What tells the content a READ opening reads from every other content its document has had or
   will have: n kmalloc'd bytes, equal for the same content. -N_ERR_UNSUPPORTED for content without
   one: a small document's, in its record, and a raw document's. */
int store_document_identity(struct handle *h, void **out, size_t *n);
int store_document_set_size(struct handle *h, uint64_t size);
int store_document_claim(struct handle *h, bool wait);
int store_document_release(struct handle *h);
int store_document_commit(struct handle *h, int how);

/* A private region (common/settings_abi.h): a document outside the names, found by the number of its
   user (0: the machine's) and its own. access: N_DOCUMENT_READ (-N_ERR_NOT_FOUND while it has no
   content yet), N_DOCUMENT_EDIT, or N_DOCUMENT_REPLACE, whose new content is swapped in whole when
   saved (store_document_commit), as for any document. The caller has checked its right to it. */
int store_private_open(uint32_t region, uint32_t user, int access, struct handle **out);

/* The user a name belongs to from now on: admin and above (user: for an app). What is in a folder
   already keeps the number it has. */
int store_entry_set_owner(const char *path, uint32_t owner, bool user);

/* Whole documents. store_save replaces the content and returns once it is on the medium;
   store_append adds to the end, making the document when there is none, and does not wait for the
   medium. */
int store_save(const char *path, const void *data, size_t n, bool user);
int store_append(const char *path, const void *data, size_t n, bool user);

/* Entries. store_folder_list fills up to max entries from the start'th on, and returns how many:
   the first bytes of each struct n_entry (N_ENTRY_FIRST_BYTES up to its size), that far apart. */
int store_entry_info(const char *path, bool user, struct n_entry *out);
int64_t store_folder_list(const char *path, uint64_t start, void *out, size_t max, size_t bytes, bool user);
int store_folder_create(const char *path, bool user);
int store_entry_remove(const char *path, bool user);
/* replace: a document may take the name of another document, which goes in the same commit. */
int store_entry_move(const char *from, const char *to, bool replace, bool user);
/* Where the boot loader's module was read from: disk and the first sector and sectors of the
   partition with that GPT identity, on the disk C: is on. -N_ERR_UNSUPPORTED when C: is not on a
   disk, -N_ERR_NOT_FOUND without the partition. */
int store_boot_partition(const uint8_t unique[16], struct blkdev **disk, uint64_t *first, uint64_t *sectors);

/* A watch on C: (n_entry_watch, fs/watch.c): what is N_WATCH_NAME (the last name of path in its
   folder) or N_WATCH_FOLDER (everything directly in the folder path names). A new handle in the
   calling app's table: its number, or an error (-N_ERR_NOT_FOUND without the folder). */
int64_t store_entry_watch(const char *path, uint32_t what, bool user);

/* Raw documents: content the kernel reads and writes on the disk itself, past the store and its
   checks (the hibernation image). Their blocks keep their place until the document is removed.
   store_raw_place makes the document at path (PRIVATE) size bytes long, every block of it placed
   and committed, and tells where they are; -N_ERR_NO_SPACE when C: has no room for them,
   -N_ERR_UNSUPPORTED when C: is not on a disk (store_raw_possible). store_raw_find tells where an
   existing one's are. */
struct store_raw {
    struct blkdev *disk;
    struct store_raw_extent {
        uint64_t lba, sectors; /* 512-byte sectors of the disk */
    } *extents;
    size_t n;
    uint64_t size;
};
bool store_raw_possible(void);
int store_raw_place(const char *path, uint64_t size, struct store_raw *out);
int store_raw_find(const char *path, struct store_raw *out);
void store_raw_free(struct store_raw *r);
