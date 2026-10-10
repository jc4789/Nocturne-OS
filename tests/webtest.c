/* webtest: the web engine (libc/web) offline: layout positions, painted pixels, links, forms,
   charsets, URLs, resources and hostile input. Prints FAIL lines and "webtest: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "web.h"
#include "web_form_validation_cases.h"

#define BASE "http://h.test/dir/page.html"
#define VW 800
#define VH 600

static int failed, total;

static void fail(const char *name, const char *fmt, int a, int b) {
    printf("FAIL %s: ", name);
    printf(fmt, a, b);
    printf("\n");
    failed++;
}

static web_doc *load(const char *html) {
    web_doc *d = web_parse(html, strlen(html), BASE, NULL);
    while (web_pending_stylesheet(d)) web_stylesheet_loaded(d, NULL, 0);
    web_layout(d, VW, VH);
    return d;
}

static web_doc *load_controls(const char *html) {
    web_doc *d=web_live(html,strlen(html),BASE,NULL,NULL);
    if(d){web_tick(d,uptime_ms());web_layout(d,VW,VH);}
    return d;
}

/* the y of #t must be want (to within a pixel) */
static void y_is(const char *name, const char *html, int want) {
    printf("WEB-STAGE %s\n",name);fflush(stdout);
    total++;
    web_doc *d = load(html);
    int y = web_anchor_y(d, "t");
    if (y < want - 1 || y > want + 1) fail(name, "#t at y=%d, expected %d", y, want);
    web_free(d);
}

/* ---------------------------------------------------------------- layout */
static void test_layout(void) {
    y_is("block", "<body style=margin:0><div style=height:100px></div><div id=t></div>", 100);
    y_is("body-margin", "<div style=height:10px></div><p id=t style=margin:0>x</p>", 18);
    y_is("margin-collapse",
         "<body style=margin:0><div style='margin-bottom:30px;height:10px'></div><div id=t style=margin-top:20px></div>", 40);
    y_is("display-none", "<body style=margin:0><div style='display:none;height:500px'></div><div id=t></div>", 0);
    y_is("float-clear",
         "<body style=margin:0><div style='float:left;width:50px;height:70px'></div><div id=t style=clear:left></div>", 70);
    y_is("clearfix",
         "<style>.cf::after{content:'';display:table;clear:both}</style><body style=margin:0>"
         "<div class=cf><div style='float:left;height:60px;width:10px'></div></div><div id=t></div>", 60);
    y_is("float-beside", /* two floats that fit side by side do not stack */
         "<body style=margin:0><div style=width:300px><div style='float:left;width:150px;height:40px'></div>"
         "<div id=t style='float:left;width:150px;height:40px'></div></div>", 0);
    y_is("flex-column-gap",
         "<body style=margin:0><div style='display:flex;flex-direction:column;gap:5px'><div style=height:30px></div>"
         "<div id=t></div></div>", 35);
    y_is("flex-wrap",
         "<body style=margin:0><div style='display:flex;flex-wrap:wrap;width:100px'><div style='width:60px;height:20px'></div>"
         "<div style='width:60px;height:20px'></div></div><div id=t></div>", 40);
    y_is("flex-align-center",
         "<body style=margin:0><div style='display:flex;align-items:center;height:100px'><div id=t style=height:20px></div></div>", 40);
    y_is("grid-rows",
         "<body style=margin:0><div style='display:grid;grid-template-rows:40px 25px'><div></div><div></div></div><div id=t></div>", 65);
    y_is("grid-auto-placement",
         "<body style=margin:0><div style='display:grid;grid-template-columns:1fr 1fr;width:200px'><div style=height:10px></div>"
         "<div style=height:30px></div><div id=t></div></div>", 30);
    y_is("grid-areas",
         "<body style=margin:0><div style=\"display:grid;grid-template-areas:'a b' 'c c';grid-template-rows:50px 10px\">"
         "<div id=t style=grid-area:c></div><div style=grid-area:a></div></div>", 50);
    y_is("table-rows",
         "<body style=margin:0><table style=border-spacing:0><tr><td style='height:50px;padding:0'>a<tr><td id=t>b</table>", 50);
    y_is("table-caption-bottom",
         "<body style=margin:0><table style='border-spacing:0;caption-side:bottom'><caption id=t>c</caption>"
         "<tr><td style='height:50px;padding:0'>a</table>", 50);
    y_is("abspos",
         "<body style=margin:0><div style='position:relative;height:10px;top:5px'><div id=t style='position:absolute;top:123px'></div></div>", 128);
    y_is("percent-height",
         "<body style=margin:0><div style=height:200px><div style=height:50%></div><div id=t></div></div>", 100);
    y_is("box-sizing",
         "<body style=margin:0><div style='box-sizing:border-box;height:50px;padding:10px;border:5px solid'></div><div id=t></div>", 50);
    y_is("cascade-specificity", "<style>#a{height:10px} div{height:99px}</style><body style=margin:0><div id=a></div><div id=t></div>", 10);
    y_is("cascade-important", "<style>#a{height:10px} div{height:99px!important}</style><body style=margin:0><div id=a></div><div id=t></div>", 99);
    y_is("css-var", "<style>:root{--h:33px} #a{height:var(--h)}</style><body style=margin:0><div id=a></div><div id=t></div>", 33);
    y_is("css-calc", "<body style=margin:0><div style='height:calc(10px + 2 * 7px)'></div><div id=t></div>", 24);
    y_is("anchor-name", "<body style=margin:0><div style=height:77px></div><a name=t>x</a>", 77);

    /* @media follows the viewport width */
    const char *mq = "<style>#a{height:10px} @media (max-width:500px){#a{height:300px}}</style><body style=margin:0><div id=a></div><div id=t></div>";
    total++;
    web_doc *d = load(mq);
    int y1 = web_anchor_y(d, "t");
    web_layout(d, 400, VH);
    int y2 = web_anchor_y(d, "t");
    if (y1 != 10 || y2 != 300) fail("media-query", "#t at %d / %d, expected 10 / 300", y1, y2);
    web_free(d);

    /* the document height */
    total++;
    d = load("<body style=margin:0><div style=height:1234px></div>");
    if (web_doc_height(d) != 1234) fail("doc-height", "height %d, expected %d", web_doc_height(d), 1234);
    web_free(d);
}

/* ---------------------------------------------------------------- painting */
static uint32_t px[VW * 100];

static uint32_t at(int x, int y) { return px[y * VW + x] & 0xFFFFFF; }

static void color_is(const char *name, int x, int y, uint32_t want, int tol) {
    total++;
    uint32_t c = at(x, y);
    int dr = (int)((c >> 16) & 255) - (int)((want >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((want >> 8) & 255),
        db = (int)(c & 255) - (int)(want & 255);
    if (abs(dr) > tol || abs(dg) > tol || abs(db) > tol) fail(name, "pixel %06x, expected %06x", (int)c, (int)want);
}

static void paint(web_doc *d, int scroll) {
    canvas_t c;
    gfx_init(&c, px, VW, 100, VW);
    web_paint(d, &c, 0, 0, VW, 100, 0, scroll);
}

static void test_paint(void) {
    web_doc *d = load("<body style='margin:0;background:#00ff00'>"
                      "<div style='width:10px;height:10px;background:red'></div>"
                      "<div style='width:100px;height:10px;margin:0 auto;background:#0000ff'></div>"
                      "<div style='width:20px;height:20px;border:4px solid #ffff00;background:white'></div>"
                      "<div style='width:10px;height:10px;background:rgba(255,0,0,.5)'></div>"
                      "<div style='width:40px;height:10px;background:linear-gradient(to right,#000,#fff)'></div>"
                      "<div style='width:100px;height:10px;background:linear-gradient(90deg,red 0 50%,blue 50%)'></div>"
                      "<div style='width:100px;height:10px;background:linear-gradient(to right,#000,#fff,#000)'></div>"
                      "<div style='width:20px;height:10px;background:#000 radial-gradient(circle closest-side,#fff,transparent)'></div>");
    paint(d, 0);
    color_is("paint-bg", 5, 5, 0xff0000, 0);
    color_is("paint-canvas", 50, 5, 0x00ff00, 0);
    color_is("paint-auto-margin-in", 400, 15, 0x0000ff, 0);
    color_is("paint-auto-margin-out", 345, 15, 0x00ff00, 0);
    color_is("paint-border", 1, 21, 0xffff00, 0);
    color_is("paint-border-inside", 10, 30, 0xffffff, 0);
    color_is("paint-alpha", 5, 52, 0x7f7f00, 3); /* half red over green */
    color_is("gradient-to-right", 20, 62, 0x828282, 4);
    color_is("gradient-hard-stop-1", 45, 72, 0xff0000, 0);
    color_is("gradient-hard-stop-2", 55, 72, 0x0000ff, 0);
    color_is("gradient-three-stops", 50, 82, 0xfafafa, 8);
    color_is("gradient-three-stops-end", 99, 82, 0x000000, 8);
    color_is("gradient-radial-centre", 10, 93, 0xdcdcdc, 8); /* 0.7 px from the centre, fading to transparent over black */
    color_is("gradient-radial-edge", 1, 89, 0x000000, 0);
    paint(d, 10); /* scrolled by 10: the blue bar is at the top */
    color_is("paint-scrolled", 400, 5, 0x0000ff, 0);
    web_free(d);
}

/* Narrow regression coverage for the paint-order defect found on the live
   DeepMind page; these pixel checks are not website acceptance tests. */
static void test_item_z_order(void) {
    const char *layouts[] = {"grid", "flex"};
    for (int i = 0; i < 2; i++) {
        char html[1024];
        snprintf(html, sizeof html,
                 "<body style=margin:0><div style='position:relative;display:%s;width:100px;height:50px'>"
                 "<a href=/foreground style='display:block;width:100px;height:50px;background:red;z-index:1'></a>"
                 "<div style='position:absolute;inset:0;background:blue;z-index:0'></div></div>", layouts[i]);
        web_doc *d = load(html);
        paint(d, 0);
        color_is(i ? "flex-item-z-index" : "grid-item-z-index", 10, 10, 0xff0000, 0);
        struct web_hit h;
        total++;
        if (!web_node_action(d, web_node_at(d, 10, 10), &h) || h.kind != WEB_HIT_LINK ||
            !h.href || strcmp(h.href, "http://h.test/foreground"))
            fail("item-z-index-hit", "foreground link missing (kind %d)", h.kind, 0);
        web_free(d);
    }
    /* A positioned child belongs to the grid item's context, not the page. */
    web_doc *d = load("<body style=margin:0><div style='position:relative;display:grid;width:100px;height:50px'>"
        "<div style='height:50px;background:red;z-index:1'>"
        "<a href=/nested style='position:relative;display:block;height:50px;background:lime'></a></div>"
        "<div style='position:absolute;inset:0;background:blue;z-index:0'></div></div>");
    paint(d, 0);
    color_is("grid-context-auto-child", 10, 10, 0x00ff00, 0);
    struct web_hit hit;
    total++;
    if (!web_node_action(d, web_node_at(d, 10, 10), &hit) || !hit.href || strcmp(hit.href, "http://h.test/nested"))
        fail("grid-context-auto-hit", "nested link missing", 0, 0);
    web_free(d);

    d = load("<body style=margin:0><div style='position:relative;z-index:1;width:100px;height:50px'>"
        "<div style='position:absolute;inset:0;background:red;z-index:99'></div></div>"
        "<div style='position:absolute;top:0;left:0;width:100px;height:50px;background:blue;z-index:2'></div>");
    paint(d, 0);
    color_is("context-descendant-cannot-escape", 10, 10, 0x0000ff, 0);
    web_free(d);

    d = load("<body style=margin:0><div style='position:relative;z-index:1;width:100px;height:50px;background:lime'>"
        "<div style='position:absolute;inset:0;background:red;z-index:-1'></div></div>");
    paint(d, 0);
    color_is("negative-after-context-background", 10, 10, 0xff0000, 0);
    web_free(d);

    d = load("<body style=margin:0><div style='position:relative;width:100px;height:50px'>"
        "<div style='position:absolute;inset:0;background:red;z-index:3'></div></div>"
        "<div style='position:absolute;top:0;left:0;width:100px;height:50px;background:blue;z-index:2'></div>");
    paint(d, 0);
    color_is("auto-does-not-isolate-z-index", 10, 10, 0xff0000, 0);
    web_free(d);

    /* A paint context is not necessarily the child's containing block. */
    const char *clip_cases[] = {
        "<div style='position:relative;z-index:1;overflow:hidden;width:20px;height:20px'>"
        "<a href=/escape style='position:fixed;left:40px;top:0;width:20px;height:20px;background:red'></a></div>",
        "<div style='position:relative;display:grid;width:100px;height:50px'>"
        "<div style='z-index:1;overflow:hidden;width:20px;height:20px'>"
        "<a href=/escape style='position:absolute;left:40px;top:0;width:20px;height:20px;background:red'></a></div></div>",
        "<div style='position:relative;z-index:1;overflow:hidden;top:1000px;width:20px;height:20px'>"
        "<div style='position:relative;z-index:2;overflow:hidden;width:10px;height:10px'>"
        "<a href=/escape style='position:fixed;left:40px;top:0;width:20px;height:20px;background:red'></a></div></div>"
    };
    for (int i = 0; i < 3; i++) {
        char html[1024];
        snprintf(html, sizeof html, "<body style=margin:0>%s", clip_cases[i]);
        d = load(html);
        paint(d, 0);
        color_is("context-overflow-escape", 45, 5, 0xff0000, 0);
        total++;
        if (!web_node_action(d, web_node_at(d, 45, 5), &hit) || !hit.href || strcmp(hit.href, "http://h.test/escape"))
            fail("context-overflow-escape-hit", "case %d lost positioned link", i, 0);
        web_free(d);
    }
    /* Conversely, overflow on the actual containing block must still apply. */
    d = load("<body style=margin:0><div style='position:relative;z-index:1;overflow:hidden;width:20px;height:20px'>"
        "<a href=/clipped style='position:absolute;left:40px;top:0;width:20px;height:20px;background:red'></a></div>");
    paint(d, 0);
    color_is("context-overflow-containing-block", 45, 5, 0xffffff, 0);
    total++;
    if (web_node_action(d, web_node_at(d, 45, 5), &hit))
        fail("context-overflow-containing-block-hit", "clipped link is actionable", 0, 0);
    web_free(d);
}

/* ---------------------------------------------------------------- links and forms */
static web_node *node_at(web_doc *d, int x, int y, int kind, const char *name) {
    struct web_hit h;
    total++;
    if (!web_hit_test(d, x, y, &h) || h.kind != kind) {
        fail(name, "hit kind %d, expected %d", h.kind, kind);
        return NULL;
    }
    return h.node;
}

static void str_is(const char *name, const char *got, const char *want) {
    total++;
    if (!got || strcmp(got, want)) {
        printf("FAIL %s: got '%s', expected '%s'\n", name, got ? got : "(null)", want);
        failed++;
    }
}

static void test_forms(void) {
    web_doc *d = load_controls("<body style=margin:0><a href='/x?y=1' style=display:block;height:20px>link</a>"
                      "<a href='sub/p#frag' style=display:block;height:20px>rel</a>");
    struct web_hit h;
    web_hit_test(d, 5, 5, &h);
    str_is("link-absolute", h.href, "http://h.test/x?y=1");
    web_hit_test(d, 5, 25, &h);
    str_is("link-relative", h.href, "http://h.test/dir/sub/p#frag");
    web_free(d);

    d = load_controls("<style>input,select{display:block;height:20px;margin:0;box-sizing:border-box}</style>"
             "<body style=margin:0><form action=/s><input name=q value=hi>"
             "<input type=hidden name=h value='a b'><input type=checkbox name=c checked>"
             "<select name=s><option>one<option value=2>two</select>"
             "<input type=submit name=go value=Go></form>");
    web_node *q = node_at(d, 5, 5, WEB_HIT_TEXT_INPUT, "form-text-hit");
    web_node *cb = node_at(d, 5, 25, WEB_HIT_CHECKBOX, "form-checkbox-hit");
    web_node *sel = node_at(d, 5, 45, WEB_HIT_SELECT, "form-select-hit");
    web_node *go = node_at(d, 5, 65, WEB_HIT_SUBMIT, "form-submit-hit");
    if (q && cb && sel && go) {
        char *url, *body;
        web_submit(d, go, &url, &body);
        str_is("form-get", url, "http://h.test/s?q=hi&h=a+b&c=on&s=one&go=Go");
        total++;
        if (body) fail("form-get-body", "a GET has a body", 0, 0);
        free(url);
        free(body);
        web_focus(d, q);
        struct gui_event e = {.type = EV_KEY, .key = 'x', .pressed = 1};
        total++;
        if (web_key(d, &e) != 1) fail("form-typing", "web_key did not take a character", 0, 0);
        e.key = NKEY_BACKSPACE;
        web_key(d, &e);
        web_key(d, &e);
        e.key = '!';
        web_key(d, &e);
        web_toggle(d, cb);
        const char *labels[4];
        int cur = -1;
        total++;
        if (web_select_options(d, sel, labels, 4, &cur) != 2 || cur != 0) fail("form-select-options", "options, selected %d", cur, 0);
        web_select_set(d, sel, 1);
        e.key = NKEY_ENTER;
        total++;
        if (web_key(d, &e) != 2) fail("form-enter", "Enter in a text field does not submit", 0, 0);
        web_submit(d, q, &url, &body);
        str_is("form-edited", url, "http://h.test/s?q=h%21&h=a+b&s=2");
        free(url);
        free(body);
    }
    web_free(d);

    d = load_controls("<body style=margin:0><form method=post action=p><input name=a value='1&2=3 4' style=display:block;height:20px></form>");
    web_node *a = node_at(d, 5, 5, WEB_HIT_TEXT_INPUT, "form-post-hit");
    if (a) {
        char *url, *body;
        web_submit(d, a, &url, &body);
        str_is("form-post-url", url, "http://h.test/dir/p");
        str_is("form-post-body", body, "a=1%262%3D3+4");
        free(url);
        free(body);
    }
    web_free(d);
}

/* ---------------------------------------------------------------- text, URLs, resources */
static void title_is(const char *name, const char *html, const char *charset, const char *want) {
    web_doc *d = web_parse(html, strlen(html), BASE, charset);
    str_is(name, web_title(d), want);
    web_free(d);
}

static void url_is(const char *name, const char *base, const char *rel, const char *want) {
    char out[512];
    str_is(name, web_resolve_url(base, rel, out, sizeof out) ? out : NULL, want);
}

static const unsigned char blue_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x05, 0x08, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x13, 0xc1, 0xf5, 0x00, 0x00, 0x00,
    0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x60, 0xf8, 0x0f, 0x43, 0xc4, 0xb1, 0x00, 0x66, 0xd9,
    0x0e, 0xf2, 0x89, 0x1a, 0x34, 0xe3, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

static void test_text(void) {
    title_is("title-entities", "<title>a &amp; b &lt;&#x263A;&hellip;</title>", NULL, "a & b <\xe2\x98\xba\xe2\x80\xa6");
    title_is("title-whitespace", "<title>\n  two   words \n</title>", NULL, "two words");
    title_is("charset-header", "<title>caf\xe9 \x93q\x94</title>", "windows-1252", "caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d");
    title_is("charset-meta", "<meta charset=iso-8859-1><title>\xe9</title>", NULL, "\xc3\xa9");
    title_is("charset-utf8", "<title>\xe6\x9c\x88</title>", NULL, "\xe6\x9c\x88");

    url_is("url-dotdot", "http://a/b/c?d", "../x#y", "http://a/x#y");
    url_is("url-scheme-relative", "https://a/b", "//c/d", "https://c/d");
    url_is("url-query-only", "http://a/b/c", "?q=1", "http://a/b/c?q=1");
    url_is("url-absolute", "http://a/b", "HTTPS://Example.COM", "https://example.com/");
    url_is("url-port", "http://a:8080/b/", "c", "http://a:8080/b/c");

    web_doc *d = web_parse("<meta http-equiv=refresh content='3; url=/next'>", 48, BASE, NULL);
    int delay = -1;
    str_is("meta-refresh", web_refresh_url(d, &delay), "http://h.test/next");
    total++;
    if (delay != 3) fail("meta-refresh-delay", "delay %d, expected %d", delay, 3);
    web_free(d);

    /* an external stylesheet, then an image */
    const char *html = "<link rel=stylesheet href=../s.css><link rel=stylesheet media=print href=p.css>"
                       "<body style=margin:0><div id=a></div><img src=i.png style=display:block><div id=t></div>";
    d = web_parse(html, strlen(html), BASE, NULL);
    str_is("stylesheet-url", web_pending_stylesheet(d), "http://h.test/s.css");
    web_stylesheet_loaded(d, "#a { height: 70px }", 19);
    total++;
    if (web_pending_stylesheet(d)) fail("stylesheet-print-skipped", "a print stylesheet was requested", 0, 0);
    web_layout(d, VW, VH);
    total++;
    if (web_image_count(d) != 1 || !web_image_wanted(d, 0)) fail("image-wanted", "%d images", web_image_count(d), 0);
    else {
        str_is("image-url", web_image_url(d, 0), "http://h.test/dir/i.png");
        web_image_loaded(d, 0, blue_png, sizeof blue_png);
        web_layout(d, VW, VH);
        total++;
        if (web_anchor_y(d, "t") != 75) fail("image-size", "#t at %d, expected %d", web_anchor_y(d, "t"), 75);
        paint(d, 0);
        color_is("image-pixels", 1, 72, 0x0000ff, 0);
    }
    web_free(d);

    d = load("<body style=margin:0><div style=height:300px></div><p style=margin:0>a needle here</p>");
    int y = web_find(d, "NEEDLE", 0);
    total++;
    if (y < 295 || y > 305) fail("find", "found at %d, expected about %d", y, 300);
    total++;
    if (web_find(d, "haystack", 0) >= 0) fail("find-missing", "found something", 0, 0);
    web_free(d);
}

/* ---------------------------------------------------------------- hostile input */
static void survives(const char *name, const char *html) {
    printf("WEB-STAGE %s\n",name);fflush(stdout);
    total++;
    web_doc *d = load(html);
    int h = web_doc_height(d);
    paint(d, 0);
    if (h < 0) fail(name, "height %d", h, 0);
    web_free(d);
}

static char *repeat(const char *a, const char *b, int n) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc((la + lb) * (size_t)n + 1), *p = s;
    for (int i = 0; i < n; i++) memcpy(p, a, la), p += la;
    for (int i = 0; i < n; i++) memcpy(p, b, lb), p += lb;
    *p = 0;
    return s;
}

static void test_hostile(void) {
    survives("tag-soup", "<table><b><tr><i><td>a<p>b</table></b></i><li><dd>c</ul></ol><select><div>d</select><frameset>");
    survives("garbage-css", "<style>}}{{ div{color:red;;;width:calc(1px+);} @media ((( { a{} } @x;</style><div style='width:-5px;"
                            "height:1e30px;margin:nanpx;grid-template-columns:repeat(99999,1fr);flex:-1'>x</div>");
    survives("huge-colspan", "<table><tr><td colspan=100000 rowspan=100000>a<td>b<tr><td>c</table>");
    survives("unterminated", "<div class='a><!-- <script>var x = '</div>");
    char *s = repeat("<div>", "</div>", 5000);
    survives("deep-divs", s);
    free(s);
    s = repeat("<table><tr><td>", "</table>", 300);
    survives("deep-tables", s);
    free(s);
    s = repeat("<span style=display:flex>", "</span>", 2000);
    survives("deep-flex", s);
    free(s);

    /* a long page: layout time is logged */
    s = repeat("<p>Lorem ipsum dolor sit amet, <b>consectetur</b> adipiscing elit, <a href=x>sed do</a> eiusmod.</p>", "", 3000);
    printf("WEB-STAGE 3000-paragraphs\n");fflush(stdout);
    uint64_t t0 = uptime_ms();
    web_doc *d = load(s);
    printf("webtest: 3000 paragraphs laid out in %d ms, height %d\n", (int)(uptime_ms() - t0), web_doc_height(d));
    web_free(d);
    free(s);
}

int main(void) {
    printf("WEB-STAGE layout\n");fflush(stdout);
    test_layout();
    printf("WEB-STAGE test_paint\n");fflush(stdout);
    test_paint();
    printf("WEB-STAGE test_item_z_order\n");fflush(stdout);
    test_item_z_order();
    printf("WEB-STAGE test_forms\n");fflush(stdout);
    test_forms();
    printf("WEB-STAGE form-validation\n");fflush(stdout);
    run_native_form_validation_cases(&total,&failed);
    printf("WEB-STAGE test_text\n");fflush(stdout);
    test_text();
    printf("WEB-STAGE test_hostile\n");fflush(stdout);
    test_hostile();
    printf("webtest: %d checks, %d failed\n", total, failed);
    return failed != 0;
}
