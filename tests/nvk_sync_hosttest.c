/* 実製品の同期実装を使う。CPU側の参照回収と待機点を検査し、GPUを模擬しない。 */
#include <stdio.h>
#include <stdlib.h>
#include "../third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_sync.c"

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* 単一スレッド試験のネイティブサービス境界。 */
int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void _simple_mtx_plain_init_once(simple_mtx_t *m) { (void)m; }
void util_call_once_data_slow(once_flag *flag, util_call_once_data_func func, const void *data) {
   (void)flag; func(data);
}
void nvkmd_mem_unref(struct nvkmd_mem *mem) { CHECK(mem == NULL); }
bool nnrm_chan_has_error(struct nnrm_chan *chan) { (void)chan; return false; }
int64_t os_time_get_nano(void) { return 0; }
void os_time_sleep(int64_t us) { (void)us; }
void thrd_yield(void) {}

static struct nvkmd_nocturne_fence *make_fence(volatile uint64_t *counter) {
   struct nvkmd_nocturne_fence *f = calloc(1, sizeof(*f));
   f->refcnt = 1;
   f->map = counter;
   return f;
}

static unsigned point_count(struct nvkmd_nocturne_sync *s) {
   return util_dynarray_num_elements(&s->points, struct nvkmd_nocturne_sync_point);
}

int main(void) {
   struct nvkmd_nocturne_sync s = {
      .base = {.type = &nvkmd_nocturne_sync_type, .flags = VK_SYNC_IS_TIMELINE},
   };
   volatile uint64_t counter = 0, other_counter = 0;
   struct nvkmd_nocturne_fence *f = make_fence(&counter);
   struct nvkmd_nocturne_fence *other = make_fence(&other_counter);
   CHECK(nvkmd_nocturne_sync_init(NULL, &s.base, 0) == VK_SUCCESS);

   /* 別のfence/queue idleで完了を知りtimelineを照会しない利用。 */
   for (uint64_t value = 1; value <= 10000; value++) {
      CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, value, f, value) == VK_SUCCESS);
      CHECK(point_count(&s) == 1 && f->refcnt == 2);
      counter = value;
   }
   CHECK(s.value == 9999);
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 10001, f, 10001) == VK_SUCCESS);
   CHECK(point_count(&s) == 1 && f->refcnt == 2 && s.value == 10000);

   /* 同じcompletion pointの重複だけ集約。異なるseqnoの旧値待機は保持。 */
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 10002, f, 10001) == VK_SUCCESS);
   CHECK(point_count(&s) == 1 && f->refcnt == 2);
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 10003, f, 10002) == VK_SUCCESS);
   CHECK(point_count(&s) == 2 && f->refcnt == 3);
   struct nvkmd_nocturne_fence *wait_fence = NULL;
   uint64_t wait_seqno = 0;
   CHECK(nvkmd_nocturne_sync_gpu_wait_point(&s.base, 10002, &wait_fence, &wait_seqno) == VK_SUCCESS);
   CHECK(wait_fence == f && wait_seqno == 10001);
   nvkmd_nocturne_fence_unref(wait_fence);

   /* 別channelの高い完了値は、未完了の低い値も安全に包含する。 */
   other_counter = 1;
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 10004, other, 1) == VK_SUCCESS);
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 10005, other, 2) == VK_SUCCESS);
   CHECK(s.value == 10004 && point_count(&s) == 1 && f->refcnt == 1 && other->refcnt == 2);

   CHECK(nvkmd_nocturne_sync_signal(NULL, &s.base, 10006) == VK_SUCCESS);
   CHECK(s.value == 10006 && point_count(&s) == 0 && other->refcnt == 1);

   CHECK(nvkmd_nocturne_sync_reset(NULL, &s.base) == VK_SUCCESS);
   CHECK(point_count(&s) == 0 && other->refcnt == 1);
   s.base.flags = 0;
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 1, f, 10002) == VK_SUCCESS);
   CHECK(nvkmd_nocturne_sync_add_gpu_signal(&s.base, 1, other, 2) == VK_SUCCESS);
   CHECK(point_count(&s) == 1 && f->refcnt == 1 && other->refcnt == 2);
   nvkmd_nocturne_sync_finish(NULL, &s.base);
   CHECK(f->refcnt == 1 && other->refcnt == 1);
   nvkmd_nocturne_fence_unref(f);
   nvkmd_nocturne_fence_unref(other);
   printf("NVK同期ホスト試験: %u件失敗（GPU実行は未検証）\n", failures);
   return failures != 0;
}
