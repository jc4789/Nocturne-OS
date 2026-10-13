/* 原UCD入力はホストdriverから供給。実layout.c helperをそのまま呼ぶ。 */
static size_t encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); return 4;
}

static int bounded_decode(void) {
    /* 最後の許可byteの直後をno-accessにし、truncated readを実際に捕捉する。 */
    SYSTEM_INFO info; GetSystemInfo(&info);
    char *pages = VirtualAlloc(NULL, 2 * info.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pages) return 1;
    DWORD old;
    if (!VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old)) return 1;
    const unsigned char *samples[] = {
        (const unsigned char *)"\xC2\xA9", (const unsigned char *)"\xE2\x82\xAC",
        (const unsigned char *)"\xF0\x9F\x98\x80", (const unsigned char *)"\xC0\xAF",
        (const unsigned char *)"\xED\xA0\x80", (const unsigned char *)"\xF4\x90\x80\x80"
    };
    const size_t lengths[] = {2, 3, 4, 2, 3, 4};
    int failed = 0;
    uint32_t cp = 0;
    if (wf_grapheme_decode(pages + info.dwPageSize, 0, &cp) != 0) ++failed;
    for (size_t sample = 0; sample < sizeof(lengths) / sizeof(*lengths); ++sample) {
        for (size_t n = 1; n <= lengths[sample]; ++n) {
            char *start = pages + info.dwPageSize - n;
            memcpy(start, samples[sample], n);
            size_t expected = sample < 3 && n == lengths[sample] ? n : 1;
            if (wf_grapheme_decode(start, n, &cp) != expected) ++failed;
            if (expected == 1 && cp != 0xFFFD) ++failed;
            for (size_t at = 0; at < n;) {
                size_t next = wf_grapheme_next(start, n, at);
                if (next <= at || next > n) { ++failed; break; }
                at = next;
            }
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    return failed;
}

int main(void) {
    printf("BOUNDED %d\n", bounded_decode());
    size_t count;
    while (scanf("%zu", &count) == 1) {
        if (count > SIZE_MAX / 4) return 2;
        char *text = malloc(count * 4 + 1);
        if (!text) return 2;
        size_t n = 0;
        for (size_t i = 0; i < count; ++i) {
            unsigned cp;
            if (scanf("%x", &cp) != 1) return 2;
            n += encode(cp, text + n);
        }
        printf("NEXT 0");
        for (size_t at = 0; at < n;) {
            size_t next = wf_grapheme_next(text, n, at);
            if (next <= at || next > n) return 3;
            printf(" %zu", next); at = next;
        }
        printf("\nSTREAM");
        wf_grapheme_state state = {0};
        for (size_t at = 0; at < n;) {
            uint32_t cp;
            size_t length = wf_grapheme_decode(text + at, n - at, &cp);
            /* 每コード点を別runとみなし、runを跨いでも状態を持ち越す。 */
            if (wf_grapheme_break(&state, cp)) printf(" %zu", at);
            at += length;
        }
        printf(" %zu\n", n);
        free(text);
    }
    return 0;
}
