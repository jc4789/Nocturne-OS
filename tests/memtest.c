/* memtest: malloc/realloc/free stress with pattern checks. Prints "memtest: ok" or what broke. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SLOTS 512

static uint32_t seed = 12345;
static uint32_t rnd(void) { return seed = seed * 1103515245 + 12345, seed >> 8; }

static unsigned char *p[SLOTS];
static size_t len[SLOTS];
static unsigned char tag[SLOTS];

static int verify(int i) {
    for (size_t k = 0; k < len[i]; k++)
        if (p[i][k] != (unsigned char)(tag[i] + k)) return 0;
    return 1;
}

static void fill(int i) {
    for (size_t k = 0; k < len[i]; k++) p[i][k] = (unsigned char)(tag[i] + k);
}

int main(void) {
    for (int round = 0; round < 40000; round++) {
        int i = rnd() % SLOTS;
        if (p[i] && !verify(i)) {
            printf("memtest: block %d (%zu bytes) corrupted at round %d\n", i, len[i], round);
            return 1;
        }
        int op = rnd() % 3;
        size_t n = rnd() % 4 == 0 ? rnd() % 200000 : rnd() % 512; /* mostly small, some large */
        if (op == 0 || !p[i]) {
            free(p[i]);
            p[i] = malloc(n + 1);
            if (!p[i]) {
                printf("memtest: malloc(%zu) failed\n", n + 1);
                return 1;
            }
            len[i] = n + 1;
        } else if (op == 1) {
            unsigned char *q = realloc(p[i], n + 1);
            if (!q) {
                printf("memtest: realloc(%zu) failed\n", n + 1);
                return 1;
            }
            size_t keep = len[i] < n + 1 ? len[i] : n + 1;
            for (size_t k = 0; k < keep; k++)
                if (q[k] != (unsigned char)(tag[i] + k)) {
                    printf("memtest: realloc lost data\n");
                    return 1;
                }
            p[i] = q;
            len[i] = n + 1;
        } else {
            free(p[i]);
            p[i] = NULL;
            len[i] = 0;
            continue;
        }
        tag[i] = (unsigned char)rnd();
        fill(i);
    }
    for (int i = 0; i < SLOTS; i++) {
        if (p[i] && !verify(i)) {
            printf("memtest: block %d corrupted at the end\n", i);
            return 1;
        }
        free(p[i]);
    }
    /* calloc must zero, even reused memory */
    for (int r = 0; r < 100; r++) {
        unsigned char *c = calloc(1, 5000 + r);
        for (int k = 0; k < 5000 + r; k++)
            if (c[k]) {
                printf("memtest: calloc memory not zero\n");
                return 1;
            }
        memset(c, 0xFF, 5000 + r);
        free(c);
    }
    printf("memtest: ok\n");
    return 0;
}
