/* Cookie policy regressions. These do not replace actual website tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webcookie.h"
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL cookietest:%d: %s\n", __LINE__, #c); failed++; } } while (0)
static const char psl[] = "// ICANN and PRIVATE examples\ncom\norg\nuk\nco.uk\n*.ck\n!www.ck\ngithub.io\nxn--p1ai\n";
static int64_t now=1700000000;
static struct webcookie_context context(const char *url,bool http) {
    struct webcookie_context c={url,url,"GET",false,http,false};return c;
}
static int set(webcookie_jar *j,const char *url,const char *value,bool http) {
    struct webcookie_context c=context(url,http);return webcookie_set(j,&c,value,strlen(value),now);
}
static void get(webcookie_jar *j,const char *url,bool http,const char *expected) {
    struct webcookie_context c=context(url,http);char out[8192];long n=webcookie_get(j,&c,out,sizeof out,now);
    CHECK(n==(long)strlen(expected));CHECK(n>=0&&!strcmp(out,expected));
}
static webcookie_jar *jar(void) {webcookie_jar *j=webcookie_create();CHECK(j!=NULL);CHECK(webcookie_psl_load(j,psl,sizeof psl-1));return j;}
/* Whether a Cookie value names name (at its start or after "; "). */
static bool has(const char *out,const char *name) {
    size_t n=strlen(name);
    for(const char *p=out;(p=strstr(p,name));p++)if((p==out||(p>=out+2&&p[-2]==';'&&p[-1]==' '))&&p[n]=='=')return true;
    return false;
}
static bool sends(webcookie_jar *j,const char *url,const char *name) {
    struct webcookie_context c=context(url,true);static char out[WEBCOOKIE_OUTPUT_MAX+1];
    return webcookie_get(j,&c,out,sizeof out,now)>=0&&has(out,name);
}
/* The store as jar would write it now (webcookie_load with the document first, as webnet does). */
static unsigned char *save(webcookie_jar *j,const unsigned char *document,long length,long *out) {
    CHECK(webcookie_load(j,document,length>0?(size_t)length:0,now)==1);
    long n=webcookie_save(j,NULL,0,now);CHECK(n>=48);
    unsigned char *bytes=n>0?malloc((size_t)n):NULL;CHECK(bytes!=NULL);
    if(bytes){CHECK(webcookie_save(j,bytes,(size_t)n,now)==n);webcookie_saved(j);}
    *out=n;return bytes;
}
static void limits(void) {
    /* §5.7 in all: past WEBCOOKIE_TOTAL_MAX the least recently used cookie of any domain goes. */
    webcookie_jar *j=jar();
    for(int i=0;i<(int)WEBCOOKIE_TOTAL_MAX+100;i++){char url[64];snprintf(url,sizeof url,"https://h%d.example.com/",i);CHECK(set(j,url,"t=1",true)==1);}
    CHECK(!sends(j,"https://h0.example.com/","t"));CHECK(!sends(j,"https://h99.example.com/","t"));
    CHECK(sends(j,"https://h100.example.com/","t"));CHECK(sends(j,"https://h3099.example.com/","t"));
    long n=webcookie_export(j,NULL,0,now);CHECK(n>0&&(size_t)n<=WEBCOOKIE_SNAPSHOT_MAX);
    void *snapshot=malloc((size_t)n);CHECK(snapshot!=NULL);
    if(snapshot){CHECK(webcookie_export(j,snapshot,(size_t)n,now)==n);webcookie_jar *k=jar();CHECK(webcookie_import(k,snapshot,(size_t)n,now));
        CHECK(sends(k,"https://h3099.example.com/","t"));webcookie_free(k);free(snapshot);}
    webcookie_free(j);
    /* Per domain field the non-Secure cookie goes before an older Secure one. */
    j=jar();CHECK(set(j,"https://example.com/","s0=1; Secure",true)==1);CHECK(set(j,"https://example.com/","n1=1",true)==1);
    for(int i=2;i<(int)WEBCOOKIE_DOMAIN_MAX+1;i++){char value[32];snprintf(value,sizeof value,"s%d=1; Secure",i);CHECK(set(j,"https://example.com/",value,true)==1);}
    CHECK(sends(j,"https://example.com/","s0"));CHECK(!sends(j,"https://example.com/","n1"));CHECK(sends(j,"https://example.com/","s180"));
    webcookie_free(j);
}
static unsigned test_rule(void *opaque,const char *host) {
    (void)opaque;size_t n=strlen(host);
    static const char *const names[]={"blocked.com","temp.com"};
    for(unsigned k=0;k<2;k++){size_t m=strlen(names[k]);
        if(n>=m&&!strcmp(host+n-m,names[k])&&(n==m||host[n-m-1]=='.'))return k?WEBCOOKIE_TRANSIENT:WEBCOOKIE_BLOCK;}
    return WEBCOOKIE_ALLOW;
}
static void rules(void) {
    webcookie_jar *j=jar();
    CHECK(set(j,"https://www.blocked.com/","a=1; Max-Age=100",true)==1);
    CHECK(set(j,"https://www.temp.com/","b=1; Max-Age=100",true)==1);
    CHECK(set(j,"https://example.com/","c=1; Max-Age=100",true)==1);
    webcookie_set_rule(j,test_rule,NULL);
    /* Blocked: none set (HTTP or document.cookie), none sent, even one set before. */
    CHECK(set(j,"https://www.blocked.com/","d=1; Max-Age=100",true)==0);
    CHECK(set(j,"https://www.blocked.com/","d=1; Max-Age=100",false)==0);
    get(j,"https://www.blocked.com/",true,"");get(j,"https://www.blocked.com/",false,"");
    CHECK(sends(j,"https://www.temp.com/","b"));CHECK(sends(j,"https://example.com/","c"));
    webcookie_free(j);
}
static void store(void) {
    /* Two processes: each jar tracks its changes and merges the other's saved store. */
    webcookie_jar *a=jar(),*b=jar();webcookie_track(a);webcookie_track(b);
    webcookie_set_rule(a,test_rule,NULL);webcookie_set_rule(b,test_rule,NULL);
    CHECK(set(a,"https://example.com/","p=1; Max-Age=1000",true)==1);
    CHECK(set(a,"https://example.com/","s=1",true)==1);
    CHECK(set(a,"https://www.temp.com/","t=1; Max-Age=1000",true)==1);
    CHECK(webcookie_changed(a));
    long n0;unsigned char *file=save(a,NULL,0,&n0);CHECK(!webcookie_changed(a));
    /* The session cookie and the transient one stay in a's memory, not in the store. */
    CHECK(webcookie_load(b,file,(size_t)n0,now)==1);
    CHECK(sends(b,"https://example.com/","p"));CHECK(!sends(b,"https://example.com/","s"));CHECK(!sends(b,"https://www.temp.com/","t"));
    CHECK(sends(a,"https://example.com/","s"));CHECK(sends(a,"https://www.temp.com/","t"));
    /* Both change before seeing the other's change: neither change is lost. */
    CHECK(set(b,"https://example.com/","q=2; Max-Age=1000",true)==1);
    CHECK(set(a,"https://example.com/","r=3; Max-Age=1000",true)==1);
    long n1;unsigned char *file1=save(b,file,n0,&n1);free(file);
    long n2;unsigned char *file2=save(a,file1,n1,&n2);free(file1);
    CHECK(sends(a,"https://example.com/","p")&&sends(a,"https://example.com/","q")&&sends(a,"https://example.com/","r")&&sends(a,"https://example.com/","s"));
    CHECK(webcookie_load(b,file2,(size_t)n2,now)==1);
    CHECK(sends(b,"https://example.com/","p")&&sends(b,"https://example.com/","q")&&sends(b,"https://example.com/","r"));
    /* A deletion reaches the other process. */
    CHECK(set(a,"https://example.com/","p=; Max-Age=0",true)==1);
    long n3;unsigned char *file3=save(a,file2,n2,&n3);free(file2);
    CHECK(webcookie_load(b,file3,(size_t)n3,now)==1);CHECK(!sends(b,"https://example.com/","p"));CHECK(sends(b,"https://example.com/","q"));
    /* A clear reaches the other process's session cookies too, once. */
    CHECK(set(b,"https://www.example.com/","z=1; Domain=example.com",true)==1);
    CHECK(set(b,"https://other.org/","o=1",true)==1);
    CHECK(webcookie_clear(a,"example.com")>=2);
    long n4;unsigned char *file4=save(a,file3,n3,&n4);free(file3);
    CHECK(webcookie_load(b,file4,(size_t)n4,now)==1);
    CHECK(!sends(b,"https://www.example.com/","z"));CHECK(!sends(b,"https://example.com/","q"));CHECK(sends(b,"https://other.org/","o"));
    CHECK(set(b,"https://www.example.com/","z=2; Domain=example.com",true)==1);
    CHECK(webcookie_load(b,file4,(size_t)n4,now)==1);CHECK(sends(b,"https://www.example.com/","z"));
    /* Sites and their bytes: name, value, domain and path. */
    CHECK(set(b,"https://a.other.org/","x=1; Max-Age=1000",true)==1);
    struct webcookie_site *sites=NULL;long count=webcookie_sites(b,&sites,now);
    CHECK(count==2&&sites&&!strcmp(sites[0].site,"example.com")&&sites[0].cookies==1&&!strcmp(sites[1].site,"other.org")&&sites[1].cookies==2);
    CHECK(count==2&&sites&&sites[1].bytes==(uint64_t)(1+1+9+1)+(1+1+11+1));
    free(sites);
    /* The session's end: session and transient cookies go, persistent ones stay. */
    webcookie_session_end(a);
    CHECK(!sends(a,"https://www.temp.com/","t"));CHECK(!sends(a,"https://example.com/","s"));
    CHECK(set(a,"https://example.com/","k=1; Max-Age=1000",true)==1);
    webcookie_session_end(a);CHECK(sends(a,"https://example.com/","k"));
    /* A store that does not parse leaves the jar as it was. */
    unsigned char bad[48]={'N','C','S','1'};bad[16]=200;
    CHECK(webcookie_load(a,bad,sizeof bad,now)==0);CHECK(sends(a,"https://example.com/","k"));
    free(file4);webcookie_free(a);webcookie_free(b);
}
int main(int argc,char **argv) {
    webcookie_jar *j=jar();
    CHECK(set(j,"https://www.example.com/a/b?x=y","first=1",true)==1);
    get(j,"https://www.example.com/a/c",false,"first=1");get(j,"https://www.example.com/ab",true,"");
    get(j,"https://sub.www.example.com/a/c",true,"");
    CHECK(set(j,"https://www.example.com/a/b","shared=2; Domain=.EXAMPLE.COM; Path=/; Secure",true)==1);
    get(j,"https://sub.example.com/",true,"shared=2");get(j,"http://sub.example.com/",true,"");
    CHECK(set(j,"https://www.example.com/","bad=1; Domain=com",true)==0);
    CHECK(set(j,"https://evil.com/","bad=1; Domain=example.com",true)==0);
    CHECK(set(j,"https://badexample.com/","bad=1; Domain=example.com",true)==0);
    CHECK(set(j,"https://a.github.io/","bad=1; Domain=github.io",true)==0);
    CHECK(set(j,"https://a.example.co.uk/","bad=1; Domain=co.uk",true)==0);
    CHECK(set(j,"https://x.a.ck/","bad=1; Domain=a.ck",true)==0);
    CHECK(set(j,"https://x.www.ck/","good=1; Domain=www.ck",true)==1);
    CHECK(webcookie_same_site(j,"https://a.example.com/","https://b.example.com:444/"));
    CHECK(!webcookie_same_site(j,"https://a.example.com/","http://a.example.com/"));
    CHECK(!webcookie_same_site(j,"https://a.github.io/","https://b.github.io/"));
    CHECK(!webcookie_same_site(j,"https://x.a.ck/","https://y.b.ck/"));
    CHECK(webcookie_same_site(j,"https://x.www.ck/","https://y.www.ck/"));
    CHECK(!webcookie_same_site(j,"https://127.0.0.1/","https://0.0.1/"));
    CHECK(set(j,"http://127.0.0.1/","ip=1; Domain=0.0.1",true)==0);
    CHECK(set(j,"http://127.0.0.1/","ip=1; Domain=127.0.0.1",true)==1);
    CHECK(set(j,"http://example.com/","s=1; Secure",true)==0);
    CHECK(set(j,"https://example.com/","secret=a; HttpOnly; Secure; Path=/",true)==1);
    get(j,"https://example.com/",false,"shared=2");
    CHECK(set(j,"https://example.com/","secret=b; Path=/",false)==0);
    CHECK(set(j,"https://example.com/","secret=; Path=/; Max-Age=0",false)==0);
    CHECK(set(j,"https://example.com/","fake=a; HttpOnly",false)==0);
    CHECK(set(j,"http://example.com/","secret=b; Path=/",true)==0);
    get(j,"https://example.com/",true,"shared=2; secret=a");
    CHECK(set(j,"https://example.com/","__Secure-x=1",true)==0);
    CHECK(set(j,"https://example.com/","__sEcUrE-x=1",true)==0);
    CHECK(set(j,"http://example.com/","__Secure-x=1; Secure",true)==0);
    CHECK(set(j,"https://example.com/","__Host-x=1; Secure",true)==0);
    CHECK(set(j,"https://example.com/","__Host-x=1; Secure; Domain=example.com; Path=/",true)==0);
    CHECK(set(j,"https://example.com/","__Host-x=1; Secure; Path=/",true)==1);
    CHECK(set(j,"https://example.com/a/b","__Host-z=1; Secure; Path=/; Path=invalid",true)==0);
    CHECK(set(j,"https://example.com/","__Http-x=1; Secure",true)==0);
    CHECK(set(j,"https://example.com/","__Host-Http-x=1; Secure; HttpOnly; Path=/",false)==0);
    CHECK(set(j,"https://example.com/","__Host-Http-x=1; Secure; HttpOnly; Path=/",true)==1);
    CHECK(set(j,"https://example.com/","partitioned=1; Secure; Partitioned",true)==0);
    CHECK(set(j,"file:///C:/Users/User/index.html","x=1",false)==0);
    CHECK(set(j,"https://example.com@evil.com/","x=1",true)==0);
    CHECK(set(j,"https://example.com/","x=1\r\nCookie: secret",true)==0);
    CHECK(set(j,"https://example.com/","invalid name=1",true)==0);
    webcookie_free(j);

    j=jar();
    CHECK(set(j,"https://example.com/","lax=1; SameSite=Lax",true)==1);
    CHECK(set(j,"https://example.com/","strict=1; SameSite=Strict",true)==1);
    CHECK(set(j,"https://example.com/","none=1; SameSite=None; Secure",true)==1);
    CHECK(set(j,"https://example.com/","bad=1; SameSite=None",true)==0);
    struct webcookie_context c={"https://example.com/","https://elsewhere.org/","GET",false,true,false};char out[8192];
    CHECK(webcookie_get(j,&c,out,sizeof out,now)==6&&!strcmp(out,"none=1"));
    c.top_level=true;CHECK(webcookie_get(j,&c,out,sizeof out,now)==13&&!strcmp(out,"lax=1; none=1"));
    c.method="POST";CHECK(webcookie_get(j,&c,out,sizeof out,now)==6&&!strcmp(out,"none=1"));
    c.method="GET";c.site_url=NULL;CHECK(webcookie_get(j,&c,out,sizeof out,now)>0&&strstr(out,"strict=1"));
    c.redirect_cross_site=true;CHECK(webcookie_get(j,&c,out,sizeof out,now)>0&&!strstr(out,"strict=1"));
    c.top_level=false;c.site_url="https://elsewhere.org/";
    CHECK(webcookie_set(j,&c,"blocked=1",9,now)==0);
    c.top_level=true;CHECK(webcookie_set(j,&c,"navigate=1; SameSite=Strict",27,now)==1);
    c=context("https://example.com/",true);CHECK(webcookie_get(j,&c,out,2,now)==-1&&out[0]==0);
    webcookie_free(j);

    j=jar();
    CHECK(set(j,"https://example.com/","a=1; Max-Age=10; Expires=Sun, 06 Nov 1994 08:49:37 GMT",true)==1);
    CHECK(set(j,"https://example.com/","b=1; Max-Age=bad; Expires=Sun, 06 Nov 1994 08:49:37 GMT",true)==1);
    get(j,"https://example.com/",true,"a=1");now+=10;get(j,"https://example.com/",true,"");
    CHECK(set(j,"https://example.com/","a=2; Expires=Wed, 09 Jun 2038 10:18:14 GMT",true)==1);
    CHECK(set(j,"https://example.com/","b=2; Expires=Wednesday, 09-Jun-38 10:18:14 GMT",true)==1);
    CHECK(set(j,"https://example.com/","c=3; Max-Age=999999999999999999999999999999",true)==1);
    now+=400LL*86400;get(j,"https://example.com/",true,"");now=1700000000;
    CHECK(set(j,"https://example.com/","a=1",true)==1);CHECK(set(j,"https://example.com/","b=2",true)==1);
    CHECK(set(j,"https://example.com/","a=3",true)==1);get(j,"https://example.com/",true,"a=3; b=2");
    CHECK(set(j,"https://example.com/abc","a=4; Path=/abc",true)==1);get(j,"https://example.com/abc",true,"a=4; a=3; b=2");
    CHECK(set(j,"https://example.com/","secret=4; HttpOnly; Secure",true)==1);
    long n=webcookie_export(j,NULL,0,now);CHECK(n>0&&(size_t)n<=WEBCOOKIE_SNAPSHOT_MAX);void *snapshot=malloc((size_t)n);
    CHECK(webcookie_export(j,snapshot,(size_t)n,now)==n);
    webcookie_jar *other=jar();CHECK(webcookie_import(other,snapshot,(size_t)n,now));
    get(other,"https://example.com/",true,"a=3; b=2; secret=4");get(other,"https://example.com/",false,"a=3; b=2");
    CHECK(!webcookie_import(other,snapshot,(size_t)n,now));webcookie_free(other);
    other=jar();CHECK(!webcookie_import(other,snapshot,(size_t)n-1,now));get(other,"https://example.com/",true,"");
    ((unsigned char*)snapshot)[0]^=1;CHECK(!webcookie_import(other,snapshot,(size_t)n,now));free(snapshot);webcookie_free(other);webcookie_free(j);

    /* A navigation keeps the browser jar: unrelated Domain cookies must also
       survive its next worker snapshot. All fields here are synthetic. */
    j=jar();
    CHECK(set(j,"https://www.example.com/","host=1; Secure; HttpOnly",true)==1);
    CHECK(set(j,"https://www.example.com/","shared=2; Domain=example.com; Path=/; Secure",true)==1);
    CHECK(set(j,"https://www.other.org/","foreign=3; Domain=other.org; Path=/",true)==1);
    CHECK(set(j,"https://x.a.github.io/","private=4; Domain=a.github.io; Secure; SameSite=None",true)==1);
    CHECK(set(j,"https://x.www.ck/","exception=5; Domain=www.ck",true)==1);
    CHECK(set(j,"https://opaque.example.com/","opaque-token; Path=/",true)==1);
    CHECK(set(j,"https://binary.example.com/","bytes=\x80\xff; Path=/",true)==1);
    n=webcookie_export(j,NULL,0,now);snapshot=malloc((size_t)n);CHECK(snapshot!=NULL);
    if(snapshot){
        CHECK(webcookie_export(j,snapshot,(size_t)n,now)==n);
        other=jar();CHECK(webcookie_import(other,snapshot,(size_t)n,now));
        get(other,"https://www.example.com/",true,"host=1; shared=2");
        get(other,"https://www.example.com/",false,"shared=2");
        get(other,"https://sub.example.com/",true,"shared=2");
        get(other,"https://sub.other.org/",true,"foreign=3");
        get(other,"https://y.a.github.io/",true,"private=4");
        get(other,"https://y.www.ck/",true,"exception=5");
        get(other,"https://opaque.example.com/",true,"shared=2; opaque-token");
        get(other,"https://binary.example.com/",true,"shared=2; bytes=\x80\xff");
        get(other,"https://unrelated.net/",true,"");webcookie_free(other);
        /* Missing policy must not widen any imported Domain cookie. A failure
           is atomic even after an earlier host-only record was parsed. */
        other=webcookie_create();CHECK(other!=NULL);
        CHECK(!webcookie_import(other,snapshot,(size_t)n,now));
        get(other,"https://www.example.com/",true,"");
        CHECK(webcookie_psl_load(other,psl,sizeof psl-1));
        CHECK(webcookie_import(other,snapshot,(size_t)n,now));
        get(other,"https://sub.other.org/",true,"foreign=3");webcookie_free(other);
        free(snapshot);
    }
    webcookie_free(j);
    /* Host-only snapshots remain usable with the documented no-PSL fallback. */
    j=jar();CHECK(set(j,"https://single.example.com/","only=1",true)==1);
    n=webcookie_export(j,NULL,0,now);snapshot=malloc((size_t)n);CHECK(snapshot!=NULL);
    if(snapshot){
        CHECK(webcookie_export(j,snapshot,(size_t)n,now)==n);
        other=webcookie_create();CHECK(other!=NULL);CHECK(webcookie_import(other,snapshot,(size_t)n,now));
        get(other,"https://single.example.com/",true,"only=1");
        get(other,"https://sub.single.example.com/",true,"");
        webcookie_free(other);free(snapshot);
    }
    webcookie_free(j);

    j=webcookie_create();CHECK(set(j,"https://a.example.com/","no=1; Domain=example.com",true)==0);
    CHECK(set(j,"https://a.example.com/","yes=1; Domain=a.example.com",true)==1);
    get(j,"https://b.a.example.com/",true,"");CHECK(!webcookie_same_site(j,"https://a.example.com/","https://b.example.com/"));
    CHECK(!webcookie_psl_load(j,"com\n\xc3\xa9.com\n",11));webcookie_free(j);

    /* RFC 6265bis §5.7 per domain field: past WEBCOOKIE_DOMAIN_MAX the least recently used goes. */
    j=jar();for(int i=0;i<(int)WEBCOOKIE_DOMAIN_MAX+20;i++){char value[32];snprintf(value,sizeof value,"c%d=1",i);CHECK(set(j,"https://example.com/",value,true)==1);}
    c=context("https://example.com/",true);CHECK(webcookie_get(j,&c,out,sizeof out,now)>0&&!has(out,"c0")&&!has(out,"c19")&&has(out,"c20")&&has(out,"c199"));
    char huge[WEBCOOKIE_FIELD_MAX+2];memset(huge,'a',sizeof huge);huge[1]='=';huge[sizeof huge-1]=0;
    CHECK(webcookie_set(j,&c,huge,sizeof huge-1,now)==-1);webcookie_free(j);
    /* Large cookies are not evicted by their bytes: the snapshot of a jar always reaches a worker. */
    j=jar();char dense[4097];memset(dense,'a',sizeof dense-1);dense[0]='q';dense[1]='=';dense[sizeof dense-1]=0;
    for(int i=0;i<70;i++){
        char url[64];snprintf(url,sizeof url,"https://h%d.example.com/",i);
        CHECK(set(j,url,dense,true)==1);
    }
    n=webcookie_export(j,NULL,0,now);CHECK(n>0&&(size_t)n<=WEBCOOKIE_SNAPSHOT_MAX);
    snapshot=malloc((size_t)n);CHECK(snapshot!=NULL);
    if(snapshot){
        CHECK(webcookie_export(j,snapshot,(size_t)n,now)==n);
        other=jar();CHECK(webcookie_import(other,snapshot,(size_t)n,now));
        get(other,"https://h69.example.com/",true,dense);
        get(other,"https://h0.example.com/",true,dense);
        webcookie_free(other);free(snapshot);
    }
    webcookie_free(j);
    limits();
    rules();
    store();
    /* The deployed complete PSL, not merely the small policy-unit table. */
    FILE *f=fopen(argc>1?argv[1]:"C:\\Nocturne\\Web\\public_suffix_list.dat","r");
    CHECK(f!=NULL);
    if(f){
        char *text=malloc(WEBCOOKIE_PSL_MAX+1);CHECK(text!=NULL);
        if(text){size_t length=fread(text,1,WEBCOOKIE_PSL_MAX+1,f);j=webcookie_create();
            CHECK(length>100000&&length<=WEBCOOKIE_PSL_MAX&&webcookie_psl_load(j,text,length));
            CHECK(set(j,"https://a.blogspot.com/","bad=1; Domain=blogspot.com",true)==0);
            CHECK(set(j,"https://a.github.io/","bad=1; Domain=github.io",true)==0);
            CHECK(set(j,"https://x.co.uk/","bad=1; Domain=co.uk",true)==0);
            CHECK(set(j,"https://www.youtube.com/","good=1; Domain=youtube.com",true)==1);
            CHECK(webcookie_same_site(j,"https://www.youtube.com/","https://accounts.youtube.com/"));
            CHECK(!webcookie_same_site(j,"https://a.blogspot.com/","https://b.blogspot.com/"));
            CHECK(webcookie_same_site(j,"https://www.city.kawasaki.jp/","https://city.kawasaki.jp/"));
            CHECK(!webcookie_same_site(j,"https://a.kawasaki.jp/","https://b.kawasaki.jp/"));
            n=webcookie_export(j,NULL,0,now);snapshot=malloc((size_t)n);CHECK(snapshot!=NULL);
            if(snapshot){
                CHECK(webcookie_export(j,snapshot,(size_t)n,now)==n);
                other=webcookie_create();CHECK(other!=NULL);CHECK(webcookie_psl_load(other,text,length));
                CHECK(webcookie_import(other,snapshot,(size_t)n,now));
                get(other,"https://accounts.youtube.com/",true,"good=1");
                get(other,"https://html5test.com/",true,"");
                webcookie_free(other);free(snapshot);
            }
            webcookie_free(j);free(text);
        }fclose(f);
    }
    printf("cookietest: %d checks, %d failed\n",checks,failed);return failed!=0;
}
