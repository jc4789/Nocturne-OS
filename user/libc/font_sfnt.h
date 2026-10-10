/* Byte-bounded TrueType input gate for downloaded faces. The bundled CFF
 * renderer is separate: this gate never treats CFF, WOFF or WOFF2 as TTF.
 * Only tables used by native metrics/cmap/outline rendering are interpreted. */
#pragma once
#include <limits.h>

struct sf_span { const unsigned char *p; size_t n; };
static uint16_t sf_u16(const unsigned char *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static int16_t sf_i16(const unsigned char *p) { return (int16_t)sf_u16(p); }
static uint32_t sf_u32(const unsigned char *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static bool sf_has(struct sf_span s,size_t at,size_t n) { return at<=s.n && n<=s.n-at; }
static bool sf_table(struct sf_span file,const char *tag,struct sf_span *out) {
    unsigned count=sf_u16(file.p+4); bool found=false;
    for(unsigned i=0;i<count;i++) {
        const unsigned char *r=file.p+12+16*i;
        if(memcmp(r,tag,4)) continue;
        if(found) return false;
        uint32_t at=sf_u32(r+8), n=sf_u32(r+12);
        if(!at || at>INT_MAX || n>INT_MAX || !sf_has(file,at,n) || n>(size_t)INT_MAX-at) return false;
        *out=(struct sf_span){file.p+at,n};found=true;
    }
    return found;
}
static bool sf_cmap(struct sf_span map,unsigned glyphs) {
    if(!sf_has(map,0,4) || sf_u16(map.p)!=0) return false;
    unsigned count=sf_u16(map.p+2); size_t chosen=0;
    if(!sf_has(map,4,(size_t)count*8)) return false;
    for(unsigned i=0;i<count;i++) {
        const unsigned char *r=map.p+4+8*i;
        unsigned platform=sf_u16(r), encoding=sf_u16(r+2);
        if(platform==0 || (platform==3 && (encoding==1 || encoding==10))) chosen=sf_u32(r+4);
    }
    if(!chosen || !sf_has(map,chosen,6)) return false;
    struct sf_span s={map.p+chosen,map.n-chosen};unsigned format=sf_u16(s.p);
    if(format==0 || format==6 || format==4) {
        size_t len=sf_u16(s.p+2);if(len<6 || len>s.n)return false;s.n=len;
    } else if(format==12 || format==13) {
        if(!sf_has(s,0,16))return false;size_t len=sf_u32(s.p+4);
        if(len<16 || len>s.n)return false;s.n=len;
    } else return false;
    if(format==0) {
        if(!sf_has(s,6,256))return false;
        for(unsigned i=0;i<256;i++)if(s.p[6+i]>=glyphs)return false;
    } else if(format==6) {
        if(!sf_has(s,0,10))return false;unsigned n=sf_u16(s.p+8);
        if(!sf_has(s,10,(size_t)n*2) || (unsigned)sf_u16(s.p+6)+n>65536)return false;
        for(unsigned i=0;i<n;i++)if(sf_u16(s.p+10+2*i)>=glyphs)return false;
    } else if(format==4) {
        if(!sf_has(s,0,16))return false;unsigned twice=sf_u16(s.p+6), n=twice/2;
        if(!n || (twice&1) || !sf_has(s,14,(size_t)n*8+2))return false;
        unsigned power=1, selector=0;while(power<=n/2){power*=2;selector++;}
        if(sf_u16(s.p+8)!=2*power || sf_u16(s.p+10)!=selector || sf_u16(s.p+12)!=2*(n-power))return false;
        unsigned previous=0;
        for(unsigned i=0;i<n;i++) {
            unsigned end=sf_u16(s.p+14+2*i), start=sf_u16(s.p+16+2*n+2*i);
            unsigned range=sf_u16(s.p+16+6*n+2*i);int delta=sf_i16(s.p+16+4*n+2*i);
            if(start>end || (i && start<=previous) || (range&1))return false;previous=end;
            if(range && !sf_has(s,16+6*n+2*i+range,(size_t)(end-start+1)*2))return false;
            for(unsigned cp=start;;cp++) {
                unsigned g=range ? sf_u16(s.p+16+6*n+2*i+range+2*(cp-start)) : (uint16_t)(cp+delta);
                if(range && g)g=(uint16_t)(g+delta);
                if(g>=glyphs)return false;if(cp==end)break;
            }
        }
        if(previous!=65535)return false;
    } else {
        uint32_t n=sf_u32(s.p+12), previous=0;
        if(n>INT_MAX || !sf_has(s,16,(size_t)n*12))return false;
        for(uint32_t i=0;i<n;i++) {
            const unsigned char *r=s.p+16+(size_t)i*12;
            uint32_t start=sf_u32(r), end=sf_u32(r+4), g=sf_u32(r+8);
            if(start>end || end>0x10ffff || (i && start<=previous) || g>=glyphs ||
               (format==12 && end-start>=glyphs-g))return false;
            previous=end;
        }
    }
    return true;
}
static uint32_t sf_glyph_at(struct sf_span loca,unsigned g,bool wide) {
    return wide ? sf_u32(loca.p+4*g) : (uint32_t)sf_u16(loca.p+2*g)*2;
}
static bool sf_simple(struct sf_span s,unsigned contours) {
    if(!contours)return true;
    if(!sf_has(s,10,(size_t)contours*2+2))return false;
    unsigned previous=0, points=0;
    for(unsigned i=0;i<contours;i++) {
        unsigned end=sf_u16(s.p+10+2*i);if(i && end<=previous)return false;
        previous=end;points=end+1;
    }
    size_t at=12+(size_t)contours*2, ins=sf_u16(s.p+at-2);
    if(!sf_has(s,at,ins))return false;at+=ins;
    unsigned char *flags=points?malloc(points):NULL;
    if(points && !flags)return false;
    for(unsigned i=0;i<points;) {
        if(!sf_has(s,at,1))goto bad;unsigned f=s.p[at++], repeat=0;
        if(f&8){if(!sf_has(s,at,1))goto bad;repeat=s.p[at++];}
        if(repeat>=points-i)goto bad;
        do {flags[i++]=(unsigned char)f;}while(repeat--);
    }
    for(unsigned axis=0;axis<2;axis++) {
        int64_t coordinate=0;unsigned short_bit=axis?4:2,same_bit=axis?32:16;
        for(unsigned i=0;i<points;i++) {
            unsigned f=flags[i];int delta=0;
            if(f&short_bit){if(!sf_has(s,at,1))goto bad;delta=s.p[at++];if(!(f&same_bit))delta=-delta;}
            else if(!(f&same_bit)){if(!sf_has(s,at,2))goto bad;delta=sf_i16(s.p+at);at+=2;}
            coordinate+=delta;if(coordinate<INT16_MIN || coordinate>INT16_MAX)goto bad;
        }
    }
    free(flags);return true;
bad:free(flags);return false;
}
static bool sf_component(struct sf_span s,size_t *at,unsigned glyphs,unsigned *child,unsigned *flags) {
    if(!sf_has(s,*at,4))return false;
    *flags=sf_u16(s.p+*at);*child=sf_u16(s.p+*at+2);*at+=4;
    /* Point-matching placement is not supported by the native outline backend;
     * fail the face, never execute stb's assertion on hostile input. */
    if(*child>=glyphs || !(*flags&2))return false;
    unsigned transforms=!!(*flags&8)+!!(*flags&64)+!!(*flags&128);
    if(transforms>1)return false;
    size_t n=(*flags&1)?4:2;n+=(*flags&8)?2:(*flags&64)?4:(*flags&128)?8:0;
    if(!sf_has(s,*at,n))return false;*at+=n;return true;
}
static bool font_sfnt_valid(const void *data,size_t bytes) {
    struct sf_span file={data,bytes},head={0},hhea={0},hmtx={0},maxp={0},cmap={0},loca={0},glyf={0};
    if(bytes>INT_MAX || !sf_has(file,0,12) || sf_u32(file.p)!=0x00010000u ||
       !sf_has(file,12,(size_t)sf_u16(file.p+4)*16))return false;
    /* Even unused table records must point into the actual source buffer. */
    for(unsigned i=0;i<sf_u16(file.p+4);i++) {
        const unsigned char *r=file.p+12+16*i;
        if(!sf_has(file,sf_u32(r+8),sf_u32(r+12)))return false;
    }
    if(!sf_table(file,"head",&head)||!sf_table(file,"hhea",&hhea)||!sf_table(file,"hmtx",&hmtx)||
       !sf_table(file,"maxp",&maxp)||!sf_table(file,"cmap",&cmap)||!sf_table(file,"loca",&loca)||!sf_table(file,"glyf",&glyf)||
       !sf_has(head,0,54)||!sf_has(hhea,0,36)||!sf_has(maxp,0,6)||!sf_u16(head.p+18))return false;
    unsigned glyphs=sf_u16(maxp.p+4), metrics=sf_u16(hhea.p+34), format=sf_u16(head.p+50);
    if(!glyphs || !metrics || metrics>glyphs || format>1 ||
       !sf_has(hmtx,0,(size_t)metrics*4+(size_t)(glyphs-metrics)*2)||
       !sf_has(loca,0,(size_t)(glyphs+1)*(format?4:2))||!sf_cmap(cmap,glyphs))return false;
    uint32_t previous=0;
    for(unsigned g=0;g<=glyphs;g++) {
        uint32_t end=sf_glyph_at(loca,g,format!=0);if(end<previous || end>glyf.n)return false;
        if(g && end!=previous) {
            struct sf_span s={glyf.p+previous,end-previous};if(!sf_has(s,0,10))return false;
            int contours=sf_i16(s.p);
            if(sf_i16(s.p+2)>sf_i16(s.p+6)||sf_i16(s.p+4)>sf_i16(s.p+8))return false;
            if(contours>=0){if(!sf_simple(s,(unsigned)contours))return false;}
            else if(contours==-1) {
                size_t at=10;unsigned flags,child;
                do {if(!sf_component(s,&at,glyphs,&child,&flags))return false;}while(flags&32);
                if(flags&256){if(!sf_has(s,at,2)||!sf_has(s,at+2,sf_u16(s.p+at)))return false;}
            } else return false;
        }
        previous=end;
    }
    /* An explicit DFS verifies composite acyclicity without attacker-controlled
     * recursion or a guessed frame/depth quota. Stack length follows numGlyphs. */
    struct sf_visit {unsigned glyph;size_t at;bool entered,more;};
    struct sf_visit *stack=calloc(glyphs,sizeof *stack);unsigned char *state=calloc(glyphs,1);
    if(!stack||!state){free(stack);free(state);return false;}
    bool ok=true;
    for(unsigned root=0;ok && root<glyphs;root++) {
        if(state[root])continue;size_t depth=1;stack[0]=(struct sf_visit){.glyph=root};
        while(ok && depth) {
            struct sf_visit *v=&stack[depth-1];uint32_t a=sf_glyph_at(loca,v->glyph,format!=0),b=sf_glyph_at(loca,v->glyph+1,format!=0);
            struct sf_span s={glyf.p+a,b-a};
            if(!v->entered){state[v->glyph]=1;v->entered=true;v->at=10;v->more=s.n && sf_i16(s.p)==-1;}
            if(!v->more){state[v->glyph]=2;depth--;continue;}
            unsigned child,flags;if(!sf_component(s,&v->at,glyphs,&child,&flags)){ok=false;break;}v->more=(flags&32)!=0;
            if(state[child]==1){ok=false;break;}
            if(!state[child]){if(depth==glyphs){ok=false;break;}stack[depth++]=(struct sf_visit){.glyph=child};}
        }
    }
    free(stack);free(state);return ok;
}
