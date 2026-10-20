/* 実frontendのscreen選択を使う。GPU未readyからreadyへの遷移と旧screenを検査する。 */
#include <stdio.h>
#include "../ports/mesa/nocturne_gl.c"

static unsigned failures, gpu_attempts, cpu_attempts, gpu_clears, cpu_maps;
static bool daemon_ready;
static struct pipe_screen software = {.caps = {.accelerated = 0}};
static struct pipe_screen hardware = {.caps = {.accelerated = 1}};
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void _simple_mtx_plain_init_once(simple_mtx_t *m) { (void)m; }
void util_call_once_data_slow(once_flag *flag, util_call_once_data_func func, const void *data) {
    (void)flag; func(data);
}
struct nvrm *nvrm_connect(void) { return daemon_ready ? (struct nvrm *)&daemon_ready : NULL; }
void nvrm_disconnect(struct nvrm *rm) { CHECK(rm == (struct nvrm *)&daemon_ready); }
struct pipe_screen *softpipe_create_screen(struct sw_winsys *ws) {
    CHECK(ws == &mem_winsys); cpu_attempts++; return &software;
}
struct pipe_screen *zink_create_screen(struct sw_winsys *ws, const struct pipe_screen_config *config) {
    CHECK(ws == &mem_winsys && config == NULL && daemon_ready); gpu_attempts++; return &hardware;
}
static void clear(struct pipe_context *pipe, struct pipe_resource *res, unsigned level,
                  const struct pipe_box *box, const void *data) {
    (void)pipe; (void)res; (void)level; (void)box; (void)data; gpu_clears++;
}
static void *map(struct pipe_context *pipe, struct pipe_resource *res, unsigned level,
                 unsigned usage, const struct pipe_box *box, struct pipe_transfer **transfer) {
    (void)pipe; (void)res; (void)level; (void)usage; (void)box;
    static uint32_t pixel;
    static struct pipe_transfer t = {.stride = sizeof(pixel)};
    cpu_maps++; *transfer = &t; return &pixel;
}
static void unmap(struct pipe_context *pipe, struct pipe_transfer *transfer) { (void)pipe; (void)transfer; }

int main(void) {
    struct pipe_frontend_screen *first = get_screen();
    CHECK(first && first->screen == &software && cpu_attempts == 1 && gpu_attempts == 0);
    CHECK(get_screen() == first && gpu_attempts == 0 && cpu_attempts == 1);
    struct ngl_context old = {.base = {.fscreen = first}, .on_gpu = false};
    CHECK(ngl_has_major_performance_caveat(&old));
    daemon_ready = true;
    struct pipe_frontend_screen *later = get_screen();
    CHECK(later && later != first && later->screen == &hardware && gpu_attempts == 1);
    CHECK(first->screen == &software && old.base.fscreen == first);
    struct ngl_context newer = {.base = {.fscreen = later}, .on_gpu = true};
    CHECK(!ngl_has_major_performance_caveat(&newer));
    CHECK(ngl_has_major_performance_caveat(&old));
    CHECK(get_screen() == later && gpu_attempts == 1 && cpu_attempts == 1);
    struct pipe_context pipe = {.clear_texture = clear, .texture_map = map, .texture_unmap = unmap};
    struct pipe_resource texture = {.width0 = 1, .height0 = 1};
    init_texture(&pipe, &texture, 0, old.on_gpu);
    init_texture(&pipe, &texture, 0, newer.on_gpu);
    CHECK(cpu_maps == 1 && gpu_clears == 1);
    free(first); free(later);
    printf("Mesa screenホスト試験: %u件失敗（実描画は未検証）\n", failures);
    return failures != 0;
}
