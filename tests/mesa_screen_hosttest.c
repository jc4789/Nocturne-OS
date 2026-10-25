/* 実frontendのscreen選択を使う。GPU未readyからreadyへの遷移と旧screenを検査する。
   直接表示（ngl_scanout）の作成・提示・破棄と、GPUの無い文脈で作らないことも検査する。
   GPUを使えない理由がMesaの記録（システムの記録）へ、変わったときだけ一行出ることも検査する。
   Zinkが作成中に書いた理由の行は預かられ（mesa_log_hold）、要約の行の前へ、変わったときだけ書かれる。 */
#include <stdarg.h>
#include <stdio.h>
#include "../ports/mesa/nocturne_gl.c"

static unsigned failures, gpu_attempts, cpu_attempts, gpu_clears, cpu_maps, probes, closes;
static bool daemon_present, daemon_ready, zink_available;
static struct pipe_screen software = {.caps = {.accelerated = 0}};
static const char *hardware_name(struct pipe_screen *screen) { (void)screen; return "zink"; }
static struct pipe_screen hardware = {.caps = {.accelerated = 1}, .get_name = hardware_name};
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void _simple_mtx_plain_init_once(simple_mtx_t *m) { (void)m; }
void util_call_once_data_slow(once_flag *flag, util_call_once_data_func func, const void *data) {
    (void)flag; func(data);
}
struct nvrm *nvrm_connect_ready(void) {
    probes++; return daemon_present && daemon_ready ? (struct nvrm *)&daemon_ready : NULL;
}
int64_t nvrm_connect_ready_status(struct nvrm **out) {
    *out = nvrm_connect_ready();
    return *out ? 0 : daemon_present ? -N_ERR_NOT_READY : -N_ERR_NOT_FOUND;
}
const char *n_result_text(n_result r) { return r == -N_ERR_NOT_READY ? "not ready" : "?"; }
static unsigned logs, line_writes;
static char logged[256], held[512], written[512], *zink_reason;
static size_t held_length;
static bool holding;
/* 預かり中の行は記録されず、預かりの文字列へ一行ずつ足される（log.cでは「TAG: level: 本文」）。 */
void mesa_log(enum mesa_log_level level, const char *tag, const char *format, ...) {
    (void)level;
    va_list va; va_start(va, format);
    if (holding) {
        held_length += snprintf(held + held_length, sizeof held - held_length, "%s: ", tag);
        held_length += vsnprintf(held + held_length, sizeof held - held_length, format, va);
        held_length += snprintf(held + held_length, sizeof held - held_length, "\n");
    } else {
        vsnprintf(logged, sizeof logged, format, va);
        logs++;
    }
    va_end(va);
}
void mesa_log_hold(void) { CHECK(!holding); holding = true; held_length = 0; held[0] = 0; }
bool mesa_log_holding(void) { return holding; }
char *mesa_log_release(void) { CHECK(holding); holding = false; return held_length ? strdup(held) : NULL; }
void mesa_log_write_lines(const char *lines) {
    CHECK(!holding);
    if (lines && *lines) { snprintf(written, sizeof written, "%s", lines); line_writes++; }
}
void nvrm_disconnect(struct nvrm *rm) { CHECK(rm == (struct nvrm *)&daemon_ready); closes++; }
struct pipe_screen *softpipe_create_screen(struct sw_winsys *ws) {
    CHECK(ws == &mem_winsys); cpu_attempts++; return &software;
}
struct pipe_screen *zink_create_screen(struct sw_winsys *ws, const struct pipe_screen_config *config) {
    CHECK(ws == &mem_winsys && config == NULL && daemon_ready && holding); gpu_attempts++;
    if (zink_reason)
        mesa_loge("%s", zink_reason);
    return zink_available ? &hardware : NULL;
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

/* 直接表示の画像：8x4、行は64バイト、1ページ。 */
static unsigned scanout_creates, scanout_destroys, scanout_presents, imports, destroyed, buffer_clears, copies, flushes;
static uint32_t present_status, last_presented;
static unsigned copy_offset, copy_stride;
static struct pipe_resource imported[3];
uint32_t nvrm_scanout_create(struct nvrm *rm, uint32_t *image, uint32_t *width, uint32_t *height,
                             uint32_t *pitch, uint64_t *bytes) {
    CHECK(rm == (struct nvrm *)&daemon_ready);
    *image = 0x100 + scanout_creates++; *width = 8; *height = 4; *pitch = 64; *bytes = 4096;
    return NV_OK;
}
uint32_t nvrm_scanout_destroy(struct nvrm *rm, uint32_t image) {
    CHECK(rm == (struct nvrm *)&daemon_ready && image >= 0x100 && image < 0x103); scanout_destroys++; return NV_OK;
}
uint32_t nvrm_scanout_present(struct nvrm *rm, uint32_t image) {
    (void)rm; last_presented = image; scanout_presents++; return present_status;
}
static struct pipe_resource *from_handle(struct pipe_screen *screen, const struct pipe_resource *templ,
                                         struct winsys_handle *handle, unsigned usage) {
    CHECK(handle->type == WINSYS_HANDLE_TYPE_NOCTURNE_SCANOUT && handle->handle == 0x100 + imports);
    CHECK(templ->target == PIPE_BUFFER && templ->width0 == 64 * 4 && usage == 0);
    struct pipe_resource *r = &imported[imports++ % 3];
    memset(r, 0, sizeof *r);
    r->target = PIPE_BUFFER; r->width0 = templ->width0; r->screen = screen;
    pipe_reference_init(&r->reference, 1);
    return r;
}
static void resource_destroy(struct pipe_screen *screen, struct pipe_resource *r) {
    (void)screen; CHECK(r >= imported && r < imported + 3); destroyed++;
}
static void clear_buffer(struct pipe_context *pipe, struct pipe_resource *res, unsigned offset, unsigned size,
                         const void *value, int value_size) {
    (void)pipe;
    CHECK(offset == 0 && size == res->width0 && value_size == 4 && *(const uint32_t *)value == 0);
    buffer_clears++;
}
static void copy_buffer(struct pipe_context *pipe, struct pipe_resource *dst, struct pipe_resource *src,
                        unsigned offset, unsigned stride, unsigned layer_stride, unsigned level,
                        const struct pipe_box *box) {
    (void)pipe; (void)src;
    CHECK(dst->target == PIPE_BUFFER && layer_stride == 0 && level == 0 && box->width == 4 && box->height == 2);
    copy_offset = offset; copy_stride = stride; copies++;
}
void st_context_flush(struct st_context *st, unsigned flags, struct pipe_fence_handle **fence,
                      void (*before_flush_cb)(void *), void *args) {
    (void)st; (void)fence; (void)before_flush_cb; (void)args; CHECK(flags == ST_FLUSH_WAIT); flushes++;
}

static void scanout_cases(struct ngl_context *cpu, struct ngl_context *gpu) {
    unsigned before_probes = probes, before_closes = closes;
    /* GPUの無い文脈（softpipe）はnvrmへ繋がず、直接表示を作らない：窓の経路のまま。 */
    CHECK(!ngl_scanout_create(cpu) && probes == before_probes && scanout_creates == 0);
    /* 取込みのできないscreenでも作らない。 */
    CHECK(!ngl_scanout_create(gpu) && probes == before_probes);
    hardware.resource_from_handle = from_handle;
    hardware.resource_destroy = resource_destroy;
    struct ngl_scanout *s = ngl_scanout_create(gpu);
    CHECK(s && scanout_creates == 3 && imports == 3 && probes == before_probes + 1);
    unsigned width = 0, height = 0;
    ngl_scanout_size(s, &width, &height);
    CHECK(width == 8 && height == 4);
    struct pipe_context pipe = {.clear_buffer = clear_buffer, .image_copy_buffer = copy_buffer};
    static struct st_context st;
    st.pipe = &pipe;
    gpu->st = &st;
    /* CPUの文脈と画面の外は提示しない。 */
    CHECK(ngl_scanout_present(s, cpu, 0, 0, 8, 4) == -1);
    CHECK(ngl_scanout_present(s, gpu, 1, 0, 8, 4) == -1 && ngl_scanout_present(s, gpu, 0, 0, 0, 4) == -1);
    CHECK(scanout_presents == 0 && buffer_clears == 0);
    /* 何も描かれていない：画像を黒で埋めて提示し、次の画像へ進む。 */
    present_status = NV_OK;
    CHECK(ngl_scanout_present(s, gpu, 0, 0, 8, 4) == 1 && last_presented == 0x100 && buffer_clears == 1 && flushes == 1);
    /* 系が直接表示をやめた直後：0を返し、同じ画像を次も使う。 */
    present_status = NV_ERR_INVALID_STATE;
    CHECK(ngl_scanout_present(s, gpu, 0, 0, 8, 4) == 0 && last_presented == 0x101 && buffer_clears == 2);
    /* 描画バッファーと同じ大きさ：位置(2,1)へ行の間隔64で写し、場所の変わった画像は先に黒で埋める。 */
    struct pipe_resource color = {.target = PIPE_TEXTURE_2D, .width0 = 4, .height0 = 2};
    gpu->textures[ST_ATTACHMENT_BACK_LEFT] = &color;
    gpu->width = gpu->tex_width = 4;
    gpu->height = gpu->tex_height = 2;
    present_status = NV_OK;
    CHECK(ngl_scanout_present(s, gpu, 2, 1, 4, 2) == 1 && last_presented == 0x101 && buffer_clears == 3);
    CHECK(copies == 1 && copy_offset == 1 * 64 + 2 * 4 && copy_stride == 64);
    /* 同じ場所なら黒で埋め直さない（三枚目は初めてなので埋める）。 */
    CHECK(ngl_scanout_present(s, gpu, 2, 1, 4, 2) == 1 && last_presented == 0x102 && buffer_clears == 4);
    CHECK(ngl_scanout_present(s, gpu, 2, 1, 4, 2) == 1 && last_presented == 0x100 && buffer_clears == 5);
    CHECK(ngl_scanout_present(s, gpu, 2, 1, 4, 2) == 1 && last_presented == 0x101 && buffer_clears == 5 && copies == 4);
    /* ドライバーの失敗は-1：呼出し側はこの文脈の直接表示をやめる。 */
    present_status = NV_ERR_GENERIC;
    CHECK(ngl_scanout_present(s, gpu, 2, 1, 4, 2) == -1 && last_presented == 0x102);
    gpu->textures[ST_ATTACHMENT_BACK_LEFT] = NULL;
    ngl_scanout_destroy(s);
    CHECK(scanout_destroys == 3 && destroyed == 3 && closes == before_closes + 1);
    gpu->st = NULL;
}

int main(void) {
    struct pipe_frontend_screen *first = get_screen();
    CHECK(first && first->screen == &software && cpu_attempts == 1 && gpu_attempts == 0);
    CHECK(get_screen() == first && gpu_attempts == 0 && cpu_attempts == 1);
    CHECK(logs == 0); /* NVIDIAのGPUが無い機械では何も書かない。 */
    struct ngl_context old = {.base = {.fscreen = first}, .on_gpu = false};
    CHECK(ngl_has_major_performance_caveat(&old));
    daemon_present = true; /* 接続可能な診断daemonだけではGPUに昇格しない。 */
    CHECK(get_screen() == first && gpu_attempts == 0 && cpu_attempts == 1);
    CHECK(logs == 1 && strstr(logged, "cannot be used (not ready)"));
    CHECK(get_screen() == first && logs == 1); /* 同じ理由は文脈ごとに繰り返さない。 */
    daemon_ready = true;
    /* probe後のZink/NVK再検査でGPUが消えてもCPU screenを失わない。 */
    CHECK(get_screen() == first && gpu_attempts == 1 && closes == 1 && old.base.fscreen == first);
    CHECK(logs == 2 && strstr(logged, "Zink did not start") && strstr(logged, "it gave no reason"));
    CHECK(line_writes == 0 && !holding);
    /* 理由が出た：理由の行を先に、要約を後に一度だけ書く。 */
    zink_reason = "the Vulkan loader has no vkEnumerateInstanceLayerProperties";
    CHECK(get_screen() == first && gpu_attempts == 2 && closes == 2 && !holding);
    CHECK(line_writes == 1 && !strcmp(written, "MESA: the Vulkan loader has no vkEnumerateInstanceLayerProperties\n"));
    CHECK(logs == 3 && strstr(logged, "Zink did not start") && strstr(logged, "why is in the lines before"));
    /* 同じ理由の再試行は文脈ごとに何も書かない。 */
    CHECK(get_screen() == first && gpu_attempts == 3 && line_writes == 1 && logs == 3);
    /* 理由が変われば、同じ失敗でも書き直す。 */
    zink_reason = "ZINK: vkCreateDevice failed (VK_ERROR_INITIALIZATION_FAILED)";
    CHECK(get_screen() == first && gpu_attempts == 4 && line_writes == 2 && logs == 4);
    CHECK(strstr(written, "vkCreateDevice failed") && strstr(logged, "why is in the lines before"));
    /* 理由が消えれば「理由なし」を書き直す。 */
    zink_reason = NULL;
    CHECK(get_screen() == first && gpu_attempts == 5 && line_writes == 2 && logs == 5);
    CHECK(strstr(logged, "it gave no reason"));
    /* 成功時にZinkが書いた行も、要約の前に一度だけ書く。 */
    zink_reason = "NVK: a line Zink wrote while it started";
    zink_available = true;
    struct pipe_frontend_screen *later = get_screen();
    CHECK(later && later != first && later->screen == &hardware && gpu_attempts == 6 && closes == 6);
    CHECK(logs == 6 && strstr(logged, "drawing on the GPU with") && line_writes == 3 && strstr(written, "a line Zink wrote"));
    CHECK(first->screen == &software && old.base.fscreen == first);
    struct ngl_context newer = {.base = {.fscreen = later}, .on_gpu = true};
    CHECK(!ngl_has_major_performance_caveat(&newer));
    CHECK(ngl_has_major_performance_caveat(&old));
    unsigned before_probes = probes;
    CHECK(get_screen() == later && gpu_attempts == 6 && cpu_attempts == 1 && probes == before_probes);
    CHECK(logs == 6 && line_writes == 3);
    struct pipe_context pipe = {.clear_texture = clear, .texture_map = map, .texture_unmap = unmap};
    struct pipe_resource texture = {.width0 = 1, .height0 = 1};
    init_texture(&pipe, &texture, 0, old.on_gpu);
    init_texture(&pipe, &texture, 0, newer.on_gpu);
    CHECK(cpu_maps == 1 && gpu_clears == 1);
    scanout_cases(&old, &newer);
    free(first); free(later);
    printf("Mesa screenホスト試験: %u件失敗（実描画は未検証）\n", failures);
    return failures != 0;
}
