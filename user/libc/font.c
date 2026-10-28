/* TrueType text with stb_truetype (compiled in third_party_ttf.c). Glyphs are rasterized once per
   (font, size, glyph, quarter-pixel x offset) and kept in a cache. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
/* Font-family lazy loading and glyph caches belong to the process, not a
 * layout worker. Internal recursion stays inside one lock acquisition. */
#define font_open font_open_unlocked
#define font_open_memory font_open_memory_unlocked
#define font_close font_close_unlocked
#define font_ui font_ui_unlocked
#define font_family font_family_unlocked
#define font_metrics font_metrics_unlocked
#define font_has font_has_unlocked
#define font_has_primary font_has_primary_unlocked
#define font_advance font_advance_unlocked
#define font_width font_width_unlocked
#define font_metrics_generation font_metrics_generation_unlocked
#define font_draw font_draw_unlocked
#define font_cell_width font_cell_width_unlocked
#define font_cell_draw font_cell_draw_unlocked
#define font_probe_begin font_probe_begin_unlocked
#define font_probe_end font_probe_end_unlocked
#include "font.h"
#include "stb_truetype.h"
#include "font_sfnt.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_FONT_FORMATS_H
#include FT_COLOR_H
#include FT_OUTLINE_H
static unsigned font_probe_depth;
static bool font_probe_failed;

enum { FALLBACK_NONE, FALLBACK_UI, FALLBACK_SANS, FALLBACK_SERIF };

struct font {
    stbtt_fontinfo info;
    unsigned char *data;
    uint64_t id;
    int ascent, descent, line_gap; /* font units; descent is negative */
    float unit_scale;              /* pixels per font unit at 1 px/em */
    int32_t *adv;                  /* full unsigned hmtx width; INT32_MIN = not looked up */
    uint16_t lo_glyph[0x250];      /* glyph index for code points below 0x250, 0xFFFF = not looked up */
    uint8_t fallback;             /* font_open は単一 face、family/Web face は欠字を補完する */
    bool merged_metrics;         /* 統合版の巨大な全字形 hhea を通常本文の行箱に使わない */
    int bold_units;              /* synthetic bold: extra advance and stroke, font units (0 = none) */
    bool web_face;               /* validated TTF; explicit-stack composite renderer */
    FT_Face outline;             /* bounded OpenType CFF/CFF2 fallback only */
    bool colr;                   /* COLRv1 paint graph rendered as RGBA glyphs */
    bool gsub_tried;
    unsigned char *gsub;         /* emoji sequence ligatures (GSUB type 4) */
    size_t gsub_len;
};

static uint64_t next_font_id = 1;
static uint64_t font_metrics_epoch = 1;
static FT_Library web_font_library;

uint64_t font_metrics_generation(void) { return font_metrics_epoch; }

static font_t *font_adopt(unsigned char *data, bool web_face) {
    font_t *f = calloc(1, sizeof *f);
    if (!next_font_id || !f || !stbtt_InitFont(&f->info, data, web_face ? 0 : stbtt_GetFontOffsetForIndex(data, 0))) {
        free(f);
        free(data);
        return NULL;
    }
    f->data = data;
    f->id = next_font_id++;
    f->web_face = web_face;
    /* A downloaded face supplies only its own glyphs. Missing characters use
     * the same bundled selection in measurement and paint; the owned web bytes
     * and primary metrics remain unchanged. Explicit font_open stays single-face. */
    if (web_face) f->fallback = FALLBACK_SANS;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    f->unit_scale = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
    f->adv = malloc(sizeof *f->adv * (size_t)(f->info.numGlyphs > 0 ? f->info.numGlyphs : 1));
    if (!f->adv) { free(f->data); free(f); return NULL; }
    for (int i = 0; i < f->info.numGlyphs; i++) f->adv[i] = INT32_MIN;
    memset(f->lo_glyph, 0xFF, sizeof f->lo_glyph);
    /* A successful lazy fallback changes the available measurement backend.
     * Unsigned wrap disables reuse permanently rather than creating an ABA. */
    if (font_metrics_epoch) font_metrics_epoch++;
    return f;
}

font_t *font_open(const char *path) {
    if(font_probe_depth){font_probe_failed=true;return NULL;}
    FILE *fp=fopen(path,"rb");if(!fp)return NULL;
    fseek(fp,0,SEEK_END);long n=ftell(fp);fseek(fp,0,SEEK_SET);
    unsigned char *data=n>0?malloc((size_t)n):NULL;
    if(!data || fread(data,1,(size_t)n,fp)!=(size_t)n){fclose(fp);free(data);return NULL;}
    fclose(fp);return font_adopt(data,false);
}

font_t *font_open_memory(const void *data,size_t length) {
    if(font_probe_depth){font_probe_failed=true;return NULL;}
    if(!data || !length || length>INT_MAX)return NULL;
    const unsigned char *input=data;
    /* EOT-Lite is only a container around SFNT. Do not misinterpret compressed
     * MicroType Express/EUDC packages as raw outlines. WOFF is preferred by CSS.
     * XOR, when declared, belongs solely to the owned FontData bytes. */
    bool xor_eot=false;
    if(length>=82 && input[34]==0x4c && input[35]==0x50) {
        uint32_t total=sf_le32(input),font_size=sf_le32(input+4),version=sf_le32(input+8),flags=sf_le32(input+12);
        if(total!=length || font_size>length-82 || !font_size ||
           (version!=0x10000 && version!=0x20001 && version!=0x20002) || (flags&0x24))return NULL;
        xor_eot=(flags&0x10000000)!=0;input+=length-font_size;length=font_size;
    }
    unsigned char *copy=malloc(length);if(!copy)return NULL;
    memcpy(copy,input,length);
    if(xor_eot)for(size_t i=0;i<length;i++)copy[i]^=0x50;
    /* Keep the established TrueType raster/metrics exactly unchanged. Other
     * containers are opened by FreeType's byte-bounded SFNT parsers and native
     * Brotli/zlib decoder, never by an external helper or a site exception. */
    if(font_sfnt_valid(copy,length))return font_adopt(copy,true);
    uint32_t signature=length>=4?sf_u32(copy):0;
    if(signature!=0x774f4646 && signature!=0x774f4632 && signature!=0x4f54544f) {free(copy);return NULL;}
    if(!web_font_library && FT_Init_FreeType(&web_font_library)) {free(copy);return NULL;}
    FT_Face outline=NULL;
    if(FT_New_Memory_Face(web_font_library,copy,(FT_Long)length,0,&outline) ||
       !outline || !FT_IS_SCALABLE(outline) || outline->num_glyphs<=0 || outline->num_glyphs>INT_MAX ||
       !outline->units_per_EM || FT_Select_Charmap(outline,FT_ENCODING_UNICODE)) {
        if(outline)FT_Done_Face(outline);free(copy);return NULL;
    }
    const char *format=FT_Get_Font_Format(outline);
    if(format && !strcmp(format,"TrueType")) {
        /* FreeType replaces WOFF/WOFF2's stream with reconstructed SFNT. Reuse
         * the strict native glyph gate and existing composite rasterizer after
         * expansion; compressed input never bypasses our rendering checks. */
        FT_ULong bytes=0;
        unsigned char *sfnt=NULL;
        if(!FT_Load_Sfnt_Table(outline,0,0,NULL,&bytes) && bytes && bytes<=INT_MAX) {
            sfnt=malloc(bytes);
            if(sfnt && (FT_Load_Sfnt_Table(outline,0,0,sfnt,&bytes) || !font_sfnt_valid(sfnt,bytes))) {free(sfnt);sfnt=NULL;}
        }
        FT_Done_Face(outline);free(copy);
        return sfnt?font_adopt(sfnt,true):NULL;
    }
    if(!format || strcmp(format,"CFF") || !next_font_id) {FT_Done_Face(outline);free(copy);return NULL;}
    font_t *f=calloc(1,sizeof *f);
    if(!f){FT_Done_Face(outline);free(copy);return NULL;}
    f->adv=malloc(sizeof *f->adv*(size_t)outline->num_glyphs);
    if(!f->adv){FT_Done_Face(outline);free(copy);free(f);return NULL;}
    f->data=copy;f->outline=outline;f->web_face=true;f->fallback=FALLBACK_SANS;f->id=next_font_id++;
    f->info.numGlyphs=(int)outline->num_glyphs;f->ascent=outline->ascender;f->descent=outline->descender;
    f->line_gap=outline->height-f->ascent+f->descent;f->unit_scale=1.0f/outline->units_per_EM;
    for(int i=0;i<f->info.numGlyphs;i++)f->adv[i]=INT32_MIN;
    memset(f->lo_glyph,0xff,sizeof f->lo_glyph);
    if(font_metrics_epoch)font_metrics_epoch++;
    return f;
}

/* The complete path of a bundled face: in C:\Nocturne\Fonts unless the launch setting FONT_DIR names another folder. */
static bool bundled_path(const char *name, char *path, size_t size) {
    const char *dir = getenv("FONT_DIR");
    return n_path_join(dir && *dir ? dir : "C:\\Nocturne\\Fonts", name, path, size) >= 0;
}

static font_t *open_bundled(const char *name) {
    char path[1024];
    return bundled_path(name, path, sizeof path) ? font_open(path) : NULL;
}

/* A bundled color face is read through a file stream: FreeType extracts only
   the tables it parses (cmap, hmtx, loca, COLR, CPAL) and each glyf record on
   demand, so the font's large SVG table never enters process memory. */
static unsigned long file_stream_read(FT_Stream s, unsigned long offset, unsigned char *buffer, unsigned long count) {
    FILE *fp = s->descriptor.pointer;
    if (offset > s->size || fseek(fp, (long)offset, SEEK_SET)) return count ? 0 : 1;
    return count ? (unsigned long)fread(buffer, 1, count, fp) : 0;
}

static font_t *open_bundled_stream(const char *name) {
    char path[1024];
    if (!bundled_path(name, path, sizeof path)) return NULL;
    if (!next_font_id || (!web_font_library && FT_Init_FreeType(&web_font_library))) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    FT_Stream stream = calloc(1, sizeof *stream);
    font_t *f = calloc(1, sizeof *f);
    FT_Face face = NULL;
    long n = stream && f && !fseek(fp, 0, SEEK_END) ? ftell(fp) : -1;
    if (n > 0) {
        stream->size = (unsigned long)n;
        stream->descriptor.pointer = fp;
        stream->read = file_stream_read;
        FT_Open_Args args = {.flags = FT_OPEN_STREAM, .stream = stream};
        if (FT_Open_Face(web_font_library, &args, 0, &face) || !FT_IS_SCALABLE(face) ||
            face->num_glyphs <= 0 || face->num_glyphs > INT_MAX || !face->units_per_EM ||
            FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
            if (face) FT_Done_Face(face);
            face = NULL;
        }
    }
    if (face) f->adv = malloc(sizeof *f->adv * (size_t)face->num_glyphs);
    if (!face || !f->adv) {
        if (face) FT_Done_Face(face);
        if (f) free(f->adv);
        free(f); free(stream); fclose(fp);
        return NULL;
    }
    f->outline = face; f->colr = true; f->id = next_font_id++;
    f->info.numGlyphs = (int)face->num_glyphs; f->ascent = face->ascender; f->descent = face->descender;
    f->line_gap = face->height - f->ascent + f->descent; f->unit_scale = 1.0f / face->units_per_EM;
    for (int i = 0; i < f->info.numGlyphs; i++) f->adv[i] = INT32_MIN;
    memset(f->lo_glyph, 0xff, sizeof f->lo_glyph);
    if (font_metrics_epoch) font_metrics_epoch++;
    return f;
}

/* 欠字を補う同梱の面。字を探す道は一つで数十 MB の面をいくつも通るので、
   面はまず cmap だけで知り、その字を持つときに初めてファイル全体を読む。
   cmap を読めない・検査に通らない面は、従来どおり開いてから字を尋ねる。 */
struct bundled_face {
    const char *name;
    bool stream;            /* カラーの面：FreeType がファイルから表を読む */
    uint8_t fallback;       /* 本文の主な面として使うときの欠字の補い方 */
    bool merged_metrics;
    bool probed, opened;
    unsigned char *cmap;    /* 面を開くまでの cmap 表（NULL＝分からない） */
    size_t subtable;        /* cmap の中で使う Unicode の小表の位置 */
    font_t *face;
};

static bool read_at(FILE *fp, long at, void *buffer, size_t n) {
    return !fseek(fp, at, SEEK_SET) && fread(buffer, 1, n, fp) == n;
}

/* 表の目録・maxp の字形数（cmap の検査に要る）・cmap だけを読む。 */
static unsigned char *read_cmap(FILE *fp, size_t *subtable) {
    unsigned char head[12], maxp[6];
    long size = fseek(fp, 0, SEEK_END) ? -1 : ftell(fp);
    if (size <= 0 || !read_at(fp, 0, head, sizeof head) ||
        (sf_u32(head) != 0x00010000u && sf_u32(head) != 0x4f54544fu)) return NULL;
    struct sf_span file = {NULL, (size_t)size}; /* 範囲の検査だけに使う */
    unsigned count = sf_u16(head + 4);
    size_t directory = 12 + (size_t)count * 16;
    unsigned char *records = sf_has(file, 0, directory) ? malloc(directory) : NULL;
    uint32_t cmap_at, cmap_n, maxp_at, maxp_n;
    bool found = records && read_at(fp, 0, records, directory) &&
                 sf_record(records, count, "cmap", &cmap_at, &cmap_n) &&
                 sf_record(records, count, "maxp", &maxp_at, &maxp_n);
    free(records);
    if (!found || !cmap_n || !sf_has(file, cmap_at, cmap_n) || maxp_n < sizeof maxp ||
        !sf_has(file, maxp_at, sizeof maxp) || !read_at(fp, maxp_at, maxp, sizeof maxp)) return NULL;
    unsigned char *cmap = malloc(cmap_n);
    const unsigned char *chosen;
    if (cmap && read_at(fp, cmap_at, cmap, cmap_n) &&
        sf_cmap((struct sf_span){cmap, cmap_n}, sf_u16(maxp + 4), &chosen)) {
        *subtable = (size_t)(chosen - cmap);
        return cmap;
    }
    free(cmap);
    return NULL;
}

static font_t *bundled_face_open(struct bundled_face *b) {
    if (!b->opened) {
        if(font_probe_depth){font_probe_failed=true;return NULL;}
        b->opened = true;
        b->face = b->stream ? open_bundled_stream(b->name) : open_bundled(b->name);
        if (b->face) {
            b->face->fallback = b->fallback;
            b->face->merged_metrics = b->merged_metrics;
        }
        free(b->cmap);
        b->cmap = NULL;
    }
    return b->face;
}

/* 面がその字を持ちうるか。開くまでは cmap だけで答える。 */
static bool bundled_face_maps(struct bundled_face *b, uint32_t cp) {
    if (b->opened) return b->face != NULL;
    if (!b->probed) {
        if(font_probe_depth){font_probe_failed=true;return false;}
        b->probed = true;
        char path[1024];
        FILE *fp = bundled_path(b->name, path, sizeof path) ? fopen(path, "rb") : NULL;
        if (fp) {
            b->cmap = read_cmap(fp, &b->subtable);
            fclose(fp);
        }
    }
    return !b->cmap || sf_cmap_glyph(b->cmap + b->subtable, cp);
}

/* 指定された実験的統合版の Regular 四ファイルだけ。style ごとの再読込や
   合成 italic はしない（bold は noto_bold が同じ face から合成する）。Historical も必要な字形に出会うまで開かない。 */
static struct bundled_face noto[4] = {
    {"NotoSansLiving-Regular.ttf", .fallback = FALLBACK_SANS, .merged_metrics = true},
    {"NotoSansHistorical-Regular.ttf", .fallback = FALLBACK_SANS, .merged_metrics = true},
    {"NotoSerifLiving-Regular.ttf", .fallback = FALLBACK_SERIF, .merged_metrics = true},
    {"NotoSerifHistorical-Regular.ttf", .fallback = FALLBACK_SERIF, .merged_metrics = true},
};
static struct bundled_face shared_cjk[3] = {
    {"MapleMono-NF-CN-Regular.ttf"}, {"PlangothicP2-Regular.ttf"}, {"PlangothicP1-Regular.ttf"},
};
/* 本文用の日本語 Regular 二つだけ。UI/mono はこの候補を呼ばない。
   CFF OTF を元の bytes のまま一度だけ読み、各 style で共有する。 */
static struct bundled_face family_cjk[2] = {{"NotoSansCJKjp-Regular.otf"}, {"NotoSerifCJKjp-Regular.otf"}};
/* 絵文字の最終候補。開くのは絵文字に出会ってからだけ。 */
static struct bundled_face emoji = {"NotoColorEmoji-Regular.ttf", .stream = true};

font_t *font_ui(int style) {
    static font_t *cache[4];
    static bool tried[4];
    style &= 3;
    if (!tried[style]) {
        if(font_probe_depth){font_probe_failed=true;return NULL;}
        static const char *names[4] = {"MapleMono-NF-Regular.ttf", "MapleMono-NF-Bold.ttf",
                                      "MapleMono-NF-Italic.ttf", "MapleMono-NF-BoldItalic.ttf"};
        cache[style] = open_bundled(names[style]);
        if (cache[style]) cache[style]->fallback = FALLBACK_UI;
        tried[style] = true;
        if (!cache[style] && style) cache[style] = font_ui(FONT_REGULAR); /* a missing style falls back */
    }
    return cache[style];
}

static font_t *noto_face(int which) { return bundled_face_open(&noto[which]); }

/* The bundled families have one Regular file each. Bold is the same outlines
   made heavier by a 24th of an em (strokes and advances), as a face of its
   own so that the glyph and face caches keep the two apart. Italic stays
   upright. */
/* The bold face of a bundled face (also used for the CJK and other faces that
   fill in missing glyphs of bold text). Colour emoji keep their own paint. */
static font_t *bold_of(font_t *base) {
    static struct { font_t *base, *bold; } made[16];
    static int count;
    if (!base || base->colr || base->bold_units || base->web_face) return base;
    for (int i = 0; i < count; i++)
        if (made[i].base == base) return made[i].bold;
    if(font_probe_depth){font_probe_failed=true;return base;}
    if (count == 16 || !next_font_id) return base;
    font_t *f = malloc(sizeof *f);
    if (!f) return base;
    *f = *base; /* shares the bytes, outlines and advance table; never freed */
    f->id = next_font_id++;
    f->bold_units = (int)(1.0f / (24.0f * base->unit_scale) + 0.5f);
    if (!f->bold_units) f->bold_units = 1;
    if (font_metrics_epoch) font_metrics_epoch++;
    made[count].base = base;
    made[count++].bold = f;
    return f;
}

static font_t *noto_bold(int which) { return bold_of(noto_face(which)); }

font_t *font_family(int family, int style) {
    if (family == FONT_FAMILY_MONO) return font_ui(style);
    int which = family == FONT_FAMILY_SERIF ? 2 : 0;
    font_t *f = style & FONT_BOLD ? noto_bold(which) : noto_face(which);
    if (!f) f = style & FONT_BOLD ? noto_bold(2 - which) : noto_face(2 - which);
    return f ? f : font_ui(style);
}

static int glyph_of(font_t *f, uint32_t cp) {
    if(f->outline)return (int)FT_Get_Char_Index(f->outline,cp);
    /* Use the validated mapping (the bundled stb path omits format 4's idDelta
     * on a nonzero glyphIdArray entry). */
    if(f->web_face)return (int)sf_cmap_glyph(f->data+f->info.index_map,cp);
    if (cp < 0x250) {
        if(font_probe_depth && f->lo_glyph[cp]==0xFFFF){font_probe_failed=true;return 0;}
        if (f->lo_glyph[cp] == 0xFFFF) f->lo_glyph[cp] = (uint16_t)stbtt_FindGlyphIndex(&f->info, (int)cp);
        return f->lo_glyph[cp];
    }
    return stbtt_FindGlyphIndex(&f->info, (int)cp);
}

static font_t *try_glyph(font_t *f, uint32_t cp, int *glyph) {
    int g = f ? glyph_of(f, cp) : 0;
    if (!g) return NULL;
    *glyph = g;
    return f;
}

static font_t *try_bundled(struct bundled_face *b, uint32_t cp, int *glyph) {
    return bundled_face_maps(b, cp) ? try_glyph(bundled_face_open(b), cp, glyph) : NULL;
}

/* 仮名・漢字・ハングルの代表的ブロックでは CJK 候補を先に試す。
   優先順だけの判定であり、この外の文字を拒否する収録範囲の制限ではない。 */
static bool cjk_codepoint(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0x1100 && cp <= 0x11ff) ||
           (cp >= 0xa960 && cp <= 0xa97f) || (cp >= 0xac00 && cp <= 0xd7ff) ||
           (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff66 && cp <= 0xffdc) ||
           (cp >= 0x1aff0 && cp <= 0x1b2ff) || (cp >= 0x20000 && cp <= 0x3ffff);
}

static bool private_codepoint(uint32_t cp) {
    return (cp >= 0xe000 && cp <= 0xf8ff) || (cp >= 0xf0000 && cp <= 0xffffd) ||
           (cp >= 0x100000 && cp <= 0x10fffd);
}

/* 収録判定・測定・描画は必ず同じ実 face を選ぶ。候補は有限で、必要時だけ
   読み込み、候補の glyph_face へ再帰しない。既存 UI/CN の優先順は維持する。 */
static font_t *fallback_face(font_t *f, uint32_t cp, int *glyph) {
    *glyph = glyph_of(f, cp);
    /* CSS Fonts character handling forbids installed/generic fallback for PUA:
     * a web icon's private mapping must not become an unrelated bundled symbol. */
    if (!*glyph && f->fallback != FALLBACK_NONE && cp > ' ' &&
        !(f->web_face && private_codepoint(cp))) {
        font_t *face;
        /* Emoji_Presentation の主な面は絵文字書体を先に、他は最後の候補にする。 */
        bool emoji_first = cp >= 0x1f000 && cp <= 0x1faff;
        if (emoji_first && (face = try_bundled(&emoji, cp, glyph))) return face;
        bool cjk_first = f->fallback == FALLBACK_UI || cjk_codepoint(cp);
        if (cjk_first) {
            if (f->fallback != FALLBACK_UI &&
                (face = try_bundled(&family_cjk[f->fallback == FALLBACK_SERIF], cp, glyph))) return face;
            for (int i = 0; i < 3; i++)
                if ((face = try_bundled(&shared_cjk[i], cp, glyph))) return face;
        }
        int first = f->fallback == FALLBACK_SERIF ? 2 : 0;
        for (int i = 0; i < 4; i++) {
            struct bundled_face *candidate = &noto[(first + i) & 3];
            if (candidate->face != f && (face = try_bundled(candidate, cp, glyph))) return face;
        }
        if (!cjk_first)
            for (int i = 0; i < 3; i++)
                if ((face = try_bundled(&shared_cjk[i], cp, glyph))) return face;
        if (!emoji_first && (face = try_bundled(&emoji, cp, glyph))) return face;
    }
    return f;
}

/* Bold text stays bold in the faces that fill in its missing glyphs (CJK). */
static font_t *glyph_face_uncached(font_t *f, uint32_t cp, int *glyph) {
    font_t *face = fallback_face(f, cp, glyph);
    return f->bold_units && face != f ? bold_of(face) : face;
}

/* 同じ文字の測定・収録判定・描画で高 Unicode の cmap と fallback 群を
   毎回たどらない。face と候補順は不変で、欠落結果も記憶する。
   サイズ／位置には依存しない。固定 4096 件、4-way の局所置換で全消去しない。
   font は現在プロセス寿命まで保持され、id は再利用されない。 */
enum { FACE_CACHE_SETS = 1024, FACE_CACHE_WAYS = 4 };
struct face_entry { uint64_t font; uint32_t cp; font_t *face; int glyph; };
static struct face_entry face_cache[FACE_CACHE_SETS][FACE_CACHE_WAYS];
static uint8_t face_victim[FACE_CACHE_SETS];

static font_t *glyph_face(font_t *f, uint32_t cp, int *glyph) {
    /* 主 face にある Latin は既存の小さな cmap 配列だけで済ませる。 */
    if (cp < 0x250) {
        *glyph = glyph_of(f, cp);
        if (*glyph || f->fallback == FALLBACK_NONE || cp <= ' ') return f;
    }
    uint32_t h = (uint32_t)f->id * 2654435761u ^ cp * 40503u;
    unsigned set = (h ^ (h >> 16)) & (FACE_CACHE_SETS - 1);
    struct face_entry *slot = NULL;
    for (unsigned i = 0; i < FACE_CACHE_WAYS; i++) {
        struct face_entry *e = &face_cache[set][i];
        if (e->font == f->id && e->cp == cp) { *glyph = e->glyph; return e->face; }
        if (!e->font && !slot) slot = e;
    }
    if(font_probe_depth){font_probe_failed=true;*glyph=0;return f;}
    font_t *face = glyph_face_uncached(f, cp, glyph);
    if (!slot) slot = &face_cache[set][face_victim[set]++ & (FACE_CACHE_WAYS - 1)];
    slot->font = f->id; slot->cp = cp; slot->face = face; slot->glyph = *glyph;
    return face;
}

static int glyph_adv(font_t *f, int g) {
    if (g < 0 || g >= f->info.numGlyphs) return 0;
    if (f->adv[g] == INT32_MIN) {
        if(font_probe_depth){font_probe_failed=true;return 0;}
        int a, lsb;
        if(f->outline) {
            if(FT_Load_Glyph(f->outline,(FT_UInt)g,FT_LOAD_NO_SCALE|FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP))return 0;
            FT_Pos advance=f->outline->glyph->metrics.horiAdvance;
            if(advance<=INT32_MIN || advance>INT32_MAX)return 0;
            a=(int)advance;
        } else stbtt_GetGlyphHMetrics(&f->info, g, &a, &lsb);
        f->adv[g] = a;
    }
    return f->adv[g] + f->bold_units;
}

void font_metrics(font_t *f, float px, float *ascent, float *descent, float *line_gap) {
    /* 統合版の約 2.7 em hhea で通常行高を広げない。行箱は従来どおりの
       固定比率（ascent 0.969 em、descent 0.242 em）にし、glyph の em scale / advance / raster と原版 bytes は保つ。
       特殊な tall glyph の縦 extent を完全に行箱へ統合する shaping ではない。 */
    if (f->merged_metrics) {
        if (ascent) *ascent = px * 0.969f;
        if (descent) *descent = px * 0.242f;
        if (line_gap) *line_gap = 0;
        return;
    }
    float s = f->unit_scale * px;
    if (ascent) *ascent = f->ascent * s;
    if (descent) *descent = -f->descent * s;
    if (line_gap) *line_gap = f->line_gap * s;
}

/* Unicode Default_Ignorable_Code_Point (the subset that reaches text runs):
   the soft hyphen, variation selectors, joiners, bidi and invisible format
   controls. Without a shaping table they have no visible glyph and no advance;
   a line broken at a soft hyphen shows its hyphen as text of its own. */
static bool default_ignorable(uint32_t cp) {
    return cp == 0x00ad || cp == 0x034f || (cp >= 0x180b && cp <= 0x180f) || (cp >= 0x200b && cp <= 0x200f) ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
           (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff || (cp >= 0x1bca0 && cp <= 0x1bca3) ||
           (cp >= 0xe0000 && cp <= 0xe0fff);
}

/* VS16 asks for the emoji presentation of the preceding character (UTS #51);
   measurement and paint make the same choice from the same lookahead. */
static font_t *present_face(font_t *f, uint32_t cp, uint32_t next, int *glyph) {
    font_t *e;
    if (next == 0xfe0f && (e = try_bundled(&emoji, cp, glyph))) return e;
    return glyph_face(f, cp, glyph);
}

static uint32_t peek_cp(const char *s, const char *end) {
    uint32_t cp = 0;
    if (s < end) gfx_utf8_decode(s, &cp);
    return cp;
}

/* ---- emoji sequences (UTS #51): ZWJ, modifier, flag and tag sequences are
   ligatures in the color face's GSUB. Only LigatureSubst (type 4, also via
   Extension type 7) is applied; every offset is bounds-checked. */
static const unsigned char *gsub_table(font_t *f, size_t *len) {
    if (!f->gsub_tried && f->outline) {
        f->gsub_tried = true;
        FT_ULong n = 0;
        FT_ULong tag = FT_MAKE_TAG('G', 'S', 'U', 'B');
        if (!FT_Load_Sfnt_Table(f->outline, tag, 0, NULL, &n) && n >= 10 && n <= (1u << 24) &&
            (f->gsub = malloc(n)) && !FT_Load_Sfnt_Table(f->outline, tag, 0, f->gsub, &n)) f->gsub_len = n;
        else { free(f->gsub); f->gsub = NULL; }
    }
    *len = f->gsub_len;
    return f->gsub;
}

static int gsub_coverage(const unsigned char *t, size_t len, size_t at, unsigned glyph) {
    if (at + 4 > len) return -1;
    unsigned format = sf_u16(t + at), count = sf_u16(t + at + 2);
    if (format == 1) {
        if (at + 4 + (size_t)count * 2 > len) return -1;
        unsigned lo = 0, hi = count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2, g = sf_u16(t + at + 4 + mid * 2);
            if (g == glyph) return (int)mid;
            if (g < glyph) lo = mid + 1; else hi = mid;
        }
    } else if (format == 2) {
        if (at + 4 + (size_t)count * 6 > len) return -1;
        unsigned lo = 0, hi = count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            const unsigned char *r = t + at + 4 + mid * 6;
            if (glyph < sf_u16(r)) hi = mid;
            else if (glyph > sf_u16(r + 2)) lo = mid + 1;
            else return (int)(sf_u16(r + 4) + glyph - sf_u16(r));
        }
    }
    return -1;
}

/* Longest ligature over gl[0..n) in any type-4 subtable; 0 when none. */
static unsigned gsub_ligature(font_t *f, const uint16_t *gl, int n, int *used) {
    size_t len;
    const unsigned char *t = gsub_table(f, &len);
    if (!t) return 0;
    size_t list = sf_u16(t + 8);
    if (list + 2 > len) return 0;
    unsigned lookups = sf_u16(t + list), best = 0;
    int best_n = 0;
    for (unsigned l = 0; l < lookups; l++) {
        if (list + 2 + (size_t)l * 2 + 2 > len) break;
        size_t lk = list + sf_u16(t + list + 2 + l * 2);
        if (lk + 6 > len) continue;
        unsigned type = sf_u16(t + lk), subs = sf_u16(t + lk + 4);
        if (type != 4 && type != 7) continue;
        for (unsigned s = 0; s < subs; s++) {
            if (lk + 6 + (size_t)s * 2 + 2 > len) break;
            size_t st = lk + sf_u16(t + lk + 6 + s * 2);
            if (type == 7) {
                if (st + 8 > len || sf_u16(t + st) != 1 || sf_u16(t + st + 2) != 4) continue;
                uint32_t ext = sf_u32(t + st + 4);
                if (ext > len - st) continue;
                st += ext;
            }
            if (st + 6 > len || sf_u16(t + st) != 1) continue;
            int index = gsub_coverage(t, len, st + sf_u16(t + st + 2), gl[0]);
            if (index < 0 || (unsigned)index >= sf_u16(t + st + 4) || st + 6 + (size_t)index * 2 + 2 > len) continue;
            size_t set = st + sf_u16(t + st + 6 + index * 2);
            if (set + 2 > len) continue;
            unsigned ligs = sf_u16(t + set);
            for (unsigned k = 0; k < ligs; k++) {
                if (set + 2 + (size_t)k * 2 + 2 > len) break;
                size_t lig = set + sf_u16(t + set + 2 + k * 2);
                if (lig + 4 > len) continue;
                unsigned comps = sf_u16(t + lig + 2);
                if (!comps || (int)comps > n || (int)comps <= best_n || lig + 4 + (size_t)(comps - 1) * 2 > len) continue;
                bool match = true;
                for (unsigned c = 1; c < comps && match; c++) match = sf_u16(t + lig + 4 + (c - 1) * 2) == gl[c];
                if (match) { best = sf_u16(t + lig); best_n = (int)comps; }
            }
        }
    }
    *used = best_n;
    return best_n > 1 ? best : 0;
}

static bool emoji_joins(uint32_t prev, uint32_t cp, int count) {
    return cp == 0x200d || prev == 0x200d || (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
           (cp >= 0xe0020 && cp <= 0xe007f) || cp == 0x20e3 ||
           (count == 1 && prev >= 0x1f1e6 && prev <= 0x1f1ff && cp >= 0x1f1e6 && cp <= 0x1f1ff);
}

/* s is just past cp. On a ligature, *glyph and *after cover the sequence;
   VS16 inside the sequence is absorbed, as the font's sequences omit it. */
static bool emoji_sequence(font_t *e, uint32_t cp, const char *s, const char *end, int *glyph, const char **after) {
    uint16_t gl[16];
    const char *pos[16];
    uint32_t prev = cp;
    int count = 1;
    gl[0] = (uint16_t)*glyph;
    pos[0] = s;
    while (s < end && count < 16) {
        uint32_t next;
        int bytes = gfx_utf8_decode(s, &next);
        if (next == 0xfe0f) { s += bytes; pos[count - 1] = s; continue; }
        if (!emoji_joins(prev, next, count)) break;
        int g = glyph_of(e, next);
        if (!g || g > UINT16_MAX) break;
        gl[count] = (uint16_t)g;
        s += bytes;
        pos[count++] = s;
        prev = next;
    }
    if (count < 2) return false;
    int used = 0;
    unsigned lig = gsub_ligature(e, gl, count, &used);
    if (!lig) return false;
    *glyph = (int)lig;
    *after = pos[used - 1];
    return true;
}

bool font_has(font_t *f, uint32_t cp) { int g; if (default_ignorable(cp)) return true; glyph_face(f, cp, &g); return g != 0; }
bool font_has_primary(font_t *f,uint32_t cp) { return f && glyph_of(f,cp)!=0; }

/* the advance of a missing glyph: an empty box */
static float tofu_adv(float px) { return px * 0.6f; }

static float advance_next(font_t *f, float px, uint32_t cp, uint32_t next) {
    if (default_ignorable(cp)) return 0;
    int g;
    f = present_face(f, cp, next, &g);
    if (!g && cp > ' ') return tofu_adv(px);
    return glyph_adv(f, g) * f->unit_scale * px;
}

float font_advance(font_t *f, float px, uint32_t cp) { return advance_next(f, px, cp, 0); }

/* The face and glyph for cp, *s just past it. An emoji sequence in the color
   face becomes its ligature and *s moves past the whole sequence. */
static font_t *shape_next(font_t *f, uint32_t cp, const char **s, const char *end, int *glyph) {
    font_t *face = present_face(f, cp, peek_cp(*s, end), glyph);
    if (*glyph && face->colr && *s < end) emoji_sequence(face, cp, *s, end, glyph, s);
    return face;
}

float font_width(font_t *f, float px, const char *s, size_t n) {
    float w = 0;
    const char *end = s + n;
    while (s < end) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        if (default_ignorable(cp)) continue;
        int g;
        font_t *face = shape_next(f, cp, &s, end, &g);
        if (!g) { if (cp > ' ') w += tofu_adv(px); continue; }
        w += glyph_adv(face, g) * face->unit_scale * px;
    }
    return w;
}

/* ---- glyph cache ---- */
#define GC_SIZE 8192 /* slots, a power of two */
#define GC_MAX  6000 /* flush everything beyond this many glyphs */

struct gent {
    uint64_t font;
    int glyph;
    uint16_t size4; /* px * 4 */
    uint8_t phase;  /* x offset in quarter pixels */
    uint8_t used;
    uint8_t color;  /* bmp holds straight-alpha ARGB words, not coverage */
    int16_t x0, y0;
    uint16_t w, h;
    uint8_t *bmp;
};

void font_probe_begin(void){if(!font_probe_depth)font_probe_failed=false;font_probe_depth++;}
bool font_probe_end(void){bool ok=!font_probe_failed;if(font_probe_depth)font_probe_depth--;return ok;}
static struct gent *gc;
static int gc_count;
static uint8_t gamma_tab[256];

static void gc_flush(void) {
    for (int i = 0; i < GC_SIZE; i++)
        if (gc[i].used) free(gc[i].bmp);
    memset(gc, 0, sizeof(struct gent) * GC_SIZE);
    gc_count = 0;
}

void font_close(font_t *f) {
    if(!f || !f->web_face)return;
    /* Remove borrowers before the owned bytes. A full glyph-table flush keeps
     * open-addressing chains intact; no dangling face cache entry survives. */
    for(unsigned i=0;i<FACE_CACHE_SETS;i++)for(unsigned j=0;j<FACE_CACHE_WAYS;j++)
        if(face_cache[i][j].font==f->id || face_cache[i][j].face==f)memset(&face_cache[i][j],0,sizeof face_cache[i][j]);
    if(gc)gc_flush();
    if(f->outline)FT_Done_Face(f->outline);
    free(f->adv);free(f->data);free(f);
    if(font_metrics_epoch)font_metrics_epoch++;
}

/* Web composites never enter stb's recursive glyph parser. Each simple glyph
 * has already passed the byte gate; placement follows the native stb matrix
 * convention, with explicit heap frames and checked vertex representation. */
static int web_glyph_shape(font_t *f,unsigned glyph,stbtt_vertex **vertices) {
    const unsigned char *data=f->data;bool wide=f->info.indexToLocFormat!=0;
    struct sf_span loca={data+f->info.loca,0};
    uint32_t start=sf_glyph_at(loca,glyph,wide),end=sf_glyph_at(loca,glyph+1,wide);
    *vertices=NULL;if(start==end)return 0;
    if(sf_i16(data+f->info.glyf+start)>=0)return stbtt_GetGlyphShape(&f->info,(int)glyph,vertices);
    struct shape_frame {unsigned glyph;size_t at;bool entered,more;float matrix[6],mx,my;};
    struct shape_frame *stack=calloc((size_t)f->info.numGlyphs,sizeof *stack);
    if(!stack)return -1;
    size_t depth=1,count=0,capacity=0;stbtt_vertex *out=NULL;
    stack[0].glyph=glyph;
    while(depth) {
        struct shape_frame *v=&stack[depth-1];
        start=sf_glyph_at(loca,v->glyph,wide);end=sf_glyph_at(loca,v->glyph+1,wide);
        const unsigned char *p=data+f->info.glyf+start;
        if(!v->entered) {
            v->entered=true;v->at=10;v->more=start!=end && sf_i16(p)==-1;
            if(start!=end && sf_i16(p)>0) {
                stbtt_vertex *simple=NULL;int n=stbtt_GetGlyphShape(&f->info,(int)v->glyph,&simple);
                if(n<=0 || !simple)goto bad;
                if((size_t)n>(size_t)INT_MAX/sizeof *out-count){stbtt_FreeShape(&f->info,simple);goto bad;}
                size_t need=count+(size_t)n;
                if(need>capacity) {
                    size_t cap=capacity?capacity:16;
                    while(cap<need){if(cap>(size_t)INT_MAX/sizeof *out/2){cap=need;break;}cap*=2;}
                    void *q=realloc(out,cap*sizeof *out);if(!q){stbtt_FreeShape(&f->info,simple);goto bad;}out=q;capacity=cap;
                }
                for(int i=0;i<n;i++) {
                    stbtt_vertex s=simple[i];
                    for(size_t a=depth;a>1;a--) {
                        const struct shape_frame *t=&stack[a-1];float x=s.x,y=s.y,cx=s.cx,cy=s.cy;
                        float values[4]={t->mx*(t->matrix[0]*x+t->matrix[2]*y+t->matrix[4]),
                            t->my*(t->matrix[1]*x+t->matrix[3]*y+t->matrix[5]),
                            t->mx*(t->matrix[0]*cx+t->matrix[2]*cy+t->matrix[4]),
                            t->my*(t->matrix[1]*cx+t->matrix[3]*cy+t->matrix[5])};
                        for(unsigned z=0;z<4;z++)if(!isfinite(values[z]) || values[z]<INT16_MIN || values[z]>INT16_MAX){stbtt_FreeShape(&f->info,simple);goto bad;}
                        s.x=(int16_t)values[0];s.y=(int16_t)values[1];s.cx=(int16_t)values[2];s.cy=(int16_t)values[3];
                    }
                    out[count++]=s;
                }
                stbtt_FreeShape(&f->info,simple);
            }
        }
        if(!v->more){depth--;continue;}
        struct sf_span bytes={p,end-start};size_t at=v->at;unsigned flags,child;
        if(!sf_component(bytes,&v->at,(unsigned)f->info.numGlyphs,&child,&flags) || depth==(size_t)f->info.numGlyphs)goto bad;
        v->more=(flags&32)!=0;
        struct shape_frame *ch=&stack[depth++];memset(ch,0,sizeof *ch);ch->glyph=child;ch->matrix[0]=ch->matrix[3]=1;
        at+=4;
        if(flags&1){ch->matrix[4]=sf_i16(p+at);ch->matrix[5]=sf_i16(p+at+2);at+=4;}
        else {ch->matrix[4]=(int8_t)p[at];ch->matrix[5]=(int8_t)p[at+1];at+=2;}
        if(flags&8)ch->matrix[0]=ch->matrix[3]=sf_i16(p+at)/16384.0f;
        else if(flags&64){ch->matrix[0]=sf_i16(p+at)/16384.0f;ch->matrix[3]=sf_i16(p+at+2)/16384.0f;}
        else if(flags&128)for(unsigned z=0;z<4;z++)ch->matrix[z]=sf_i16(p+at+2*z)/16384.0f;
        ch->mx=sqrtf(ch->matrix[0]*ch->matrix[0]+ch->matrix[1]*ch->matrix[1]);
        ch->my=sqrtf(ch->matrix[2]*ch->matrix[2]+ch->matrix[3]*ch->matrix[3]);
    }
    free(stack);*vertices=out;return (int)count;
bad:free(stack);free(out);return -1;
}

static bool outline_bitmap(font_t *f,int glyph,unsigned size4,int phase,struct gent *e) {
    FT_Vector offset={(FT_Pos)phase*16,0};
    if(FT_Set_Char_Size(f->outline,0,(FT_F26Dot6)size4*16,72,72))return false;
    FT_Set_Transform(f->outline,NULL,&offset);
    if(FT_Load_Glyph(f->outline,(FT_UInt)glyph,FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP) ||
       FT_Render_Glyph(f->outline->glyph,FT_RENDER_MODE_NORMAL))return false;
    FT_GlyphSlot slot=f->outline->glyph;FT_Bitmap *b=&slot->bitmap;
    if(b->pixel_mode!=FT_PIXEL_MODE_GRAY || b->width>UINT16_MAX || b->rows>UINT16_MAX ||
       slot->bitmap_left<INT16_MIN || slot->bitmap_left>INT16_MAX ||
       slot->bitmap_top< -INT16_MAX || slot->bitmap_top>32768)return false;
    e->x0=(int16_t)slot->bitmap_left;e->y0=(int16_t)-slot->bitmap_top;
    e->w=(uint16_t)b->width;e->h=(uint16_t)b->rows;
    if(e->w && e->h) {
        e->bmp=malloc((size_t)e->w*e->h);if(!e->bmp)return false;
        for(unsigned y=0;y<e->h;y++) {
            const unsigned char *row=b->pitch>=0?b->buffer+(size_t)y*b->pitch:
                b->buffer+(size_t)(e->h-y-1)*(size_t)(-(int64_t)b->pitch);
            memcpy(e->bmp+(size_t)y*e->w,row,e->w);
        }
    }
    return true;
}

/* ---- COLRv1 color glyphs ----
 * FreeType parses the paint graph; it is rasterized here into a premultiplied
 * float surface. Paint coordinates are font units, y up; the matrix maps them
 * to bitmap pixels, y down. Layers composite source-over. */
typedef struct { double xx, xy, dx, yx, yy, dy; } colr_mat;
struct colr_ctx { FT_Face face; const FT_Color *palette; unsigned palette_size; int w, h; unsigned depth; };
struct colr_stop { float off, c[4]; };

static double colr_fix(FT_Fixed v) { return v / 65536.0; }

static colr_mat colr_mul(colr_mat a, colr_mat b) {
    return (colr_mat){a.xx*b.xx + a.xy*b.yx, a.xx*b.xy + a.xy*b.yy, a.xx*b.dx + a.xy*b.dy + a.dx,
                      a.yx*b.xx + a.yy*b.yx, a.yx*b.xy + a.yy*b.yy, a.yx*b.dx + a.yy*b.dy + a.dy};
}

/* Palette entry 0xFFFF is the text foreground; glyph rasters are cached
   independent of text color, so it is drawn black. */
static void colr_color(const struct colr_ctx *c, FT_ColorIndex ci, float out[4]) {
    float r = 0, g = 0, b = 0, a = 1;
    if (ci.palette_index != 0xFFFF && c->palette && ci.palette_index < c->palette_size) {
        FT_Color k = c->palette[ci.palette_index];
        r = k.red / 255.0f; g = k.green / 255.0f; b = k.blue / 255.0f; a = k.alpha / 255.0f;
    }
    a *= ci.alpha / 16384.0f;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    out[0] = r * a; out[1] = g * a; out[2] = b * a; out[3] = a;
}

static void colr_over(float *d, const float s[4], float cov) {
    float k = 1 - s[3] * cov;
    for (int i = 0; i < 4; i++) d[i] = s[i] * cov + d[i] * k;
}

static int colr_stops(struct colr_ctx *c, const FT_ColorLine *line, struct colr_stop **out) {
    FT_ColorStopIterator it = line->color_stop_iterator;
    FT_ColorStop s;
    struct colr_stop *v = NULL;
    int n = 0, cap = 0;
    while (n < 1024 && FT_Get_Colorline_Stops(c->face, &s, &it)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            void *q = realloc(v, (size_t)cap * sizeof *v);
            if (!q) { free(v); return 0; }
            v = q;
        }
        v[n].off = (float)colr_fix(s.stop_offset);
        colr_color(c, s.color, v[n].c);
        n++;
    }
    for (int i = 1; i < n; i++) {
        struct colr_stop t = v[i];
        int j = i;
        while (j > 0 && v[j - 1].off > t.off) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
    *out = v;
    return n;
}

static void colr_lookup(const struct colr_stop *s, int n, FT_PaintExtend ext, float t, float out[4]) {
    float a = s[0].off, b = s[n - 1].off;
    if (ext != FT_COLR_PAINT_EXTEND_PAD && b > a) {
        float u = (t - a) / (b - a);
        if (ext == FT_COLR_PAINT_EXTEND_REPEAT) u -= floorf(u);
        else { u = fmodf(fabsf(u), 2.0f); if (u > 1) u = 2 - u; }
        t = a + u * (b - a);
    }
    if (t <= a) { memcpy(out, s[0].c, sizeof s[0].c); return; }
    if (t >= b) { memcpy(out, s[n - 1].c, sizeof s[0].c); return; }
    int i = 1;
    while (i < n - 1 && s[i].off < t) i++;
    float d = s[i].off - s[i - 1].off, k = d > 0 ? (t - s[i - 1].off) / d : 1;
    for (int z = 0; z < 4; z++) out[z] = s[i - 1].c[z] + (s[i].c[z] - s[i - 1].c[z]) * k;
}

static void colr_gradient(struct colr_ctx *c, const FT_COLR_Paint *p, colr_mat m, float *dst, const uint8_t *clip) {
    double det = m.xx * m.yy - m.xy * m.yx;
    if (fabs(det) < 1e-12) return;
    colr_mat inv = {m.yy / det, -m.xy / det, 0, -m.yx / det, m.xx / det, 0};
    inv.dx = -(inv.xx * m.dx + inv.xy * m.dy);
    inv.dy = -(inv.yx * m.dx + inv.yy * m.dy);
    const FT_ColorLine *line = p->format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT ? &p->u.linear_gradient.colorline :
        p->format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT ? &p->u.radial_gradient.colorline : &p->u.sweep_gradient.colorline;
    struct colr_stop *stops = NULL;
    int n = colr_stops(c, line, &stops);
    if (!n) return;
    /* Linear: p2 sets the iso-color direction; the color line runs from p0 to
       p1 projected onto the normal of p0p2. Radial: two-point conical, the
       largest t with a non-negative radius. Sweep: counter-clockwise degrees,
       1.0 = 180. */
    double x0 = 0, y0 = 0, vx = 0, vy = 0, vv = 0, cdx = 0, cdy = 0, r0 = 0, dr = 0, qa = 0, start = 0, span = 0;
    if (p->format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT) {
        const FT_PaintLinearGradient *l = &p->u.linear_gradient;
        x0 = colr_fix(l->p0.x); y0 = colr_fix(l->p0.y);
        double ux = colr_fix(l->p1.x) - x0, uy = colr_fix(l->p1.y) - y0;
        double nx = colr_fix(l->p2.y) - y0, ny = -(colr_fix(l->p2.x) - x0), nn = nx * nx + ny * ny;
        if (nn > 0) { double k = (ux * nx + uy * ny) / nn; vx = nx * k; vy = ny * k; } else { vx = ux; vy = uy; }
        vv = vx * vx + vy * vy;
        if (vv <= 0) { free(stops); return; }
    } else if (p->format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT) {
        const FT_PaintRadialGradient *r = &p->u.radial_gradient;
        x0 = colr_fix(r->c0.x); y0 = colr_fix(r->c0.y); r0 = colr_fix(r->r0);
        cdx = colr_fix(r->c1.x) - x0; cdy = colr_fix(r->c1.y) - y0; dr = colr_fix(r->r1) - r0;
        qa = cdx * cdx + cdy * cdy - dr * dr;
    } else {
        const FT_PaintSweepGradient *s = &p->u.sweep_gradient;
        x0 = colr_fix(s->center.x); y0 = colr_fix(s->center.y);
        start = colr_fix(s->start_angle) * 180; span = colr_fix(s->end_angle) * 180 - start;
    }
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            size_t i = (size_t)y * c->w + x;
            if (clip && !clip[i]) continue;
            double px = x + 0.5, py = y + 0.5;
            double fx = inv.xx * px + inv.xy * py + inv.dx - x0, fy = inv.yx * px + inv.yy * py + inv.dy - y0;
            double t;
            if (p->format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT) t = (fx * vx + fy * vy) / vv;
            else if (p->format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT) {
                double b = fx * cdx + fy * cdy + r0 * dr, cc = fx * fx + fy * fy - r0 * r0;
                if (fabs(qa) < 1e-9) {
                    if (fabs(b) < 1e-9) continue;
                    t = cc / (2 * b);
                    if (r0 + t * dr < 0) continue;
                } else {
                    double disc = b * b - qa * cc;
                    if (disc < 0) continue;
                    double sq = sqrt(disc), t1 = (b + sq) / qa, t2 = (b - sq) / qa;
                    double hi = t1 > t2 ? t1 : t2, lo = t1 > t2 ? t2 : t1;
                    if (r0 + hi * dr >= 0) t = hi;
                    else if (r0 + lo * dr >= 0) t = lo;
                    else continue;
                }
            } else {
                double ang = atan2(fy, fx) * (180 / 3.14159265358979323846);
                if (ang < 0) ang += 360;
                t = span != 0 ? (ang - start) / span : ang >= start ? 1 : 0;
            }
            float col[4];
            colr_lookup(stops, n, line->extend, (float)t, col);
            colr_over(dst + i * 4, col, clip ? clip[i] / 255.0f : 1.0f);
        }
    free(stops);
}

static bool colr_glyph_mask(struct colr_ctx *c, unsigned gid, colr_mat m, uint8_t *mask) {
    if (FT_Load_Glyph(c->face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM))
        return false;
    FT_GlyphSlot slot = c->face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return false;
    FT_Outline *o = &slot->outline;
    const double lim = 1 << 24;
    for (int i = 0; i < o->n_points; i++) {
        double x = o->points[i].x, y = o->points[i].y;
        double X = (m.xx * x + m.xy * y + m.dx) * 64, Y = (c->h - (m.yx * x + m.yy * y + m.dy)) * 64;
        if (!(X > -lim)) X = -lim; if (!(X < lim)) X = lim;
        if (!(Y > -lim)) Y = -lim; if (!(Y < lim)) Y = lim;
        o->points[i].x = (FT_Pos)lround(X);
        o->points[i].y = (FT_Pos)lround(Y);
    }
    FT_Bitmap b = {0};
    b.rows = (unsigned)c->h; b.width = (unsigned)c->w; b.pitch = c->w;
    b.buffer = mask; b.num_grays = 256; b.pixel_mode = FT_PIXEL_MODE_GRAY;
    return !FT_Outline_Get_Bitmap(web_font_library, o, &b);
}

static void colr_composite(FT_Composite_Mode mode, const float *s, float *d) {
    float sa = s[3], da = d[3], r[4];
    for (int i = 0; i < 4; i++) {
        float sc = s[i], dc = d[i];
        switch (mode) {
        case FT_COLR_COMPOSITE_CLEAR: r[i] = 0; break;
        case FT_COLR_COMPOSITE_SRC: r[i] = sc; break;
        case FT_COLR_COMPOSITE_DEST: r[i] = dc; break;
        case FT_COLR_COMPOSITE_DEST_OVER: r[i] = dc + sc * (1 - da); break;
        case FT_COLR_COMPOSITE_SRC_IN: r[i] = sc * da; break;
        case FT_COLR_COMPOSITE_DEST_IN: r[i] = dc * sa; break;
        case FT_COLR_COMPOSITE_SRC_OUT: r[i] = sc * (1 - da); break;
        case FT_COLR_COMPOSITE_DEST_OUT: r[i] = dc * (1 - sa); break;
        case FT_COLR_COMPOSITE_SRC_ATOP: r[i] = sc * da + dc * (1 - sa); break;
        case FT_COLR_COMPOSITE_DEST_ATOP: r[i] = dc * sa + sc * (1 - da); break;
        case FT_COLR_COMPOSITE_XOR: r[i] = sc * (1 - da) + dc * (1 - sa); break;
        case FT_COLR_COMPOSITE_PLUS: r[i] = fminf(1, sc + dc); break;
        case FT_COLR_COMPOSITE_SCREEN: r[i] = sc + dc - sc * dc; break;
        case FT_COLR_COMPOSITE_MULTIPLY: r[i] = i == 3 ? sa + da - sa * da : sc * (1 - da) + dc * (1 - sa) + sc * dc; break;
        case FT_COLR_COMPOSITE_DARKEN: r[i] = i == 3 ? sa + da - sa * da : sc + dc - fmaxf(sc * da, dc * sa); break;
        case FT_COLR_COMPOSITE_LIGHTEN: r[i] = i == 3 ? sa + da - sa * da : sc + dc - fminf(sc * da, dc * sa); break;
        case FT_COLR_COMPOSITE_DIFFERENCE: r[i] = i == 3 ? sa + da - sa * da : sc + dc - 2 * fminf(sc * da, dc * sa); break;
        case FT_COLR_COMPOSITE_EXCLUSION: r[i] = i == 3 ? sa + da - sa * da : sc + dc - 2 * sc * dc; break;
        default: r[i] = sc + dc * (1 - sa); break; /* SRC_OVER; unimplemented blends degrade to it */
        }
    }
    memcpy(d, r, sizeof r);
}

static bool colr_paint(struct colr_ctx *c, FT_OpaquePaint op, colr_mat m, float *dst, const uint8_t *clip) {
    FT_COLR_Paint p;
    if (c->depth >= 64 || !FT_Get_Paint(c->face, op, &p)) return c->depth < 64;
    c->depth++;
    bool ok = true;
    size_t n = (size_t)c->w * c->h;
    colr_mat t = {1, 0, 0, 0, 1, 0};
    FT_OpaquePaint child = {0};
    bool transform = false;
    switch (p.format) {
    case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
        FT_LayerIterator it = p.u.colr_layers.layer_iterator;
        FT_OpaquePaint layer;
        while (ok && FT_Get_Paint_Layers(c->face, &it, &layer)) ok = colr_paint(c, layer, m, dst, clip);
        break;
    }
    case FT_COLR_PAINTFORMAT_SOLID: {
        float col[4];
        colr_color(c, p.u.solid.color, col);
        for (size_t i = 0; i < n; i++)
            if (!clip || clip[i]) colr_over(dst + i * 4, col, clip ? clip[i] / 255.0f : 1.0f);
        break;
    }
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
    case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
        colr_gradient(c, &p, m, dst, clip);
        break;
    case FT_COLR_PAINTFORMAT_GLYPH: {
        uint8_t *mask = calloc(n, 1);
        if (!mask) { ok = false; break; }
        if (colr_glyph_mask(c, p.u.glyph.glyphID, m, mask)) {
            if (clip) for (size_t i = 0; i < n; i++) mask[i] = (uint8_t)(mask[i] * clip[i] / 255);
            ok = colr_paint(c, p.u.glyph.paint, m, dst, mask);
        }
        free(mask);
        break;
    }
    case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
        FT_OpaquePaint root = {0};
        if (FT_Get_Color_Glyph_Paint(c->face, p.u.colr_glyph.glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &root))
            ok = colr_paint(c, root, m, dst, clip);
        break;
    }
    case FT_COLR_PAINTFORMAT_TRANSFORM: {
        const FT_Affine23 *a = &p.u.transform.affine;
        t = (colr_mat){colr_fix(a->xx), colr_fix(a->xy), colr_fix(a->dx), colr_fix(a->yx), colr_fix(a->yy), colr_fix(a->dy)};
        child = p.u.transform.paint; transform = true;
        break;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE:
        t.dx = colr_fix(p.u.translate.dx); t.dy = colr_fix(p.u.translate.dy);
        child = p.u.translate.paint; transform = true;
        break;
    case FT_COLR_PAINTFORMAT_SCALE: {
        double sx = colr_fix(p.u.scale.scale_x), sy = colr_fix(p.u.scale.scale_y);
        double cx = colr_fix(p.u.scale.center_x), cy = colr_fix(p.u.scale.center_y);
        t = (colr_mat){sx, 0, cx - sx * cx, 0, sy, cy - sy * cy};
        child = p.u.scale.paint; transform = true;
        break;
    }
    case FT_COLR_PAINTFORMAT_ROTATE: {
        double a = colr_fix(p.u.rotate.angle) * 3.14159265358979323846, co = cos(a), si = sin(a);
        double cx = colr_fix(p.u.rotate.center_x), cy = colr_fix(p.u.rotate.center_y);
        t = (colr_mat){co, -si, cx - co * cx + si * cy, si, co, cy - si * cx - co * cy};
        child = p.u.rotate.paint; transform = true;
        break;
    }
    case FT_COLR_PAINTFORMAT_SKEW: {
        double tx = tan(colr_fix(p.u.skew.x_skew_angle) * 3.14159265358979323846);
        double ty = tan(colr_fix(p.u.skew.y_skew_angle) * 3.14159265358979323846);
        double cx = colr_fix(p.u.skew.center_x), cy = colr_fix(p.u.skew.center_y);
        t = (colr_mat){1, -tx, tx * cy, ty, 1, -ty * cx};
        child = p.u.skew.paint; transform = true;
        break;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE: {
        float *back = calloc(n * 4, sizeof *back), *src = calloc(n * 4, sizeof *src);
        if (back && src && colr_paint(c, p.u.composite.backdrop_paint, m, back, clip) &&
            colr_paint(c, p.u.composite.source_paint, m, src, clip)) {
            for (size_t i = 0; i < n; i++) {
                colr_composite(p.u.composite.composite_mode, src + i * 4, back + i * 4);
                colr_over(dst + i * 4, back + i * 4, 1);
            }
        } else ok = false;
        free(back); free(src);
        break;
    }
    default:
        break;
    }
    if (transform) ok = colr_paint(c, child, colr_mul(m, t), dst, clip);
    c->depth--;
    return ok;
}

/* The raster box comes from the glyph's ClipBox (or its advance and the face
   extents), scaled at this size and quarter-pixel phase. */
static bool colr_bitmap(font_t *f, int glyph, unsigned size4, int phase, struct gent *e) {
    FT_Face face = f->outline;
    FT_OpaquePaint root = {0};
    if (!FT_Get_Color_Glyph_Paint(face, (FT_UInt)glyph, FT_COLOR_NO_ROOT_TRANSFORM, &root)) return false;
    FT_Vector offset = {(FT_Pos)phase * 16, 0};
    if (FT_Set_Char_Size(face, 0, (FT_F26Dot6)size4 * 16, 72, 72)) return false;
    FT_Set_Transform(face, NULL, &offset);
    double s = size4 / 4.0 / face->units_per_EM;
    double xmin, xmax, ymin, ymax;
    FT_ClipBox cb;
    if (FT_Get_Color_Glyph_ClipBox(face, (FT_UInt)glyph, &cb)) {
        const FT_Vector *v[4] = {&cb.bottom_left, &cb.top_left, &cb.top_right, &cb.bottom_right};
        xmin = xmax = v[0]->x; ymin = ymax = v[0]->y;
        for (int i = 1; i < 4; i++) {
            xmin = fmin(xmin, v[i]->x); xmax = fmax(xmax, v[i]->x);
            ymin = fmin(ymin, v[i]->y); ymax = fmax(ymax, v[i]->y);
        }
        xmin /= 64; xmax /= 64; ymin /= 64; ymax /= 64;
    } else {
        xmin = phase / 4.0; xmax = glyph_adv(f, glyph) * s + phase / 4.0;
        ymin = face->descender * s; ymax = face->ascender * s;
    }
    FT_Set_Transform(face, NULL, NULL);
    int x0 = (int)floor(xmin), x1 = (int)ceil(xmax), yb = (int)floor(ymin), yt = (int)ceil(ymax);
    int w = x1 - x0, h = yt - yb;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || x0 < INT16_MIN || x0 > INT16_MAX || -yt < INT16_MIN || -yt > INT16_MAX)
        return false;
    float *surface = calloc((size_t)w * h * 4, sizeof *surface);
    uint32_t *out = malloc((size_t)w * h * sizeof *out);
    if (!surface || !out) { free(surface); free(out); return false; }
    struct colr_ctx c = {face, NULL, 0, w, h, 0};
    FT_Palette_Data pd;
    FT_Color *palette = NULL;
    if (!FT_Palette_Data_Get(face, &pd) && !FT_Palette_Select(face, 0, &palette)) {
        c.palette = palette; c.palette_size = pd.num_palette_entries;
    }
    colr_mat base = {s, 0, phase / 4.0 - x0, 0, -s, yt};
    colr_paint(&c, root, base, surface, NULL);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float a = surface[i * 4 + 3];
        if (a <= 0.002f) { out[i] = 0; continue; }
        uint32_t A = (uint32_t)(fminf(a, 1) * 255 + 0.5f), rgb = 0;
        for (int z = 0; z < 3; z++) {
            float v = surface[i * 4 + z] / a;
            rgb = rgb << 8 | (uint32_t)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
        }
        out[i] = A << 24 | rgb;
    }
    free(surface);
    e->x0 = (int16_t)x0; e->y0 = (int16_t)-yt;
    e->w = (uint16_t)w; e->h = (uint16_t)h;
    e->bmp = (uint8_t *)out; e->color = 1;
    return true;
}

/* Synthetic bold: each row spread to the right by s pixels (whole pixels at
   full coverage, the fraction at partial), matching the wider advance. */
static void embolden(struct gent *e, float s) {
    int k = (int)s, w = e->w + k + 1;
    float frac = s - k;
    if (w > UINT16_MAX) return;
    uint8_t *out = calloc((size_t)w, e->h);
    if (!out) return;
    for (int y = 0; y < e->h; y++) {
        const uint8_t *in = e->bmp + (size_t)y * e->w;
        uint8_t *o = out + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int v = 0;
            for (int j = 0; j <= k; j++)
                if (x - j >= 0 && x - j < e->w && in[x - j] > v) v = in[x - j];
            int x2 = x - k - 1;
            if (x2 >= 0 && x2 < e->w) { int p = (int)(in[x2] * frac + 0.5f); if (p > v) v = p; }
            o[x] = (uint8_t)v;
        }
    }
    free(e->bmp);
    e->bmp = out;
    e->w = (uint16_t)w;
}

static struct gent *gc_get(font_t *f, int g, float px, int phase) {
    if (!gc) {
        if(font_probe_depth){font_probe_failed=true;return NULL;}
        gc = calloc(GC_SIZE, sizeof(struct gent));
        if (!gc) return NULL;
        /* coverage -> alpha: a little heavier than linear, so text does not look washed out */
        for (int i = 0; i < 256; i++) gamma_tab[i] = (uint8_t)(255.0 * pow(i / 255.0, 0.78) + 0.5);
    }
    uint16_t size4 = (uint16_t)(px * 4 + 0.5f);
    uint32_t h = (uint32_t)f->id * 2654435761u ^ (uint32_t)g * 40503u ^ (uint32_t)size4 * 977u ^ (uint32_t)phase * 7u;
    for (uint32_t i = 0;; i++) {
        struct gent *e = &gc[(h + i) & (GC_SIZE - 1)];
        if (!e->used) {
            if(font_probe_depth){font_probe_failed=true;return NULL;}
            if (gc_count >= GC_MAX) {
                gc_flush();
                return gc_get(f, g, px, phase);
            }
            if(f->outline) {
                if(!(f->colr && colr_bitmap(f,g,size4,phase,e)) && !outline_bitmap(f,g,size4,phase,e)) {
                    free(e->bmp);memset(e,0,sizeof *e);return NULL;
                }
                if(f->bold_units && !e->color && e->bmp)embolden(e,f->bold_units*f->unit_scale*size4/4.0f);
                e->font=f->id;e->glyph=g;e->size4=size4;e->phase=(uint8_t)phase;e->used=1;
                gc_count++;return e;
            }
            float scale = f->unit_scale * size4 / 4.0f, shift = phase / 4.0f;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
            e->font = f->id;
            e->glyph = g;
            e->size4 = size4;
            e->phase = (uint8_t)phase;
            e->used = 1;
            e->x0 = (int16_t)x0;
            e->y0 = (int16_t)y0;
            e->w = (uint16_t)(x1 > x0 ? x1 - x0 : 0);
            e->h = (uint16_t)(y1 > y0 ? y1 - y0 : 0);
            e->bmp = NULL;
            if (e->w && e->h && (e->bmp = calloc((size_t)e->w,e->h))) {
                if(f->web_face) {
                    stbtt_vertex *vertices=NULL;int count=web_glyph_shape(f,(unsigned)g,&vertices);
                    if(count<0){free(e->bmp);memset(e,0,sizeof *e);return NULL;}
                    stbtt__bitmap bitmap={e->w,e->h,e->w,e->bmp};
                    stbtt_Rasterize(&bitmap,0.35f,vertices,count,scale,scale,shift,0,x0,y0,1,f->info.userdata);
                    free(vertices);
                } else stbtt_MakeGlyphBitmapSubpixel(&f->info, e->bmp, e->w, e->h, e->w, scale, scale, shift, 0, g);
                if (f->bold_units) embolden(e, f->bold_units * scale);
            }
            gc_count++;
            return e;
        }
        if (e->font == f->id && e->glyph == g && e->size4 == size4 && e->phase == phase) return e;
    }
}

static void blit_glyph(canvas_t *c, const struct gent *e, int x, int y, uint32_t color) {
    if (!e->bmp) return;
    int alpha = color >> 24;
    if (e->color) {
        /* Color glyphs keep their own paint; text alpha still applies. */
        const uint32_t *src = (const uint32_t *)e->bmp;
        for (int j = 0; j < e->h; j++) {
            int py = y + e->y0 + j;
            if (py < c->cy0 || py >= c->cy1) continue;
            struct gfx_clip_span span={0};
            if(c->mask_clip&&!gfx_clip_row(c,py,c->cx0,c->cx1,&span))continue;
            uint32_t *dst = c->px + (size_t)py * c->pitch;
            for (int i = 0; i < e->w; i++) {
                int px = x + e->x0 + i;
                uint32_t s = src[(size_t)j * e->w + i];
                if (!(s >> 24) || px < c->cx0 || px >= c->cx1) continue;
                int a = (int)(s >> 24) * alpha / 255;
                if(c->mask_clip&&(px<span.opaque0||px>=span.opaque1))gfx_blend_pixel(c,px,py,((uint32_t)a<<24)|(s&0xffffffu));
                else dst[px] = a >= 255 ? (s | 0xFF000000u) : gfx_mix(dst[px], s, a);
            }
        }
        return;
    }
    for (int j = 0; j < e->h; j++) {
        int py = y + e->y0 + j;
        if (py < c->cy0 || py >= c->cy1) continue;
        struct gfx_clip_span span={0};
        if(c->mask_clip&&!gfx_clip_row(c,py,c->cx0,c->cx1,&span))continue;
        const uint8_t *row = e->bmp + (size_t)j * e->w;
        uint32_t *dst = c->px + (size_t)py * c->pitch;
        for (int i = 0; i < e->w; i++) {
            int px = x + e->x0 + i;
            if (!row[i] || px < c->cx0 || px >= c->cx1) continue;
            int a = gamma_tab[row[i]] * alpha / 255;
            if(c->mask_clip&&(px<span.opaque0||px>=span.opaque1))gfx_blend_pixel(c,px,py,((uint32_t)a<<24)|(color&0xffffffu));
            else dst[px] = a >= 255 ? (color | 0xFF000000u) : gfx_mix(dst[px], color, a);
        }
    }
}

float font_draw(canvas_t *c, font_t *f, float px, float x, int y, const char *s, size_t n, uint32_t color) {
    const char *end = s + n;
    while (s < end) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        if (default_ignorable(cp)) continue;
        int g;
        font_t *face = shape_next(f, cp, &s, end, &g);
        if (!g) {
            if (cp > ' ') {
                float a, d;
                font_metrics(f, px, &a, &d, NULL);
                int bw = (int)(px * 0.5f), bh = (int)(a * 0.75f);
                gfx_rect(c, (int)(x + px * 0.05f), y - bh, bw, bh, color | 0xFF000000u);
                x += tofu_adv(px);
            }
            continue;
        }
        if (cp > ' ') {
            float fx = floorf(x);
            int phase = (int)((x - fx) * 4) & 3;
            struct gent *e = gc_get(face, g, px, phase);
            if (e) blit_glyph(c, e, (int)fx, y, color);
        }
        x += glyph_adv(face, g) * face->unit_scale * px;
    }
    return x;
}

/* Existing application chrome uses 8x16 / 16x32 cells. Preserve that geometry
   and its ABI, but draw the default TrueType face rather than changing the OS.
   Wide bundled glyphs occupy two cells. The kernel retains its bitmap renderer. */
int font_cell_width(uint32_t cp, int size) {
    font_t *f = font_ui(FONT_REGULAR);
    if (!f) return -1;
    float a = font_advance(f, 10, cp);
    int cells = a <= 0 ? 0 : a > 9 ? 2 : 1;
    return cells * (size == FONT_LARGE ? 16 : 8);
}

int font_cell_draw(canvas_t *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int size) {
    font_t *f = font_ui(FONT_REGULAR);
    if (!f) return -1;
    int scale = size == FONT_LARGE ? 2 : 1, w = font_cell_width(cp, size);
    if (bg >> 24) gfx_fill(c, x, y, w, 16 * scale, bg);
    char text[4]; int n;
    if (cp < 0x80) { text[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { text[0] = (char)(0xc0 | cp >> 6); text[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        text[0] = (char)(0xe0 | cp >> 12); text[1] = (char)(0x80 | (cp >> 6 & 63));
        text[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        text[0] = (char)(0xf0 | cp >> 18); text[1] = (char)(0x80 | (cp >> 12 & 63));
        text[2] = (char)(0x80 | (cp >> 6 & 63)); text[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    float px = 12.0f * scale, advance = font_advance(f, px, cp);
    font_draw(c, f, px, x + (w - advance) * 0.5f, y + 12 * scale, text, (size_t)n, fg);
    return w;
}

#undef font_open
#undef font_open_memory
#undef font_close
#undef font_ui
#undef font_family
#undef font_metrics
#undef font_has
#undef font_has_primary
#undef font_advance
#undef font_width
#undef font_metrics_generation
#undef font_draw
#undef font_cell_width
#undef font_cell_draw
#undef font_probe_begin
#undef font_probe_end

static volatile uint32_t font_lock;
static void font_acquire(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&font_lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    while (__atomic_exchange_n(&font_lock, 2, __ATOMIC_ACQUIRE))
        wait_on_address(&font_lock, 2, 100);
}
static void font_release(void) {
    if (__atomic_exchange_n(&font_lock, 0, __ATOMIC_RELEASE) == 2)
        wake_address(&font_lock, 1);
}
font_t * font_open(const char *path) { font_acquire(); font_t * result = font_open_unlocked(path); font_release(); return result; }
font_t * font_open_memory(const void *data, size_t length) { font_acquire(); font_t * result = font_open_memory_unlocked(data, length); font_release(); return result; }
void font_close(font_t *f) { font_acquire(); font_close_unlocked(f); font_release(); }
font_t * font_ui(int style) { font_acquire(); font_t * result = font_ui_unlocked(style); font_release(); return result; }
font_t * font_family(int family, int style) { font_acquire(); font_t * result = font_family_unlocked(family, style); font_release(); return result; }
void font_metrics(font_t *f, float px, float *ascent, float *descent, float *gap) { font_acquire(); font_metrics_unlocked(f, px, ascent, descent, gap); font_release(); }
bool font_has(font_t *f, uint32_t cp) { font_acquire(); bool result = font_has_unlocked(f, cp); font_release(); return result; }
bool font_has_primary(font_t *f,uint32_t cp) { font_acquire();bool result=font_has_primary_unlocked(f,cp);font_release();return result; }
float font_advance(font_t *f, float px, uint32_t cp) { font_acquire(); float result = font_advance_unlocked(f, px, cp); font_release(); return result; }
float font_width(font_t *f, float px, const char *s, size_t n) { font_acquire(); float result = font_width_unlocked(f, px, s, n); font_release(); return result; }
uint64_t font_metrics_generation(void) { font_acquire(); uint64_t result = font_metrics_generation_unlocked(); font_release(); return result; }
float font_draw(canvas_t *c, font_t *f, float px, float x, int y, const char *s, size_t n, uint32_t color) { font_acquire(); float result = font_draw_unlocked(c, f, px, x, y, s, n, color); font_release(); return result; }
int font_cell_width(uint32_t cp, int size) { font_acquire(); int result = font_cell_width_unlocked(cp, size); font_release(); return result; }
int font_cell_draw(canvas_t *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int size) { font_acquire(); int result = font_cell_draw_unlocked(c, x, y, cp, fg, bg, size); font_release(); return result; }
void font_probe_begin(void) { font_acquire(); font_probe_begin_unlocked(); font_release(); }
bool font_probe_end(void) { font_acquire(); bool result = font_probe_end_unlocked(); font_release(); return result; }
