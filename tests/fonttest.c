/* Native rasterizer/measurement regressions. Not a substitute for real-site tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "font.h"

static int checks, failed;
static uint32_t pixels[640 * 96];
static canvas_t canvas;
static void check(int ok, const char *why) {
    checks++;
    if (!ok) { failed++; printf("FAIL font: %s\n", why); }
}
static uint32_t raster(font_t *f, const char *text, float px) {
    memset(pixels, 0, sizeof pixels);
    float x = font_draw(&canvas, f, px, 4, 48, text, strlen(text), RGB(240,240,240));
    check(fabsf(x - 4 - font_width(f, px, text, strlen(text))) < .01f, "draw and width agree");
    uint32_t hash = 2166136261u; int ink = 0;
    for (unsigned i = 0; i < sizeof pixels / sizeof *pixels; i++) {
        hash = (hash ^ pixels[i]) * 16777619u;
        if (pixels[i]) ink++;
    }
    check(ink > 0, "visible glyph pixels");
    return hash;
}
static font_t *memory_face(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    void *bytes = malloc((size_t)length);
    font_t *face = NULL;
    if (bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length)
        face = font_open_memory(bytes, (size_t)length);
    free(bytes); fclose(file);
    return face;
}
int main(void) {
    gfx_init(&canvas, pixels, 640, 96, 640);
    font_t *cn = font_open("/usr/share/fonts/MapleMono-NF-CN-Regular.ttf");
    font_t *raw = font_open("/usr/share/fonts/MapleMono-NF-Regular.ttf");
    check(cn && raw, "bundled original fonts open");
    if (!cn || !raw) return 1;
    check(!font_has(raw, 0x65e5), "explicit Latin face does not pretend to contain CJK");
    uint32_t regular = 0, bold = 0, italic = 0;
    const char *mixed = "Maple 日本語 中文 あいう カタカナ";
    for (int style = 0; style < 4; style++) {
        font_t *f = font_ui(style);
        check(f != NULL, "each default style opens");
        if (!f) continue;
        check(f == font_ui(style), "default face is cached");
        check(font_has(f,0x65e5) && font_has(f,0x8a9e) && font_has(f,0x4e2d) &&
              font_has(f,0x3042) && font_has(f,0x30ab), "shared Japanese/Chinese fallback");
        check(font_has(f,0xd55c) && !font_has(cn,0xd55c), "Hangul uses actual extra fallback, not CN");
        check(!font_has(f,0x10ffff), "unassigned glyph is absent");
        check(fabsf(font_advance(f,20,'i')-font_advance(f,20,'W')) < .001f, "Maple monospaced Latin");
        check(fabsf(font_advance(f,20,0x65e5)-2*font_advance(f,20,'A')) < .001f, "CJK is two Latin columns");
        float sum = 0; const char *p = mixed;
        while (*p) { uint32_t cp; p += gfx_utf8_decode(p,&cp); sum += font_advance(f,20,cp); }
        check(fabsf(sum-font_width(f,20,mixed,strlen(mixed))) < .01f, "mixed measurement is glyph-consistent");
        raster(f,mixed,20);
        check(raster(f,"日本語",24) == raster(cn,"日本語",24), "fallback raster is the actual CN face");
        uint32_t latin = raster(f,"Maple ABC ijk",24);
        if (!style) regular = latin;
        else if (style == 1) bold = latin;
        else if (style == 2) italic = latin;
        if (!style) check(latin == raster(raw,"Maple ABC ijk",24), "default is Maple not former Inter");
    }
    check(regular != bold && regular != italic && bold != italic, "real style outlines differ");
    check(raster(cn,"日",24) != raster(cn,"本",24), "distinct CJK glyphs, not a shared tofu box");
    check(gfx_text_width("ABC日本",FONT_SMALL) == 56, "chrome width uses two CJK cells");
    check(gfx_text_width("ABC日本",FONT_LARGE) == 112, "large chrome width doubles");
    check(gfx_text(&canvas,0,0,"ABC日本",RGB(255,255,255),0,FONT_SMALL) == 56, "chrome draw agrees with width");
    font_t *symbols = font_open("/usr/share/fonts/PlangothicP2-Regular.ttf");
    check(symbols && font_has(symbols,0x1f0a1), "bundled Plangothic has actual supplementary outline");
    check(!font_has(raw,0x1f0a1), "single Maple face still has no added fallback");
    font_t *cjk[2] = {font_open("/usr/share/fonts/NotoSansCJKjp-Regular.otf"),
                     font_open("/usr/share/fonts/NotoSerifCJKjp-Regular.otf")};
    check(cjk[0] && cjk[1], "both actual CFF CJK faces open");
    uint32_t family_hash[3] = {0};
    for (int family = 0; family < 3; family++) {
        uint32_t outline[4] = {0};
        for (int style = 0; style < 4; style++) {
            font_t *f = font_family(family,style);
            check(f != NULL, "bundled generic family/style opens");
            if (!f) continue;
            check(f == font_family(family,style), "family/style is cached");
            float delta = fabsf(font_advance(f,20,'i')-font_advance(f,20,'W'));
            check(family == FONT_FAMILY_MONO ? delta < .001f : delta > 1, "generic family uses real width class");
            check(font_has(f,0x4e2d) && font_has(f,0x1e6c) && font_has(f,0x1f0a1) &&
                  font_has(f,0x0db0) && font_has(f,0x10300) && font_has(f,0x20bb7), "living/historical/CJK glyphs resolve");
            check(!font_has(f,0x10ffff), "unassigned glyph is not invented");
            outline[style] = raster(f,"Noto Maple Wi",24);
            if (symbols) check(raster(f,"\xf0\x9f\x82\xa1",24) == raster(symbols,"\xf0\x9f\x82\xa1",24), "fallback is actual Plangothic raster");
        }
        family_hash[family] = outline[0];
        if (family < 2 && cjk[family])
            check(raster(font_family(family,0),"日本語",24) == raster(cjk[family],"日本語",24), "CJK generic uses its actual Sans/Serif CFF face");
        if (family == FONT_FAMILY_SERIF)
            check(outline[0] == outline[1] && outline[0] == outline[2] && outline[0] == outline[3], "merged Serif provides only real Regular");
        else check(outline[0] != outline[1] && outline[0] != outline[2] && outline[2] != outline[3], "Sans/Mono styles have real outlines");
    }
    check(family_hash[0] != family_hash[1] && family_hash[0] != family_hash[2] && family_hash[1] != family_hash[2], "generic families have distinct rasters");
    check(font_family(FONT_FAMILY_MONO,FONT_REGULAR) == font_ui(FONT_REGULAR), "mono preserves UI face identity");
    font_t *latin = font_open("/usr/share/fonts/Inter-Regular.ttf");
    font_t *web = memory_face("/usr/share/fonts/Inter-Regular.ttf");
    check(latin && web, "validated Latin-only web face opens from bytes");
    if (latin && web) {
        check(!font_has(latin,0x65e5) && font_has(web,0x65e5) && font_has(web,0x20bb7),
              "web missing BMP and supplementary CJK use bundled glyphs");
        check(fabsf(font_advance(web,24,'W')-font_advance(latin,24,'W')) < .001f &&
              raster(web,"Latin Wi",24) == raster(latin,"Latin Wi",24), "web Latin metrics and raster are unchanged");
        if (cjk[0]) {
            check(fabsf(font_advance(web,24,0x65e5)-font_advance(cjk[0],24,0x65e5)) < .001f &&
                  raster(web,"日本語",24) == raster(cjk[0],"日本語",24), "web CJK advance and raster use actual bundled CFF face");
        }
        float sum = 0; const char *p = mixed;
        while (*p) { uint32_t cp; p += gfx_utf8_decode(p,&cp); sum += font_advance(web,24,cp); }
        check(fabsf(sum-font_width(web,24,mixed,strlen(mixed))) < .01f, "web mixed advance and width agree");
        raster(web,mixed,24);
        check(!font_has(web,0x10ffff), "web absent glyph stays absent after fallback");
        check(font_has(raw,0xf17c) && !font_has(latin,0xf17c) && !font_has(web,0xf17c),
              "web private-use icon does not borrow an unrelated bundled glyph");
        uint32_t bundled = raster(font_family(FONT_FAMILY_SANS,FONT_REGULAR),"日本語",24);
        font_close(web); web = NULL;
        check(raster(font_family(FONT_FAMILY_SANS,FONT_REGULAR),"日本語",24) == bundled,
              "closing owned web face preserves borrowed bundled fallback");
    }
    font_close(web);
    font_close(latin);
    uint32_t before[640 * 4];
    memset(pixels,0x5a,sizeof pixels); memcpy(before,pixels,sizeof before);
    gfx_clip(&canvas,10,10,70,40);
    font_draw(&canvas,font_ui(0),24,-5,32,mixed,strlen(mixed),RGB(255,255,255));
    check(memcmp(before,pixels,sizeof before) == 0, "text respects canvas clip");
    printf("fonttest: %d checks, %d failed\n",checks,failed);
    return failed != 0;
}
