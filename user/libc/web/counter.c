/* CSS counters (CSS Lists 3 §4), the counter styles that represent them (CSS Counter Styles 3
 * §2-§6) and quotation marks (CSS Generated Content 3 §2): the one owner that counts a document's
 * counters, in tree order, and writes the text that list markers and generated content make of
 * them. The values come from cssprop.c (counter-*, list-style-*, quotes, content); the boxes that
 * count are box.c's, which marks each when it generates it (counter_kind) and calls
 * web_counters_update after a publication that made or retired one. */
#include <stdio.h>
#include "webi.h"

/* ---------------------------------------------------------------- counter styles */
enum { SYS_CYCLIC, SYS_FIXED, SYS_SYMBOLIC, SYS_ALPHABETIC, SYS_NUMERIC, SYS_ADDITIVE };
/* an additive tuple: a weight and the one or two code points of its symbol */
struct additive { uint16_t weight; uint16_t cp[2]; };
/* A counter style's definition (§3): its system, its symbols (the code points first .. first + n
   - 1, those of cps, or the strings of a symbols() function), its additive tuples, its range
   (auto when not ranged), prefix, suffix (NULL: the initial ". "), fallback and pad of '0'. */
struct counter_style_def {
    uint8_t system;
    uint32_t n, first;
    const uint16_t *cps;
    const char *const *strings;
    const struct additive *add;
    uint32_t nadd;
    bool ranged;
    int32_t lo, hi;
    const char *prefix, *suffix;
    uint8_t fallback, pad;
};

static const uint16_t cjk_decimal[] = {0x3007, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D};
static const uint16_t lower_greek[] = {0x3B1, 0x3B2, 0x3B3, 0x3B4, 0x3B5, 0x3B6, 0x3B7, 0x3B8, 0x3B9, 0x3BA, 0x3BB, 0x3BC,
                                       0x3BD, 0x3BE, 0x3BF, 0x3C0, 0x3C1, 0x3C3, 0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3C8, 0x3C9};
static const uint16_t hiragana[] = {
    0x3042, 0x3044, 0x3046, 0x3048, 0x304A, 0x304B, 0x304D, 0x304F, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059, 0x305B,
    0x305D, 0x305F, 0x3061, 0x3064, 0x3066, 0x3068, 0x306A, 0x306B, 0x306C, 0x306D, 0x306E, 0x306F, 0x3072, 0x3075,
    0x3078, 0x307B, 0x307E, 0x307F, 0x3080, 0x3081, 0x3082, 0x3084, 0x3086, 0x3088, 0x3089, 0x308A, 0x308B, 0x308C,
    0x308D, 0x308F, 0x3090, 0x3091, 0x3092, 0x3093};
static const uint16_t hiragana_iroha[] = {
    0x3044, 0x308D, 0x306F, 0x306B, 0x307B, 0x3078, 0x3068, 0x3061, 0x308A, 0x306C, 0x308B, 0x3092, 0x308F, 0x304B,
    0x3088, 0x305F, 0x308C, 0x305D, 0x3064, 0x306D, 0x306A, 0x3089, 0x3080, 0x3046, 0x3090, 0x306E, 0x304A, 0x304F,
    0x3084, 0x307E, 0x3051, 0x3075, 0x3053, 0x3048, 0x3066, 0x3042, 0x3055, 0x304D, 0x3086, 0x3081, 0x307F, 0x3057,
    0x3091, 0x3072, 0x3082, 0x305B, 0x3059};
static const uint16_t katakana[] = {
    0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB,
    0x30BD, 0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5,
    0x30D8, 0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, 0x30EA, 0x30EB, 0x30EC,
    0x30ED, 0x30EF, 0x30F0, 0x30F1, 0x30F2, 0x30F3};
static const uint16_t katakana_iroha[] = {
    0x30A4, 0x30ED, 0x30CF, 0x30CB, 0x30DB, 0x30D8, 0x30C8, 0x30C1, 0x30EA, 0x30CC, 0x30EB, 0x30F2, 0x30EF, 0x30AB,
    0x30E8, 0x30BF, 0x30EC, 0x30BD, 0x30C4, 0x30CD, 0x30CA, 0x30E9, 0x30E0, 0x30A6, 0x30F0, 0x30CE, 0x30AA, 0x30AF,
    0x30E4, 0x30DE, 0x30B1, 0x30D5, 0x30B3, 0x30A8, 0x30C6, 0x30A2, 0x30B5, 0x30AD, 0x30E6, 0x30E1, 0x30DF, 0x30B7,
    0x30F1, 0x30D2, 0x30E2, 0x30BB, 0x30B9};
static const uint16_t earthly_branch[] = {0x5B50, 0x4E11, 0x5BC5, 0x536F, 0x8FB0, 0x5DF3, 0x5348, 0x672A, 0x7533, 0x9149,
                                          0x620C, 0x4EA5};
static const uint16_t heavenly_stem[] = {0x7532, 0x4E59, 0x4E19, 0x4E01, 0x620A, 0x5DF1, 0x5E9A, 0x8F9B, 0x58EC, 0x7678};
static const uint16_t disc[] = {0x2022}, circle[] = {0x25E6}, square[] = {0x25AA};
static const uint16_t disclosure_open[] = {0x25BE}, disclosure_closed[] = {0x25B8};
static const struct additive armenian[] = {
    {9000, {0x554}}, {8000, {0x553}}, {7000, {0x552}}, {6000, {0x551}}, {5000, {0x550}}, {4000, {0x54F}}, {3000, {0x54E}},
    {2000, {0x54D}}, {1000, {0x54C}}, {900, {0x54B}}, {800, {0x54A}}, {700, {0x549}}, {600, {0x548}}, {500, {0x547}},
    {400, {0x546}}, {300, {0x545}}, {200, {0x544}}, {100, {0x543}}, {90, {0x542}}, {80, {0x541}}, {70, {0x540}},
    {60, {0x53F}}, {50, {0x53E}}, {40, {0x53D}}, {30, {0x53C}}, {20, {0x53B}}, {10, {0x53A}}, {9, {0x539}}, {8, {0x538}},
    {7, {0x537}}, {6, {0x536}}, {5, {0x535}}, {4, {0x534}}, {3, {0x533}}, {2, {0x532}}, {1, {0x531}}};
static const struct additive lower_armenian[] = {
    {9000, {0x584}}, {8000, {0x583}}, {7000, {0x582}}, {6000, {0x581}}, {5000, {0x580}}, {4000, {0x57F}}, {3000, {0x57E}},
    {2000, {0x57D}}, {1000, {0x57C}}, {900, {0x57B}}, {800, {0x57A}}, {700, {0x579}}, {600, {0x578}}, {500, {0x577}},
    {400, {0x576}}, {300, {0x575}}, {200, {0x574}}, {100, {0x573}}, {90, {0x572}}, {80, {0x571}}, {70, {0x570}},
    {60, {0x56F}}, {50, {0x56E}}, {40, {0x56D}}, {30, {0x56C}}, {20, {0x56B}}, {10, {0x56A}}, {9, {0x569}}, {8, {0x568}},
    {7, {0x567}}, {6, {0x566}}, {5, {0x565}}, {4, {0x564}}, {3, {0x563}}, {2, {0x562}}, {1, {0x561}}};
static const struct additive georgian[] = {
    {10000, {0x10F5}}, {9000, {0x10F0}}, {8000, {0x10EF}}, {7000, {0x10F4}}, {6000, {0x10EE}}, {5000, {0x10ED}},
    {4000, {0x10EC}}, {3000, {0x10EB}}, {2000, {0x10EA}}, {1000, {0x10E9}}, {900, {0x10E8}}, {800, {0x10E7}},
    {700, {0x10E6}}, {600, {0x10E5}}, {500, {0x10E4}}, {400, {0x10F3}}, {300, {0x10E2}}, {200, {0x10E1}}, {100, {0x10E0}},
    {90, {0x10DF}}, {80, {0x10DE}}, {70, {0x10DD}}, {60, {0x10F2}}, {50, {0x10DC}}, {40, {0x10DB}}, {30, {0x10DA}},
    {20, {0x10D9}}, {10, {0x10D8}}, {9, {0x10D7}}, {8, {0x10F1}}, {7, {0x10D6}}, {6, {0x10D5}}, {5, {0x10D4}},
    {4, {0x10D3}}, {3, {0x10D2}}, {2, {0x10D1}}, {1, {0x10D0}}};
static const struct additive hebrew[] = {
    {10000, {0x5D9, 0x5F3}}, {9000, {0x5D8, 0x5F3}}, {8000, {0x5D7, 0x5F3}}, {7000, {0x5D6, 0x5F3}}, {6000, {0x5D5, 0x5F3}},
    {5000, {0x5D4, 0x5F3}}, {4000, {0x5D3, 0x5F3}}, {3000, {0x5D2, 0x5F3}}, {2000, {0x5D1, 0x5F3}}, {1000, {0x5D0, 0x5F3}},
    {400, {0x5EA}}, {300, {0x5E9}}, {200, {0x5E8}}, {100, {0x5E7}}, {90, {0x5E6}}, {80, {0x5E4}}, {70, {0x5E2}},
    {60, {0x5E1}}, {50, {0x5E0}}, {40, {0x5DE}}, {30, {0x5DC}}, {20, {0x5DB}}, {19, {0x5D9, 0x5D8}}, {18, {0x5D9, 0x5D7}},
    {17, {0x5D9, 0x5D6}}, {16, {0x5D8, 0x5D6}}, {15, {0x5D8, 0x5D5}}, {10, {0x5D9}}, {9, {0x5D8}}, {8, {0x5D7}},
    {7, {0x5D6}}, {6, {0x5D5}}, {5, {0x5D4}}, {4, {0x5D3}}, {3, {0x5D2}}, {2, {0x5D1}}, {1, {0x5D0}}};
static const struct additive lower_roman[] = {
    {1000, {'m'}}, {900, {'c', 'm'}}, {500, {'d'}}, {400, {'c', 'd'}}, {100, {'c'}}, {90, {'x', 'c'}}, {50, {'l'}},
    {40, {'x', 'l'}}, {10, {'x'}}, {9, {'i', 'x'}}, {5, {'v'}}, {4, {'i', 'v'}}, {1, {'i'}}};
static const struct additive upper_roman[] = {
    {1000, {'M'}}, {900, {'C', 'M'}}, {500, {'D'}}, {400, {'C', 'D'}}, {100, {'C'}}, {90, {'X', 'C'}}, {50, {'L'}},
    {40, {'X', 'L'}}, {10, {'X'}}, {9, {'I', 'X'}}, {5, {'V'}}, {4, {'I', 'V'}}, {1, {'I'}}};

#define COUNT(a) (uint32_t)(sizeof a / sizeof *a)
#define NUMERIC(zero) {.system = SYS_NUMERIC, .n = 10, .first = zero}
#define LIST(sys, a) .system = sys, .n = COUNT(a), .cps = a
#define ADDITIVE(a, l, h) {.system = SYS_ADDITIVE, .add = a, .nadd = COUNT(a), .ranged = true, .lo = l, .hi = h}
#define CJK_SUFFIX "\xe3\x80\x81" /* U+3001 IDEOGRAPHIC COMMA */
/* §6.1-§6.4 */
static const struct counter_style_def predefined[LS_PREDEFINED] = {
    [LS_DISC] = {LIST(SYS_CYCLIC, disc), .suffix = " "},
    [LS_CIRCLE] = {LIST(SYS_CYCLIC, circle), .suffix = " "},
    [LS_SQUARE] = {LIST(SYS_CYCLIC, square), .suffix = " "},
    [LS_DISCLOSURE_OPEN] = {LIST(SYS_CYCLIC, disclosure_open), .suffix = " "},
    [LS_DISCLOSURE_CLOSED] = {LIST(SYS_CYCLIC, disclosure_closed), .suffix = " "},
    [LS_DECIMAL] = NUMERIC('0'),
    [LS_DECIMAL_LEADING_ZERO] = {.system = SYS_NUMERIC, .n = 10, .first = '0', .pad = 2},
    [LS_ARABIC_INDIC] = NUMERIC(0x660),
    [LS_ARMENIAN] = ADDITIVE(armenian, 1, 9999),
    [LS_UPPER_ARMENIAN] = ADDITIVE(armenian, 1, 9999),
    [LS_LOWER_ARMENIAN] = ADDITIVE(lower_armenian, 1, 9999),
    [LS_BENGALI] = NUMERIC(0x9E6),
    [LS_CAMBODIAN] = NUMERIC(0x17E0),
    [LS_KHMER] = NUMERIC(0x17E0),
    [LS_CJK_DECIMAL] = {LIST(SYS_NUMERIC, cjk_decimal), .ranged = true, .lo = 0, .hi = INT32_MAX, .suffix = CJK_SUFFIX},
    [LS_DEVANAGARI] = NUMERIC(0x966),
    [LS_GEORGIAN] = ADDITIVE(georgian, 1, 19999),
    [LS_GUJARATI] = NUMERIC(0xAE6),
    [LS_GURMUKHI] = NUMERIC(0xA66),
    [LS_HEBREW] = ADDITIVE(hebrew, 1, 10999),
    [LS_KANNADA] = NUMERIC(0xCE6),
    [LS_LAO] = NUMERIC(0xED0),
    [LS_MALAYALAM] = NUMERIC(0xD66),
    [LS_MONGOLIAN] = NUMERIC(0x1810),
    [LS_MYANMAR] = NUMERIC(0x1040),
    [LS_ORIYA] = NUMERIC(0xB66),
    [LS_PERSIAN] = NUMERIC(0x6F0),
    [LS_LOWER_ROMAN] = ADDITIVE(lower_roman, 1, 3999),
    [LS_UPPER_ROMAN] = ADDITIVE(upper_roman, 1, 3999),
    [LS_TAMIL] = NUMERIC(0xBE6),
    [LS_TELUGU] = NUMERIC(0xC66),
    [LS_THAI] = NUMERIC(0xE50),
    [LS_TIBETAN] = NUMERIC(0xF20),
    [LS_LOWER_ALPHA] = {.system = SYS_ALPHABETIC, .n = 26, .first = 'a'},
    [LS_LOWER_LATIN] = {.system = SYS_ALPHABETIC, .n = 26, .first = 'a'},
    [LS_UPPER_ALPHA] = {.system = SYS_ALPHABETIC, .n = 26, .first = 'A'},
    [LS_UPPER_LATIN] = {.system = SYS_ALPHABETIC, .n = 26, .first = 'A'},
    [LS_LOWER_GREEK] = {LIST(SYS_ALPHABETIC, lower_greek)},
    [LS_HIRAGANA] = {LIST(SYS_ALPHABETIC, hiragana), .suffix = CJK_SUFFIX},
    [LS_HIRAGANA_IROHA] = {LIST(SYS_ALPHABETIC, hiragana_iroha), .suffix = CJK_SUFFIX},
    [LS_KATAKANA] = {LIST(SYS_ALPHABETIC, katakana), .suffix = CJK_SUFFIX},
    [LS_KATAKANA_IROHA] = {LIST(SYS_ALPHABETIC, katakana_iroha), .suffix = CJK_SUFFIX},
    [LS_CJK_EARTHLY_BRANCH] = {LIST(SYS_FIXED, earthly_branch), .suffix = CJK_SUFFIX, .fallback = LS_CJK_DECIMAL},
    [LS_CJK_HEAVENLY_STEM] = {LIST(SYS_FIXED, heavenly_stem), .suffix = CJK_SUFFIX, .fallback = LS_CJK_DECIMAL},
};

static void put_symbol(sbuf *out, const struct counter_style_def *s, uint32_t i) {
    if (s->strings) sb_puts(out, s->strings[i]);
    else sb_utf8(out, s->first ? s->first + i : s->cps[i]);
}
/* §3.1.3: a representation of the symbolic system longer than this uses the fallback style */
enum { SYMBOLIC_LIMIT = 60 };
/* §3.1: the initial representation of value (its absolute value, when the style uses a negative
   sign); false: the system cannot represent it */
static bool initial_representation(const struct counter_style_def *s, int64_t value, sbuf *out) {
    int64_t n = s->n;
    uint32_t digits[64];
    int k = 0;
    switch (s->system) {
    case SYS_CYCLIC:
        if (!n) return false;
        put_symbol(out, s, (uint32_t)(((value - 1) % n + n) % n));
        return true;
    case SYS_FIXED: /* the first symbol's value is 1 */
        if (value < 1 || value > n) return false;
        put_symbol(out, s, (uint32_t)(value - 1));
        return true;
    case SYS_SYMBOLIC: {
        if (value < 1 || !n) return false;
        int64_t repeat = (value + n - 1) / n;
        if (repeat > SYMBOLIC_LIMIT) return false;
        for (int64_t i = 0; i < repeat; i++) put_symbol(out, s, (uint32_t)((value - 1) % n));
        return true;
    }
    case SYS_ALPHABETIC:
        if (value < 1 || n < 2) return false;
        while (value && k < 64) { value--; digits[k++] = (uint32_t)(value % n); value /= n; }
        break;
    case SYS_NUMERIC:
        if (n < 2) return false;
        if (!value) digits[k++] = 0;
        while (value && k < 64) { digits[k++] = (uint32_t)(value % n); value /= n; }
        break;
    case SYS_ADDITIVE:
        for (uint32_t i = 0; i < s->nadd; i++) {
            const struct additive *t = &s->add[i];
            if (!value && !t->weight) { for (int j = 0; j < 2 && t->cp[j]; j++) sb_utf8(out, t->cp[j]); return true; }
            if (!t->weight || t->weight > value) continue;
            int64_t repeat = value / t->weight;
            value -= repeat * t->weight;
            for (int64_t r = 0; r < repeat; r++)
                for (int j = 0; j < 2 && t->cp[j]; j++) sb_utf8(out, t->cp[j]);
            if (!value) return true;
        }
        return false;
    }
    while (k) put_symbol(out, s, digits[--k]);
    return true;
}
static size_t code_points(const char *s, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) count += ((unsigned char)s[i] & 0xC0) != 0x80;
    return count;
}
static bool in_range(const struct counter_style_def *s, int32_t value) {
    if (s->ranged) return value >= s->lo && value <= s->hi;
    if (s->system == SYS_ALPHABETIC || s->system == SYS_SYMBOLIC) return value >= 1; /* §3.1.1's auto */
    if (s->system == SYS_ADDITIVE) return value >= 0;
    return true;
}
/* §2.2 "generate a counter representation" */
static void represent(const struct counter_style_def *s, int32_t value, sbuf *out, int depth) {
    if (depth < 4 && !in_range(s, value)) {
        represent(&predefined[s->fallback ? s->fallback : LS_DECIMAL], value, out, depth + 1);
        return;
    }
    /* the symbolic, alphabetic, numeric and additive systems use a negative sign (§3.1.1) */
    bool negative = value < 0 && s->system != SYS_CYCLIC && s->system != SYS_FIXED;
    sbuf rep = {0};
    if (!initial_representation(s, negative ? -(int64_t)value : value, &rep)) {
        sb_free(&rep);
        if (depth < 4) represent(&predefined[s->fallback ? s->fallback : LS_DECIMAL], value, out, depth + 1);
        else { char t[16]; snprintf(t, sizeof t, "%d", (int)value); sb_puts(out, t); }
        return;
    }
    /* §3.8: pad to its length, the negative sign counted */
    long pad = (long)s->pad - (long)code_points(rep.p ? rep.p : "", rep.n) - (negative ? 1 : 0);
    if (negative) sb_putc(out, '-');
    for (; pad > 0; pad--) sb_putc(out, '0');
    sb_put(out, rep.p ? rep.p : "", rep.n);
    sb_free(&rep);
}
/* the definition style names: a predefined one, symbols() (§6.1: prefix "", suffix " ", fallback
   decimal), or decimal for a name no rule defines */
static const struct counter_style_def *definition(const struct css_counter_style *style, struct counter_style_def *own) {
    if (style->id < LS_PREDEFINED) return &predefined[style->id];
    if (style->id == LS_SYMBOLS && style->symbols && style->symbols->n) {
        static const uint8_t systems[] = {[CSS_SYMBOLS_CYCLIC] = SYS_CYCLIC, [CSS_SYMBOLS_NUMERIC] = SYS_NUMERIC,
            [CSS_SYMBOLS_ALPHABETIC] = SYS_ALPHABETIC, [CSS_SYMBOLS_SYMBOLIC] = SYS_SYMBOLIC,
            [CSS_SYMBOLS_FIXED] = SYS_FIXED};
        *own = (struct counter_style_def){.system = systems[style->symbols->type], .n = style->symbols->n,
                                          .strings = style->symbols->symbol, .suffix = " "};
        return own;
    }
    return &predefined[LS_DECIMAL];
}
void css_counter_text(const struct css_counter_style *style, int32_t value, bool marker, sbuf *out) {
    if (style->id == LS_NONE) return;
    if (style->id == LS_STRING) { if (marker) sb_puts(out, style->name ? style->name : ""); return; }
    struct counter_style_def own;
    const struct counter_style_def *s = definition(style, &own);
    if (marker && s->prefix) sb_puts(out, s->prefix);
    represent(s, value, out, 0);
    if (marker) sb_puts(out, s->suffix ? s->suffix : ". ");
}

/* ---------------------------------------------------------------- counting */
/* A counter instance (CSS Lists 3 §4.4): its name; its scope, the node whose descendants and
   pseudo-elements it reaches, and that node's depth in the flat tree; its creator (origin: the
   element's node, or the pseudo-element's box); its value and whether it is reversed; pending: the
   index of its reversed_start while the first walk finds the initial value a reversed() reset
   without one has (§4.4.2), -1 otherwise.
   §4.4.1 (Issue 5477): a participant inherits its parent's counters first and its preceding
   sibling's only of a name the parent's set does not have. So a counter whose name the creator's
   parent has is nested: its scope is the creating element (its descendants, not its following
   siblings), or for a pseudo-element the pseudo-element alone (scope NULL, depth UINT32_MAX).
   Any other's scope is the creator's flat parent (a pseudo-element's is its originating element):
   the creator, the siblings after it and their descendants. level: how many style-contained
   elements enclosed its creator (struct contained). */
struct instance {
    const char *name;
    node_t *scope;
    uint32_t depth;
    const void *origin;
    int32_t value;
    bool reversed, nested;
    int32_t pending;
    uint32_t level;
};
/* CSS Containment 2 §3.3: an element with style containment whose subtree (below it) holds the
   participant, its depth in the flat tree, and the quote depth when its subtree began (which the
   subtree's quotes do not change for what follows it) */
struct contained { node_t *node; uint32_t depth, quote_depth; };
/* §4.4.2's num and lastNonZeroIncrementNegated for one such counter, whether an element of its
   scope set it (which ends the sum), and what the participant being applied did to it */
struct reversed_start { int64_t num; int32_t last; bool done; int64_t increment; bool set; int32_t set_value; bool touched; };
struct counting {
    web_doc *d;
    struct instance *v; /* the counters set of the participant, in creation order */
    uint32_t n, cap;
    struct reversed_start *rev;
    uint32_t nrev, caprev, rev_next;
    bool measure; /* a first walk: it also finds the reversed counters' initial values */
    /* the participant's flat ancestors, its parent first: at[0 .. len) of chain; the participant
       (origin), and its node when it is an element (creator, NULL for a pseudo-element) */
    node_t **chain, **at;
    uint32_t nchain, capchain, len;
    node_t *creator;
    const void *origin;
    uint32_t quote_depth;
    struct contained *frames; /* the style-contained elements around the participant, innermost last */
    uint32_t nframes, capframes;
    bool failed;
    sbuf text;
};
static bool grow(void **v, uint32_t *cap, uint32_t need, size_t size) {
    if (need <= *cap) return true;
    uint32_t c = *cap ? *cap : 16;
    while (c < need) { if (c > UINT32_MAX / 2) return false; c *= 2; }
    if ((size_t)c > SIZE_MAX / size) return false;
    void *p = realloc(*v, (size_t)c * size);
    if (!p) return false;
    *v = p;
    *cap = c;
    return true;
}
/* the chain of first and its flat ancestors */
static bool ancestors(struct counting *c, node_t *first) {
    c->nchain = 0;
    for (node_t *p = first; p; p = doc_flat_parent(p)) {
        if (!grow((void **)&c->chain, &c->capchain, c->nchain + 1, sizeof *c->chain)) return c->failed = true, false;
        c->chain[c->nchain++] = p;
    }
    return true;
}
/* the participant whose parent is chain[skip]: the element chain[0] (skip 1), or a pseudo-element
   of it (skip 0, origin its box) */
static void participant(struct counting *c, uint32_t skip, const void *pseudo) {
    c->at = c->chain + skip;
    c->len = c->nchain - skip;
    c->creator = skip ? c->chain[0] : NULL;
    c->origin = skip ? (const void *)c->chain[0] : pseudo;
}
/* whether node, at depth in the flat tree, is the participant's parent or an ancestor of it */
static bool encloses(const struct counting *c, const node_t *node, uint32_t depth) {
    return depth < c->len && c->at[c->len - 1 - depth] == node;
}
/* §4.4.1: the counters the participant inherits are those whose scope encloses it (a nested one's
   scope ends before the scopes of the counters made after it can) */
static void inherit(struct counting *c) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < c->n; i++)
        if (encloses(c, c->v[i].scope, c->v[i].depth)) c->v[k++] = c->v[i];
    c->n = k;
}
static struct instance *innermost(struct counting *c, const char *name) {
    for (uint32_t i = c->n; i--;) if (!strcmp(c->v[i].name, name)) return &c->v[i];
    return NULL;
}
/* whether a counter of the name is in the CSS counters set of the participant's parent: the ones
   of its scope that its preceding siblings made (the parent's scope, not nested) are not */
static bool parent_has(const struct counting *c, const char *name) {
    for (uint32_t i = 0; i < c->n; i++) {
        const struct instance *x = &c->v[i];
        if ((x->depth < c->len - 1 || (x->scope == c->at[0] && x->nested)) && !strcmp(x->name, name)) return true;
    }
    return false;
}
/* §4.4.2 "instantiate a counter" on the participant: it replaces the innermost counter of the name
   when the participant or a previous sibling created that one */
static struct instance *instantiate(struct counting *c, const char *name, bool reversed, bool valued, int32_t value) {
    if (!c->len) return NULL;
    struct instance *last = innermost(c, name);
    if (last && ((last->scope == c->at[0] && !last->nested) || last->origin == c->origin)) {
        memmove(last, last + 1, (size_t)(c->v + c->n - (last + 1)) * sizeof *last);
        c->n--;
    }
    bool nested = parent_has(c, name);
    node_t *scope = nested ? c->creator : c->at[0];
    uint32_t depth = !nested ? c->len - 1 : c->creator ? c->len : UINT32_MAX;
    if (!grow((void **)&c->v, &c->cap, c->n + 1, sizeof *c->v)) { c->failed = true; return NULL; }
    struct instance *x = &c->v[c->n++];
    *x = (struct instance){name, scope, depth, c->origin, valued ? value : 0, reversed, nested, -1, c->nframes};
    if (reversed && !valued) {
        uint32_t k = c->rev_next++;
        if (c->measure) {
            if (!grow((void **)&c->rev, &c->caprev, k + 1, sizeof *c->rev)) { c->failed = true; return x; }
            c->rev[k] = (struct reversed_start){0};
            c->nrev = k + 1;
            x->pending = (int32_t)k;
        } else if (k < c->nrev) {
            int64_t start = c->rev[k].num + c->rev[k].last;
            x->value = start > INT32_MAX ? INT32_MAX : start < INT32_MIN ? INT32_MIN : (int32_t)start;
        }
    }
    return x;
}
/* the counter of the name the participant uses, increments or sets: one it does not have is
   instantiated on it with 0 (§4.4.1) */
static struct instance *counter_of(struct counting *c, const char *name) {
    struct instance *x = innermost(c, name);
    return x ? x : instantiate(c, name, false, true, 0);
}
/* CSS Containment 2 §3.3: in a style-contained element's subtree (below the element: its own counter
   properties are outside), counter-increment and counter-set are scoped to the subtree and create a
   new counter. The counter they change is one the subtree made; for one made outside it, a new counter
   of 0 as the participant's counter-reset would make it, not nested (the participant, its following
   siblings and their descendants): the outer counter stays as it was for the rest of the document,
   the subtree's other elements among them (WPT css-contain/contain-style-counters-003, -004). */
static struct instance *counter_to_change(struct counting *c, const char *name) {
    struct instance *x = innermost(c, name);
    if (!x || !c->nframes || x->level >= c->nframes) return x ? x : instantiate(c, name, false, true, 0);
    if (!c->len) return NULL; /* a node without a parent creates no counter */
    if (!grow((void **)&c->v, &c->cap, c->n + 1, sizeof *c->v)) { c->failed = true; return NULL; }
    x = &c->v[c->n++];
    *x = (struct instance){name, c->at[0], c->len - 1, c->origin, 0, false, false, -1, c->nframes};
    return x;
}
static void increment(struct counting *c, const char *name, int32_t by) {
    struct instance *x = counter_to_change(c, name);
    if (!x) return;
    int64_t next = (int64_t)x->value + by;
    if (next <= INT32_MAX && next >= INT32_MIN) x->value = (int32_t)next; /* one that would overflow is not made */
    if (x->pending >= 0) { struct reversed_start *r = &c->rev[x->pending]; r->increment -= by; r->touched = true; }
}
static void set(struct counting *c, const char *name, int32_t to) {
    struct instance *x = counter_to_change(c, name);
    if (!x) return;
    x->value = to;
    if (x->pending >= 0) { struct reversed_start *r = &c->rev[x->pending]; r->set = true; r->set_value = to; r->touched = true; }
}
/* §4.1-4.3, in that order: the participant's counter-reset, its counter-increment (with a list
   item's implicit list-item increment, §4.6, unless counter-increment names list-item) and its
   counter-set */
static void apply(struct counting *c, const style_t *st, bool list_item) {
    const struct css_counters *g = css_style_counters(st);
    const struct css_counter_ops *ops = g->ops[CSS_COUNTER_RESET];
    for (uint32_t i = 0; ops && i < ops->n; i++)
        instantiate(c, ops->op[i].name, ops->op[i].reversed, ops->op[i].valued || !ops->op[i].reversed, ops->op[i].value);
    ops = g->ops[CSS_COUNTER_INCREMENT];
    bool mentioned = false;
    for (uint32_t i = 0; ops && i < ops->n; i++) {
        mentioned = mentioned || !strcmp(ops->op[i].name, "list-item");
        increment(c, ops->op[i].name, ops->op[i].value);
    }
    if (list_item && !mentioned) {
        const struct instance *x = innermost(c, "list-item");
        increment(c, "list-item", x && x->reversed ? -1 : 1);
    }
    ops = g->ops[CSS_COUNTER_SET];
    for (uint32_t i = 0; ops && i < ops->n; i++) set(c, ops->op[i].name, ops->op[i].value);
    if (!c->measure) return;
    /* §4.4.2, one element of each pending counter's scope: what it incremented and set */
    for (uint32_t i = 0; i < c->n; i++) {
        if (c->v[i].pending < 0) continue;
        struct reversed_start *r = &c->rev[c->v[i].pending];
        if (!r->touched) continue;
        if (!r->done) {
            if (r->increment) r->last = r->increment > INT32_MAX ? INT32_MAX : r->increment < INT32_MIN ? INT32_MIN : (int32_t)r->increment;
            if (r->set) { r->num += r->set_value; r->done = true; }
            else r->num += r->increment;
        }
        r->increment = 0;
        r->set = r->touched = false;
    }
}
/* whether this walk writes texts: a walk that finds a reversed counter's initial value counts
   with a wrong one from there, so the walk after it writes */
static bool writes(const struct counting *c) { return !c->measure || !c->nrev; }

/* CSS Generated Content 3 §2.1: quotes: auto takes the quotation marks of the content language,
   which needs CLDR's data (not here): the English ones stand for every language */
static const char *quote_of(const style_t *st, uint32_t depth, bool close) {
    static const char *const fallback[4] = {"\xe2\x80\x9c", "\xe2\x80\x9d", "\xe2\x80\x98", "\xe2\x80\x99"};
    const struct css_quotes *q = css_style_list(st)->quotes;
    if (!q) return fallback[(depth ? 2 : 0) + close];
    if (!q->pairs) return "";
    uint32_t pair = depth < q->pairs ? depth : q->pairs - 1; /* deeper ones repeat the last pair */
    return q->quote[2 * pair + close];
}
/* the text a content list generates (§1.1), its counters' values and quotes now theirs; the
   quote depth (§2.2) follows the document's open-quote and close-quote in tree order */
static void content_text(struct counting *c, const style_t *st, const struct css_content *content, sbuf *out) {
    for (uint32_t i = 0; i < content->n; i++) {
        const struct css_content_item *it = &content->item[i];
        switch (it->kind) {
        case CSS_CONTENT_STRING: sb_puts(out, it->text ? it->text : ""); break;
        case CSS_CONTENT_COUNTER: {
            const struct instance *x = counter_of(c, it->text);
            if (x) css_counter_text(&it->style, x->value, false, out);
            break;
        }
        case CSS_CONTENT_COUNTERS: /* every counter of the name in the set, the outermost first */
            if (!counter_of(c, it->text)) break;
            for (uint32_t k = 0, first = 1; k < c->n; k++) {
                if (strcmp(c->v[k].name, it->text)) continue;
                if (!first) sb_puts(out, it->separator ? it->separator : "");
                first = 0;
                css_counter_text(&it->style, c->v[k].value, false, out);
            }
            break;
        case CSS_CONTENT_OPEN_QUOTE: sb_puts(out, quote_of(st, c->quote_depth, false)); c->quote_depth++; break;
        case CSS_CONTENT_CLOSE_QUOTE: /* one that would make the depth negative is nothing */
            if (c->quote_depth) sb_puts(out, quote_of(st, --c->quote_depth, true));
            break;
        case CSS_CONTENT_NO_OPEN_QUOTE: c->quote_depth++; break;
        case CSS_CONTENT_NO_CLOSE_QUOTE: if (c->quote_depth) c->quote_depth--; break;
        }
    }
}
/* CSS Lists 3 §3.2: a marker's text, by the first of its content (not normal), the list item's
   marker image (paint.c draws it, followed by a space, in the place of this text once it loads)
   and its marker string (§3.4). shape: the disc, circle or square that string is (1-3), which
   paint.c draws for an outside marker. */
static void marker_text(struct counting *c, const style_t *item, const style_t *marker, sbuf *out, uint8_t *shape) {
    *shape = 0;
    if (marker && marker->content && !marker->content->none) { content_text(c, marker, marker->content, out); return; }
    const struct css_list *l = css_style_list(item);
    struct css_counter_style style = {item->list_style, l->type_name, l->type_symbols};
    if (style.id == LS_NONE) return;
    const struct instance *x = counter_of(c, "list-item");
    css_counter_text(&style, x ? x->value : 0, true, out);
    if (style.id == LS_DISC || style.id == LS_CIRCLE || style.id == LS_SQUARE)
        *shape = (uint8_t)(style.id == LS_DISC ? 1 : style.id == LS_CIRCLE ? 2 : 3);
}
/* One counting box, in tree order: its element (and then the list item's ::marker, the first of
   its children, CSS Lists 3 §3.1) or its ::before or ::after (whose parent is the element). Of the
   ::marker's own values only content counts: the counter properties do not apply to it (§3.1.1). */
/* the style-contained elements whose subtrees the walk has left (c->chain: the next participant's
   ancestors): the quote depth is what it was before each */
static void leave_contained(struct counting *c) {
    while (c->nframes) {
        const struct contained *f = &c->frames[c->nframes - 1];
        if (f->depth < c->nchain && c->chain[c->nchain - 1 - f->depth] == f->node) return;
        c->quote_depth = f->quote_depth;
        c->nframes--;
    }
}
/* b is the principal box of an element with style containment (contain, or content-visibility
   auto or hidden, CSS Containment 2 §4) */
static bool style_contained(const box_t *b) {
    const node_t *n = b->node;
    return n && n->type == N_ELEM && n->box == b && b->st &&
           ((css_style_effects(b->st)->contain & CSS_CONTAIN_STYLE) || b->st->content_visibility != CV_VISIBLE);
}
static void enter_contained(struct counting *c, box_t *b) {
    if (!ancestors(c, b->node)) return;
    leave_contained(c);
    if (!grow((void **)&c->frames, &c->capframes, c->nframes + 1, sizeof *c->frames)) { c->failed = true; return; }
    c->frames[c->nframes++] = (struct contained){b->node, c->nchain - 1, c->quote_depth};
}
static void record(struct counting *c, box_t *b) {
    node_t *n = b->node;
    unsigned kind = b->counter_kind & BOX_COUNTER_KIND;
    if (!n || !ancestors(c, n)) return;
    leave_contained(c);
    if (kind == BOX_COUNTER_ELEMENT) {
        participant(c, 1, NULL);
        if (!c->len) return; /* a node without a parent creates no counter */
        inherit(c);
        apply(c, b->st, b->st->display == D_LIST_ITEM);
        if (!(b->counter_kind & BOX_COUNTER_MARKER)) return;
        participant(c, 0, b);
        inherit(c);
        if (!writes(c)) return;
        uint8_t shape;
        c->text.n = 0;
        marker_text(c, b->st, b->st->marker, &c->text, &shape);
        const char *text = c->text.p ? c->text.p : "";
        if (b->counter_text) box_counter_text(b->counter_text, text, c->text.n);
        else box_counter_marker(c->d, b, text, c->text.n, shape);
        return;
    }
    participant(c, 0, b);
    inherit(c);
    apply(c, b->st, false);
    if (!writes(c) || !b->counter_text || !b->st->content) return;
    c->text.n = 0;
    content_text(c, b->st, b->st->content, &c->text);
    box_counter_text(b->counter_text, c->text.p ? c->text.p : "", c->text.n);
}
/* the counting boxes below root in tree order: only the subtrees that hold one (counter_tree) */
static void walk(struct counting *c, box_t *root) {
    for (box_t *b = root; b && !c->failed;) {
        if (b->counter_kind & BOX_COUNTER_KIND) record(c, b);
        if (style_contained(b)) enter_contained(c, b); /* its subtree, after the element itself */
        box_t *x = b->first;
        while (x && !x->counter_tree) x = x->next;
        if (x) { b = x; continue; }
        for (;;) {
            if (b == root) return;
            for (x = b->next; x && !x->counter_tree; x = x->next) {}
            if (x) { b = x; break; }
            b = b->parent;
        }
    }
}
void web_counters_update(web_doc *d) {
    if (!d || !d->counters_dirty) return;
    d->counters_dirty = false;
    if (!d->root_box || !d->root_box->counter_tree) return;
    struct counting c = {.d = d, .measure = true};
    walk(&c, d->root_box);
    if (c.nrev && !c.failed) { /* the reversed counters' initial values are known: count again, and write */
        c.measure = false;
        c.n = c.rev_next = c.quote_depth = c.nframes = 0;
        walk(&c, d->root_box);
    }
    if (c.failed) d->counters_dirty = true; /* out of memory: the next publication tries again */
    free(c.v);
    free(c.rev);
    free(c.chain);
    free(c.frames);
    sb_free(&c.text);
}
