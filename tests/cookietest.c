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
    CHECK(set(j,"file:///home/index.html","x=1",false)==0);
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

    j=jar();for(int i=0;i<70;i++){char value[32];snprintf(value,sizeof value,"c%d=1",i);CHECK(set(j,"https://example.com/",value,true)==1);}
    c=context("https://example.com/",true);CHECK(webcookie_get(j,&c,out,sizeof out,now)>0&&!strstr(out,"c0=")&&strstr(out,"c69="));
    char huge[WEBCOOKIE_FIELD_MAX+2];memset(huge,'a',sizeof huge);huge[1]='=';huge[sizeof huge-1]=0;
    CHECK(webcookie_set(j,&c,huge,sizeof huge-1,now)==-1);webcookie_free(j);
    /* Snapshot quota eviction must preserve export/import validity, not leave
       stale count/byte bookkeeping. Exercise a payload near the transport cap. */
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
        get(other,"https://h0.example.com/",true,"");
        webcookie_free(other);free(snapshot);
    }
    webcookie_free(j);
    /* The deployed complete PSL, not merely the small policy-unit table. */
    FILE *f=fopen(argc>1?argv[1]:"/usr/share/browser/public_suffix_list.dat","r");
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
