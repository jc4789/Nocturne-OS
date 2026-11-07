/* dmadisktest write|check DIR：block の層と記憶の装置の DMA を、文書の中身で確かめる。
   write は大きさの違う文書（block の端、blk.c の緩衝 128 KiB の端、MiB の単位）を、大きな塊と頁の中の半端な位置の
   塊で書く。check は全部を読み、型と合うかを見る。write の後に機械を切って起こし直してから check を走らせると、
   読出しは cache に無く、装置の DMA を通る。"dmadisktest: N failed" を出す。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

static int failed;
static char dir[200];

static const size_t sizes[] = {1,           511,         512,         4095,           4096,           4097,
                               65535,       131072 - 1,  131072,      131072 + 7,     (1u << 20) + 3, 8u << 20,
                               (24u << 20) + 4096 + 17};

static unsigned char pat(size_t i, unsigned seed) { return (unsigned char)(i * 131 + seed * 7 + (i >> 12)); }

static char *path(unsigned k) {
    static char buf[400];
    char leaf[32];
    snprintf(leaf, sizeof leaf, "dma%02u.bin", k);
    if (n_path_join(dir, leaf, buf, sizeof buf) < 0) buf[0] = 0;
    return buf;
}

/* 塊の大きさ：1 MiB と、頁の中の半端な位置から始まる 3 頁足らず */
static size_t piece(size_t done, size_t left) {
    size_t c = (done / 4096) % 2 ? 1u << 20 : 3 * 4096 - 1 - done % 4096;
    return c < left ? c : left;
}

static int write_one(unsigned k, unsigned char *buf) {
    const char *p = path(k);
    n_entry_remove(p);
    n_handle h = n_document_open(p, N_DOCUMENT_CREATE);
    if (h < 0) return 0;
    size_t n = sizes[k], done = 0;
    while (done < n) {
        size_t c = piece(done, n - done);
        for (size_t i = 0; i < c; i++) buf[i] = pat(done + i, k);
        if (n_document_write_at(h, done, buf, c) != (n_result)c) {
            n_handle_close(h);
            return 0;
        }
        done += c;
    }
    return n_handle_close(h) == 0;
}

static int check_one(unsigned k, unsigned char *buf) {
    const char *p = path(k);
    struct n_entry st;
    if (n_entry_info(p, &st) < 0 || st.kind != N_KIND_DOCUMENT || st.size != sizes[k]) return 0;
    n_handle h = n_document_open(p, N_DOCUMENT_READ);
    if (h < 0) return 0;
    size_t n = sizes[k], done = 0;
    while (done < n) {
        size_t c = piece(done, n - done);
        n_result r = n_document_read_at(h, done, buf, c);
        if (r != (n_result)c) break;
        for (size_t i = 0; i < c; i++)
            if (buf[i] != pat(done + i, k)) {
                printf("dmadisktest: %s differs at byte %zu\n", p, done + i);
                n_handle_close(h);
                return 0;
            }
        done += c;
    }
    n_handle_close(h);
    return done == n;
}

int main(int argc, char **argv) {
    if (argc < 3 || (strcmp(argv[1], "write") && strcmp(argv[1], "check"))) {
        fprintf(stderr, "usage: dmadisktest write|check DIR\n");
        return 2;
    }
    if (n_path_complete(argv[2], dir, sizeof dir) < 0) {
        fprintf(stderr, "dmadisktest: DIR is too long\n");
        return 2;
    }
    bool writing = !strcmp(argv[1], "write");
    if (writing) n_folder_create(dir);
    unsigned char *buf = malloc(1u << 20);
    if (!buf) return 2;
    unsigned count = sizeof sizes / sizeof sizes[0];
    uint64_t start = n_clock_monotonic();
    size_t total = 0;
    for (unsigned k = 0; k < count; k++) {
        int ok = writing ? write_one(k, buf) : check_one(k, buf);
        if (!ok) {
            printf("FAIL %s %s (%zu bytes)\n", argv[1], path(k), sizes[k]);
            failed++;
        }
        total += sizes[k];
    }
    uint64_t ns = n_clock_monotonic() - start;
    printf("dmadisktest: %s %u documents, %zu bytes in %llu ms\n", argv[1], count, total,
           (unsigned long long)(ns / 1000000));
    printf("dmadisktest: %d failed\n", failed);
    free(buf);
    return failed != 0;
}
