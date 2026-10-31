/* In-place sorting: heapsort, O(n log n) in every case, no recursion and no allocation, so it
   runs anywhere, before any allocator too. Not stable. */
#include "kernel.h"

static void swap(uint8_t *a, uint8_t *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* The heap below end, its root at root, made a max-heap again after the root changed. */
static void sift(uint8_t *base, size_t root, size_t end, size_t size, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= end) return;
        if (child + 1 < end && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        swap(base + root * size, base + child * size, size);
        root = child;
    }
}

void sort(void *items, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    uint8_t *base = items;
    if (count < 2) return;
    for (size_t i = count / 2; i-- > 0;) sift(base, i, count, size, cmp);
    for (size_t end = count - 1; end > 0; end--) {
        swap(base, base + end * size, size);
        sift(base, 0, end, size, cmp);
    }
}
