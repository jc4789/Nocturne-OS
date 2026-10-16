/* fstest DIR: exercise a filesystem under DIR (a complete path; created, then removed again except
   for "keep/"). Documents of many sizes (around FAT cluster edges), replace, append, resize,
   positional writes, move, long names, nested folders, folder listing, remove. The host checks
   the FAT afterwards with scripts/fatcheck.py. Prints "FAIL ..." per failed check and
   "fstest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

static int failed;
static char dir[200];

static void check(const char *name, int ok) {
    if (!ok) {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static char *path(const char *name) {
    static char buf[2][400];
    static int k;
    k ^= 1;
    snprintf(buf[k], sizeof buf[k], "%s/%s", dir, name);
    return buf[k];
}

static unsigned char pat(size_t i, int seed) { return (unsigned char)(i * 31 + seed + (i >> 9)); }

/* a new document; a replacement swapped in whole on close; an existing document edited in place after truncating */
enum { CREATE, REPLACE, RESIZE };

static int write_file(const char *p, size_t n, int seed, int how) {
    n_handle h = n_document_open(p, how == CREATE ? N_DOCUMENT_CREATE : how == REPLACE ? N_DOCUMENT_REPLACE : N_DOCUMENT_EDIT);
    if (h == -N_ERR_EXISTS && how == CREATE) { /* left over from an earlier run */
        n_entry_remove(p);
        h = n_document_open(p, N_DOCUMENT_CREATE);
    }
    if (h < 0) return 0;
    if (how == RESIZE && n_document_set_size(h, 0) < 0) {
        n_handle_close(h);
        return 0;
    }
    unsigned char buf[3000];
    size_t done = 0;
    while (done < n) {
        size_t c = n - done < sizeof buf ? n - done : sizeof buf;
        c = c > 1 + done % 2999 ? 1 + done % 2999 : c; /* odd write sizes */
        for (size_t k = 0; k < c; k++) buf[k] = pat(done + k, seed);
        if (n_document_write_at(h, done, buf, c) != (n_result)c) {
            n_handle_close(h);
            return 0;
        }
        done += c;
    }
    return n_handle_close(h) == 0;
}

static int check_file(const char *p, size_t n, int seed) {
    struct n_entry st;
    if (n_entry_info(p, &st) < 0 || st.kind != N_KIND_DOCUMENT || st.size != n) return 0;
    n_handle h = n_document_open(p, N_DOCUMENT_READ);
    if (h < 0) return 0;
    unsigned char buf[4096];
    size_t done = 0;
    n_result r;
    while ((r = n_document_read_at(h, done, buf, sizeof buf)) > 0) {
        for (n_result k = 0; k < r; k++)
            if (buf[k] != pat(done + (size_t)k, seed)) {
                n_handle_close(h);
                return 0;
            }
        done += (size_t)r;
    }
    n_handle_close(h);
    return r == 0 && done == n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fstest DIR\n");
        return 2;
    }
    snprintf(dir, sizeof dir, "%s", argv[1]);
    n_folder_create(dir);
    char name[64];

    /* sizes around 512-byte sectors and 4 KiB clusters, and a few big ones */
    static const size_t sizes[] = {0, 1, 511, 512, 513, 4095, 4096, 4097, 8192, 12289, 65536, 100000, 1048576 + 7};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(name, sizeof name, "size-%zu.bin", sizes[i]);
        check(name, write_file(path(name), sizes[i], (int)i, CREATE));
    }
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(name, sizeof name, "size-%zu.bin", sizes[i]);
        if (!check_file(path(name), sizes[i], (int)i)) {
            strcat(name, " readback");
            check(name, 0);
        }
    }
    check("create-existing-fails", n_document_open(path("size-1.bin"), N_DOCUMENT_CREATE) == -N_ERR_EXISTS);

    /* replace a big document with a small one and back: clusters must be freed and reused */
    check("shrink", write_file(path("size-100000.bin"), 300, 7, REPLACE) && check_file(path("size-100000.bin"), 300, 7));
    check("grow", write_file(path("size-100000.bin"), 200000, 8, REPLACE) && check_file(path("size-100000.bin"), 200000, 8));
    /* the same in place: truncate to nothing, then write */
    check("resize-shrink", write_file(path("size-65536.bin"), 700, 5, RESIZE) && check_file(path("size-65536.bin"), 700, 5));
    check("resize-grow", write_file(path("size-65536.bin"), 90000, 6, RESIZE) && check_file(path("size-65536.bin"), 90000, 6));

    /* a replacement is invisible until its explicit close */
    {
        n_handle h = n_document_open(path("size-513.bin"), N_DOCUMENT_REPLACE);
        check("replace-open", h >= 0 && n_document_write_at(h, 0, "temporary", 9) == 9);
        check("replace-invisible", check_file(path("size-513.bin"), 513, 4));
        check("replace-commit", h >= 0 && n_handle_close(h) == 0);
        struct n_entry st;
        check("replace-visible", n_entry_info(path("size-513.bin"), &st) == 0 && st.size == 9);
        check("restore", write_file(path("size-513.bin"), 513, 4, REPLACE) && check_file(path("size-513.bin"), 513, 4));
    }

    /* append, by path and through a handle at the end */
    {
        check("append-save", n_document_save(path("append.txt"), "hello ", 6) == 0);
        check("append-path", n_document_append(path("append.txt"), "world", 5) == 0);
        char buf[32] = {0};
        n_handle h = n_document_open(path("append.txt"), N_DOCUMENT_READ);
        n_result got = h >= 0 ? n_document_read_at(h, 0, buf, sizeof buf - 1) : -1;
        if (h >= 0) n_handle_close(h);
        check("append", got == 11 && !strcmp(buf, "hello world"));
        h = n_document_open(path("append.txt"), N_DOCUMENT_EDIT);
        n_result size = h >= 0 ? n_document_size(h) : -1;
        check("append-handle", size == 11 && n_document_write_at(h, (uint64_t)size, "!", 1) == 1 && n_document_size(h) == 12);
        if (h >= 0) n_handle_close(h);
        void *data = NULL;
        size_t length = 0;
        check("append-load", n_document_load(path("append.txt"), &data, &length) == 0 && length == 12 &&
                                 !memcmp(data, "hello world!", 12));
        if (data) n_memory_release(data, length);
    }

    /* positional overwrite in the middle, across a cluster boundary */
    {
        n_handle h = n_document_open(path("size-12289.bin"), N_DOCUMENT_EDIT);
        char buf[16] = {0};
        n_result wrote = h >= 0 ? n_document_write_at(h, 4090, "ABCDEFGHIJKL", 12) : -1;
        n_result got = h >= 0 ? n_document_read_at(h, 4088, buf, 16) : -1;
        if (h >= 0) n_handle_close(h);
        check("seek-write", wrote == 12 && got == 16 && !memcmp(buf + 2, "ABCDEFGHIJKL", 12) &&
                                (unsigned char)buf[0] == pat(4088, 9) && (unsigned char)buf[14] == pat(4102, 9));
    }

    /* long and odd names, nested folders, move across folders */
    const char *longname = "a rather long file name with spaces, Mixed Case and dots.v1.2.txt";
    check("long-name", write_file(path(longname), 1234, 3, CREATE) && check_file(path(longname), 1234, 3));
    check("mkdir-nested", n_folder_create(path("sub")) == 0 && n_folder_create(path("sub/deeper")) == 0);
    check("move-never-replaces", n_entry_move(path(longname), path("append.txt")) == -N_ERR_EXISTS &&
                                     check_file(path(longname), 1234, 3));
    check("rename", n_entry_move(path(longname), path("sub/deeper/moved.txt")) == 0 &&
                        check_file(path("sub/deeper/moved.txt"), 1234, 3));
    struct n_entry st;
    check("rename-removes-old", n_entry_info(path(longname), &st) < 0);
    check("folder-entry", n_entry_info(path("sub/deeper"), &st) == 0 && st.kind == N_KIND_FOLDER && !strcmp(st.name, "deeper"));

    /* many files in one folder (the folder grows past one cluster) */
    n_folder_create(path("many"));
    int okmany = 1;
    for (int i = 0; i < 150; i++) {
        snprintf(name, sizeof name, "many/file-number-%03d.txt", i);
        okmany &= write_file(path(name), (size_t)i * 10, i, CREATE);
    }
    check("many-files", okmany);
    {
        struct n_entry *all = NULL;
        n_result total = n_folder_entries(path("many"), &all);
        int n = 0;
        for (n_result i = 0; i < total; i++)
            if (strncmp(all[i].name, "file-number-", 12) == 0) n++;
        free(all);
        check("folder-entries-count", total == 150 && n == 150);
        /* the same listing in pages of 16: every entry exactly once */
        static struct n_entry page[16];
        static char seen[150];
        memset(seen, 0, sizeof seen);
        int counted = 0, pages_ok = 1;
        for (uint64_t start = 0;; start += 16) {
            n_result got = n_folder_list(path("many"), page, 16, start);
            if (got < 0) {
                pages_ok = 0;
                break;
            }
            for (n_result k = 0; k < got; k++) {
                int number = -1;
                if (sscanf(page[k].name, "file-number-%d.txt", &number) != 1 || number < 0 || number >= 150 || seen[number])
                    pages_ok = 0;
                else
                    seen[number] = 1;
                counted++;
            }
            if (got < 16) break;
        }
        check("folder-list-pages", pages_ok && counted == 150);
    }
    for (int i = 0; i < 150; i += 7) {
        snprintf(name, sizeof name, "many/file-number-%03d.txt", i);
        okmany &= check_file(path(name), (size_t)i * 10, i);
    }
    check("many-files-readback", okmany);

    /* clean up everything */
    int okrm = 1;
    for (int i = 0; i < 150; i++) {
        snprintf(name, sizeof name, "many/file-number-%03d.txt", i);
        okrm &= n_entry_remove(path(name)) == 0;
    }
    okrm &= n_entry_remove(path("many")) == 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(name, sizeof name, "size-%zu.bin", sizes[i]);
        okrm &= n_entry_remove(path(name)) == 0;
    }
    okrm &= n_entry_remove(path("append.txt")) == 0 && n_entry_remove(path("sub/deeper/moved.txt")) == 0;
    okrm &= n_entry_remove(path("sub/deeper")) == 0 && n_entry_remove(path("sub")) == 0;
    check("remove-all", okrm);
    n_folder_create(path("full"));
    write_file(path("full/f"), 1, 1, CREATE);
    check("remove-nonempty-fails", n_entry_remove(path("full")) == -N_ERR_NOT_EMPTY);
    check("remove-after-emptying", n_entry_remove(path("full/f")) == 0 && n_entry_remove(path("full")) == 0);
    check("remove-dir", n_entry_remove(dir) == 0);
    check("gone", n_entry_info(dir, &st) < 0);

    printf("fstest: %d failed\n", failed);
    return failed != 0;
}
