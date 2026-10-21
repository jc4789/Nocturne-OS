/* 実PMMのhint付き上限確保。IRQと物理メモリー表だけをホストで模擬する。
   時間はこのモデルの比較であり、実GPUやsyscall全体の速度ではない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"

static unsigned irq_depth;
static uint64_t host_irq_save(void) { return irq_depth++; }
static void host_irq_restore(uint64_t old) { irq_depth = (unsigned)old; }
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#include "../kernel/src/mm/pmm.c"
#undef irq_save
#undef irq_restore

uint64_t hhdm_offset;
void cpu_require_bsp(void) {}
int kprintf(const char *format, ...) { (void)format; return 0; }
void panic(const char *format, ...) { (void)format; abort(); }
static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr,"失敗 %d: %s\n",__LINE__,#c); } } while (0)

static void pool(uint64_t pages) {
    free(bitmap);
    bitmap = calloc((size_t)((pages + 63) / 64), sizeof *bitmap);
    if (!bitmap) abort();
    total_pages = pages;
    free_pages = pages;
    search_hint = 256;
    for (uint64_t p = 0; p < 256 && p < pages; p++) { set_used(p); free_pages--; }
}

static void test_bounds(void) {
    pool(400);
    search_hint = 390;
    uint64_t limit = 325 * PAGE_SIZE + PAGE_SIZE / 2;
    bool sequential = true;
    for (uint64_t p = 256; p < 325; p++) {
        uint64_t frame = pmm_alloc_below(limit);
        sequential &= frame == p * PAGE_SIZE && !irq_depth;
    }
    CHECK(sequential && !pmm_alloc_below(limit));
    CHECK(free_pages == 75 && !is_used(325)); /* 半ページは返さない、範囲外を消費しない */
    CHECK(!pmm_alloc_below(0) && !pmm_alloc_below(PAGE_SIZE - 1) && !irq_depth);
    pmm_free(300 * PAGE_SIZE);
    CHECK(pmm_alloc_below(limit) == 300 * PAGE_SIZE); /* hintの折返し後も解放pageを再利用 */
    CHECK(pmm_alloc() >= 325 * PAGE_SIZE); /* 有限上限による通常割当の機能退行なし */
    pool(321);
    CHECK(pmm_alloc_below(257 * PAGE_SIZE) == 256 * PAGE_SIZE);
    CHECK(!pmm_alloc_below(257 * PAGE_SIZE)); /* word途中の上限 */
    CHECK(pmm_alloc() == 257 * PAGE_SIZE);
    pool((1ull << 32) / PAGE_SIZE + 64);
    search_hint = (1ull << 32) / PAGE_SIZE;
    CHECK(pmm_alloc_below(1ull << 32) < (1ull << 32)); /* 32bit DMAで高位hintを使わない */
    CHECK(!is_used((1ull << 32) / PAGE_SIZE));
}

static void test_large_allocation(void) {
    const uint64_t count = (256ull * 1024 * 1024) / PAGE_SIZE;
    const uint64_t limit = (count + 256) * PAGE_SIZE;
    pool(count + 256);
    clock_t start = clock();
    bool correct = true;
    for (uint64_t i = 0; i < count; i++) correct &= pmm_alloc_contig(1) == (i + 256) * PAGE_SIZE;
    clock_t legacy = clock() - start;
    CHECK(correct && !free_pages && !irq_depth);
    pool(count + 256);
    start = clock();
    correct = true;
    for (uint64_t i = 0; i < count; i++) correct &= pmm_alloc_below(limit) == (i + 256) * PAGE_SIZE;
    clock_t bounded = clock() - start;
    CHECK(correct && !free_pages && !irq_depth && search_hint == count + 255);
    printf("256MiB相当の65536ページ: 旧contig(1) %.6f秒、hint付き上限確保 %.6f秒（ホストPMMモデルのみ）\n",
           (double)legacy / CLOCKS_PER_SEC, (double)bounded / CLOCKS_PER_SEC);
}

int main(void) {
    test_bounds();
    test_large_allocation();
    free(bitmap);
    printf("PMM/DMA検査: %u判定、失敗%u\n",checks,failed);
    return failed ? 1 : 0;
}
