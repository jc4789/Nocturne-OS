/* https://html.spec.whatwg.org/multipage/parsing.html#serialising-html-fragments
 * native DOMだけを直列化する。innerHTMLはshadow treeを公開しない。 */
#include "html_serialize.h"

static bool serializes_as_void(const node_t *n) {
    if (!n || n->type != N_ELEM || n->namespace_id != NS_HTML) return false;
    switch (n->tag) {
    case T_area: case T_base: case T_br: case T_col: case T_embed: case T_hr:
    case T_img: case T_input: case T_link: case T_meta: case T_source:
    case T_track: case T_wbr: case T_frame: case T_keygen: case T_param:
        return true;
    default:
        return n->name && (!strcmp(n->name, "basefont") || !strcmp(n->name, "bgsound"));
    }
}

static bool literal_text(const node_t *n) {
    const node_t *p = n->parent;
    if (!p || p->type != N_ELEM || p->namespace_id != NS_HTML) return false;
    switch (p->tag) {
    case T_style: case T_script: case T_xmp: case T_iframe: case T_noembed:
    case T_noframes: case T_plaintext: return true;
    case T_noscript: return n->owner && n->owner->live && !n->owner->inert;
    default: return false;
    }
}

static void escaped(sbuf *out, const char *text, size_t length, bool attribute) {
    if (!text) return;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '&') sb_puts(out, "&amp;");
        else if (c == '<') sb_puts(out, "&lt;");
        else if (c == '>') sb_puts(out, "&gt;");
        else if (attribute && c == '"') sb_puts(out, "&quot;");
        else if (c == 0xc2 && i + 1 < length && (unsigned char)text[i + 1] == 0xa0) {
            sb_puts(out, "&nbsp;"); i++;
        } else sb_putc(out, (char)c);
    }
}

static void attribute_name(const struct attr *a, sbuf *out) {
    const char *local = a->local ? a->local : a->raw ? a->raw : a->name;
    if (!a->namespace_uri) sb_puts(out, local);
    else if (!strcmp(a->namespace_uri, "http://www.w3.org/XML/1998/namespace")) {
        sb_puts(out, "xml:"); sb_puts(out, local);
    } else if (!strcmp(a->namespace_uri, "http://www.w3.org/2000/xmlns/")) {
        if (strcmp(local, "xmlns")) sb_puts(out, "xmlns:");
        sb_puts(out, local);
    } else if (!strcmp(a->namespace_uri, "http://www.w3.org/1999/xlink")) {
        sb_puts(out, "xlink:"); sb_puts(out, local);
    } else sb_puts(out, a->raw ? a->raw : local);
}

struct serialization_options {bool serializable;node_t **roots;size_t count;};
static void serialize_node(const node_t *node,sbuf *out,const struct serialization_options *options);
static bool include_shadow(const node_t *root,const struct serialization_options *options){
    if(!options)return false;
    if(options->serializable && root->shadow_serializable)return true;
    for(size_t i=0;i<options->count;i++)if(options->roots[i]==root)return true;
    return false;
}
static void serialize_children(const node_t *node, sbuf *out,const struct serialization_options *options) {
    if (!node || serializes_as_void(node)) return;
    const node_t *parent = node->type == N_ELEM && node->namespace_id == NS_HTML && node->tag == T_template ?
        node->template_content : node;
    if(node->shadow_root && include_shadow(node->shadow_root,options)){
        const node_t *r=node->shadow_root;
        sb_puts(out,"<template shadowrootmode=\"");sb_puts(out,r->shadow_closed?"closed":"open");sb_putc(out,'"');
        if(r->shadow_delegates_focus)sb_puts(out," shadowrootdelegatesfocus=\"\"");
        if(r->shadow_clonable)sb_puts(out," shadowrootclonable=\"\"");
        if(r->shadow_serializable)sb_puts(out," shadowrootserializable=\"\"");
        if(r->shadow_manual)sb_puts(out," shadowrootslotassignment=\"manual\"");
        if(r->custom_registry_id==-1)sb_puts(out," shadowrootcustomelementregistry=\"\"");
        sb_putc(out,'>');serialize_children(r,out,options);sb_puts(out,"</template>");
    }
    for (const node_t *child = parent ? parent->first : NULL; child; child = child->next)
        serialize_node(child, out,options);
}

static void serialize_node(const node_t *node, sbuf *out,const struct serialization_options *options) {
    if (!node) return;
    if (node->type == N_TEXT) {
        if (literal_text(node)) sb_put(out, node->text ? node->text : "", node->textlen);
        else escaped(out, node->text, node->textlen, false);
    } else if (node->type == N_COMMENT) {
        sb_puts(out, "<!--"); sb_put(out, node->text ? node->text : "", node->textlen); sb_puts(out, "-->");
    } else if (node->type == N_PI) {
        sb_puts(out, "<?"); sb_puts(out, node->name); sb_putc(out, ' ');
        sb_put(out, node->text ? node->text : "", node->textlen); sb_puts(out, "?>");
    } else if (node->type == N_DOCTYPE) {
        sb_puts(out, "<!DOCTYPE "); sb_puts(out, node->name); sb_putc(out, '>');
    } else if (node->type == N_ELEM) {
        const char *name = node->namespace_id == NS_HTML ? node->name : node->raw_name ? node->raw_name : node->name;
        sb_putc(out, '<'); sb_puts(out, name);
        if(node->custom_is && !node_attr(node,"is")){
            sb_puts(out," is=\"");escaped(out,node->custom_is,strlen(node->custom_is),true);sb_putc(out,'"');
        }
        for (int i = 0; i < node->nattrs; i++) {
            const struct attr *a = &node->attrs[i];
            sb_putc(out, ' '); attribute_name(a, out); sb_puts(out, "=\"");
            escaped(out, a->value, strlen(a->value), true); sb_putc(out, '"');
        }
        sb_putc(out, '>');
        if (!serializes_as_void(node)) {
            serialize_children(node, out,options);
            sb_puts(out, "</"); sb_puts(out, name); sb_putc(out, '>');
        }
    } else if (node->type == N_DOC || node->type == N_FRAGMENT) serialize_children(node, out,options);
}
void html_serialize_node(const node_t *node,sbuf *out){serialize_node(node,out,NULL);}
void html_serialize_children(const node_t *node,sbuf *out){serialize_children(node,out,NULL);}
void html_serialize_get(const node_t *node,sbuf *out,bool serializable,node_t **roots,size_t count){
    struct serialization_options options={serializable,roots,count};serialize_children(node,out,&options);
}
