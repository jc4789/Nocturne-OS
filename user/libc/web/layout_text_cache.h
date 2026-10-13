/* Document-owned measurements, with an owned text/font-choice value key.
 * Entries unused by two measurement layouts are reclaimed from the LRU tail;
 * no full-table sweep is added to an unchanged layout. OOM loses only cache. */
struct layout_text_choices {
    struct layout_text_choices *next;
    uint64_t hash;
    size_t length, refs;
    unsigned char bytes[];
};
struct layout_text_entry {
    struct layout_text_entry *next;
    struct layout_text_entry *older, *newer;
    uint64_t hash, font_generation, used;
    font_t *font;
    size_t length;
    struct layout_text_choices *choices;
    float px, letter_spacing, word_spacing, width;
    bool bold;
    char text[];
};

struct layout_text_cache {
    struct layout_text_entry **buckets;
    size_t capacity, count;
    struct layout_text_choices **choices_buckets;
    size_t choices_capacity, choices_count;
    struct layout_text_entry *oldest, *newest;
    uint64_t generation, pass;
    bool busy, measuring;
};

#define layout_text_active (layout_current()->text_cache)

static uint64_t layout_text_hash_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *s = p;
    while (n--) { h ^= *s++; h *= UINT64_C(1099511628211); }
    return h;
}

/* CSS arenas retire on restyle, so choice/range pointers must never be keys.
 * Resources stay document-owned; capture their current selected face and
 * failure state too (a failed load need not advance global font generation). */
static size_t layout_text_choices_size(const wfont *f) {
    size_t n = 0;
    for (const struct web_font_choice *p = f->choices; p; p = p->next) {
        size_t fixed = sizeof(uintptr_t) * 2 + sizeof(size_t) + 1;
        if (p->range_count > (SIZE_MAX - fixed) / sizeof *p->ranges ||
            n > SIZE_MAX - fixed - p->range_count * sizeof *p->ranges) return SIZE_MAX;
        n += fixed + p->range_count * sizeof *p->ranges;
    }
    return n;
}

static uint64_t layout_text_choices_hash(uint64_t h, const wfont *f) {
    for (const struct web_font_choice *p = f->choices; p; p = p->next) {
        uintptr_t resource = (uintptr_t)p->resource, face = (uintptr_t)p->resource->face;
        unsigned char failed = p->resource->failed;
        h = layout_text_hash_bytes(h, &resource, sizeof resource);
        h = layout_text_hash_bytes(h, &face, sizeof face);
        h = layout_text_hash_bytes(h, &failed, 1);
        h = layout_text_hash_bytes(h, &p->range_count, sizeof p->range_count);
        h = layout_text_hash_bytes(h, p->ranges, p->range_count * sizeof *p->ranges);
    }
    return h;
}

static bool layout_text_choices_copy(unsigned char *dst, const unsigned char *key,
                                     const wfont *f) {
    size_t at = 0;
    for (const struct web_font_choice *p = f->choices; p; p = p->next) {
        uintptr_t resource = (uintptr_t)p->resource, face = (uintptr_t)p->resource->face;
        unsigned char failed = p->resource->failed;
        const void *parts[] = {&resource, &face, &failed, &p->range_count, p->ranges};
        size_t sizes[] = {sizeof resource, sizeof face, 1, sizeof p->range_count,
                          p->range_count * sizeof *p->ranges};
        for (unsigned i = 0; i < 5; i++) {
            if (key && sizes[i] && memcmp(key + at, parts[i], sizes[i])) return false;
            if (dst && sizes[i]) memcpy(dst + at, parts[i], sizes[i]);
            at += sizes[i];
        }
    }
    return true;
}

static uint64_t layout_text_hash(const wfont *f, const style_t *st,
                                 const char *s, size_t n, uint64_t generation,
                                 uint64_t choices_hash) {
    uint64_t h = layout_text_hash_bytes(UINT64_C(14695981039346656037), s, n);
    uintptr_t font = (uintptr_t)f->ttf;
    h = layout_text_hash_bytes(h, &font, sizeof font);
    h = layout_text_hash_bytes(h, &generation, sizeof generation);
    h = layout_text_hash_bytes(h, &f->px, sizeof f->px);
    h = layout_text_hash_bytes(h, &f->bold, sizeof f->bold);
    h = layout_text_hash_bytes(h, &st->letter_spacing, sizeof st->letter_spacing);
    h = layout_text_hash_bytes(h, &st->word_spacing, sizeof st->word_spacing);
    return layout_text_hash_bytes(h, &choices_hash, sizeof choices_hash);
}

static bool layout_text_same(const struct layout_text_entry *e, uint64_t hash,
                             const wfont *f, const style_t *st,
                             const char *s, size_t n, uint64_t generation) {
    return e->hash == hash && e->length == n &&
           e->font_generation == generation && e->font == f->ttf &&
           e->bold == f->bold && !memcmp(&e->px, &f->px, sizeof e->px) &&
           !memcmp(&e->letter_spacing, &st->letter_spacing, sizeof e->letter_spacing) &&
           !memcmp(&e->word_spacing, &st->word_spacing, sizeof e->word_spacing) &&
           !memcmp(e->text, s, n) &&
           (e->choices ? e->choices->length : 0) == layout_text_choices_size(f) &&
           (!e->choices || layout_text_choices_copy(NULL, e->choices->bytes, f));
}

static bool layout_text_choices_rehash(struct layout_text_cache *c, size_t capacity) {
    if (capacity > SIZE_MAX / sizeof *c->choices_buckets) return false;
    struct layout_text_choices **buckets = calloc(capacity, sizeof *buckets);
    if (!buckets) return false;
    for (size_t i = 0; i < c->choices_capacity; i++) {
        struct layout_text_choices *key = c->choices_buckets[i];
        while (key) {
            struct layout_text_choices *next = key->next;
            size_t bucket = (size_t)key->hash & (capacity - 1);
            key->next = buckets[bucket]; buckets[bucket] = key; key = next;
        }
    }
    free(c->choices_buckets); c->choices_buckets = buckets;
    c->choices_capacity = capacity; return true;
}

/* Many words/runs share one unicode-range list. Keep its owned bytes once,
   not one potentially large tail per measurement. Hash collisions still
   compare every serialized value; neither CSS pointer nor hash is identity. */
static struct layout_text_choices *layout_text_choices_get(struct layout_text_cache *c,
        const wfont *f, size_t length, uint64_t hash) {
    if (c->choices_capacity) {
        size_t bucket = (size_t)hash & (c->choices_capacity - 1);
        for (struct layout_text_choices *key = c->choices_buckets[bucket]; key; key = key->next)
            if (key->hash == hash && key->length == length &&
                layout_text_choices_copy(NULL, key->bytes, f)) {
                if (key->refs == SIZE_MAX) return NULL;
                key->refs++; return key;
            }
    }
    if (length > SIZE_MAX - sizeof(struct layout_text_choices) || c->choices_count == SIZE_MAX) return NULL;
    if (!c->choices_capacity || c->choices_count >= c->choices_capacity - c->choices_capacity / 4) {
        if (c->choices_capacity > SIZE_MAX / 2) return NULL;
        size_t capacity = c->choices_capacity ? c->choices_capacity * 2 : 16;
        if (!layout_text_choices_rehash(c, capacity)) return NULL;
    }
    struct layout_text_choices *key = malloc(sizeof *key + length);
    if (!key) return NULL;
    key->hash = hash; key->length = length; key->refs = 1;
    layout_text_choices_copy(key->bytes, NULL, f);
    size_t bucket = (size_t)hash & (c->choices_capacity - 1);
    key->next = c->choices_buckets[bucket]; c->choices_buckets[bucket] = key;
    c->choices_count++; return key;
}

static void layout_text_choices_release(struct layout_text_cache *c, struct layout_text_choices *key) {
    if (!key || --key->refs) return;
    size_t bucket = (size_t)key->hash & (c->choices_capacity - 1);
    struct layout_text_choices **at = &c->choices_buckets[bucket];
    while (*at && *at != key) at = &(*at)->next;
    if (*at) *at = key->next;
    free(key); c->choices_count--;
    if (!c->choices_count) {
        free(c->choices_buckets); c->choices_buckets = NULL; c->choices_capacity = 0;
    }
}

static void layout_text_touch(struct layout_text_cache *c, struct layout_text_entry *e) {
    e->used = c->pass;
    if (e == c->newest) return;
    if (e->older) e->older->newer = e->newer;
    else if (c->oldest == e) c->oldest = e->newer;
    if (e->newer) e->newer->older = e->older;
    e->older = c->newest; e->newer = NULL;
    if (c->newest) c->newest->newer = e;
    else c->oldest = e;
    c->newest = e;
}

static void layout_text_remove_oldest(struct layout_text_cache *c) {
    struct layout_text_entry *e = c->oldest;
    if (!e) return;
    size_t bucket = (size_t)e->hash & (c->capacity - 1);
    struct layout_text_entry **at = &c->buckets[bucket];
    while (*at && *at != e) at = &(*at)->next;
    if (*at) *at = e->next;
    c->oldest = e->newer;
    if (c->oldest) c->oldest->older = NULL;
    else c->newest = NULL;
    c->count--; layout_text_choices_release(c, e->choices); free(e);
}

static void layout_text_begin(struct layout_text_cache *c) {
    if (!c) return;
    uint64_t generation = font_metrics_generation();
    if (generation != c->generation || c->pass == UINT64_MAX) {
        while (c->oldest) layout_text_remove_oldest(c);
        c->generation = generation; c->pass = 0;
    }
    c->busy = true; c->measuring = false;
}

static void layout_text_measure_begin(struct layout_text_cache *c) {
    if (c->measuring) return;
    c->measuring = true; c->pass++;
    while (c->oldest && c->pass - c->oldest->used > 2) layout_text_remove_oldest(c);
    /* Hysteresis makes shrinking amortized; a former large document must
       not retain its peak bucket array after all of its text is replaced. */
    if (c->capacity > 16 && c->count < c->capacity / 8) {
        size_t capacity = 16;
        while (capacity < c->count * 2) capacity *= 2;
        struct layout_text_entry **buckets = calloc(capacity, sizeof *buckets);
        if (buckets) {
            for (struct layout_text_entry *e = c->oldest; e; e = e->newer) {
                size_t bucket = (size_t)e->hash & (capacity - 1);
                e->next = buckets[bucket]; buckets[bucket] = e;
            }
            free(c->buckets); c->buckets = buckets; c->capacity = capacity;
        }
    }
    if (c->choices_capacity > 16 && c->choices_count < c->choices_capacity / 8) {
        size_t capacity = 16;
        while (capacity < c->choices_count * 2) capacity *= 2;
        layout_text_choices_rehash(c, capacity); /* OOM keeps the old table */
    }
}

static void layout_text_demand(const wfont *f, const char *s, size_t n) {
    if (web_avmedia_snapshot_is_probe()) return;
    for (const struct web_font_choice *p = f->choices; p; p = p->next) {
        if (p->resource->face || p->resource->failed ||
            __atomic_load_n(&p->resource->wanted, __ATOMIC_RELAXED)) continue;
        if (!p->range_count) { __atomic_store_n(&p->resource->wanted, true, __ATOMIC_RELAXED); continue; }
        for (size_t i = 0; i < n;) {
            uint32_t cp; int bytes = gfx_utf8_decode(s + i, &cp);
            if (bytes <= 0 || (size_t)bytes > n - i) break;
            i += (size_t)bytes;
            size_t lo = 0, hi = p->range_count;
            while (lo < hi) { size_t mid = lo + (hi - lo) / 2;
                if (cp > p->ranges[mid].last) lo = mid + 1; else hi = mid; }
            if (lo < p->range_count && cp >= p->ranges[lo].first) {
                __atomic_store_n(&p->resource->wanted, true, __ATOMIC_RELAXED); break;
            }
        }
    }
}

static bool layout_text_grow(struct layout_text_cache *c) {
    size_t capacity = c->capacity ? c->capacity : 16;
    if (c->capacity) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof *c->buckets) return false;
    struct layout_text_entry **buckets = calloc(capacity, sizeof *buckets);
    if (!buckets) return false;
    for (size_t i = 0; i < c->capacity; i++) {
        struct layout_text_entry *e = c->buckets[i];
        while (e) {
            struct layout_text_entry *next = e->next;
            size_t bucket = (size_t)e->hash & (capacity - 1);
            e->next = buckets[bucket];
            buckets[bucket] = e;
            e = next;
        }
    }
    free(c->buckets);
    c->buckets = buckets;
    c->capacity = capacity;
    return true;
}

static void layout_text_cache_free(struct layout_text_cache *c) {
    if (!c) return;
    for (size_t i = 0; i < c->capacity; i++) {
        struct layout_text_entry *e = c->buckets[i];
        while (e) {
            struct layout_text_entry *next = e->next;
            layout_text_choices_release(c, e->choices);
            free(e);
            e = next;
        }
    }
    free(c->buckets);
    free(c->choices_buckets);
    free(c);
}

static float text_width(const style_t *st, const wfont *f, const char *s, size_t n) {
    struct layout_text_cache *c = layout_text_active;
    uint64_t generation = font_metrics_generation();
    /* Zero means the native generation counter has exhausted its identity
     * space. Never let a wrapped generation reuse an earlier measurement. */
    size_t choices_length = layout_text_choices_size(f);
    if (!c || !generation || choices_length == SIZE_MAX) return text_width_uncached(st, f, s, n);
    layout_text_measure_begin(c);
    uint64_t choices_hash = layout_text_choices_hash(UINT64_C(14695981039346656037), f);
    uint64_t hash = layout_text_hash(f, st, s, n, generation, choices_hash);
    if (c->capacity) {
        for (struct layout_text_entry *e = c->buckets[(size_t)hash & (c->capacity - 1)];
             e; e = e->next)
            if (layout_text_same(e, hash, f, st, s, n, generation)) {
                layout_text_touch(c, e);
                /* A partial cascade clears wanted bits even for unchanged
                   users. Cache reuse must preserve unicode subset demand. */
                layout_text_demand(f, s, n); return e->width;
            }
    }
    float width = text_width_uncached(st, f, s, n);
    /* A lazy fallback may have opened while measuring. Do not publish a
     * measurement spanning two backend generations; the next call measures
     * the now-stable backend normally. */
    if (font_metrics_generation() != generation ||
        n > SIZE_MAX - sizeof(struct layout_text_entry) ||
        choices_length > SIZE_MAX - sizeof(struct layout_text_choices) || c->count == SIZE_MAX)
        return width;
    if ((!c->capacity || c->count >= c->capacity - c->capacity / 4) && !layout_text_grow(c))
        return width;
    struct layout_text_choices *choices = choices_length ?
        layout_text_choices_get(c, f, choices_length, choices_hash) : NULL;
    if (choices_length && !choices) return width;
    struct layout_text_entry *e = malloc(sizeof *e + n);
    if (!e) { layout_text_choices_release(c, choices); return width; }
    e->hash = hash; e->font_generation = generation; e->font = f->ttf;
    e->length = n; e->choices = choices; e->px = f->px; e->bold = f->bold;
    e->older = e->newer = NULL;
    e->letter_spacing = st->letter_spacing; e->word_spacing = st->word_spacing;
    e->width = width;
    memcpy(e->text, s, n);
    size_t bucket = (size_t)hash & (c->capacity - 1);
    e->next = c->buckets[bucket]; c->buckets[bucket] = e; c->count++;
    layout_text_touch(c, e);
    return width;
}
