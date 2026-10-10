/* Product parser/native-DOM checks. They supplement, never replace, real-site
   GUI acceptance. Keep DSD/patch construction observable at script boundaries. */
#include <stdio.h>
#include "webi.h"
#include "html_pi.h"

static unsigned checks, failures;
static void check(const char *name, bool ok) {
    checks++;
    if (!ok) { failures++; printf("FAIL html_modern_tree_test %s\n", name); }
}
static node_t *id(node_t *n, const char *value) {
    if (!n) return NULL;
    if (n->id && !strcmp(n->id, value)) return n;
    for (node_t *c = n->first; c; c = c->next) {
        node_t *found = id(c, value); if (found) return found;
    }
    return NULL;
}
static bool text_is(node_t *n, const char *expected) {
    sbuf text = {0}; if (n) node_text_content(n, &text);
    bool ok = n && !strcmp(sb_cstr(&text), expected); sb_free(&text); return ok;
}
static web_doc *begin(const char *source, bool dsd) {
    web_doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->url = strdup("https://parser.test/modern-tree.html");
    if (!d->url) { web_free(d); return NULL; }
    snprintf(d->base, sizeof d->base, "%s", d->url);
    d->live = true;
    d->parser = html_begin(d, source, strlen(source), "utf-8", true);
    if (!d->parser) { web_free(d); return NULL; }
    html_declarative_shadow_set(d->parser, dsd);
    return d;
}
static web_doc *parse(const char *source, bool dsd) {
    web_doc *d = begin(source, dsd);
    check("文書生成", d != NULL);
    if (!d) return NULL;
    node_t *script = NULL;
    check("非script文書の完了", html_resume(d->parser, &script) == 0 && !script);
    return d;
}
static void pseudo_attributes(void) {
    struct html_pi_attribute *attrs = NULL; size_t count = 0;
    const char *data = "name='a&amp;b' digit=\"&#x1F600;\"";
    check("PI文字参照", html_pi_parse(data, strlen(data), &attrs, &count));
    if (attrs) {
        check("PI順序と復号", count == 2 && !strcmp(attrs[0].name, "name") &&
               !strcmp(attrs[0].value, "a&b") && attrs[1].value_length == 4);
        html_pi_free(attrs, count);
    }
    static const char *const invalid[] = {
        "name='a' name='b'", "name=a", "name='&unknown;'", "name='&#0;'",
        "name='a'broken", "name='a' other", "name='&amp'", "name='<'"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        attrs = NULL; count = 0;
        check("PI不正データは全体を拒否", !html_pi_parse(invalid[i], strlen(invalid[i]), &attrs, &count) && !attrs && !count);
    }
    char *value = html_pi_get("Name='X' name='y'", 17, "name");
    check("PI属性名はcase-sensitive", value && !strcmp(value, "y")); free(value);
}
static void declarative_shadow(void) {
    web_doc *d = parse("<!doctype html><div id=host>light<template shadowrootmode=OPEN "
        "shadowrootdelegatesfocus shadowrootclonable shadowrootserializable "
        "shadowrootslotassignment=manual><span id=shadow>inside</span></template>tail</div>", true);
    if (!d) return;
    node_t *host = id(d->root, "host"), *root = host ? host->shadow_root : NULL;
    check("DSD token-time root", root && root->shadow_host == host && root->shadow_declarative && !root->parent);
    check("DSD flags", root && !root->shadow_closed && root->shadow_delegates_focus &&
          root->shadow_clonable && root->shadow_serializable && root->shadow_manual);
    check("DSD content所有者と分離", root && id(root, "shadow") && id(root, "shadow")->owner == d &&
          !id(d->root, "shadow") && text_is(host, "lighttail") && text_is(root, "inside"));
    node_t *copy = host ? doc_node_clone(d, host, true) : NULL;
    check("DSD既存clone経路", copy && copy->shadow_root && copy->shadow_root != root &&
          copy->shadow_root->shadow_declarative && text_is(copy->shadow_root, "inside"));
    web_free(d);

    d = parse("<!doctype html><div id=host><template id=ordinary shadowrootmode=open><b>x</b></template></div>", false);
    if (!d) return;
    host = id(d->root, "host"); node_t *normal = id(d->root, "ordinary");
    check("DSD無効は既存template", host && !host->shadow_root && normal && normal->template_content &&
          normal->template_content->owner->inert && text_is(normal->template_content, "x"));
    web_free(d);

    d = parse("<!doctype html><div id=host><template shadowrootmode=closed shadowrootcustomelementregistry><i>x</i></template>"
              "<template id=second shadowrootmode=open><b>fallback</b></template></div>", true);
    if (!d) return;
    host = id(d->root, "host"); root = host ? host->shadow_root : NULL;
    check("closed/null registry", root && root->shadow_closed && root->custom_registry_id == -1);
    normal = id(d->root, "second");
    check("既存shadow rootを上書きしない", root && text_is(root, "x") && normal &&
          text_is(normal->template_content, "fallback"));
    web_free(d);
}
static void shadow_script_boundary(void) {
    web_doc *d = begin("<!doctype html><div id=host><template shadowrootmode=open><b id=before>x</b>"
        "<script id=boundary>pause</script><span id=after>y</span></template></div><p id=tail>z</p>", true);
    check("DSD script文書生成", d != NULL); if (!d) return;
    node_t *script = NULL;
    check("DSD script停止", html_resume(d->parser, &script) == 1 && script && script->id && !strcmp(script->id, "boundary"));
    node_t *host = id(d->root, "host"), *root = host ? host->shadow_root : NULL;
    check("script前にshadow木を公開", root && script && script->parent == root &&
          script->owner == d && id(root, "before") && !id(root, "after") && !id(d->root, "tail"));
    node_t *before = root ? id(root, "before") : NULL;
    check("shadow木のnative変更", before && doc_node_text(d, before, "changed", 7));
    script = NULL;
    check("shadow script後の再開", html_resume(d->parser, &script) == 0 && !script);
    check("shadow identityと変更保持", host && host->shadow_root == root && id(root, "before") == before &&
          text_is(before, "changed") && id(root, "after") && id(d->root, "tail"));
    web_free(d);
}
static void content_patching(void) {
    web_doc *d = parse("<!doctype html><select id=target><?marker name='options'?></select>"
        "<template for=options><option id=added>new</option></template>", true);
    if (!d) return;
    node_t *target = id(d->root, "target"), *added = id(d->root, "added");
    check("marker patchの実挿入先", target && added && added->parent == target && target->first == added && target->last == added);
    check("patch追加ノードはlive所有者", added && added->owner == d && !added->template_host);
    web_free(d);

    d = parse("<!doctype html><aside id=range>a<?start name='r'?>old<?start?>nested<?end?>old<?end?>z</aside>"
        "<template for=r><b id=new>replacement</b></template><template id=missing for=absent>kept</template>", true);
    if (!d) return;
    target = id(d->root, "range"); added = id(d->root, "new");
    check("nested rangeの置換", target && added && added->parent == target && text_is(target, "areplacementz"));
    node_t *missing = id(d->root, "missing");
    check("marker不在は通常template", missing && missing->template_content && text_is(missing->template_content, "kept"));
    web_free(d);

    d = parse("<!doctype html><div id=range><?start name='r'?>old<i>old</i></div>"
              "<template for=r><b>endless</b></template>", true);
    if (d) { check("end不在は親末尾まで", text_is(id(d->root, "range"), "endless")); web_free(d); }

    d = begin("<!doctype html><div id=target><?start name='r'?>old<?end?></div><div id=other></div>"
              "<template for=r><b id=first>a</b><script id=pause>pause</script><i id=next>b</i></template>", true);
    check("移動marker文書生成", d != NULL);
    if (!d) return;
    node_t *script = NULL;
    check("patch内script境界", html_resume(d->parser, &script) == 1 && script && script->id && !strcmp(script->id, "pause"));
    target = id(d->root, "target"); node_t *other = id(d->root, "other");
    node_t *start = target ? target->first : NULL, *end = target ? target->last : NULL;
    check("markerを作者が別親へ移動", start && end && start->type == N_PI && end->type == N_PI &&
          other && doc_node_move(d, other, end, NULL));
    script = NULL;
    check("marker移動後の完了", html_resume(d->parser, &script) == 0 && !script);
    added = id(d->root, "next");
    check("end移動後は元target末尾へ挿入", added && added->parent == target && target->last == added);
    check("closeは移動markerも除去", start && end && !start->parent && !end->parent && other && !other->first);
    web_free(d);
}
static void parser_form_owner(void) {
    web_doc *d = begin("<!doctype html><table><form id=f><input id=i type=hidden></table>"
                      "<script id=pause>pause</script><p>tail</p>", true);
    check("form owner文書生成", d != NULL); if (!d) return;
    node_t *script = NULL;
    check("form owner変更前のscript境界", html_resume(d->parser, &script) == 1 && script);
    node_t *form = id(d->root, "f"), *input = id(d->root, "i");
    check("parser form pointerは非祖先ownerを保持", form && input && input->parent != form && input->parser_form_owner == form);
    if (input && d->body) {
        check("作者移動はparser-only関連を解除", doc_node_move(d, d->body, input, NULL) && !input->parser_form_owner);
        node_t *none = NULL;
        check("export/importで旧form ownerを復活させない", html_resume(d->parser, &none) == 0 && !input->parser_form_owner);
    }
    web_free(d);
}
int main(void) {
    pseudo_attributes(); declarative_shadow(); shadow_script_boundary(); content_patching(); parser_form_owner();
    printf("html_modern_tree_test: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
