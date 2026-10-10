/* One layout invocation only. Entries own their text: DOM/scratch addresses
 * are not identities. Allocation failure only loses this optimization. */
struct layout_text_entry {
    struct layout_text_entry *next;
    uint64_t hash, font_generation;
    font_t *font;
    size_t length;
    float px, letter_spacing, word_spacing, width;
    bool bold;
    char text[];
};

struct layout_text_cache {
    struct layout_text_entry **buckets;
    size_t capacity, count;
};

#define layout_text_active (layout_current()->text_cache)

static uint64_t layout_text_hash_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *s = p;
    while (n--) { h ^= *s++; h *= UINT64_C(1099511628211); }
    return h;
}

static uint64_t layout_text_hash(const wfont *f, const style_t *st,
                                 const char *s, size_t n, uint64_t generation) {
    uint64_t h = layout_text_hash_bytes(UINT64_C(14695981039346656037), s, n);
    uintptr_t font = (uintptr_t)f->ttf;
    h = layout_text_hash_bytes(h, &font, sizeof font);
    h = layout_text_hash_bytes(h, &generation, sizeof generation);
    h = layout_text_hash_bytes(h, &f->px, sizeof f->px);
    h = layout_text_hash_bytes(h, &f->bold, sizeof f->bold);
    h = layout_text_hash_bytes(h, &st->letter_spacing, sizeof st->letter_spacing);
    return layout_text_hash_bytes(h, &st->word_spacing, sizeof st->word_spacing);
}

static bool layout_text_same(const struct layout_text_entry *e, uint64_t hash,
                             const wfont *f, const style_t *st,
                             const char *s, size_t n, uint64_t generation) {
    return e->hash == hash && e->length == n &&
           e->font_generation == generation && e->font == f->ttf &&
           e->bold == f->bold && !memcmp(&e->px, &f->px, sizeof e->px) &&
           !memcmp(&e->letter_spacing, &st->letter_spacing, sizeof e->letter_spacing) &&
           !memcmp(&e->word_spacing, &st->word_spacing, sizeof e->word_spacing) &&
           !memcmp(e->text, s, n);
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
            free(e);
            e = next;
        }
    }
    free(c->buckets);
    free(c);
}

static float text_width(const style_t *st, const wfont *f, const char *s, size_t n) {
    struct layout_text_cache *c = layout_text_active;
    uint64_t generation = font_metrics_generation();
    /* Zero means the native generation counter has exhausted its identity
     * space. Never let a wrapped generation reuse an earlier measurement. */
    if (!c || !generation) return text_width_uncached(st, f, s, n);
    uint64_t hash = layout_text_hash(f, st, s, n, generation);
    if (c->capacity) {
        for (struct layout_text_entry *e = c->buckets[(size_t)hash & (c->capacity - 1)];
             e; e = e->next)
            if (layout_text_same(e, hash, f, st, s, n, generation)) return e->width;
    }
    float width = text_width_uncached(st, f, s, n);
    /* A lazy fallback may have opened while measuring. Do not publish a
     * measurement spanning two backend generations; the next call measures
     * the now-stable backend normally. */
    if (font_metrics_generation() != generation ||
        n > SIZE_MAX - sizeof(struct layout_text_entry) || c->count == SIZE_MAX)
        return width;
    if ((!c->capacity || c->count >= c->capacity - c->capacity / 4) && !layout_text_grow(c))
        return width;
    struct layout_text_entry *e = malloc(sizeof *e + n);
    if (!e) return width;
    e->hash = hash; e->font_generation = generation; e->font = f->ttf;
    e->length = n; e->px = f->px; e->bold = f->bold;
    e->letter_spacing = st->letter_spacing; e->word_spacing = st->word_spacing;
    e->width = width;
    memcpy(e->text, s, n);
    size_t bucket = (size_t)hash & (c->capacity - 1);
    e->next = c->buckets[bucket]; c->buckets[bucket] = e; c->count++;
    return width;
}
