/* Native shadow DOM -> cascade -> flat boxes -> raster/hit/resource checks.
   This is an offline engine regression, not real-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"

#define VW 320
#define VH 240
static uint32_t pixels[VW * VH];
static int checks, failed;
static void check(bool ok, const char *name) {
    checks++;
    if (!ok) { failed++; printf("FAIL shadow %s\n", name); }
}
static node_t *query(web_doc *d, node_t *scope, const char *selector) {
    pvec list = {0};
    bool valid = css_select(d, scope, selector, &list);
    node_t *n = valid && list.n == 1 ? list.v[0] : NULL;
    pv_free(&list); return n;
}
static web_doc *load(const char *markup) {
    web_doc *d = web_live(markup, strlen(markup), "https://shadow.test/page", "utf-8", NULL);
    if (d) for (int i = 0; i < 8; i++) web_tick(d, uptime_ms());
    return d;
}
static void paint(web_doc *d) {
    canvas_t canvas;
    memset(pixels, 0, sizeof pixels);
    gfx_init(&canvas, pixels, VW, VH, VW);
    web_layout(d, VW, VH);
    web_paint(d, &canvas, 0, 0, VW, VH, 0, 0);
}
static int rgb(int x, int y) { return pixels[y * VW + x] & 0xffffff; }
static bool rect(web_doc *d, node_t *n, int left, int top, int width, int height) {
    int x = -1, y = -1, w = -1, h = -1;
    bool ok = web_node_rect(d, n, &x, &y, &w, &h);
    if (!ok || x != left || y != top || w != width || h != height)
        printf("  shadow rect actual=%d %d,%d %dx%d expected=%d,%d %dx%d\n", ok, x, y, w, h, left, top, width, height);
    return ok && x == left && y == top && w == width && h == height;
}
static node_t *attach(web_doc *d, node_t *host, const char *markup, bool closed, bool delegates) {
    node_t *root = doc_shadow_attach(d, host, closed, delegates, false, false, false);
    if (!root || !doc_node_html(d, root, markup, strlen(markup))) return NULL;
    return root;
}
static void rendering(void) {
    const char *html = "<!doctype html><html><head><style>html,body{margin:0}"
        ".scope{background:blue}#host{display:block;width:120px!important;height:160px;background:blue;color:#112233}"
        "</style></head><body><div id=host class=enabled>"
        "<span id=assigned class=light slot=n style='display:block;width:24px;height:12px'><b id=lightchild></b></span>"
        "<span id=hidden slot=absent style='display:block;width:24px;height:180px;background:magenta'></span>"
        "</div><div id=outside class=scope style='height:12px;width:24px'></div></body></html>";
    const char *shadow = "<style>:host{width:80px!important;height:140px;background:green;color:#336699}"
        ":host(.enabled)>#inside{background:lime}#host{padding:31px!important}"
        ".enabled:host{margin-top:33px}.scope{display:block;width:24px;height:12px}"
        ".light{background:yellow}slot::slotted(.light){background:red}"
        "</style><div id=inside class=scope></div>"
        "<slot id=slot name=n style='color:#abcdef'><span id=fallback style='display:block;width:24px;height:12px;background:yellow'></span></slot>"
        "<div id=inherited style='width:24px;height:12px;background:currentColor'></div>";
    web_doc *d = load(html);
    check(d != NULL, "document allocation"); if (!d) return;
    node_t *host = query(d, d->root, "#host"), *assigned = query(d, d->root, "#assigned");
    node_t *hidden = query(d, d->root, "#hidden"), *outside = query(d, d->root, "#outside");
    node_t *root = host ? attach(d, host, shadow, false, false) : NULL;
    check(root != NULL && assigned && hidden && outside, "native attachment");
    if (!root || !assigned || !hidden || !outside) { web_free(d); return; }
    node_t *inside = query(d, root, "#inside"), *slot = query(d, root, "#slot");
    node_t *fallback = query(d, root, "#fallback"), *inherited = query(d, root, "#inherited");
    check(inside && slot && fallback && inherited, "shadow query");
    if (!inside || !slot || !fallback || !inherited) { web_free(d); return; }
    paint(d);
    check(root->parent == NULL && root->shadow_host == host && assigned->parent == host, "DOM parents are unchanged");
    check(!query(d, d->root, "#inside") && !query(d, root, "#assigned"), "query does not cross shadow or slot boundary");
    check(doc_flat_parent(assigned) == slot && doc_flat_parent(inside) == host && !doc_flat_parent(hidden), "native flat parents");
    check(rect(d, host, 0, 0, 80, 160), "important inner context wins; ordinary outer height wins");
    check(rect(d, inside, 0, 0, 24, 12), "host compound and child combinator");
    check(rect(d, assigned, 0, 12, 24, 12), "slot projection boxes");
    check(!hidden->box && !fallback->box && !fallback->style, "unassigned light and assigned fallback are suppressed");
    check(inside->style->color == RGB(17,34,51) && inherited->style->color == RGB(17,34,51), "shadow inheritance from host");
    check(assigned->style->color == RGB(171,205,239), "assigned inheritance from slot");
    check(rgb(4,4) == 0x00ff00 && rgb(4,16) == 0xff0000 && rgb(4,28) == 0x112233 && rgb(4,164) == 0x0000ff,
          "raster host/slotted styling and two-way stylesheet isolation");
    check(web_node_at(d,4,4) == inside && web_node_at(d,4,16) == assigned, "hit testing actual projected nodes");
    check(doc_node_attr(d, assigned, "slot", "missing"), "change slot name");
    paint(d);
    check(!assigned->box && fallback->box && rgb(4,16) == 0xffff00, "reassignment removes old box and shows fallback");
    check(doc_node_attr(d, assigned, "slot", "n"), "restore assignment");
    paint(d);
    check(assigned->box && !fallback->box && rgb(4,16) == 0xff0000, "reassignment restores original identity and paint");
    web_free(d);
}
static void resources_and_focus(void) {
    web_doc *d = load("<!doctype html><body style='margin:0'><div id=h></div><div id=outer class=external></div></body>");
    check(d != NULL, "resource document"); if (!d) return;
    node_t *host = query(d,d->root,"#h");
    node_t *root = host ? attach(d,host,
        "<link rel=stylesheet href='/component.css'><style>:host{display:block}.focused:focus{background:lime}"
        ".wrap:focus-within{color:red}</style><div id=wrap class=wrap>"
        "<input disabled><input id=focus class=focused style='width:24px;height:12px'>"
        "<div id=external class=external style='width:24px;height:12px'></div>"
        "<img id=image src='/shadow.png' width=8 height=8></div>",true,true) : NULL;
    check(root != NULL, "closed delegatesFocus attachment"); if (!root) { web_free(d); return; }
    web_layout(d,VW,VH);
    check(web_pending_stylesheet(d) && !strcmp(web_pending_stylesheet(d),"https://shadow.test/component.css"), "shadow link resource discovery");
    const char *css = "@import '/imported.css';.external{background:red}";
    doc_css_loaded(d,"https://shadow.test/component.css",NULL,css,strlen(css));
    web_layout(d,VW,VH);
    check(web_pending_stylesheet(d) && !strcmp(web_pending_stylesheet(d),"https://shadow.test/imported.css"), "shadow import resource discovery");
    const char *imported = ".external{color:blue}";
    doc_css_loaded(d,"https://shadow.test/imported.css",NULL,imported,strlen(imported));
    paint(d);
    node_t *inside = query(d,root,"#external"), *outside = query(d,d->root,"#outer"), *image = query(d,root,"#image");
    check(inside && outside && inside->style->bg_color == RGB(255,0,0) && inside->style->color == RGB(0,0,255) &&
          outside->style->bg_color != RGB(255,0,0) && outside->style->color != RGB(0,0,255), "external/imported sheets retain shadow scope");
    check(image && image->image_request >= 0 && !strcmp(web_image_url(d,image->image_request),"https://shadow.test/shadow.png"), "shadow image resource discovery");
    node_t *focus = query(d,root,"#focus"), *wrap = query(d,root,"#wrap");
    web_focus(d,host); paint(d);
    check(focus && web_focused(d) == focus, "delegated native focus skips disabled control");
    check(focus && focus->style->bg_color == RGB(0,255,0) && wrap && wrap->style->color == RGB(255,0,0), "focus and focus-within cascade across shadow");
    struct web_hit hit;
    check(focus && web_node_action(d,focus,&hit) && hit.node == focus, "closed shadow native action identity");
    int x,y,w,h;
    check(focus && web_node_rect(d,focus,&x,&y,&w,&h), "connected shadow geometry");
    web_focus(d,NULL); paint(d);
    check(focus && focus->style->bg_color != RGB(0,255,0), "blur invalidates focused cascade");
    web_free(d);
}
static void contents_items(void) {
    const char *displays[] = {"flex", "grid"};
    for (int i = 0; i < 2; i++) {
        char html[512];
        snprintf(html,sizeof html,"<!doctype html><body style='margin:0'><div id=h style='display:%s;width:60px;font-size:0'>"
                 "<span id=item slot=n style='width:20px;height:12px;background:red'></span></div></body>",displays[i]);
        web_doc *d = load(html);
        node_t *host = d ? query(d,d->root,"#h") : NULL;
        node_t *item = d ? query(d,d->root,"#item") : NULL;
        node_t *root = host ? attach(d,host,"<slot name=n></slot>",false,false) : NULL;
        check(root && item,"contents item attachment");
        if (root && item) {
            paint(d);
            check(item->style && item->style->display == D_BLOCK,"slot items blockify in host flex/grid");
            check(rect(d,item,0,0,20,12) && rgb(4,4) == 0xff0000,"slot flex/grid items retain geometry and paint");
        }
        web_free(d);
    }
}
static void focus_boundaries(void) {
    web_doc *d = load("<!doctype html><body><div id=h><input id=light></div></body>");
    node_t *host = d ? query(d,d->root,"#h") : NULL;
    node_t *root = host ? attach(d,host,"<slot></slot><div id=nested></div><input id=first><input id=auto autofocus>",false,true) : NULL;
    check(root != NULL,"focus boundary attachment");
    if (!root) { web_free(d); return; }
    node_t *nested_host = query(d,root,"#nested");
    node_t *nested = nested_host ? attach(d,nested_host,"<input id=secret>",false,false) : NULL;
    node_t *first = query(d,root,"#first"), *automatic = query(d,root,"#auto");
    check(nested && first && automatic,"focus boundary controls");
    if (!nested || !first || !automatic) { web_free(d); return; }
    web_focus(d,host);
    check(web_focused(d) == automatic,"delegated focus honors autofocus priority");
    web_focus(d,first); web_focus(d,host);
    check(web_focused(d) == first,"delegated refocus preserves an existing inner focus");
    web_focus(d,NULL); doc_node_attr(d,automatic,"autofocus",NULL); web_focus(d,host);
    check(web_focused(d) == first,"delegation does not enter nondelegating roots or assigned light children");
    web_focus(d,NULL); nested->shadow_delegates_focus = true; web_focus(d,host);
    check(web_focused(d) == query(d,nested,"#secret"),"delegating nested root permits recursive focus");
    web_free(d);
}
struct css_failure_log { int errors, level; char message[384]; };
static void css_failure_console(void *opaque, int level, const char *message) {
    struct css_failure_log *log = opaque;
    if (!message || strncmp(message,"CSS arena allocation failed during stylesheet/resource scan;",60)) return;
    log->errors++; log->level = level;
    snprintf(log->message,sizeof log->message,"%s",message);
}
static void css_arena_boundary(void) {
    const char *html = "<!doctype html><body><style>#arena{display:block;width:24px;height:12px;background:red}</style><div id=arena></div></body>";
    struct css_failure_log log = {0};
    struct web_host callbacks = {.opaque=&log,.console=css_failure_console};
    web_doc *d = web_live(html,strlen(html),"https://shadow.test/arena","utf-8",&callbacks);
    check(d != NULL,"CSS arena boundary document"); if (!d) return;
    check(d->cssmem.limit == (32u << 20),"live CSS AST default has a finite 32 MiB budget");
    for (int i = 0; i < 8; i++) web_tick(d,uptime_ms());
    node_t *n = query(d,d->root,"#arena");
    web_layout(d,VW,VH);
    check(n && n->style && n->style->bg_color == RGB(255,0,0),"CSS before allocation boundary");
    size_t limit = d->cssmem.limit;
    d->cssmem.limit = 1; d->resources_dirty = true;
    web_layout(d,VW,VH);
    check(log.errors == 1 && log.level == 2,"CSS arena failure reaches the public console");
    check(strstr(log.message,"CSS arena allocated=0 limit=1;") &&
          strstr(log.message,"DOM arena allocated=") && strstr(log.message,"authored sheets discarded="),
          "CSS arena diagnostic records real accounting before cleanup");
    check(d->sty.sheets.n == 0 && d->cssmem.allocated == 0,"failed CSS snapshot has no dangling sheets");
    d->cssmem.limit = limit; d->resources_dirty = true;
    web_layout(d,VW,VH);
    check(n && n->style && n->style->bg_color == RGB(255,0,0) && d->sty.sheets.n > 0 && log.errors == 1,
          "CSS rebuild recovers after an allocation boundary failure");
    web_free(d);
}
struct css_expansion_host {
    struct css_failure_log log;
    uint64_t document_request, shadow_request;
    int requests;
};
static bool css_expansion_request(void *opaque, const struct web_request *request) {
    struct css_expansion_host *host = opaque;
    if (request->kind != WEB_RESOURCE_CSS) return false;
    host->requests++;
    uint64_t *id = !strcmp(request->url,"https://shadow.test/expanded-doc.css") ? &host->document_request
        : !strcmp(request->url,"https://shadow.test/expanded-shadow.css") ? &host->shadow_request : NULL;
    if (!id || *id) return false;
    *id = request->id;
    return true;
}
static void css_expansion_console(void *opaque, int level, const char *message) {
    struct css_expansion_host *host = opaque;
    css_failure_console(&host->log,level,message);
}
static void css_ast_expansion(void) {
    const char *html = "<!doctype html><head><link rel=stylesheet href='/expanded-doc.css'></head>"
        "<body><div id=outer class=probe></div><div id=host></div></body>";
    struct css_expansion_host fixture = {0};
    struct web_host callbacks = {.opaque=&fixture,.request=css_expansion_request,.console=css_expansion_console};
    web_doc *d = web_live(html,strlen(html),"https://shadow.test/expanded","utf-8",&callbacks);
    check(d != NULL,"expanded CSS document"); if (!d) return;
    for (int i = 0; i < 128 && d->parser; i++) web_tick(d,uptime_ms());
    check(d->parser == NULL,"expanded CSS incremental HTML parsing has completed");
    check(fixture.document_request && fixture.requests == 1,"expanded document CSS request is accepted and retained");
    if (d->parser || !fixture.document_request) { web_free(d); return; }
    node_t *host = query(d,d->root,"#host"), *outside = query(d,d->root,"#outer");
    node_t *root = host ? attach(d,host,
        "<link rel=stylesheet href='/expanded-shadow.css'><div id=inside class=probe></div>",false,false) : NULL;
    node_t *inside = root ? query(d,root,"#inside") : NULL;
    check(root && outside && inside,"expanded CSS shadow attachment");
    if (!root || !outside || !inside) { web_free(d); return; }

    /* Valid, nonmatching rules expand into selectors/declarations without
       making the cascade itself process thousands of matching declarations.
       Raw source stays below 8 MiB; their shared AST exceeds 16 MiB. */
    const char rule[] = ".unused{color:red}";
    const char document_tail[] = ".probe{background:red}";
    const char shadow_tail[] = ".probe{background:lime}";
    const size_t count = 50000, prefix = count * (sizeof rule - 1);
    char *css = malloc(prefix + sizeof shadow_tail);
    check(css != NULL,"expanded CSS fixture allocation");
    if (!css) { web_free(d); return; }
    for (size_t i = 0; i < count; i++) memcpy(css + i * (sizeof rule - 1),rule,sizeof rule - 1);
    memcpy(css + prefix,document_tail,sizeof document_tail);
    size_t document_n = prefix + sizeof document_tail - 1;
    /* Deliver through the live host completion queue. Direct cache injection
       after a rejected request cannot replace its already-completed failure. */
    struct web_response response = {.status=200,.body=css,.body_len=document_n};
    snprintf(response.url,sizeof response.url,"https://shadow.test/expanded-doc.css");
    snprintf(response.headers,sizeof response.headers,"Content-Type: text/css\r\n");
    web_resource_loaded(d,fixture.document_request,&response);
    web_tick(d,uptime_ms());
    check(d->css_bytes == document_n && fixture.shadow_request && fixture.requests == 2,
          "expanded document CSS body is received before the shadow request");
    if (!fixture.shadow_request) { free(css); web_free(d); return; }
    memcpy(css + prefix,shadow_tail,sizeof shadow_tail);
    size_t shadow_n = prefix + sizeof shadow_tail - 1;
    response.body_len = shadow_n;
    snprintf(response.url,sizeof response.url,"https://shadow.test/expanded-shadow.css");
    web_resource_loaded(d,fixture.shadow_request,&response);
    web_tick(d,uptime_ms());
    free(css);
    check(d->css_bytes == document_n + shadow_n && d->css_bytes < (8u << 20),
          "expanded CSS uses the unchanged shared 8 MiB source budget");
    web_layout(d,VW,VH);
    printf("shadow expanded CSS: source=%zu AST=%zu limit=%zu sheets=%d requests=%d errors=%d\n",
           d->css_bytes,d->cssmem.allocated,d->cssmem.limit,d->sty.sheets.n,fixture.requests,fixture.log.errors);
    check(d->cssmem.allocated > (16u << 20) && d->cssmem.allocated <= (32u << 20) && d->sty.sheets.n == 2 && !fixture.log.errors,
          "document and shadow sheets share a valid AST above 16 MiB within 32 MiB");
    check(outside->style && outside->style->bg_color == RGB(255,0,0) &&
          inside->style && inside->style->bg_color == RGB(0,255,0),
          "expanded document and shadow ASTs retain functional scoped styling");
    web_free(d);
}
int main(void) {
    rendering(); resources_and_focus(); contents_items(); focus_boundaries(); css_arena_boundary(); css_ast_expansion();
    check(css_supports_condition("selector(:host)",15,false), "host selector support");
    check(css_supports_condition("selector(:host(.x))",19,false), "host compound support");
    check(css_supports_condition("selector(slot::slotted(.x))",27,false), "slotted selector support");
    check(!css_supports_condition("selector(::part(x))",19,false), "part is not advertised");
    printf("shadowtest: %d checks, %d failed\n",checks,failed);
    return failed != 0;
}
