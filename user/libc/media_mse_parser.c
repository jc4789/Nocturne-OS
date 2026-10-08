#include "media_mse_parser.h"
#include <math.h>
#include <string.h>

#define LIMIT ((size_t)32 * 1024 * 1024)
#define MAX_ELEMENTS 32768u
#define MAX_DEPTH 16u
#define MAX_RUNS 256u
#define TAG(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

struct parser { const uint8_t *b; size_t n; unsigned count; char *error; size_t error_size; };
struct box { uint32_t type; size_t start, body, end; };
struct run { size_t first, last; bool known, covered; };
struct fragment { struct run runs[MAX_RUNS]; unsigned count, trafs; bool unknown, explicit_base; };
struct element { uint32_t id; size_t start, body, end; uint64_t size; bool unknown; };

static int fail(struct parser *p, const char *text) {
    if (p->error && p->error_size) {
        size_t i = 0;
        while (text[i] && i + 1 < p->error_size) { p->error[i] = text[i]; ++i; }
        p->error[i] = 0;
    }
    return -1;
}
static uint32_t be32(const uint8_t *b) {
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}
static uint64_t be64(const uint8_t *b) { return ((uint64_t)be32(b)<<32)|be32(b+4); }
static int more(struct parser *p, bool eos) { return eos ? fail(p, "Truncated media segment") : 0; }
static int count(struct parser *p) {
    return ++p->count > MAX_ELEMENTS ? fail(p, "Too many container elements") : 1;
}

/* A zero-sized top-level MP4 box is only closed by EOS. Nested zero sizes
 * consume their finite parent, as defined by ISO BMFF. */
static int box_at(struct parser *p, size_t at, size_t end, bool closed, struct box *b) {
    if (at > end || end - at < 8) return 0;
    uint64_t size = be32(p->b + at);
    size_t header = 8;
    b->type = be32(p->b + at + 4); b->start = at;
    if (size == 1) {
        if (end - at < 16) return 0;
        size = be64(p->b + at + 8); header = 16;
    } else if (!size) {
        if (!closed) return 0;
        size = end - at;
    }
    if (b->type == TAG('u','u','i','d')) header += 16;
    if (size < header || size > LIMIT) return fail(p, "Invalid or oversized MP4 box");
    if (size > end - at) return 0;
    b->body = at + header; b->end = at + (size_t)size;
    return count(p);
}
static bool encrypted_box(uint32_t t) {
    return t == TAG('p','s','s','h') || t == TAG('s','e','n','c') ||
           t == TAG('s','i','n','f') || t == TAG('t','e','n','c') ||
           t == TAG('e','n','c','v') || t == TAG('e','n','c','a') ||
           t == TAG('s','a','i','z') || t == TAG('s','a','i','o');
}
static bool container_box(uint32_t t) {
    return t == TAG('m','o','o','v') || t == TAG('t','r','a','k') ||
           t == TAG('m','d','i','a') || t == TAG('m','i','n','f') ||
           t == TAG('s','t','b','l') || t == TAG('m','v','e','x') ||
           t == TAG('e','d','t','s') || t == TAG('d','i','n','f') ||
           t == TAG('m','o','o','f') || t == TAG('t','r','a','f');
}
static bool visual_entry(uint32_t t) {
    return t == TAG('a','v','c','1') || t == TAG('a','v','c','3') ||
           t == TAG('h','v','c','1') || t == TAG('h','e','v','1') ||
           t == TAG('a','v','0','1') || t == TAG('v','p','0','8') ||
           t == TAG('v','p','0','9') || t == TAG('m','p','4','v');
}
static bool audio_entry(uint32_t t) {
    return t == TAG('m','p','4','a') || t == TAG('O','p','u','s') ||
           t == TAG('f','L','a','C') || t == TAG('a','c','-','3') ||
           t == TAG('e','c','-','3') || t == TAG('a','l','a','c');
}
static int inspect_boxes(struct parser *p, size_t at, size_t end, unsigned depth, bool init);
static int sample_entries(struct parser *p, const struct box *b, unsigned depth) {
    if (b->end - b->body < 8 || p->b[b->body]) return fail(p, "Invalid sample description");
    uint32_t entries = be32(p->b + b->body + 4);
    if (!entries || entries > 256) return fail(p, "Invalid sample description count");
    size_t at = b->body + 8;
    for (uint32_t i = 0; i < entries; ++i) {
        struct box e;
        int r = box_at(p, at, b->end, true, &e);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated sample description");
        if (encrypted_box(e.type)) return fail(p, "Encrypted media is unsupported");
        if (e.end - e.body < 8 || p->b[e.body + 7] != 1 || p->b[e.body + 6])
            return fail(p, "External sample data is unsupported");
        size_t fixed = 0;
        if (visual_entry(e.type)) fixed = 78;
        else if (audio_entry(e.type)) {
            if (e.end - e.body < 28) return fail(p, "Truncated audio sample description");
            unsigned version = ((unsigned)p->b[e.body+8]<<8)|p->b[e.body+9];
            if (version > 2) return fail(p, "Unsupported audio sample description");
            fixed = version == 2 ? 64 : version == 1 ? 44 : 28;
        } else if (e.type == TAG('r','t','p',' ')) {
            /* RTP hint tracks accompany some clear AVC/AAC fragmented MP4s.
             * They are not codec tracks; their hint description is opaque to
             * the playback demuxer after the generic data-reference header. */
            at = e.end; continue;
        } else return fail(p, "Unsupported sample description");
        if (fixed > e.end - e.body) return fail(p, "Truncated sample description");
        r = inspect_boxes(p, e.body + fixed, e.end, depth + 1, true);
        if (r < 0) return r;
        at = e.end;
    }
    return at == b->end ? 1 : fail(p, "Invalid sample description length");
}
static int inspect_boxes(struct parser *p, size_t at, size_t end, unsigned depth, bool init) {
    if (depth > MAX_DEPTH) return fail(p, "MP4 nesting limit exceeded");
    while (at < end) {
        struct box b;
        int r = box_at(p, at, end, true, &b);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated nested MP4 box");
        if (encrypted_box(b.type)) return fail(p, "Encrypted media is unsupported");
        if (b.type == TAG('u','u','i','d')) return fail(p, "Unsupported UUID media extension");
        if (b.type == TAG('s','g','p','d') || b.type == TAG('s','b','g','p')) {
            if (b.end - b.body < 8) return fail(p, "Truncated sample grouping");
            if (be32(p->b+b.body+4) == TAG('s','e','i','g'))
                return fail(p, "Encrypted media is unsupported");
        }
        if (init && (b.type == TAG('s','t','t','s') || b.type == TAG('s','t','s','c') ||
                     b.type == TAG('s','t','c','o') || b.type == TAG('c','o','6','4'))) {
            if (b.end - b.body != 8 || be32(p->b+b.body+4))
                return fail(p, "Non-fragmented MP4 initialization");
        }
        if (b.type == TAG('s','t','s','d')) r = sample_entries(p, &b, depth);
        else if (b.type == TAG('d','r','e','f')) {
            if (b.end-b.body < 8 || p->b[b.body] || be32(p->b+b.body+4) != 1)
                return fail(p, "External data references are unsupported");
            struct box ref;
            r = box_at(p, b.body+8, b.end, true, &ref);
            if (r <= 0 || ref.end != b.end || ref.type != TAG('u','r','l',' ') ||
                ref.end-ref.body != 4 || be32(p->b+ref.body) != 1)
                return fail(p, "External data references are unsupported");
        } else if (container_box(b.type)) r = inspect_boxes(p, b.body, b.end, depth+1, init);
        if (r < 0) return r;
        at = b.end;
    }
    return 1;
}
static int validate_init(struct parser *p, const struct box *moov) {
    bool mvex = false, track = false;
    size_t at = moov->body;
    while (at < moov->end) {
        struct box b;
        int r = box_at(p, at, moov->end, true, &b);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated movie box");
        if (b.type == TAG('m','v','e','x')) mvex = true;
        if (b.type == TAG('t','r','a','k')) track = true;
        at = b.end;
    }
    if (!mvex || !track) return fail(p, "Missing fragmented MP4 initialization");
    return inspect_boxes(p, moov->body, moov->end, 1, true);
}
static int validate_traf(struct parser *p, const struct box *traf, size_t moof,
                         struct fragment *f) {
    size_t at = traf->body, cursor = 0;
    uint32_t default_size = 0;
    bool tfhd = false, tfdt = false, trun = false, cursor_known = false;
    bool default_known = false, base_is_moof = false;
    /* tfhd defaults apply to all runs, regardless of sibling ordering. */
    while (at < traf->end) {
        struct box b; int r = box_at(p, at, traf->end, true, &b);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated track fragment");
        if (b.type == TAG('t','f','h','d')) {
            if (tfhd || b.end-b.body < 8 || p->b[b.body]) return fail(p, "Invalid track fragment header");
            tfhd = true;
            uint32_t flags = be32(p->b+b.body)&0xffffff;
            if (flags & ~0x03003bu || flags & 1u) return fail(p, "External MP4 base offset is unsupported");
            if (!be32(p->b+b.body+4)) return fail(p, "Invalid track ID");
            base_is_moof = (flags&0x020000u) != 0;
            size_t off = b.body+8;
            if (flags&2) off += 4;
            if (flags&8) off += 4;
            if (flags&16) {
                if (off > b.end || b.end-off < 4) return fail(p, "Truncated sample defaults");
                default_size = be32(p->b+off); default_known = true; off += 4;
            }
            if (flags&32) off += 4;
            if (off != b.end) return fail(p, "Invalid track fragment defaults");
        } else if (b.type == TAG('t','f','d','t')) {
            if (tfdt || b.end-b.body < 4 || p->b[b.body] > 1 ||
                b.end-b.body != (p->b[b.body] ? 12u : 8u)) return fail(p, "Invalid decode time");
            tfdt = true;
        }
        at = b.end;
    }
    if (!tfhd || !tfdt) return fail(p, "Missing track fragment header or decode time");
    if (!base_is_moof) f->explicit_base = true;
    at = traf->body;
    while (at < traf->end) {
        struct box b; int r = box_at(p, at, traf->end, true, &b);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated track run");
        if (b.type == TAG('t','r','u','n')) {
            if (b.end-b.body < 8 || p->b[b.body] > 1) return fail(p, "Invalid track run");
            uint32_t flags = be32(p->b+b.body)&0xffffff, samples = be32(p->b+b.body+4);
            if (flags & ~0xf05u || (!trun && !(flags&1)) || samples > LIMIT)
                return fail(p, "Invalid track run flags or count");
            size_t off = b.body+8;
            if (flags&1) {
                if (b.end-off < 4) return fail(p, "Truncated track run offset");
                uint32_t offset = be32(p->b+off); off += 4;
                if (offset > LIMIT || moof > LIMIT-offset) return fail(p, "Invalid track run data offset");
                cursor = moof+offset; cursor_known = true;
            }
            if (flags&4) off += 4;
            size_t fields = ((flags&0x100)?4u:0u)+((flags&0x200)?4u:0u)+
                            ((flags&0x400)?4u:0u)+((flags&0x800)?4u:0u);
            if (off > b.end || (uint64_t)samples*fields != b.end-off)
                return fail(p, "Invalid track run length");
            bool known = cursor_known && ((flags&0x200) || default_known);
            uint64_t bytes = 0;
            if (flags&0x200) {
                size_t size_offset = (flags&0x100)?4:0;
                for (uint32_t i = 0; i < samples; ++i) {
                    bytes += be32(p->b+off+size_offset+(size_t)i*fields);
                    if (bytes > LIMIT) return fail(p, "Oversized track run");
                }
            } else if (default_known) bytes = (uint64_t)samples*default_size;
            if (bytes > LIMIT || (known && cursor > LIMIT-bytes)) return fail(p, "Oversized track run");
            if (samples) {
                if (f->count == MAX_RUNS) return fail(p, "Too many track runs");
                f->runs[f->count++] = (struct run){cursor, known?cursor+(size_t)bytes:0, known, false};
                if (!known) f->unknown = true;
            }
            if (known) cursor += (size_t)bytes; else cursor_known = false;
            trun = true;
        }
        at = b.end;
    }
    return trun ? 1 : fail(p, "Missing track run");
}
static int validate_moof(struct parser *p, const struct box *moof, struct fragment *f) {
    int r = inspect_boxes(p, moof->body, moof->end, 1, false);
    if (r < 0) return r;
    size_t at = moof->body;
    while (at < moof->end) {
        struct box b; r = box_at(p, at, moof->end, true, &b);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated movie fragment");
        if (b.type == TAG('t','r','a','f')) {
            if (++f->trafs > 32) return fail(p, "Too many track fragments");
            r = validate_traf(p, &b, moof->start, f);
            if (r < 0) return r;
        }
        at = b.end;
    }
    if (!f->trafs || (f->trafs > 1 && f->explicit_base))
        return fail(p, "Missing movie-fragment relative addressing");
    return 1;
}
static bool runs_complete(const struct fragment *f) {
    if (f->unknown) return false;
    for (unsigned i = 0; i < f->count; ++i) if (!f->runs[i].covered) return false;
    return true;
}
static int mp4_boundary(struct parser *p, bool have_init, bool eos, size_t *init_end, size_t *segment_end) {
    size_t at = 0, media_start = 0;
    bool ftyp = false, initialized = have_init, moof = false, mdat = false;
    struct fragment f = {0};
    while (at < p->n) {
        /* A complete next-segment header closes a fragment whose sizes are
         * inherited from trex. Do not wait for that next fragment's payload. */
        if (moof && mdat && p->n-at >= 8) {
            uint32_t type = be32(p->b+at+4), size = be32(p->b+at);
            if ((type == TAG('m','o','o','f') || type == TAG('s','t','y','p') || type == TAG('s','i','d','x')) &&
                (size != 1 || p->n-at >= 16)) {
                for (unsigned i = 0; i < f.count; ++i)
                    if (f.runs[i].known && !f.runs[i].covered) return fail(p, "Track run exceeds media data");
                *segment_end = at; return 1;
            }
        }
        struct box b; int r = box_at(p, at, p->n, eos, &b);
        if (r <= 0) {
            if (r < 0) return r;
            if (*init_end) return 1;
            return more(p, eos);
        }
        if (encrypted_box(b.type) || b.type == TAG('u','u','i','d'))
            return fail(p, "Encrypted or unsupported MP4 extension");
        if (!initialized) {
            if (b.type == TAG('f','t','y','p')) {
                if (ftyp || b.end-b.body < 8 || (b.end-b.body)%4)
                    return fail(p, "Invalid MP4 file type box");
                ftyp = true;
            } else if (b.type == TAG('m','o','o','v')) {
                if (!ftyp) return fail(p, "Missing MP4 file type box");
                r = validate_init(p, &b); if (r < 0) return r;
                *init_end = b.end; initialized = true; media_start = b.end;
            } else if (b.type != TAG('f','r','e','e') && b.type != TAG('s','k','i','p') &&
                       b.type != TAG('p','d','i','n') && b.type != TAG('s','i','d','x'))
                return fail(p, "Missing MP4 initialization segment");
        } else if (b.type == TAG('m','o','o','f')) {
            if (moof) {
                if (!mdat) return fail(p, "Movie fragment has no media data");
                for (unsigned i = 0; i < f.count; ++i)
                    if (f.runs[i].known && !f.runs[i].covered) return fail(p, "Track run exceeds media data");
                *segment_end = b.start; return 1;
            }
            moof = true; r = validate_moof(p, &b, &f); if (r < 0) return r;
        } else if (b.type == TAG('m','d','a','t')) {
            if (!moof) return fail(p, "Media data precedes movie fragment");
            mdat = true;
            for (unsigned i = 0; i < f.count; ++i) {
                struct run *run = &f.runs[i];
                if (run->known && run->first >= b.body && run->last <= b.end) run->covered = true;
                if (run->known && run->first < b.end && run->last > b.body && !run->covered)
                    return fail(p, "Track run crosses media box boundary");
            }
            if (runs_complete(&f)) { *segment_end = b.end; return 1; }
        } else if (b.type == TAG('f','t','y','p') || b.type == TAG('m','o','o','v')) {
            return fail(p, "Unexpected MP4 initialization segment");
        } else if (b.type == TAG('s','t','y','p') || b.type == TAG('s','i','d','x')) {
            if (moof && mdat) {
                for (unsigned i = 0; i < f.count; ++i)
                    if (f.runs[i].known && !f.runs[i].covered) return fail(p, "Track run exceeds media data");
                *segment_end = b.start; return 1;
            }
            if (moof) return fail(p, "Unexpected media segment prefix");
        } else if (b.type != TAG('f','r','e','e') && b.type != TAG('s','k','i','p') &&
                   b.type != TAG('e','m','s','g') && b.type != TAG('p','r','f','t') &&
                   b.type != TAG('m','f','r','a')) return fail(p, "Unsupported MP4 top-level box");
        at = b.end;
    }
    if (eos && moof) {
        if (!mdat) return fail(p, "Truncated movie fragment data");
        for (unsigned i = 0; i < f.count; ++i)
            if (f.runs[i].known && !f.runs[i].covered) return fail(p, "Track run exceeds media data");
        *segment_end = at; return 1;
    }
    if (*init_end) return 1;
    if (eos && at > media_start && initialized && !moof) { *segment_end = at; return 1; }
    return more(p, eos);
}

/* EBML VINT decoding never searches byte strings for delimiters: frame data
 * can contain arbitrary Cluster IDs. Only actual child boundaries are read. */
static int vint(const uint8_t *b, size_t n, bool id, uint64_t *value, unsigned *width, bool *unknown) {
    if (!n) return 0;
    unsigned w = 1; uint8_t marker = 0x80;
    while (marker && !(b[0]&marker)) { marker >>= 1; ++w; }
    if (!marker || w > (id?4u:8u)) return -1;
    if (w > n) return 0;
    uint64_t v = id ? b[0] : b[0]&~marker;
    for (unsigned i = 1; i < w; ++i) v = (v<<8)|b[i];
    if (id && v == (((uint64_t)1<<(7*w+1))-1)) return -1;
    *value = v; *width = w;
    if (unknown) *unknown = !id && v == (((uint64_t)1<<(7*w))-1);
    return 1;
}
static int element_at(struct parser *p, size_t at, size_t end, bool header_only, struct element *e) {
    if (at >= end) return 0;
    uint64_t id, size; unsigned a, b; bool unknown;
    int r = vint(p->b+at, end-at, true, &id, &a, 0);
    if (r < 0) return fail(p, "Invalid EBML element ID");
    if (!r) return 0;
    r = vint(p->b+at+a, end-at-a, false, &size, &b, &unknown);
    if (r < 0) return fail(p, "Invalid EBML element size");
    if (!r) return 0;
    e->id = (uint32_t)id; e->start = at; e->body = at+a+b;
    e->size = size; e->unknown = unknown;
    if (!unknown && size > LIMIT && !(header_only && id == 0x18538067))
        return fail(p, "Oversized WebM element");
    if (unknown) { e->end = end; return count(p); }
    if (size > end-e->body) {
        e->end = 0;
        return header_only ? count(p) : 0;
    }
    e->end = e->body+(size_t)size;
    return count(p);
}
static bool ebml_master(uint32_t id) {
    return id == 0x1a45dfa3 || id == 0x1549a966 || id == 0x1654ae6b || id == 0xae ||
           id == 0xe0 || id == 0xe1 || id == 0x6d80 || id == 0x6240 || id == 0x5034 ||
           id == 0x41e4 || id == 0x6624 || id == 0x7670 || id == 0x55b0 || id == 0x55d0 ||
           id == 0xa0 || id == 0x75a1 || id == 0xa6;
}
static int ebml_uint(struct parser *p, const struct element *e, uint64_t *v) {
    if (e->unknown || e->end-e->body > 8) return fail(p, "Invalid EBML integer");
    uint64_t n = 0;
    for (size_t i = e->body; i < e->end; ++i) n = (n<<8)|p->b[i];
    *v = n; return 1;
}
static int inspect_ebml(struct parser *p, size_t at, size_t end, unsigned depth) {
    if (depth > MAX_DEPTH) return fail(p, "WebM nesting limit exceeded");
    while (at < end) {
        struct element e; int r = element_at(p, at, end, false, &e);
        if (r <= 0) return r < 0 ? r : fail(p, "Truncated nested WebM element");
        if (e.unknown) return fail(p, "Unknown size on finite WebM metadata");
        if (e.id == 0x5035 || e.id == 0x47e1 || e.id == 0x47e2 || e.id == 0x47e7)
            return fail(p, "Encrypted WebM is unsupported");
        if (e.id == 0x5033) {
            uint64_t type; r = ebml_uint(p, &e, &type);
            if (r < 0) return r;
            if (type) return fail(p, "Encrypted WebM is unsupported");
        }
        if (ebml_master(e.id)) { r = inspect_ebml(p, e.body, e.end, depth+1); if (r < 0) return r; }
        at = e.end;
    }
    return 1;
}
static int validate_header(struct parser *p, const struct element *header) {
    size_t at = header->body; bool webm = false;
    while (at < header->end) {
        struct element e; int r = element_at(p, at, header->end, false, &e);
        if (r <= 0 || e.unknown) return r < 0 ? r : fail(p, "Truncated EBML header");
        if (e.id == 0x4282) {
            if (webm || e.end-e.body != 4 || p->b[e.body]!='w' || p->b[e.body+1]!='e' ||
                p->b[e.body+2]!='b' || p->b[e.body+3]!='m') return fail(p, "Non-WebM EBML document");
            webm = true;
        }
        if (e.id == 0x42f7 || e.id == 0x42f2 || e.id == 0x42f3 || e.id == 0x4285) {
            uint64_t v; r = ebml_uint(p, &e, &v); if (r < 0) return r;
            uint64_t max = e.id == 0x42f2 ? 4 : e.id == 0x42f3 ? 8 : e.id == 0x4285 ? 4 : 1;
            if (!v || v > max) return fail(p, "Unsupported EBML read version");
        }
        at = e.end;
    }
    return webm ? 1 : fail(p, "Missing WebM document type");
}
/* Validate lacing lengths without touching codec payloads. Signed EBML lace
 * differences use the VINT bias, not a two's-complement representation. */
static int validate_block(struct parser *p, const struct element *e) {
    size_t at = e->body, end = e->end; uint64_t track; unsigned w; bool unknown;
    int r = vint(p->b+at, end-at, false, &track, &w, &unknown);
    if (r <= 0 || !track || end-at < w+3u) return fail(p, "Invalid WebM block header");
    at += w;
    unsigned lace = (p->b[at+2]>>1)&3; at += 3;
    if (!lace) return at < end ? 1 : fail(p, "Empty WebM block");
    if (at == end) return fail(p, "Truncated WebM lace count");
    unsigned frames = (unsigned)p->b[at++]+1;
    if (frames < 2) return fail(p, "Invalid WebM lace count");
    uint64_t total = 0, previous = 0;
    if (lace == 2) return (end-at)%frames == 0 && end-at >= frames ? 1 : fail(p, "Invalid fixed WebM lacing");
    for (unsigned i = 0; i+1 < frames; ++i) {
        uint64_t size = 0;
        if (lace == 1) {
            unsigned part;
            do {
                if (at == end) return fail(p, "Truncated Xiph lacing");
                part = p->b[at++]; size += part;
                if (size > LIMIT) return fail(p, "Oversized WebM lace");
            } while (part == 255);
        } else {
            uint64_t encoded;
            r = vint(p->b+at, end-at, false, &encoded, &w, &unknown);
            if (r <= 0) return fail(p, "Truncated EBML lacing");
            at += w;
            if (!i) size = encoded;
            else {
                int64_t delta = (int64_t)encoded - (int64_t)(((uint64_t)1<<(7*w-1))-1);
                if (delta < -(int64_t)previous || delta > (int64_t)LIMIT-(int64_t)previous)
                    return fail(p, "Invalid EBML lace difference");
                size = (uint64_t)((int64_t)previous+delta);
            }
        }
        if (!size || size > LIMIT || total > LIMIT-size) return fail(p, "Invalid WebM lace size");
        total += size; previous = size;
    }
    return total < end-at ? 1 : fail(p, "WebM lace exceeds block data");
}
static int validate_group(struct parser *p, const struct element *group, bool timecode) {
    bool block = false; size_t at = group->body;
    while (at < group->end) {
        struct element e; int r = element_at(p, at, group->end, false, &e);
        if (r <= 0 || e.unknown) return r < 0 ? r : fail(p, "Truncated WebM block group");
        if (e.id == 0xa1) {
            if (block || !timecode) return fail(p, "Invalid WebM block timestamp");
            r = validate_block(p, &e); if (r < 0) return r; block = true;
        }
        at = e.end;
    }
    int r = inspect_ebml(p, group->body, group->end, 1);
    if (r < 0) return r;
    return block ? 1 : fail(p, "Missing WebM block");
}
static int cluster_span(struct parser *p, const struct element *cluster, bool eos, size_t *end) {
    size_t at = cluster->body, limit = cluster->unknown ? p->n : cluster->end;
    bool timecode = false;
    while (at < limit) {
        struct element e; int r = element_at(p, at, limit, true, &e);
        if (r <= 0) return r < 0 ? r : more(p, eos || !cluster->unknown);
        if (cluster->unknown && (e.id == 0x1f43b675 || e.id == 0x1a45dfa3)) {
            if (!timecode) return fail(p, "Missing WebM Cluster timestamp");
            *end = at; return 1;
        }
        if (e.unknown) return fail(p, "Unknown size on WebM Cluster child");
        if (!e.end) return more(p, eos || !cluster->unknown);
        if (e.id == 0xe7) {
            uint64_t v; r = ebml_uint(p, &e, &v); if (r < 0) return r;
            if (timecode) return fail(p, "Duplicate WebM Cluster timestamp");
            timecode = true;
        } else if (e.id == 0xa3 || e.id == 0xa1) {
            if (!timecode) return fail(p, "WebM block precedes Cluster timestamp");
            r = validate_block(p, &e); if (r < 0) return r;
        } else if (e.id == 0xa0) {
            r = validate_group(p, &e, timecode); if (r < 0) return r;
        } else if (e.id != 0xec && e.id != 0xbf && e.id != 0xa7 && e.id != 0xab && e.id != 0x5854)
            return fail(p, "Unsupported WebM Cluster child");
        at = e.end;
    }
    if (cluster->unknown && !eos) return 0;
    if (!timecode) return fail(p, "Missing WebM Cluster timestamp");
    *end = at; return 1;
}
static int webm_boundary(struct parser *p, bool have_init, bool eos, size_t *init_end, size_t *segment_end) {
    size_t at = 0; bool info = false, tracks = false;
    if (!have_init) {
        struct element h; int r = element_at(p, at, p->n, false, &h);
        if (r <= 0) return r < 0 ? r : more(p, eos);
        if (h.id != 0x1a45dfa3 || h.unknown) return fail(p, "Missing finite WebM EBML header");
        r = validate_header(p, &h); if (r < 0) return r;
        at = h.end;
        struct element segment; r = element_at(p, at, p->n, true, &segment);
        if (r <= 0) return r < 0 ? r : more(p, eos);
        if (segment.id != 0x18538067) return fail(p, "Missing WebM Segment header");
        at = segment.body;
        while (at < p->n) {
            struct element e; r = element_at(p, at, p->n, true, &e);
            if (r <= 0) return r < 0 ? r : (*init_end ? 1 : more(p, eos));
            if (!segment.unknown && ((uint64_t)(e.body-segment.body) > segment.size ||
                (!e.unknown && e.size > segment.size-(e.body-segment.body))))
                return fail(p, "WebM initialization exceeds Segment size");
            if (e.id == 0x1f43b675) {
                if (!info || !tracks) return fail(p, "Missing WebM Info or Tracks");
                *init_end = at; break;
            }
            if (e.id == 0x1a45dfa3 || e.id == 0x18538067 || e.unknown)
                return fail(p, "Invalid WebM initialization element");
            if (!e.end) return *init_end ? 1 : more(p, eos);
            if (e.id == 0x1549a966) {
                if (info || tracks) return fail(p, "Invalid WebM Info order");
                info = true;
            } else if (e.id == 0x1654ae6b) {
                if (!info || tracks) return fail(p, "Invalid WebM Tracks order");
                tracks = true;
            }
            if (ebml_master(e.id)) { r = inspect_ebml(p, e.body, e.end, 1); if (r < 0) return r; }
            at = e.end;
            if (tracks) *init_end = at;
        }
        if (!*init_end) return more(p, eos);
        if (at == p->n) return 1;
    }
    while (at < p->n) {
        struct element e; int r = element_at(p, at, p->n, true, &e);
        if (r <= 0) return r < 0 ? r : (*init_end ? 1 : more(p, eos));
        if (e.id == 0x1f43b675) {
            if (!e.unknown && !e.end) return *init_end ? 1 : more(p, eos);
            r = cluster_span(p, &e, eos, segment_end);
            return r == 0 && *init_end ? 1 : r;
        }
        if (e.id != 0xec && e.id != 0xbf && e.id != 0x1c53bb6b && e.id != 0x1043a770 && e.id != 0x114d9b74)
            return fail(p, "Expected WebM Cluster");
        if (e.unknown) return fail(p, "Unknown size on WebM metadata");
        if (!e.end) return *init_end ? 1 : more(p, eos);
        at = e.end;
    }
    if (eos && at && have_init) { *segment_end = at; return 1; }
    return *init_end ? 1 : more(p, eos);
}
int nmedia_mse_boundary(const uint8_t *bytes, size_t length, bool webm,
                        bool have_init, bool eos, size_t *init_end,
                        size_t *segment_end, char *error, size_t error_size) {
    struct parser p = { bytes, length, 0, error, error_size };
    if (error && error_size) error[0] = 0;
    if (!init_end || !segment_end) return fail(&p, "Missing media span output");
    *init_end = 0; *segment_end = 0;
    if ((!bytes && length) || length > LIMIT) return fail(&p, "Media staging limit exceeded");
    if (!length) return 0;
    int r = webm ? webm_boundary(&p, have_init, eos, init_end, segment_end) :
                   mp4_boundary(&p, have_init, eos, init_end, segment_end);
    if (r < 0) { *init_end = 0; *segment_end = 0; }
    return r;
}

static int64_t duration_ms(uint64_t units, uint32_t scale) {
    if (!scale || !units || units == UINT64_MAX) return -1;
    uint64_t seconds=units/scale;
    if (seconds > 1000000000ULL) return -1;
    uint64_t ms=seconds*1000+(units%scale)*1000/scale;
    return ms <= 1000000000000ULL ? (int64_t)ms : -1;
}
static int64_t mp4_init_duration(struct parser *p) {
    size_t at=0; struct box moov={0};
    while (at < p->n) {
        struct box b; if (box_at(p,at,p->n,true,&b)<=0) return -1;
        if (b.type==TAG('m','o','o','v')) { moov=b; break; }
        at=b.end;
    }
    if (!moov.end) return -1;
    uint32_t scale=0; uint64_t movie=0,fragment=0; bool mvhd=false,mehd=false;
    at=moov.body;
    while (at < moov.end) {
        struct box b; if (box_at(p,at,moov.end,true,&b)<=0) return -1;
        if (b.type==TAG('m','v','h','d')) {
            if (mvhd || b.end-b.body<4 || p->b[b.body]>1) return -1;
            unsigned version=p->b[b.body]; size_t off=b.body+(version?20:12);
            if (off>b.end || b.end-off<(version?12u:8u)) return -1;
            scale=be32(p->b+off); movie=version?be64(p->b+off+4):be32(p->b+off+4);
            if (!version && movie==UINT32_MAX) movie=UINT64_MAX;
            mvhd=true;
        } else if (b.type==TAG('m','v','e','x')) {
            size_t child=b.body;
            while (child < b.end) {
                struct box e; if (box_at(p,child,b.end,true,&e)<=0) return -1;
                if (e.type==TAG('m','e','h','d')) {
                    if (mehd || e.end-e.body<4 || p->b[e.body]>1 ||
                        e.end-e.body!=(p->b[e.body]?12u:8u)) return -1;
                    fragment=p->b[e.body]?be64(p->b+e.body+4):be32(p->b+e.body+4);
                    if (!p->b[e.body] && fragment==UINT32_MAX) fragment=UINT64_MAX;
                    mehd=true;
                }
                child=e.end;
            }
        }
        at=b.end;
    }
    return mvhd ? duration_ms(mehd?fragment:movie,scale) : -1;
}
static int64_t webm_init_duration(struct parser *p) {
    struct element header,segment;
    if (element_at(p,0,p->n,false,&header)<=0 || header.unknown || header.id!=0x1a45dfa3) return -1;
    if (element_at(p,header.end,p->n,true,&segment)<=0 || segment.id!=0x18538067) return -1;
    size_t at=segment.body;
    while (at < p->n) {
        struct element e;
        if (element_at(p,at,p->n,false,&e)<=0 || e.unknown) return -1;
        if (e.id==0x1f43b675) return -1;
        if (e.id==0x1549a966) {
            uint64_t scale=1000000; double units=-1; bool have_scale=false,have_duration=false;
            size_t child=e.body;
            while (child < e.end) {
                struct element v;
                if (element_at(p,child,e.end,false,&v)<=0 || v.unknown) return -1;
                if (v.id==0x2ad7b1) {
                    if (have_scale || ebml_uint(p,&v,&scale)<0 || !scale) return -1;
                    have_scale=true;
                } else if (v.id==0x4489) {
                    if (have_duration) return -1;
                    if (v.end-v.body==8) { uint64_t bits=be64(p->b+v.body); memcpy(&units,&bits,8); }
                    else if (v.end-v.body==4) { uint32_t bits=be32(p->b+v.body); float value; memcpy(&value,&bits,4); units=value; }
                    else return -1;
                    have_duration=true;
                }
                child=v.end;
            }
            if (!have_duration || !isfinite(units) || units<=0) return -1;
            double ms=units*((double)scale/1000000.0);
            return isfinite(ms) && ms>0 && ms<=1000000000000.0 ? (int64_t)ms : -1;
        }
        at=e.end;
    }
    return -1;
}
int64_t nmedia_mse_init_duration(const uint8_t *bytes, size_t length, bool webm) {
    if (!bytes || !length || length>LIMIT) return -1;
    struct parser p={bytes,length,0,NULL,0};
    return webm?webm_init_duration(&p):mp4_init_duration(&p);
}
