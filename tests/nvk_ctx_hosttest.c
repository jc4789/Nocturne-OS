/* 実行文脈（GPFIFOチャネルへの投入）を実製品のctx.cとsync.cで検査する。RMのチャネルとGPUだけを置き換える。
   偽のGPUはCPUがフェンスを待つ間だけflushを一つずつ終え、USERDのGP_GETは書き戻さない：Hostが書き戻すかは
   RMが決め、dev_ram.refはセマフォーで進みを追うよう勧めている。投入は環が溢れないこと、flushの分割が
   不完全な命令の連なりを切らないことを確かめる。 */
#include <stdio.h>
#include <stdlib.h>
#include "../third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_sync.c"
#include "../third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_ctx.c"

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* 単一スレッド試験のネイティブサービス境界 */
int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void _simple_mtx_plain_init_once(simple_mtx_t *m) { (void)m; }
void util_call_once_data_slow(once_flag *flag, util_call_once_data_func func, const void *data) {
   (void)flag; func(data);
}
#ifndef NDEBUG
void nv_push_validate(struct nv_push *push) { (void)push; }
#endif
VkResult __vk_errorf(const void *obj, VkResult error, const char *file, int line, const char *format, ...) {
   (void)obj; (void)file; (void)line; (void)format; return error;
}
VkResult vk_sync_wait(struct vk_device *d, struct vk_sync *s, uint64_t v, enum vk_sync_wait_flags f, uint64_t t) {
   (void)d; (void)s; (void)v; (void)f; (void)t; return VK_SUCCESS;
}
VkResult vk_sync_signal(struct vk_device *d, struct vk_sync *s, uint64_t v) {
   (void)d; (void)s; (void)v; return VK_SUCCESS;
}
const char *nnrm_status_name(uint32_t st) { (void)st; return "?"; }

/* ---- 偽のメモリー：CPUで読み書きでき、GPUの番地は割り振るだけ ---- */
struct fake_mem { struct nvkmd_mem mem; struct nvkmd_va va; };
static uint64_t next_va = NNRM_VA_START;
static unsigned live_mems;
VkResult nvkmd_dev_alloc_mapped_mem(struct nvkmd_dev *dev, struct vk_object_base *log_obj, uint64_t size_B,
                                    uint64_t align_B, enum nvkmd_mem_flags flags,
                                    enum nvkmd_mem_map_flags map_flags, struct nvkmd_mem **mem_out) {
   (void)dev; (void)log_obj; (void)align_B; (void)flags; (void)map_flags;
   struct fake_mem *m = calloc(1, sizeof(*m));
   m->mem.map = calloc(1, size_B); m->mem.size_B = size_B; m->mem.va = &m->va;
   m->va.addr = next_va; next_va += (size_B + 0xffff) & ~0xffffull;
   live_mems++; *mem_out = &m->mem; return VK_SUCCESS;
}
void nvkmd_mem_unref(struct nvkmd_mem *mem) {
   if (!mem) return;
   free(mem->map); free(container_of(mem, struct fake_mem, mem)); live_mems--;
}

/* ---- 偽のチャネルとGPU ---- */
#define MAX_FLUSHES 4096
static struct {
   volatile uint64_t *fence;     /* 文脈のフェンス（GPUがseqnoを書く） */
   uint32_t counts[MAX_FLUSHES]; /* flushごとのGP項目数（投入の順） */
   uint64_t client_lo[MAX_FLUSHES], client_hi[MAX_FLUSHES]; /* そのflushの利用者の押し込みの番地の範囲 */
   unsigned submitted, done;     /* 投入したflush、終えたflush */
   uint64_t put, consumed;       /* GP項目：投入した数、GPUが読み終えた数 */
   uint64_t most_outstanding;
   unsigned steps;               /* GPUが進んだ回数（CPUが待った回数） */
} gpu;
static int chan_token;

uint32_t nnrm_chan_create(struct nnrm *rm, uint32_t engines, struct nnrm_chan **out, const char **step) {
   (void)rm; (void)engines; (void)step; *out = (struct nnrm_chan *)&chan_token; return 0;
}
void nnrm_chan_destroy(struct nnrm_chan *chan) { CHECK(chan == (struct nnrm_chan *)&chan_token); }
bool nnrm_chan_has_error(struct nnrm_chan *chan) { (void)chan; return false; }

/* 利用者の押し込みの番地は NNRM_VA_END から上（文脈のホスト用の領域と区別する） */
#define CLIENT_VA NNRM_VA_END
uint32_t nnrm_chan_submit(struct nnrm_chan *chan, const struct nnrm_gp *gps, uint32_t count) {
   CHECK(chan == (struct nnrm_chan *)&chan_token);
   CHECK(count <= NNRM_CHAN_GP_ENTRIES - 1);
   CHECK(gpu.submitted < MAX_FLUSHES);
   uint64_t lo = UINT64_MAX, hi = 0;
   for (uint32_t i = 0; i < count; i++) {
      if (gps[i].addr < CLIENT_VA) continue;
      if (gps[i].addr < lo) lo = gps[i].addr;
      if (gps[i].addr > hi) hi = gps[i].addr;
   }
   gpu.client_lo[gpu.submitted] = lo; gpu.client_hi[gpu.submitted] = hi;
   gpu.counts[gpu.submitted++] = count;
   gpu.put += count;
   /* 一つ空ける環：GPUが読み終えていない項目は NNRM_CHAN_GP_ENTRIES - 1 まで */
   const uint64_t outstanding = gpu.put - gpu.consumed;
   CHECK(outstanding <= NNRM_CHAN_GP_ENTRIES - 1);
   if (outstanding > gpu.most_outstanding) gpu.most_outstanding = outstanding;
   return 0;
}
/* CPUが待つ間に、GPUは次のflushを一つ終え、そのGP項目を読み終えてフェンスを進める */
static void gpu_step(void) {
   if (gpu.done == gpu.submitted) return;
   gpu.consumed += gpu.counts[gpu.done++];
   gpu.steps++;
   *gpu.fence = gpu.done;
}
static int64_t now;
int64_t os_time_get_nano(void) { return now += 1000; }
int64_t os_time_get_absolute_timeout(uint64_t timeout) { return now + (int64_t)timeout; }
void os_time_sleep(int64_t us) { (void)us; gpu_step(); }
void thrd_yield(void) { gpu_step(); }

static struct nvkmd_nocturne_pdev pdev;
static struct nvkmd_nocturne_dev dev;

static struct nvkmd_ctx *open_ctx(void) {
   memset(&gpu, 0, sizeof(gpu));
   struct nvkmd_ctx *ctx = NULL;
   CHECK(nvkmd_nocturne_create_ctx(&dev.base, NULL, NVKMD_ENGINE_3D, &ctx) == VK_SUCCESS && ctx);
   gpu.fence = nvkmd_nocturne_exec_ctx(ctx)->fence->map;
   return ctx;
}
static void close_ctx(struct nvkmd_ctx *ctx) {
   CHECK(nvkmd_nocturne_exec_ctx_sync(ctx, NULL) == VK_SUCCESS);
   CHECK(gpu.done == gpu.submitted && gpu.consumed == gpu.put);
   nvkmd_nocturne_exec_ctx_destroy(ctx);
   CHECK(live_mems == 0);
}
/* count個の押し込み（番地は CLIENT_VA + 4*(first+i)）、incompleteは指定の番号だけ */
static VkResult exec_pushes(struct nvkmd_ctx *ctx, uint32_t first, uint32_t count,
                            uint32_t incomplete_lo, uint32_t incomplete_hi) {
   struct nvkmd_ctx_exec *execs = calloc(count, sizeof(*execs));
   for (uint32_t i = 0; i < count; i++) {
      execs[i].addr = CLIENT_VA + 4ull * (first + i);
      execs[i].size_B = 4;
      execs[i].incomplete = first + i >= incomplete_lo && first + i < incomplete_hi;
   }
   VkResult r = nvkmd_nocturne_exec_ctx_exec(ctx, NULL, count, execs);
   free(execs);
   return r;
}

int main(void) {
   pdev.base.dev_info.cls_eng3d = 0xc997;
   dev.base.pdev = &pdev.base;

   /* 小さなflushを多く：ホスト用の区画（64）が先に巡り、環は埋まらない。CPUが待つのは区画を使い回す時だけ */
   struct nvkmd_ctx *ctx = open_ctx();
   for (uint32_t i = 0; i < 1000; i++) {
      CHECK(exec_pushes(ctx, i, 1, 0, 0) == VK_SUCCESS);
      CHECK(nvkmd_nocturne_exec_ctx_flush(ctx, NULL) == VK_SUCCESS);
      CHECK(gpu.submitted - gpu.done <= NVKMD_NOCTURNE_HOST_SLOTS);
   }
   CHECK(gpu.submitted == 1000 && gpu.most_outstanding < NNRM_CHAN_GP_ENTRIES - 1);
   close_ctx(ctx);

   /* 大きなflush：二つで環が埋まり、三つ目は一つ目の終わりを待つ（以前はUSERDのGP_GETを待っていた） */
   ctx = open_ctx();
   for (uint32_t i = 0; i < 20; i++) {
      CHECK(exec_pushes(ctx, i * 500, 500, 0, 0) == VK_SUCCESS);
      CHECK(nvkmd_nocturne_exec_ctx_flush(ctx, NULL) == VK_SUCCESS);
   }
   CHECK(gpu.submitted == 20 && gpu.steps > 0 && gpu.most_outstanding > NNRM_CHAN_GP_ENTRIES - 500);
   close_ctx(ctx);

   /* 環より大きな一回のexec：環に入る大きさのflushに分かれ、押し込みは一つも失われない
      （ホスト用の項目は、最初のflushがSET_OBJECTと解放の二つ、後は解放の一つ） */
   ctx = open_ctx();
   CHECK(exec_pushes(ctx, 0, 5000, 0, 0) == VK_SUCCESS);
   CHECK(nvkmd_nocturne_exec_ctx_flush(ctx, NULL) == VK_SUCCESS);
   uint64_t client = 0;
   for (unsigned f = 0; f < gpu.submitted; f++) client += gpu.counts[f] - (f == 0 ? 2 : 1);
   CHECK(gpu.submitted == 5 && client == 5000);
   close_ctx(ctx);

   /* 不完全な命令の連なり（1018〜1021）は分割の境目（1020）をまたいでも一つのflushに入る */
   ctx = open_ctx();
   CHECK(exec_pushes(ctx, 0, 1030, 1018, 1021) == VK_SUCCESS);
   CHECK(nvkmd_nocturne_exec_ctx_flush(ctx, NULL) == VK_SUCCESS);
   CHECK(gpu.submitted == 2);
   CHECK(gpu.client_hi[0] == CLIENT_VA + 4 * 1017 && gpu.client_lo[1] == CLIENT_VA + 4 * 1018);
   close_ctx(ctx);

   printf("NVK実行文脈ホスト試験: %u件失敗（実GPUは未検証）\n", failures);
   return failures != 0;
}
