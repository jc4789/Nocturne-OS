/* fstest DIR: exercise a filesystem under DIR (created, then removed again except for "keep/").
   Files of many sizes (around FAT cluster edges), overwrite, append, truncate, seek, rename,
   long names, nested directories, readdir, unlink. The host checks the FAT afterwards with
   scripts/fatcheck.py. Prints "PASS/FAIL ..." per check and "fstest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
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

static int write_file(const char *p, size_t n, int seed) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    unsigned char buf[3000];
    size_t done = 0;
    while (done < n) {
        size_t c = n - done < sizeof buf ? n - done : sizeof buf;
        c = c > 1 + done % 2999 ? 1 + done % 2999 : c; /* odd write sizes */
        for (size_t k = 0; k < c; k++) buf[k] = pat(done + k, seed);
        if (write(fd, buf, c) != (long)c) {
            close(fd);
            return 0;
        }
        done += c;
    }
    return close(fd) == 0;
}

static int check_file(const char *p, size_t n, int seed) {
    struct n_stat st;
    if (stat(p, &st) < 0 || st.size != n) return 0;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char buf[4096];
    size_t done = 0;
    long r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        for (long k = 0; k < r; k++)
            if (buf[k] != pat(done + k, seed)) {
                close(fd);
                return 0;
            }
        done += r;
    }
    close(fd);
    return done == n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fstest DIR\n");
        return 2;
    }
    snprintf(dir, sizeof dir, "%s", argv[1]);
    mkdir(dir);
    char name[64];

    /* sizes around 512-byte sectors and 4 KiB clusters, and a few big ones */
    static const size_t sizes[] = {0, 1, 511, 512, 513, 4095, 4096, 4097, 8192, 12289, 65536, 100000, 1048576 + 7};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(name, sizeof name, "size-%zu.bin", sizes[i]);
        check(name, write_file(path(name), sizes[i], (int)i));
    }
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(name, sizeof name, "size-%zu.bin", sizes[i]);
        if (!check_file(path(name), sizes[i], (int)i)) {
            strcat(name, " readback");
            check(name, 0);
        }
    }

    /* overwrite a big file with a small one and back: clusters must be freed and reused */
    check("shrink", write_file(path("size-100000.bin"), 300, 7) && check_file(path("size-100000.bin"), 300, 7));
    check("grow", write_file(path("size-100000.bin"), 200000, 8) && check_file(path("size-100000.bin"), 200000, 8));

    /* append */
    {
        int fd = open(path("append.txt"), O_WRONLY | O_CREAT | O_TRUNC);
        write(fd, "hello ", 6);
        close(fd);
        fd = open(path("append.txt"), O_WRONLY | O_APPEND);
        write(fd, "world", 5);
        close(fd);
        char buf[32] = {0};
        fd = open(path("append.txt"), O_RDONLY);
        read(fd, buf, sizeof buf - 1);
        close(fd);
        check("append", !strcmp(buf, "hello world"));
    }

    /* seek and overwrite in the middle, across a cluster boundary */
    {
        int fd = open(path("size-12289.bin"), O_RDWR);
        lseek(fd, 4090, 0);
        write(fd, "ABCDEFGHIJKL", 12);
        lseek(fd, 4088, 0);
        char buf[16] = {0};
        read(fd, buf, 16);
        close(fd);
        check("seek-write", !memcmp(buf + 2, "ABCDEFGHIJKL", 12) && (unsigned char)buf[0] == pat(4088, 9) &&
                                (unsigned char)buf[14] == pat(4102, 9));
    }

    /* long and odd names, nested directories, rename across directories */
    const char *longname = "a rather long file name with spaces, Mixed Case and dots.v1.2.txt";
    check("long-name", write_file(path(longname), 1234, 3) && check_file(path(longname), 1234, 3));
    check("mkdir-nested", mkdir(path("sub")) == 0 && mkdir(path("sub/deeper")) == 0);
    check("rename", rename(path(longname), path("sub/deeper/moved.txt")) == 0 &&
                        check_file(path("sub/deeper/moved.txt"), 1234, 3));
    struct n_stat st;
    check("rename-removes-old", stat(path(longname), &st) < 0);

    /* many files in one directory (directory grows past one cluster) */
    mkdir(path("many"));
    int okmany = 1;
    for (int i = 0; i < 150; i++) {
        snprintf(name, sizeof name, "many/file-number-%03d.txt", i);
        okmany &= write_file(path(name), (size_t)i * 10, i);
    }
    check("many-files", okmany);
    {
        int fd = open(path("many"), O_RDONLY);
        struct n_dirent d;
        int n = 0;
        for (int i = 0; readdir(fd, i, &d) > 0; i++)
            if (strncmp(d.name, "file-number-", 12) == 0) n++;
        close(fd);
        check("readdir-count", n == 150);
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
        okrm &= unlink(path(name)) == 0;
    }
    okrm &= rmdir(path("many")) == 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        snprintf(name, sizeof name, "size-%zu.bin", sizes[i]);
        okrm &= unlink(path(name)) == 0;
    }
    okrm &= unlink(path("append.txt")) == 0 && unlink(path("sub/deeper/moved.txt")) == 0;
    okrm &= rmdir(path("sub/deeper")) == 0 && rmdir(path("sub")) == 0;
    check("unlink-all", okrm);
    mkdir(path("full"));
    write_file(path("full/f"), 1, 1);
    check("rmdir-nonempty-fails", rmdir(path("full")) < 0);
    check("rmdir-after-emptying", unlink(path("full/f")) == 0 && rmdir(path("full")) == 0);
    check("rmdir", rmdir(dir) == 0);
    check("gone", stat(dir, &st) < 0);

    printf("fstest: %d failed\n", failed);
    return failed != 0;
}
