/* Focused product-code codec regression. Not website/GUI acceptance.
 * Link with image.c and third_party_img.c; serial dispatch replaces only the
 * native scheduling boundary, never image/container decoding. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "parallel.h"
#include "nocturne.h"
void parallel_record_work(enum parallel_stage stage, uint64_t units) { (void)stage; (void)units; }
bool parallel_for_stage(enum parallel_stage stage, size_t count, parallel_fn fn, void *context,
                        parallel_service_fn service, void *service_context) {
    (void)stage; (void)service; (void)service_context;
    for (size_t i = 0; i < count; i++) fn(i, context);
    return true;
}
bool parallel_call_stage(enum parallel_stage stage, parallel_fn fn, void *context,
                         parallel_service_fn service, void *service_context) {
    return parallel_for_stage(stage, 1, fn, context, service, service_context);
}
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout) {
    (void)address; (void)expected; (void)timeout; abort();
}
n_result wake_address(volatile uint32_t *address, unsigned count) { (void)address; (void)count; return 0; }
static int failures, checks;
static void check(bool ok, const char *name) {
    checks++; if (!ok) { failures++; printf("FAIL %s\n", name); }
}
static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (i * 8); }
static void be32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (24 - i * 8); }
static size_t icon_dib(uint8_t *p, unsigned bits) {
    size_t palette = bits <= 8 ? (1u << bits) * 4 : 0;
    size_t stride = (2 * bits + 31) / 32 * 4, bytes = 40 + palette + stride * 2 + 8;
    memset(p, 0, 22 + bytes); le16(p + 2, 1); le16(p + 4, 1);
    p[6] = p[7] = 2; le16(p + 10, 1); le16(p + 12, bits);
    le32(p + 14, bytes); le32(p + 18, 22);
    uint8_t *d = p + 22; le32(d, 40); le32(d + 4, 2); le32(d + 8, 4);
    le16(d + 12, 1); le16(d + 14, bits);
    uint8_t *row = d + 40 + palette;
    for (int y = 0; y < 2; y++) {
        if (palette) {
            d[44 + 2] = 255;
            row[y * stride] = bits == 1 ? 0xc0 : bits == 4 ? 0x11 : 1;
            if (bits == 8) row[y * stride + 1] = 1;
        } else for (int x = 0; x < 2; x++) {
            uint8_t *v = row + y * stride + x * (bits / 8);
            if (bits == 16) le16(v, 0x7c00); else { v[2] = 255; if (bits == 32) v[3] = 0; }
        }
    }
    /* Bottom-right transparent; top row opaque verifies the DIB flip. */
    row[stride * 2] = 0x40;
    return 22 + bytes;
}
static size_t png_chunk(uint8_t *p, const char *type, const uint8_t *data, size_t n) {
    be32(p, n); memcpy(p + 4, type, 4); if (n) memcpy(p + 8, data, n);
    uint32_t crc = ~0u;
    for (size_t i = 4; i < 8 + n; i++) {
        crc ^= p[i]; for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    be32(p + 8 + n, ~crc); return n + 12;
}
int main(void) {
    uint8_t data[2048]; const unsigned depths[] = {1, 4, 8, 16, 24, 32};
    for (unsigned i = 0; i < sizeof depths / sizeof depths[0]; i++) {
        size_t n = icon_dib(data, depths[i]); image_t *img = image_decode(data, n);
        check(img && img->w == 2 && img->h == 2 && img->px[0] == 0xffff0000u &&
              img->px[1] == 0xffff0000u && img->px[2] == 0xffff0000u && img->px[3] == 0,
              "DIB depth, palette, orientation and AND transparency");
        image_free(img); le32(data + 14, n - 22 - 8); img = image_decode(data, n - 8);
        check(!img, "missing transparency rejects legacy DIB"); image_free(img);
    }
    size_t n = icon_dib(data, 32);
    for (int i = 0; i < 4; i++) data[22 + 40 + i * 4 + 3] = 128;
    le32(data + 14, n - 22 - 8);
    image_t *img = image_decode(data, n - 8);
    check(img && img->px[0] == 0x80ff0000u && img->px[3] == 0x80ff0000u, "32-bit alpha permits absent AND mask"); image_free(img);
    le32(data + 18, 0xfffffff0u); img = image_decode(data, n);
    check(!img, "overflowing directory offset rejected"); image_free(img);
    n = icon_dib(data, 8); data[6] = 3; img = image_decode(data, n);
    check(!img, "directory and DIB dimensions must agree"); image_free(img);
    n = icon_dib(data, 4); le32(data + 22 + 32, 2); data[22 + 40 + 8] = 0x22; img = image_decode(data, n);
    check(!img, "palette bounds reject invalid shifted payload"); image_free(img);
    uint8_t png[100]; memcpy(png, "\x89PNG\r\n\x1a\n", 8); size_t used = 8;
    const uint8_t ihdr[] = {0,0,0,1,0,0,0,1,8,6,0,0,0};
    const uint8_t idat[] = {0x78,1,1,5,0,0xfa,0xff,0,255,0,0,255,5,0,1,255};
    used += png_chunk(png + used, "IHDR", ihdr, sizeof ihdr);
    used += png_chunk(png + used, "IDAT", idat, sizeof idat);
    used += png_chunk(png + used, "IEND", NULL, 0);
    memset(data, 0, 22); le16(data + 2, 1); le16(data + 4, 1); data[6] = data[7] = 1;
    le32(data + 14, used); le32(data + 18, 22); memcpy(data + 22, png, used);
    img = image_decode(data, used + 22);
    check(img && img->w == 1 && img->h == 1 && img->px[0] == 0xffff0000u, "PNG ICO uses actual PNG codec"); image_free(img);
    printf("ICO codec: %d checks, %d failures\n", checks, failures); return failures != 0;
}
