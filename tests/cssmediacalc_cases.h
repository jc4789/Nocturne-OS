/* Shared exact-input cases for host arithmetic isolation and native cascade. */
static void media_query_cases(void) {
    static const struct { const char *query; int width; bool result; } cases[] = {
        {"(width<=calc(48rem - .02px))", 1908, false},
        {"(width<=calc(48rem - .02px))", 767, true},
        {"(width<=calc(48rem - .02px))", 768, false},
        {"(max-width:calc(48rem - .02px))", 767, true},
        {"(min-width:calc(1px - 1px))", 1908, true},
        {"(width>calc(40em - 1px))", 767, true},
        {"(calc(40em - 1px)<width<calc(90em + 1px))", 767, true},
        {"(width<=min(1000px, 48rem))", 767, true},
        {"(width>=max(1000px, 48rem))", 767, false},
        {"(width<=clamp(200px, 48rem, 900px))", 767, true},
        {"(width=calc(20vw * 5))", 767, true},
        {"(width>calc(-1px))", 767, true},
        {"(width<calc(1px + 1))", 767, false},
        {"not (width<calc(1px + 1))", 767, false},
        {"(width<calc(50% + 1px))", 767, false},
        {"(width<calc(0% + 1000px))", 767, false},
        {"(width<calc(1000px/0))", 767, false},
        {"(width<calc(1e999px))", 767, false},
        {"(width<=calc(48rem-.02px))", 767, false},
        {"(width<=calc(48rem +.02px))", 767, false},
        {"(width<=calc(48rem + .02px)garbage)", 767, false},
        {"(width<calc(1px + 1)) or (width=767px)", 767, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        check(css_media_evaluate(cases[i].query, cases[i].width, 600, true, NULL, 0) == cases[i].result, cases[i].query);
    check(css_supports_declaration("width", 5, "calc(100% - 1px)", 16), "normal property calc retains shared grammar");
    check(!css_supports_declaration("width", 5, "calc(100%+1px)", 14), "property calc invalid binary spacing is rejected");
}
