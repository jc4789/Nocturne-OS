#include "cssom.h"
#include "http.h"

static void discard_parse(struct cssom_sheet *s) { ar_free(&s->arena); free(s->source); free(s->base); }
static int parse(struct cssom_sheet *s, const char *text, size_t length, const char *base) {
    if (length == SIZE_MAX || (length && !text)) return CSSOM_OOM;
    s->source = malloc(length + 1); s->base = strdup(base ? base : "");
    if (!s->source || !s->base) return CSSOM_OOM;
    if (length) memcpy(s->source, text, length);
    s->source[length] = 0; s->length = length;
    jmp_buf trap; s->arena.trap = &trap;
    if (setjmp(trap)) { s->arena.trap = NULL; return CSSOM_OOM; }
    s->ast = css_parse_sheet(&s->arena, s->source, length, s->base, 0, NULL);
    s->arena.trap = NULL; return CSSOM_OK;
}
static void invalidate(web_doc *d) {
    css_styling_free(&d->sty);
    d->resources_dirty = d->dirty = d->need_style = true;
    d->layout_valid = false;
}
void cssom_changed(struct cssom_sheet *s) {
    if (s->constructed) invalidate(s->document);
    else if (s->associated && s->owner->cssom_current == s && s->owner->owner == s->document)
        invalidate(s->document);
}
struct cssom_sheet *cssom_construct(web_doc *d, int *error) {
    *error = CSSOM_OK;
    node_t *owner = d->root;
    if (owner->cssom_serial == UINT32_MAX) { *error = CSSOM_OOM; return NULL; }
    struct cssom_sheet *s = calloc(1, sizeof *s);
    if (!s) { *error = CSSOM_OOM; return NULL; }
    *error = parse(s, "", 0, d->base);
    if (*error) { discard_parse(s); free(s); return NULL; }
    s->document = d; s->owner = owner; s->id = ++owner->cssom_serial;
    s->constructed = s->origin_clean = true;
    s->next = owner->cssom_sheets; owner->cssom_sheets = s;
    return s;
}
int cssom_adopt(node_t *root, struct cssom_sheet **sheets, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (!sheets[i] || !sheets[i]->constructed || sheets[i]->document != root->owner) return CSSOM_NOT_ALLOWED;
    struct cssom_sheet **copy = count ? malloc((size_t)count * sizeof *copy) : NULL;
    if (count && !copy) return CSSOM_OOM;
    if (count) memcpy(copy, sheets, (size_t)count * sizeof *copy);
    free(root->adopted_sheets); root->adopted_sheets = copy; root->adopted_count = count;
    invalidate(root->owner); return CSSOM_OK;
}
int cssom_replace_sync(struct cssom_sheet *s, const char *text, size_t length) {
    if (!s || !s->constructed) return CSSOM_NOT_ALLOWED;
    struct cssom_sheet fresh = {0};
    int error = parse(&fresh, text, length, s->base);
    if (error) { discard_parse(&fresh); return error; }
    /* Constructed sheets never fetch @import; drop these rules from the AST
       and source via a second parse, preserving all other parsed rules. */
    sbuf source = {0};
    for (struct css_rule_info *r = css_sheet_rules(fresh.ast, NULL); r; r = r->next)
        if (r->type != 3) { sb_puts(&source, r->text); sb_putc(&source, '\n'); }
    discard_parse(&fresh); memset(&fresh, 0, sizeof fresh);
    error = parse(&fresh, sb_cstr(&source), source.n, s->base); sb_free(&source);
    if (error) { discard_parse(&fresh); return error; }
    uint32_t count; css_sheet_rules(fresh.ast, &count);
    if (count > UINT32_MAX - s->next_rule_id) { discard_parse(&fresh); return CSSOM_OOM; }
    for (struct css_rule_info *r = css_sheet_rules(fresh.ast, NULL); r; r = r->next) r->id = ++s->next_rule_id;
    cssom_changed(s); /* unpublish borrowed AST pointers before releasing */
    discard_parse(s); s->arena = fresh.arena; s->ast = fresh.ast;
    s->source = fresh.source; s->base = fresh.base; s->length = fresh.length;
    return CSSOM_OK;
}
static bool active(web_doc *d, node_t *n) {
    return d && n && n->owner == d && n->type == N_ELEM && !n->foreign && (n->tag == T_style || n->tag == T_link) &&
        d->live && !d->inert && doc_node_root(n, true) == d->root && !node_ancestor(n,T_template);
}
bool cssom_owner_disabled(node_t *n) {
    return n && (n->style_disabled || (n->tag==T_link && !n->style_disabled_set && node_attr(n,"disabled")));
}
static int link_origin_clean(web_doc *d,const struct cssom_link_source *source,bool *clean) {
    *clean=false;
    if(!strncasecmp(source->url,"data:",5)){*clean=true;return CSSOM_OK;}
    char *origin=NULL,*request=NULL,*final=NULL;
    enum http_url_result a=http_origin_owned(web_effective_url(d),&origin),
        b=http_origin_owned(source->url,&request),c=http_origin_owned(source->base,&final);
    int error=(a==HTTP_URL_OOM||b==HTTP_URL_OOM||c==HTTP_URL_OOM)?CSSOM_OOM:CSSOM_OK;
    *clean=!error&&a==HTTP_URL_TUPLE&&b==HTTP_URL_TUPLE&&c==HTTP_URL_TUPLE&&
        !strcmp(origin,request)&&!strcmp(origin,final);
    free(origin);free(request);free(final);return error;
}
static bool available(web_doc *d,node_t *n,int *error) {
    if(!active(d,n))return false;
    const char *type=node_attr(n,"type");
    if(type&&*type&&!str_ieq(type,"text/css"))return false;
    if(n->tag==T_style)return true;
    struct cssom_link_source source;
    int status=doc_cssom_link_source(d,n,&source);
    free(source.url);free(source.owned_text);
    if(status==CSSOM_OOM)*error=status;
    return status==CSSOM_OK;
}
node_t *cssom_list_owner(web_doc *d,node_t *root,uint32_t index,bool select,uint32_t *count,int *error) {
    *error=CSSOM_OK;*count=0;
    if(!d||!root||root->owner!=d||!d->live||d->inert)return NULL;
    /* Ordinary native tree order, not the flattened styling tree: a document
       excludes its shadow sheets and a ShadowRoot excludes nested shadows. */
    for(node_t *n=root->first;n;) {
        if(available(d,n,error)) {
            if(select&&*count==index)return n;
            if(*count==UINT32_MAX){*error=CSSOM_OOM;return NULL;}
            (*count)++;
        }
        if(*error)return NULL;
        if(n->first && !(n->type==N_ELEM&&!n->foreign&&n->tag==T_template)){n=n->first;continue;}
        while(n!=root&&!n->next)n=n->parent;
        if(n==root)break;n=n->next;
    }
    return NULL;
}
struct cssom_sheet *cssom_style_find(node_t *n, uint32_t id) {
    for (struct cssom_sheet *s = n ? n->cssom_sheets : NULL; s; s = s->next) if (s->id == id) return s;
    return NULL;
}
static bool append_text(char **p, size_t *used, size_t *capacity, const char *text, size_t n) {
    if (*used == SIZE_MAX || n > SIZE_MAX - *used - 1) return false;
    size_t need = *used + n + 1;
    if (need > *capacity) {
        size_t cap = *capacity ? *capacity : 256;
        while (cap < need) cap = cap > SIZE_MAX/2 ? need : cap*2;
        char *q = realloc(*p, cap); if (!q) return false; *p = q; *capacity = cap;
    }
    if (n) memcpy(*p + *used, text, n);
    *used += n; (*p)[*used] = 0; return true;
}
static bool owner_text(node_t *owner, char **out, size_t *length) {
    char *text = NULL; size_t used = 0, cap = 0;
    for (node_t *n = owner->first; n;) {
        if (n->type == N_TEXT && !append_text(&text,&used,&cap,n->text ? n->text : "",n->textlen)) { free(text); return false; }
        if (n->first) { n = n->first; continue; }
        while (n != owner && !n->next) n = n->parent;
        if (n == owner) break; n = n->next;
    }
    if (!text && !append_text(&text,&used,&cap,"",0)) return false;
    *out = text; *length = used; return true;
}
struct cssom_sheet *cssom_style_sheet(web_doc *d, node_t *n, int *error) {
    *error = CSSOM_OK;
    if (!active(d,n)) { *error = CSSOM_INACTIVE; return NULL; }
    const char *type = node_attr(n,"type");
    if (type && *type && !str_ieq(type,"text/css")) {
        cssom_style_text_changed(n); *error = CSSOM_INACTIVE; return NULL;
    }
    struct cssom_link_source input={0};bool link=n->tag==T_link;
    if(link) {
        *error=doc_cssom_link_source(d,n,&input);
        if(*error)return NULL;
    }
    if (n->cssom_current && n->cssom_current->associated && n->cssom_current->document == d) {
        struct cssom_sheet *current=n->cssom_current;
        if (current->observed_dom_revision==d->dom_revision &&
            (!link || (current->dom_href && !strcmp(current->dom_href,input.url)))) {
            free(input.url);free(input.owned_text);return current;
        }
        char *text; size_t length;
        if(link){text=(char*)input.text;length=input.length;}
        else if (!owner_text(n,&text,&length)) { *error=CSSOM_OOM;return NULL; }
        bool same=length==current->dom_length&&!memcmp(text,current->dom_source,length)&&
            (!link||(current->dom_href&&!strcmp(current->dom_href,input.url)));
        if(!link)free(text);
        if (same) { current->observed_dom_revision=d->dom_revision;free(input.url);free(input.owned_text);return current; }
        current->disabled=cssom_owner_disabled(n);current->associated=false;n->cssom_current=NULL;
    }
    if (n->cssom_serial == UINT32_MAX) { *error = CSSOM_OOM;goto fail_input; }
    char *text; size_t length;
    if(link) {
        length=input.length;text=malloc(length+1);
        if(!text){*error=CSSOM_OOM;goto fail_input;}
        memcpy(text,input.text,length);text[length]=0;
    }else if (!owner_text(n,&text,&length)) { *error = CSSOM_OOM; return NULL; }
    struct cssom_sheet *s = calloc(1,sizeof *s);
    if (!s) { free(text); *error = CSSOM_OOM;goto fail_input; }
    s->origin_clean=true;
    if(link) {
        *error=link_origin_clean(d,&input,&s->origin_clean);
        s->href=strdup(!strncasecmp(input.url,"data:",5)?input.url:input.base);
        if(!s->href)*error=CSSOM_OOM;
    }
    if(!*error)*error = parse(s,text,length,link?input.base:d->base);
    if (*error) { free(text);discard_parse(s);free(s->href);free(s);goto fail_input; }
    s->dom_source=text;s->dom_length=length;s->observed_dom_revision=d->dom_revision;
    s->dom_href=input.url;input.url=NULL;free(input.owned_text);input.owned_text=NULL;
    uint32_t count; css_sheet_rules(s->ast,&count);
    for (struct css_rule_info *r=css_sheet_rules(s->ast,NULL);r;r=r->next) r->id=++s->next_rule_id;
    s->document=d; s->owner=n; s->id=++n->cssom_serial; s->associated=true; s->disabled=cssom_owner_disabled(n);
    if (n->cssom_current) n->cssom_current->associated=false;
    s->next=n->cssom_sheets; n->cssom_sheets=n->cssom_current=s;
    d->resources_dirty=d->dirty=d->need_style=true;
    return s;
fail_input:
    free(input.url);free(input.owned_text);return NULL;
}
static int replace(struct cssom_sheet *s, const char *text, size_t length, uint32_t index, bool insertion) {
    uint32_t count; css_sheet_rules(s->ast,&count);
    if (index > count || (!insertion && index == count)) return CSSOM_INDEX;
    if (insertion && (count == UINT32_MAX || s->next_rule_id == UINT32_MAX)) return CSSOM_OOM;
    struct cssom_sheet *candidate = calloc(1,sizeof *candidate);
    if (!candidate) return CSSOM_OOM;
    int error = CSSOM_OK;
    struct css_rule_info *removed=NULL;
    if (!insertion) {
        struct css_rule_info *r=css_sheet_rules(s->ast,NULL);
        for(uint32_t i=0;i<index;i++)r=r->next;
        removed=calloc(1,sizeof *removed);
        if (!removed) { free(candidate);return CSSOM_OOM; }
        removed->id=r->id;removed->type=r->type;
        removed->text=strdup(r->text);removed->selector=r->selector ? strdup(r->selector) : NULL;
        if (!removed->text || (r->selector&&!removed->selector)) { free((void*)removed->text);free((void*)removed->selector);free(removed);free(candidate);return CSSOM_OOM; }
    }
    if (insertion) {
        error=parse(candidate,text,length,s->base);
        if (!error && !css_sheet_single_style(candidate->ast,text,length)) {
            uint32_t rule_count;struct css_rule_info*r=css_sheet_rules(candidate->ast,&rule_count);
            error=rule_count==1&&r->type!=1 ? CSSOM_UNSUPPORTED : CSSOM_SYNTAX;
        }
        discard_parse(candidate); memset(candidate,0,sizeof *candidate);
        if (error) { free(candidate); return error; }
        uint32_t i=0;
        for(struct css_rule_info*r=css_sheet_rules(s->ast,NULL);r;r=r->next,i++) {
            if(i>=index&&(r->type==3||r->type==10)){free(candidate);return CSSOM_HIERARCHY;}
        }
    }
    char *source=NULL; size_t used=0,cap=0; uint32_t at=0;
    struct css_rule_info *old=css_sheet_rules(s->ast,NULL);
    for (struct css_rule_info *r=old;;r=r?r->next:NULL,at++) {
        if (insertion && at==index && (!append_text(&source,&used,&cap,text,length) || !append_text(&source,&used,&cap,"\n",1))) { error=CSSOM_OOM; break; }
        if (!r) break;
        if (!insertion && at==index) continue;
        if (!append_text(&source,&used,&cap,r->text,strlen(r->text)) || !append_text(&source,&used,&cap,"\n",1)) { error=CSSOM_OOM; break; }
    }
    if (!error) error=parse(candidate,source ? source : "",used,s->base);
    free(source);
    uint32_t fresh_count; css_sheet_rules(candidate->ast,&fresh_count);
    if (!error && fresh_count != count + (insertion ? 1u : (uint32_t)-1)) error=CSSOM_SYNTAX;
    if (!error) {
        struct css_rule_info *prior=old; uint32_t i=0;
        for (struct css_rule_info *r=css_sheet_rules(candidate->ast,NULL);r;r=r->next,i++) {
            if (insertion && i==index) r->id=s->next_rule_id+1;
            else { if (!insertion && i==index) prior=prior->next; r->id=prior->id; prior=prior->next; }
        }
        /* Unpublish all index/wrapper pointers before freeing the old AST.
           The next native layout/task scan builds from the new real AST. */
        cssom_changed(s);
        discard_parse(s); s->arena=candidate->arena; s->ast=candidate->ast;
        s->source=candidate->source; s->base=candidate->base; s->length=candidate->length;
        if (insertion) s->next_rule_id++;
        if (removed) { removed->next=s->removed;s->removed=removed;removed=NULL; }
        memset(candidate,0,sizeof *candidate);
    }
    if(removed){free((void*)removed->text);free((void*)removed->selector);free(removed);}
    discard_parse(candidate); free(candidate); return error;
}
int cssom_insert(struct cssom_sheet *s,const char *text,size_t length,uint32_t index) { return s ? replace(s,text,length,index,true) : CSSOM_INACTIVE; }
int cssom_delete(struct cssom_sheet *s,uint32_t index) { return s ? replace(s,NULL,0,index,false) : CSSOM_INACTIVE; }
void cssom_style_text_changed(node_t *n) {
    for (;n;n=n->parent) if (!n->foreign && n->type==N_ELEM && n->tag==T_style && n->cssom_current) {
        n->cssom_current->disabled=cssom_owner_disabled(n);n->cssom_current->associated=false; n->cssom_current=NULL;
    }
}
void cssom_style_lifecycle(node_t *root) {
    for (node_t *n=root;n;) {
        if (n->cssom_current) { n->cssom_current->disabled=cssom_owner_disabled(n);n->cssom_current->associated=false; n->cssom_current=NULL; }
        if (n->first) { n=n->first;continue; }
        if (n->shadow_root) { n=n->shadow_root;continue; }
        for (;;) {
            if(n==root){n=NULL;break;}
            if(n->parent){
                if(n->next){n=n->next;break;}
                node_t *parent=n->parent;
                if(parent->shadow_root){n=parent->shadow_root;break;}
                n=parent;
            }else if(n->shadow_host)n=n->shadow_host;
            else {n=NULL;break;}
        }
    }
}
void cssom_style_free(node_t *n) {
    free(n->adopted_sheets); n->adopted_sheets = NULL; n->adopted_count = 0;
    for (struct cssom_sheet *s=n->cssom_sheets;s;) {
        struct cssom_sheet *next=s->next;
        for(struct css_rule_info*r=s->removed;r;){struct css_rule_info*next_rule=r->next;free((void*)r->text);free((void*)r->selector);free(r);r=next_rule;}
        discard_parse(s);free(s->media);free(s->dom_source);free(s->dom_href);free(s->href);free(s);s=next;
    }
    n->cssom_sheets=n->cssom_current=NULL;
}
