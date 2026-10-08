#include "image_source.h"
#include <stdio.h>

static bool supported(const char *type) {
    if(!type||!*type)return true;
    static const char *const types[]={"image/jpeg","image/png","image/gif","image/webp","image/svg+xml","image/bmp","image/x-icon","image/vnd.microsoft.icon"};
    size_t n=strcspn(type,";\t ");
    for(size_t i=0;i<sizeof types/sizeof types[0];i++)if(n==strlen(types[i])&&!strncasecmp(type,types[i],n))return true;
    return false;
}
static float source_size(web_doc *d,const char *sizes) {
    float viewport=d->width>0?d->width:1024;
    if(!sizes)return viewport;
    for(const char *p=sizes;*p;){
        const char *end=p;unsigned depth=0;
        while(*end&&(*end!=','||depth)){if(*end=='(')depth++;else if(*end==')'&&depth)depth--;end++;}
        const char *last=end;while(last>p&&is_space(last[-1]))last--;
        const char *value=last;while(value>p&&!is_space(value[-1]))value--;
        char number[64];size_t n=(size_t)(last-value);
        if(n&&n<sizeof number){memcpy(number,value,n);number[n]=0;
            char *unit;float amount=strtof(number,&unit),pixels=0;
            if(amount>0&&amount<1000000){if(!strcmp(unit,"px"))pixels=amount;else if(!strcmp(unit,"vw"))pixels=amount*viewport/100;}
            if(pixels>0){const char *condition_end=value;while(condition_end>p&&is_space(condition_end[-1]))condition_end--;
                if(condition_end==p)return pixels;
                char condition[512];size_t length=(size_t)(condition_end-p);
                if(length<sizeof condition){memcpy(condition,p,length);condition[length]=0;
                    if(css_media_evaluate(condition,(int)viewport,d->height>0?d->height:768,d->live,NULL,0))return pixels;}
            }
        }
        p=*end?end+1:end;while(is_space(*p))p++;
    }
    return viewport;
}
static char *candidate(web_doc *d,const char *list,const char *sizes,const char *fallback) {
    if(!list||!*list)return NULL;
    float width=source_size(d,sizes),chosen=0;const char *best=NULL;size_t best_length=0;
    int mode=0;bool mixed=false;
    for(const char *p=list;*p;){
        while(is_space(*p)||*p==',')p++;if(!*p)break;
        const char *url=p;while(*p&&!is_space(*p))p++;const char *last=p;
        bool comma=last>url&&last[-1]==',';while(last>url&&last[-1]==',')last--;
        const char *descriptor=p,*end=p;
        if(!comma){while(*end&&*end!=',')end++;p=*end?end+1:end;}
        while(descriptor<end&&is_space(*descriptor))descriptor++;
        while(end>descriptor&&is_space(end[-1]))end--;
        float density=1;int kind=1;bool valid=last>url;
        if(descriptor<end){char text[64];size_t n=(size_t)(end-descriptor);
            if(n>=sizeof text)valid=false;
            else {memcpy(text,descriptor,n);text[n]=0;char *unit;float number=strtof(text,&unit);
                valid=number>0&&number<1000000000&&unit[0]&&(unit[0]=='x'||unit[0]=='w')&&!unit[1];
                if(valid&&unit[0]=='w'){for(char *q=text;q<unit;q++)if(*q<'0'||*q>'9')valid=false;
                    density=number/width;kind=2;}else density=number;}
        }
        if(valid){if(mode&&mode!=kind)mixed=true;else mode=kind;
            if(!best||(density>=1&&(chosen<1||density<chosen))||(density<1&&chosen<1&&density>chosen)){
                best=url;best_length=(size_t)(last-url);chosen=density;}}
    }
    if(mixed||!best)return NULL;
    /* src is the implicit 1x candidate only for a density-descriptor set. */
    if(mode==1&&fallback&&*fallback&&chosen!=1)return strdup(fallback);
    return strndup(best,best_length);
}
char *web_image_source_pick(web_doc *d,node_t *image,const char *fallback) {
    node_t *picture=image->tag==T_img?image->parent:NULL;
    if(picture&&picture->tag==T_picture&&!picture->foreign){
        for(node_t *source=picture->first;source&&source!=image;source=source->next){
            if(source->type!=N_ELEM||source->foreign||source->tag!=T_source||!supported(node_attr(source,"type")))continue;
            const char *media=node_attr(source,"media");
            if(media&&*media&&!css_media_evaluate(media,d->width>0?d->width:1024,d->height>0?d->height:768,d->live,NULL,0))continue;
            char *pick=candidate(d,node_attr(source,"srcset"),node_attr(source,"sizes"),NULL);if(pick)return pick;
        }
    }
    return candidate(d,node_attr(image,"srcset"),node_attr(image,"sizes"),fallback);
}
