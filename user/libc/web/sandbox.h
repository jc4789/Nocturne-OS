#ifndef WEB_SANDBOX_H
#define WEB_SANDBOX_H
/* Active restrictions, not grants. Pending iframe policy is copied to a
 * Document before parser/bootstrap creation and never reread from author DOM. */
enum web_sandbox_flag {
    SB_NAVIGATION=1u<<0, SB_TOP=1u<<1, SB_TOP_ACTIVATION=1u<<2,
    SB_ORIGIN=1u<<3, SB_SCRIPTS=1u<<4, SB_FORMS=1u<<5,
    SB_POPUPS=1u<<6, SB_PROPAGATE=1u<<7, SB_DOMAIN=1u<<8,
    SB_DOWNLOADS=1u<<9, SB_MODALS=1u<<10, SB_POINTER=1u<<11,
    SB_ORIENTATION=1u<<12, SB_PRESENTATION=1u<<13,
    SB_CUSTOM_PROTOCOLS=1u<<14, SB_UNSUPPORTED=1u<<15
};
#define SB_ALL ((1u<<15)-1)
static bool sandbox_space(unsigned char c) {
    return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f';
}
static bool sandbox_word_equal(const char *p,size_t n,const char *word) {
    size_t i=0;
    for(;i<n && word[i];i++) {
        unsigned char c=(unsigned char)p[i];if(c>='A' && c<='Z')c+=32;
        if(c!=(unsigned char)word[i])return false;
    }
    return i==n && !word[i];
}
/* Known allow words only; unknown author text is never copied to diagnostics.
 * Overlong directives fail closed rather than silently dropping restrictions. */
static uint32_t sandbox_parse(const char *value,char *known,size_t capacity) {
    static const struct {const char *word;uint32_t clear;} words[]={
        {"allow-downloads",SB_DOWNLOADS},{"allow-forms",SB_FORMS},
        {"allow-modals",SB_MODALS},{"allow-orientation-lock",SB_ORIENTATION},
        {"allow-pointer-lock",SB_POINTER},{"allow-popups",SB_POPUPS|SB_CUSTOM_PROTOCOLS},
        {"allow-popups-to-escape-sandbox",SB_PROPAGATE},
        {"allow-presentation",SB_PRESENTATION},{"allow-same-origin",SB_ORIGIN},
        {"allow-scripts",SB_SCRIPTS},{"allow-top-navigation",SB_TOP|SB_TOP_ACTIVATION|SB_CUSTOM_PROTOCOLS},
        {"allow-top-navigation-by-user-activation",SB_TOP_ACTIVATION},
        {"allow-top-navigation-to-custom-protocols",SB_CUSTOM_PROTOCOLS}
    };
    if(known && capacity)known[0]=0;
    if(!value)return 0;
    uint32_t flags=SB_ALL,seen=0;size_t used=0,read=0;
    for(const char *p=value;*p;) {
        while(*p && sandbox_space((unsigned char)*p)){p++;if(++read>4096)return flags|SB_UNSUPPORTED;}
        const char *start=p;
        while(*p && !sandbox_space((unsigned char)*p)){p++;if(++read>4096)return flags|SB_UNSUPPORTED;}
        size_t n=(size_t)(p-start);if(!n)continue;
        for(unsigned i=0;i<sizeof words/sizeof *words;i++)if(sandbox_word_equal(start,n,words[i].word)) {
            flags&=~words[i].clear;
            if(!(seen&(1u<<i)) && known && capacity) {
                size_t length=strlen(words[i].word),gap=used?1:0;
                if(length+gap<capacity-used) {
                    if(gap)known[used++]=' ';
                    memcpy(known+used,words[i].word,length);used+=length;known[used]=0;
                }
            }
            seen|=1u<<i;break;
        }
    }
    /* This bounded profile does not grant unactivated top navigation or
       external protocols. The corresponding profiles remain fail closed. */
    if(!(flags&SB_TOP))flags|=SB_UNSUPPORTED;
    return flags;
}
static bool sandbox_supported(uint32_t flags) {
    return !(flags&(SB_ORIGIN|SB_SCRIPTS|SB_UNSUPPORTED));
}
#endif
