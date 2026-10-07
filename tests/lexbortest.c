/* 製品側 HTML/DOM 契約の回帰テスト。Lexbor を直接パースして成功としない。
   実サイトの実行・描画・操作検証を置き換えるものではない。
   基準: https://html.spec.whatwg.org/multipage/parsing.html */
#include <stdio.h>
#include "webi.h"

#define BASE "http://parser.test/page.html"
static int checks, failures;

static void check(const char *name, bool ok) {
    checks++;
    if (!ok) { failures++; printf("FAIL lexbortest %s\n", name); }
}
static void string_is(const char *name, const char *actual, const char *expected) {
    checks++;
    if (!actual || strcmp(actual, expected)) {
        failures++;
        printf("FAIL lexbortest %s: got '%s', expected '%s'\n", name,
               actual ? actual : "(null)", expected);
    }
}
static void text_is(const char *name, const node_t *node, const char *expected) {
    sbuf text = {0};
    if (node) node_text_content(node, &text);
    string_is(name, node ? sb_cstr(&text) : NULL, expected);
    sb_free(&text);
}
/* 通常の DOM 探索と同じく template.content は横断しない。 */
static node_t *find_id(node_t *root, const char *id) {
    if (!root) return NULL;
    if (root->type == N_ELEM && root->id && !strcmp(root->id, id)) return root;
    for (node_t *child = root->first; child; child = child->next) {
        node_t *found = find_id(child, id);
        if (found) return found;
    }
    return NULL;
}
static node_t *element_at(node_t *parent, int index) {
    for (node_t *node = parent ? parent->first : NULL; node; node = node->next)
        if (node->type == N_ELEM && index-- == 0) return node;
    return NULL;
}
static const char *attribute(node_t *node, const char *name) {
    return node ? node_attr(node, name) : NULL;
}
static void element_is(const char *name, node_t *node, int tag) {
    check(name, node && node->type == N_ELEM && node->tag == tag);
}
static void namespace_is(const char *name, node_t *node, unsigned ns) {
    check(name, node && node->namespace_id == ns && node->foreign == (ns != NS_HTML));
}
static void links_valid(node_t *parent, unsigned depth) {
    check("DOM 深さ制限", depth < 128);
    if (!parent || depth >= 128) return;
    node_t *previous = NULL;
    unsigned count = 0;
    for (node_t *child = parent->first; child; child = child->next) {
        if (++count > 1000) { check("DOM 兄弟循環なし", false); return; }
        check("DOM 親・兄弟・所有者", child->parent == parent && child->prev == previous &&
              child->owner && child->allocation_doc);
        links_valid(child, depth + 1);
        if (child->template_content) {
            check("template.content は独立断片", child->template_content->type == N_FRAGMENT &&
                  !child->template_content->parent && child->template_content->template_host == child);
            links_valid(child->template_content, depth + 1);
        }
        previous = child;
    }
    check("DOM last 一致", parent->last == previous && (!previous || !previous->next));
}
static web_doc *new_document(void) {
    web_doc *doc = calloc(1, sizeof *doc);
    check("文書割り当て", doc != NULL);
    if (!doc) return NULL;
    doc->url = strdup(BASE);
    if (!doc->url) { check("URL 割り当て", false); web_free(doc); return NULL; }
    snprintf(doc->base, sizeof doc->base, "%s", BASE);
    return doc;
}
static web_doc *parse_document(const char *html) {
    web_doc *doc = new_document();
    if (!doc) return NULL;
    doc->root = html_parse(doc, html, strlen(html), "utf-8");
    check("html_parse 製品経路", doc->root && doc->root->type == N_DOC);
    if (!doc->root) { web_free(doc); return NULL; }
    return doc;
}
static web_doc *begin_document(const char *html, bool scripting) {
    web_doc *doc = new_document();
    if (!doc) return NULL;
    doc->live = scripting;
    doc->parser = html_begin(doc, html, strlen(html), "utf-8", scripting);
    check("html_begin 製品経路", doc->parser != NULL);
    if (!doc->parser) { web_free(doc); return NULL; }
    return doc;
}
static node_t *resume_script(web_doc *doc, const char *id) {
    node_t *script = NULL;
    int result = html_resume(doc->parser, &script);
    check("script 境界で停止", result == 1 && script && script->type == N_ELEM && script->tag == T_script);
    string_is("停止した script の識別子", script ? node_attr(script, "id") : NULL, id);
    return result == 1 ? script : NULL;
}
static void complete_document(web_doc *doc) {
    node_t *script = (node_t *)doc;
    check("解析完了・script 出力クリア", html_resume(doc->parser, &script) == 0 && !script);
    script = (node_t *)doc;
    check("完了後の再開は冪等", html_resume(doc->parser, &script) == 0 && !script);
    check("完了後 html_write を拒否", !html_write(doc->parser, "<b>late</b>", 11));
    links_valid(doc->root, 0);
}

static void test_tree_construction(void) {
    web_doc *doc = parse_document("<!doctype html><p id=p><b id=b>1<i id=i>2</b>3</i>4</p>");
    if (!doc) return;
    node_t *p = find_id(doc->root, "p"), *b = element_at(p, 0), *first_i = element_at(b, 0);
    node_t *second_i = element_at(p, 1);
    element_is("adoption agency 元 b", b, T_b);
    element_is("adoption agency 元 i", first_i, T_i);
    element_is("adoption agency 再構築 i", second_i, T_i);
    check("adoption agency i は別ノード", first_i && second_i && first_i != second_i);
    text_is("adoption agency b 内テキスト", b, "12");
    text_is("adoption agency 再構築テキスト", second_i, "3");
    text_is("adoption agency p 全体", p, "1234");
    check("adoption agency p 最終テキスト", p && p->last && p->last->type == N_TEXT &&
          !strcmp(p->last->text, "4"));
    links_valid(doc->root, 0);
    web_free(doc);

    doc = parse_document("<!doctype html><body><b id=old><p id=p>one</b>two</p>");
    if (!doc) return;
    b = find_id(doc->root, "old"); p = find_id(doc->root, "p");
    node_t *recreated = element_at(p, 0);
    check("adoption agency ブロック再配置", b && p && b->parent == doc->body && p->parent == doc->body && b->next == p);
    text_is("adoption agency 元 b は空", b, "");
    element_is("adoption agency ブロック内 b", recreated, T_b);
    check("adoption agency b の複製", recreated && recreated != b);
    text_is("adoption agency ブロック内テキスト", recreated, "one");
    text_is("adoption agency ブロック全体", p, "onetwo");
    web_free(doc);

    doc = parse_document("<!doctype html><div id=host><table id=t>before<div id=f>foster</div>"
                         "<tr><td id=c>cell</td></tr>after</table><span id=tail>end</span></div>");
    if (!doc) return;
    node_t *host = find_id(doc->root, "host"), *table = find_id(doc->root, "t"), *foster = find_id(doc->root, "f");
    node_t *tbody = element_at(table, 0), *row = element_at(tbody, 0), *cell = element_at(row, 0);
    check("foster parenting table の前へ", host && table && foster && foster->parent == host && element_at(host, 0) == foster && element_at(host, 1) == table);
    element_is("暗黙 tbody", tbody, T_tbody);
    element_is("table 行", row, T_tr);
    check("cell は table 内", cell && cell == find_id(doc->root, "c") && cell->parent == row);
    text_is("table 非空白テキストを排出", table, "cell");
    text_is("foster parenting 文書順", host, "beforefosteraftercellend");
    links_valid(doc->root, 0);
    web_free(doc);
}

static void test_entities_and_tokens(void) {
    const char *expected = "\xe2\x88\xb3|\xe2\x89\x82\xcc\xb8|\xe2\x88\xbe\xcc\xb3|fj|\xf0\x9d\x94\x84|\xe2\x82\xac|\xc2\xacit;|&x|&unknown;";
    web_doc *doc = parse_document("<!doctype html><p id=p>&CounterClockwiseContourIntegral;|&NotEqualTilde;|&acE;|&fjlig;|&Afr;|&#x80;|&notit;|&ampx|&unknown;</p>"
        "<div id=a data-a='&notit;' data-b='&ampx' data-c='&NotEqualTilde;' title='one>two' title='discarded' data-d=ok></div>");
    if (!doc) return;
    text_is("長い・複数符号点・補助平面・曖昧な文字参照", find_id(doc->root, "p"), expected);
    node_t *attr = find_id(doc->root, "a");
    string_is("属性では曖昧な not 参照を維持", attr ? node_attr(attr, "data-a") : NULL, "&notit;");
    string_is("属性では曖昧な amp 参照を維持", attr ? node_attr(attr, "data-b") : NULL, "&ampx");
    string_is("属性内の複数符号点参照", attr ? node_attr(attr, "data-c") : NULL, "\xe2\x89\x82\xcc\xb8");
    string_is("引用属性の >・重複は先勝ち", attr ? node_attr(attr, "title") : NULL, "one>two");
    string_is("引用属性の後を正しく解析", attr ? node_attr(attr, "data-d") : NULL, "ok");
    web_free(doc);

    doc = parse_document("<!--before--><!DOCTYPE html SYSTEM 'test-identifier'><html><head></head>"
                         "<body><!--inside--><p id=p>x</p></body></html><!--after-->");
    if (!doc) return;
    node_t *before = doc->root->first, *doctype = before ? before->next : NULL, *after = doc->root->last;
    check("DOCTYPE 標準モード", !doc->quirks);
    check("文書前コメント", before && before->type == N_COMMENT);
    string_is("文書前コメント内容", before ? before->text : NULL, "before");
    check("ネイティブ DocumentType", doctype && doctype->type == N_DOCTYPE);
    string_is("DOCTYPE 名", doctype ? doctype->name : NULL, "html");
    string_is("DOCTYPE の正常な引用識別子", doctype ? doctype->system_id : NULL, "test-identifier");
    check("body コメント", doc->body && doc->body->first && doc->body->first->type == N_COMMENT);
    check("文書後コメント", after && after->type == N_COMMENT);
    string_is("文書後コメント内容", after ? after->text : NULL, "after");
    text_is("コメントは textContent に混入しない", doc->body, "x");
    links_valid(doc->root, 0);
    web_free(doc);

    /* HTML の DOCTYPE 識別子は XML と異なり、引用中の > でも打ち切られる。
       WHATWG 13.2.5.66: abrupt-doctype-system-identifier は force-quirks。 */
    doc = parse_document("<!DOCTYPE html SYSTEM 'test>identifier'><p id=p>x</p>");
    if (!doc) return;
    doctype = doc->root->first;
    check("引用中の > で打ち切られた DOCTYPE は quirks", doc->quirks);
    check("打ち切られた DOCTYPE ノードを保持", doctype && doctype->type == N_DOCTYPE);
    string_is("打ち切られた DOCTYPE systemId", doctype ? doctype->system_id : NULL, "test");
    text_is("DOCTYPE 打ち切り後の文字列は body テキスト", doc->body, "identifier'>x");
    web_free(doc);
}

static void test_templates_and_namespaces(void) {
    web_doc *doc = begin_document("<!doctype html><body><template id=t><table><tr><td id=hidden>x</td></tr></table>"
        "<script id=inert>must not execute</script><template id=nested><b id=deep>deep</b></template></template>"
        "<script id=active>active</script><p id=future>future</p>", true);
    if (!doc) return;
    resume_script(doc, "active");
    node_t *t = find_id(doc->root, "t"), *content = t ? t->template_content : NULL;
    check("template の通常子は空", t && !t->first && !t->last);
    check("template に独立 content", content && content->type == N_FRAGMENT);
    check("template content は inert 所有文書", content && content->owner && content->owner != doc && content->owner->inert);
    check("template は通常探索から不可視", !find_id(doc->root, "hidden") && !find_id(doc->root, "inert") && !find_id(doc->root, "deep"));
    check("template content はネイティブ DOM", find_id(content, "hidden") && find_id(content, "inert"));
    node_t *nested = find_id(content, "nested");
    check("入れ子 template は独立 content", nested && !nested->first && nested->template_content && find_id(nested->template_content, "deep"));
    check("template 内 script で停止しない", !find_id(doc->root, "future"));
    complete_document(doc);
    check("template 解析後の実子", find_id(doc->root, "future") != NULL);
    web_free(doc);

    doc = parse_document("<!doctype html><body><svg id=s viewBox='0 0 10 10'><linearGradient id=g></linearGradient>"
        "<foreignObject id=fo><div id=html>HTML</div></foreignObject><title><span id=st>title</span></title></svg>"
        "<math id=m><mtext id=mt><b id=mh>HTML</b><mglyph id=mg /></mtext>"
        "<annotation-xml id=ann encoding='text/html'><div id=ah>HTML</div><svg id=as /></annotation-xml></math>"
        "<svg id=break><g></g><p id=out>outside</p>");
    if (!doc) return;
    node_t *svg = find_id(doc->root, "s");
    namespace_is("SVG namespace", svg, NS_SVG);
    bool raw_viewbox = false;
    for (int i = 0; svg && i < svg->nattrs; i++)
        if (!strcmp(svg->attrs[i].raw, "viewBox") && !strcmp(svg->attrs[i].value, "0 0 10 10")) raw_viewbox = true;
    check("SVG 属性の大文字調整", raw_viewbox);
    node_t *g = find_id(doc->root, "g"), *fo = find_id(doc->root, "fo");
    namespace_is("SVG 子 namespace", g, NS_SVG);
    string_is("SVG 名の大文字調整", g ? g->raw_name : NULL, "linearGradient");
    string_is("SVG foreignObject 名調整", fo ? fo->raw_name : NULL, "foreignObject");
    namespace_is("foreignObject HTML 統合点", find_id(doc->root, "html"), NS_HTML);
    namespace_is("SVG title HTML 統合点", find_id(doc->root, "st"), NS_HTML);
    namespace_is("MathML namespace", find_id(doc->root, "m"), NS_MATHML);
    namespace_is("MathML テキスト統合点", find_id(doc->root, "mh"), NS_HTML);
    namespace_is("mglyph は MathML のまま", find_id(doc->root, "mg"), NS_MATHML);
    namespace_is("annotation-xml HTML 統合点", find_id(doc->root, "ah"), NS_HTML);
    namespace_is("annotation-xml 内 SVG", find_id(doc->root, "as"), NS_SVG);
    node_t *outside = find_id(doc->root, "out");
    check("foreign breakout は body へ", outside && outside->parent == doc->body);
    namespace_is("foreign breakout HTML namespace", outside, NS_HTML);
    links_valid(doc->root, 0);
    web_free(doc);
}

static void test_fragments(void) {
    web_doc *doc = parse_document("<!doctype html><body><table id=t><tbody id=tb><tr id=r><td id=c></td></tr></tbody></table>"
        "<select id=s></select><svg id=v></svg><textarea id=ta></textarea>");
    if (!doc) return;
    node_t *saved_root = doc->root, *saved_body = doc->body, *table = find_id(doc->root, "t");
    const char *source = "<tr><td id=fragmentcell>x</td></tr>";
    node_t *fragment = html_fragment(doc, table, source, strlen(source));
    check("table 文脈の断片", fragment && fragment->type == N_FRAGMENT);
    node_t *tbody = element_at(fragment, 0), *tr = element_at(tbody, 0);
    element_is("table 断片に暗黙 tbody", tbody, T_tbody);
    element_is("table 断片の tr", tr, T_tr);
    check("断片は本体と独立", !find_id(doc->root, "fragmentcell") && find_id(fragment, "fragmentcell"));
    if (fragment) check("断片を実 DOM に挿入", doc_node_move(doc, table, fragment, NULL));
    check("挿入後は通常探索から可視", find_id(doc->root, "fragmentcell") != NULL);
    check("挿入後の断片は空", fragment && !fragment->first && !fragment->last);

    source = "<td id=cell1>one<td id=cell2>two";
    fragment = html_fragment(doc, find_id(doc->root, "r"), source, strlen(source));
    element_is("tr 文脈の第一 td", element_at(fragment, 0), T_td);
    element_is("tr 文脈の暗黙閉鎖 td", element_at(fragment, 1), T_td);
    text_is("tr 文脈のテキスト", fragment, "onetwo");
    source = "<option id=one>one<option id=two>two<optgroup label=g><option id=three>three</optgroup>";
    fragment = html_fragment(doc, find_id(doc->root, "s"), source, strlen(source));
    node_t *one = element_at(fragment, 0), *two = element_at(fragment, 1), *group = element_at(fragment, 2);
    element_is("select 文脈の第一 option", one, T_option);
    element_is("select 文脈の暗黙閉鎖 option", two, T_option);
    element_is("select 文脈の optgroup", group, T_optgroup);
    text_is("select option one", one, "one");
    text_is("select option two", two, "two");
    check("optgroup の option", group && find_id(group, "three") && find_id(group, "three")->parent == group);

    source = "<circle id='circle'/><foreignObject><div id=inside>x</div></foreignObject>";
    fragment = html_fragment(doc, find_id(doc->root, "v"), source, strlen(source));
    namespace_is("SVG 文脈の断片 namespace", find_id(fragment, "circle"), NS_SVG);
    namespace_is("SVG 断片内 HTML 統合点", find_id(fragment, "inside"), NS_HTML);
    source = "&NotEqualTilde;<b>literal</b>";
    fragment = html_fragment(doc, find_id(doc->root, "ta"), source, strlen(source));
    text_is("textarea 文脈は RCDATA", fragment, "\xe2\x89\x82\xcc\xb8<b>literal</b>");
    check("textarea 文脈に b 要素なし", !element_at(fragment, 0));
    check("断片解析は文書 root/body/doctype モードを維持", doc->root == saved_root && doc->body == saved_body && !doc->quirks);
    links_valid(doc->root, 0);
    web_free(doc);
}

static void test_write_and_identity(void) {
    web_doc *doc = begin_document("<!doctype html><body><div id=host><script id=outer>outer</script><p id=tail>tail</p></div>", true);
    if (!doc) return;
    node_t *root = doc->root, *outer = resume_script(doc, "outer"), *host = find_id(doc->root, "host");
    check("停止中は将来 markup が存在しない", !find_id(doc->root, "tail"));
    const char *write1 = "<span id=a>A</span><script id=nested>nested</script>";
    const char *write2 = "<span id=b>B</span>";
    check("第一 document.write", html_write(doc->parser, write1, strlen(write1)));
    check("第二 document.write", html_write(doc->parser, write2, strlen(write2)));
    check("書き込みは再開前に先読みしない", !find_id(doc->root, "a") && !find_id(doc->root, "nested") && !find_id(doc->root, "b"));
    node_t *nested = resume_script(doc, "nested"), *a = find_id(doc->root, "a");
    check("入れ子 script 境界の可視性", a && !find_id(doc->root, "b") && !find_id(doc->root, "tail"));
    check("ネイティブ root/host/script の同一性", doc->root == root && find_id(root, "host") == host && find_id(root, "outer") == outer);
    write1 = "<em id=c>C</em><script id=deep>deep</script><i id=d>D</i>";
    check("入れ子 document.write", html_write(doc->parser, write1, strlen(write1)));
    node_t *deep = resume_script(doc, "deep");
    check("更なる入れ子境界の可視性", find_id(root, "c") && !find_id(root, "d") && !find_id(root, "b") && !find_id(root, "tail"));
    write1 = "<u id=e>E</u>";
    check("深い document.write", html_write(doc->parser, write1, strlen(write1)));
    complete_document(doc);
    const char *order[] = {"outer", "a", "nested", "c", "deep", "e", "d", "b", "tail"};
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++)
        string_is("document.write の正確な挿入順", attribute(element_at(host, (int)i), "id"), order[i]);
    check("すべての script 境界で同一ネイティブノードを維持", find_id(root, "outer") == outer && find_id(root, "nested") == nested && find_id(root, "deep") == deep && find_id(root, "a") == a);
    web_free(doc);

    doc = begin_document("<!doctype html><body><script id=split>split</script><p id=tail>tail</p>", true);
    if (!doc) return;
    resume_script(doc, "split");
    write1 = "<div id='"; write2 = "joined' title='a>b'>&NotEqualTilde;</div>";
    check("分割トークン前半 write", html_write(doc->parser, write1, strlen(write1)));
    check("分割トークン後半 write", html_write(doc->parser, write2, strlen(write2)));
    complete_document(doc);
    node_t *joined = find_id(doc->root, "joined");
    string_is("write 跨ぎの引用属性", joined ? node_attr(joined, "title") : NULL, "a>b");
    text_is("write 跨ぎの文字参照", joined, "\xe2\x89\x82\xcc\xb8");
    check("write は入力末尾に追加せず挿入点を使用", joined && joined->next == find_id(doc->root, "tail"));
    web_free(doc);
}

static void test_resume_mutations(void) {
    web_doc *doc = begin_document("<!doctype html><body><section id=left><div id=gone>gone</div>"
        "<div id=keep data-old=old><span id=leaf>old</span></div></section><section id=right></section>"
        "<div id=open>pre<script id=mutate>mutate</script><span id=added>post</span></div><p id=tail>end</p>", true);
    if (!doc) return;
    node_t *script = resume_script(doc, "mutate"), *gone = find_id(doc->root, "gone"), *keep = find_id(doc->root, "keep");
    node_t *leaf = find_id(doc->root, "leaf"), *right = find_id(doc->root, "right"), *open = find_id(doc->root, "open");
    node_t *original_text = open ? open->first : NULL;
    check("変更対象を境界で公開", script && gone && keep && leaf && right && open && original_text && original_text->type == N_TEXT);
    if (!script || !gone || !keep || !leaf || !right || !open || !original_text) { web_free(doc); return; }
    doc_node_remove(doc, gone);
    check("境界で閉じた要素を移動", doc_node_move(doc, right, keep, NULL));
    check("境界で開いた要素を移動", doc_node_move(doc, right, open, NULL));
    check("境界で属性を追加", doc_node_attr(doc, keep, "data-new", "new"));
    check("境界で属性を削除", doc_node_attr(doc, keep, "data-old", NULL));
    check("境界で id を変更", doc_node_attr(doc, keep, "id", "changed"));
    check("境界で既存子テキストを置換", doc_node_text(doc, leaf, "edited", 6));
    check("境界で既存 Text ノードを編集", doc_node_text(doc, original_text, "newpre", 6));
    node_t *created = doc_node_create(doc, N_ELEM, "strong", NULL, 0);
    check("境界でネイティブノードを作成", created && doc_node_attr(doc, created, "id", "created") &&
          doc_node_text(doc, created, "created", 7) && doc_node_move(doc, right, created, open));
    complete_document(doc);
    check("削除済みノードを復活させない", !gone->parent && !find_id(doc->root, "gone"));
    check("移動済みノードの位置・同一性を維持", find_id(doc->root, "changed") == keep && keep->parent == right && !find_id(doc->root, "keep"));
    string_is("境界の属性追加を保持", node_attr(keep, "data-new"), "new");
    check("境界の属性削除を保持", !node_attr(keep, "data-old"));
    text_is("境界の子テキスト変更を保持", leaf, "edited");
    check("境界の Text 同一性を保持", open->first == original_text);
    string_is("境界の Text 値変更を保持", original_text->text, "newpre");
    check("parser stack は移動後の同じ開いた要素へ挿入", open->parent == right && find_id(open, "added") && find_id(open, "added")->parent == open);
    check("script ノード同一性を保持", find_id(doc->root, "mutate") == script);
    check("作成したネイティブノードを保持", created && find_id(doc->root, "created") == created && created->next == open);
    check("閉じタグの後は通常 body へ復帰", find_id(doc->root, "tail") && find_id(doc->root, "tail")->parent == doc->body);
    web_free(doc);

    doc = begin_document("<!doctype html><body><div id=open><script id=pause>pause</script>"
                         "<b id=detachedchild>post</b></div><p id=tail>live</p>", true);
    if (!doc) return;
    resume_script(doc, "pause"); open = find_id(doc->root, "open");
    check("削除する開いた要素", open != NULL);
    if (!open) { web_free(doc); return; }
    doc_node_remove(doc, open);
    complete_document(doc);
    check("開いた要素の削除を反映", !open->parent && !find_id(doc->root, "open") && !find_id(doc->root, "detachedchild"));
    check("parser stack の切り離した要素に解析を続行", find_id(open, "detachedchild") && find_id(open, "detachedchild")->parent == open);
    check("切り離した stack の閉鎖後に body へ復帰", find_id(doc->root, "tail") && find_id(doc->root, "tail")->parent == doc->body);
    links_valid(open, 0);
    web_free(doc);
}

static void test_script_tokenizer(void) {
    const char *body = "<!--<script>nested</script>still-->end &amp; </scriptx>";
    web_doc *doc = begin_document("<!doctype html><body><script id=s data-a='a>b' data-b=\"c>d\">"
        "<!--<script>nested</script>still-->end &amp; </scriptx></script><p id=after>x</p>", true);
    if (!doc) return;
    node_t *script = resume_script(doc, "s");
    text_is("script escaped/double-escaped の完全な本文", script, body);
    string_is("script 引用属性 > 前半", script ? node_attr(script, "data-a") : NULL, "a>b");
    string_is("script 引用属性 > 後半", script ? node_attr(script, "data-b") : NULL, "c>d");
    check("double-escaped の内側閉じタグで停止しない", !find_id(doc->root, "after"));
    complete_document(doc);
    check("実際の script 終端の後を解析", find_id(doc->root, "after") != NULL);
    web_free(doc);

    doc = begin_document("<!doctype html><body><script id=s>unterminated &amp; <!--", true);
    if (!doc) return;
    node_t *unexpected = NULL;
    check("EOF の未閉鎖 script は実行境界にしない", html_resume(doc->parser, &unexpected) == 0 && !unexpected);
    text_is("EOF の未閉鎖 script 本文を保持", find_id(doc->root, "s"), "unterminated &amp; <!--");
    web_free(doc);

    doc = begin_document("<!doctype html><body><noscript><b id=fallback>fallback</b></noscript>"
                         "<script id=s>script</script><p id=after>x</p>", true);
    if (!doc) return;
    resume_script(doc, "s");
    check("scripting 有効時 noscript は RAWTEXT", !find_id(doc->root, "fallback"));
    complete_document(doc);
    web_free(doc);
    doc = begin_document("<!doctype html><body><noscript><b id=fallback>fallback</b></noscript>"
                         "<script id=s>script</script><p id=after>x</p>", false);
    if (!doc) return;
    complete_document(doc);
    check("scripting 無効時 script 停止なし・noscript を解析", find_id(doc->root, "fallback") && find_id(doc->root, "after"));
    web_free(doc);
}

/* suspended parser の binding はネイティブ DOM の所有文書・生存期間・
   編集済み control 値を上書きしてはならない。独立 Lexbor API は使わない。 */
static void test_bridge_lifetimes(void) {
    web_doc *moving = begin_document("<!doctype html><body><div id=moving a=1 b=2></div>"
                                    "<script id=pause>pause</script><p id=tail>tail</p>", true);
    if (moving) {
        resume_script(moving, "pause");
        node_t *item = find_id(moving->root, "moving");
        node_t *host = doc_node_create(moving, N_ELEM, "template", NULL, 0);
        node_t *content = host ? doc_template_content(moving, host) : NULL;
        check("新規 template に bound ノードを移す", item && content &&
            doc_node_attr(moving, item, "a", NULL) && doc_node_attr(moving, item, "a", "1") &&
            doc_node_move(content->owner, content, item, NULL) && doc_node_move(moving, moving->body, host, NULL));
        complete_document(moving);
        check("新規 template content の対応を共有", content && find_id(content, "moving") == item &&
            item->parent == content && item->owner == content->owner && !find_id(moving->root, "moving"));
        check("属性再追加の順序を保持", item && item->nattrs == 3 &&
            !strcmp(item->attrs[0].name, "id") && !strcmp(item->attrs[1].name, "b") && !strcmp(item->attrs[2].name, "a"));
        web_free(moving);
    }
    web_doc *doc = begin_document("<!doctype html><body><div id=open><template id=tpl>"
        "<span id=inside>inside</span><b id=promoted>promoted</b></template>"
        "<script id=pause>pause</script><b id=continued>continued</b></div><p id=tail>tail</p>", true);
    if (!doc) return;
    node_t *paused = resume_script(doc, "pause"), *open = find_id(doc->root, "open");
    node_t *tpl = find_id(open, "tpl"), *content = tpl ? tpl->template_content : NULL;
    node_t *inside = find_id(content, "inside"), *promoted = find_id(content, "promoted");
    const char *destination_html = "<!doctype html><body><section id=target></section>";
    web_doc *destination = doc_inert(doc, destination_html, strlen(destination_html), BASE);
    node_t *target = destination ? find_id(destination->root, "target") : NULL;
    check("adopt 対象と同じ寿命 family の別文書", paused && open && tpl && content && inside && promoted &&
          destination && destination->dom_family == doc && target);
    if (!paused || !open || !content || !inside || !promoted || !target) { web_free(doc); return; }
    web_doc *old_content_allocation = content->allocation_doc;
    check("bound template content 子を別文書へ昇格", doc_node_adopt(destination, promoted) &&
          doc_node_move(destination, target, promoted, NULL));
    check("bound open 要素を別文書へ adopt", doc_node_adopt(destination, open) &&
          doc_node_move(destination, target, open, NULL));
    node_t *none = NULL;
    check("adopt 後の parser 再開完了", html_resume(doc->parser, &none) == 0 && !none);
    check("adopt 後も open/script の所有者・同一性・割り当て元を維持", find_id(destination->root, "open") == open &&
          open->owner == destination && open->allocation_doc == doc && open->parent == target &&
          find_id(open, "pause") == paused && paused->owner == destination && paused->allocation_doc == doc);
    check("新しい parser 子は移動先の論理所有文書へ", find_id(open, "continued") &&
          find_id(open, "continued")->parent == open && find_id(open, "continued")->owner == destination &&
          !find_id(doc->root, "open") && !find_id(doc->root, "continued"));
    check("template content の同一性と新しい inert 所有者を維持", tpl->owner == destination &&
          tpl->template_content == content && content->owner == destination->template_doc &&
          content->owner && content->owner->inert && content->allocation_doc == old_content_allocation &&
          content->template_host == tpl && find_id(content, "inside") == inside && inside->owner == content->owner &&
          inside->allocation_doc == old_content_allocation);
    check("template から昇格した bound 子を戻さない", find_id(destination->root, "promoted") == promoted &&
          promoted->owner == destination && promoted->parent == target && !find_id(content, "promoted"));
    check("adopt した stack の閉鎖後は元文書へ復帰", find_id(doc->root, "tail") &&
          find_id(doc->root, "tail")->parent == doc->body && find_id(doc->root, "tail")->owner == doc);
    web_free(doc); /* destination/template 所有文書も同じ family として破棄する。 */

    doc = begin_document("<!doctype html><body><p><b id=format>old</p><script id=first>first</script>"
                         "new</b><script id=second>second</script><p id=tail>tail</p>", true);
    if (!doc) return;
    resume_script(doc, "first");
    node_t *original = find_id(doc->root, "format");
    check("AFE だけに残す formatting 対象", original && original->tag == T_b);
    if (!original) { web_free(doc); return; }
    /* </p> が open-elements から b を除去しても AFE は b を保持する。 */
    doc_node_remove(doc, original);
    check("切り離した AFE 要素の native 編集", doc_node_attr(doc, original, "data-edited", "yes") &&
          doc_node_text(doc, original, "detached", 8));
    resume_script(doc, "second");
    node_t *reconstructed = find_id(doc->root, "format");
    check("AFE reconstruction は元ノードと同一性を共有しない", reconstructed && reconstructed != original &&
          reconstructed->parent == doc->body && !original->parent && original->owner == doc &&
          node_attr(original, "data-edited") && !strcmp(node_attr(original, "data-edited"), "yes"));
    text_is("切り離した AFE 元ノードの内容を保持", original, "detached");
    text_is("再構築 AFE ノードは新しい本文のみ", reconstructed, "new");
    node_t *copy = reconstructed ? doc_node_clone(doc, reconstructed, true) : NULL;
    check("再構築ノードの native clone は独立", copy && copy != reconstructed && copy != original &&
          doc_node_attr(doc, copy, "id", "copy") && doc_node_text(doc, copy, "usercopy", 8) &&
          doc_node_move(doc, doc->body, copy, NULL));
    none = NULL;
    check("AFE/native clone 変更後の再開完了", html_resume(doc->parser, &none) == 0 && !none);
    check("再構築と native clone の binding 同一性を維持", find_id(doc->root, "format") == reconstructed &&
          find_id(doc->root, "copy") == copy && !original->parent);
    text_is("native clone 変更が再構築元に漏れない", reconstructed, "new");
    text_is("native clone 内容を再開後も保持", copy, "usercopy");
    web_free(doc);

    doc = begin_document("<!doctype html><body><textarea id=edited>default</textarea>"
                         "<script id=pause>pause</script><p id=tail>tail</p>", true);
    if (!doc) return;
    resume_script(doc, "pause");
    node_t *textarea = find_id(doc->root, "edited");
    check("ユーザー編集前の textarea", textarea && textarea->tag == T_textarea);
    if (!textarea) { web_free(doc); return; }
    doc_control_init(doc, textarea);
    check("textarea のユーザー値を編集", doc_node_value(doc, textarea, "user\nvalue", 10));
    doc_control_selection(doc, textarea, 2, 7, 2);
    none = NULL;
    check("textarea 編集後の parser 再開完了", html_resume(doc->parser, &none) == 0 && !none);
    doc_control_init(doc, textarea);
    check("textarea の同一性・dirty 値・選択範囲を維持", find_id(doc->root, "edited") == textarea &&
          textarea->value_dirty && textarea->control_ready && textarea->value && !strcmp(textarea->value, "user\nvalue") &&
          textarea->selection_start == 2 && textarea->selection_end == 7 && textarea->selection_direction == 2);
    text_is("textarea default DOM text と編集値を混同しない", textarea, "default");
    web_free(doc);
}

int main(int argc, char **argv) {
#define RUN(name, fn) do { if (argc == 1 || !strcmp(argv[1], name)) { printf("lexbortest: %s\n", name); fn(); } } while (0)
    RUN("tree", test_tree_construction);
    RUN("tokens", test_entities_and_tokens);
    RUN("namespaces", test_templates_and_namespaces);
    RUN("fragments", test_fragments);
    RUN("write", test_write_and_identity);
    RUN("mutations", test_resume_mutations);
    RUN("scripts", test_script_tokenizer);
    RUN("bridge", test_bridge_lifetimes);
#undef RUN
    check("最低一つのケース", checks != 0);
    printf("lexbortest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
