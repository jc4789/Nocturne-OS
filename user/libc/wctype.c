/* ISO C <wctype.h> over the Unicode tables scripts/unicode_wctype.py
   generates. The class definitions are in <wctype.h>. */
#include <wctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "wctype_data.h"

#define CODE_POINTS 0x110000u

/* The class numbers wctype gives out: one more than the name's index. */
enum {
    CLASS_ALNUM = 1, CLASS_ALPHA, CLASS_BLANK, CLASS_CNTRL, CLASS_DIGIT, CLASS_GRAPH,
    CLASS_LOWER, CLASS_PRINT, CLASS_PUNCT, CLASS_SPACE, CLASS_UPPER, CLASS_XDIGIT,
};
static const char *const class_names[] = {
    "alnum", "alpha", "blank", "cntrl", "digit", "graph",
    "lower", "print", "punct", "space", "upper", "xdigit",
};
enum { TRANS_TOLOWER = 1, TRANS_TOUPPER };

/* The table's bits for c; none for WEOF and values that are not code points. */
static unsigned classes_of(wint_t c) {
    uint32_t u = (uint32_t)c;
    if (u >= CODE_POINTS)
        return 0;
    uint32_t block = wctype_index[u >> WCTYPE_SHIFT];
    return wctype_blocks[block << WCTYPE_SHIFT | (u & ((1u << WCTYPE_SHIFT) - 1))];
}

static int is_digit(wint_t c) { return c >= '0' && c <= '9'; }

int iswctype(wint_t c, wctype_t type) {
    unsigned bits = classes_of(c);
    switch (type) {
    case CLASS_ALNUM:  return (bits & WC_ALPHA) || is_digit(c);
    case CLASS_ALPHA:  return (bits & WC_ALPHA) != 0;
    case CLASS_BLANK:  return (bits & WC_BLANK) != 0;
    case CLASS_CNTRL:  return (bits & WC_CNTRL) != 0;
    case CLASS_DIGIT:  return is_digit(c);
    case CLASS_GRAPH:  return (bits & WC_GRAPH) != 0;
    case CLASS_LOWER:  return (bits & WC_LOWER) != 0;
    case CLASS_PRINT:  return (bits & WC_GRAPH) || (bits & (WC_BLANK | WC_CNTRL)) == WC_BLANK;
    case CLASS_PUNCT:  return (bits & WC_PUNCT) != 0;
    case CLASS_SPACE:  return (bits & WC_SPACE) != 0;
    case CLASS_UPPER:  return (bits & WC_UPPER) != 0;
    case CLASS_XDIGIT: return is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
    }
    return 0;
}

wctype_t wctype(const char *name) {
    for (size_t i = 0; i < sizeof class_names / sizeof *class_names; i++)
        if (strcmp(name, class_names[i]) == 0)
            return (wctype_t)(i + 1);
    return 0;
}

int iswalnum(wint_t c) { return iswctype(c, CLASS_ALNUM); }
int iswalpha(wint_t c) { return iswctype(c, CLASS_ALPHA); }
int iswblank(wint_t c) { return iswctype(c, CLASS_BLANK); }
int iswcntrl(wint_t c) { return iswctype(c, CLASS_CNTRL); }
int iswdigit(wint_t c) { return iswctype(c, CLASS_DIGIT); }
int iswgraph(wint_t c) { return iswctype(c, CLASS_GRAPH); }
int iswlower(wint_t c) { return iswctype(c, CLASS_LOWER); }
int iswprint(wint_t c) { return iswctype(c, CLASS_PRINT); }
int iswpunct(wint_t c) { return iswctype(c, CLASS_PUNCT); }
int iswspace(wint_t c) { return iswctype(c, CLASS_SPACE); }
int iswupper(wint_t c) { return iswctype(c, CLASS_UPPER); }
int iswxdigit(wint_t c) { return iswctype(c, CLASS_XDIGIT); }

/* The runs are sorted and disjoint: find the last one starting at or before
   c, then whether c is one of its members. */
static wint_t map_case(wint_t c, const struct wctype_case_run *runs, size_t n) {
    uint32_t u = (uint32_t)c;
    if (u >= CODE_POINTS)
        return c;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (runs[mid].first <= u)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return c;
    const struct wctype_case_run *run = &runs[lo - 1];
    uint32_t offset = u - run->first;
    if (offset % run->step || offset / run->step >= run->count)
        return c;
    return (wint_t)((int32_t)u + run->delta);
}

wint_t towlower(wint_t c) {
    if ((uint32_t)c < 0x80)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    return map_case(c, wctype_to_lower, sizeof wctype_to_lower / sizeof *wctype_to_lower);
}

wint_t towupper(wint_t c) {
    if ((uint32_t)c < 0x80)
        return c >= 'a' && c <= 'z' ? c - 32 : c;
    return map_case(c, wctype_to_upper, sizeof wctype_to_upper / sizeof *wctype_to_upper);
}

wctrans_t wctrans(const char *name) {
    if (strcmp(name, "tolower") == 0)
        return TRANS_TOLOWER;
    if (strcmp(name, "toupper") == 0)
        return TRANS_TOUPPER;
    return 0;
}

wint_t towctrans(wint_t c, wctrans_t trans) {
    switch (trans) {
    case TRANS_TOLOWER: return towlower(c);
    case TRANS_TOUPPER: return towupper(c);
    }
    return c;
}
