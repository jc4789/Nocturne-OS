/* Fuzzes user/libc/image.c: format sniffing, ICO (PNG/DIB) directory + DIB decoder, the stb/jebp/nanosvg
   adapters, image_scale and image_draw. Data is split: last 4 bytes pick scale target / draw offset. */
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include <image.h>
#include <parallel.h>

n_result wait_on_address(volatile uint32_t *p, uint32_t value, unsigned timeout) { return 0; }
n_result wake_address(volatile uint32_t *p, unsigned count) { return 0; }
void parallel_record_work(enum parallel_stage s, uint64_t u) {}
bool parallel_for_stage(enum parallel_stage s, size_t count, parallel_fn fn, void *ctx, parallel_service_fn sv, void *sc) {
    for (size_t i = 0; i < count; i++) fn(i, ctx);
    return true;
}
bool parallel_call_stage(enum parallel_stage s, parallel_fn fn, void *ctx, parallel_service_fn sv, void *sc) { fn(0, ctx); return true; }
int gfx_clip_row(const canvas_t *c, int y, int x0, int x1, struct gfx_clip_span *out) { __builtin_trap(); }
void gfx_blend_pixel(canvas_t *c, int x, int y, uint32_t argb) { __builtin_trap(); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 12) return 0;
    size_t n = size - 4;
    unsigned tw = data[n] % 97 + 1, th = data[n + 1] % 97 + 1;
    int dx = (int)(signed char)data[n + 2], dy = (int)(signed char)data[n + 3];
    uint8_t *copy = malloc(n); memcpy(copy, data, n); /* exact-size heap copy for ASan */
    image_t *img = image_decode(copy, n);
    if (img) {
        image_t *s = image_scale(img, (int)tw, (int)th);
        static uint32_t pixels[64 * 64];
        canvas_t c = {.px = pixels, .w = 64, .h = 64, .pitch = 64, .cx0 = 0, .cy0 = 0, .cx1 = 64, .cy1 = 64};
        if (s) { image_draw(&c, s, dx, dy); image_free(s); }
        image_draw(&c, img, dx / 2, dy / 2);
        image_free(img);
    }
    if (image_is_svg(copy, n)) { image_t *v = image_decode_svg((const char *)copy, n, (int)tw, (int)th); image_free(v); }
    free(copy);
    return 0;
}
