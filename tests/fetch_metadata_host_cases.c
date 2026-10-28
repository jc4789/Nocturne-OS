/* Fetch の「fetch」の段（Accept・Accept-Language）と Fetch Metadata（2.1〜2.4、3、4.1）の
   期待値は仕様の原文から決める。試す関数は check_fetch_metadata.py が webfetch.c から
   切り出した製品の本文で、ここでは呼ぶだけ。 */
#include <stdarg.h>
static unsigned checks,failed;
static void check(bool condition,const char *label){checks++;if(!condition){failed++;printf("失敗 %s\n",label);}}
static void checkf(bool condition,const char *format,...){
    char text[400];va_list a;va_start(a,format);vsnprintf(text,sizeof text,format,a);va_end(a);check(condition,text);
}
static void clear(struct job *j){
    free(j->document);free(j->headers);free(j->origin);free(j->target_origin);free(j->top_level_url);
    free(j->referrer);free(j->referrer_origin);free(j->final_url);free(j->outgoing.p);*j=(struct job){0};
}
/* 要求の wire の Accept-Language の欄（窓の側が fetch の始まりに決めた値）。request が job に
   写す。"" は欄なし。 */
static const char *wire_language="";
/* 要求を作る。document は要求を始めた文書の URL（"" は文書なし）。j->origin は製品の
   run_http と同じく http_url で導く（data: などは opaque で NULL のまま）。 */
static struct job request(unsigned kind,unsigned destination,unsigned fetch_flags,unsigned wire_flags,const char *document){
    struct job j={0};
    j.wire.kind=kind;j.wire.user_navigation=wire_flags;
    j.fetch.destination=destination;j.fetch.flags=fetch_flags;
    snprintf(j.accept_language,sizeof j.accept_language,"%s",wire_language);
    j.headers=strdup("");j.document=strdup(document);
    if(*document){
        enum http_url_result r=http_url(j.document,NULL,&j.origin);
        checkf(r==HTTP_URL_TUPLE||r==HTTP_URL_OPAQUE,"文書の URL %s を製品の正規化が読む",document);
    }
    return j;
}
/* 要求の URL の一覧へ一つ加える（最初の URL か、リダイレクトの先）。製品の run_http と
   同じく、正規化した URL とその origin で note_fetch_site を呼ぶ。 */
static void visit(struct job *j,const char *url){
    enum http_url_result r=http_url(url,&j->final_url,&j->target_origin);
    checkf(r==HTTP_URL_TUPLE,"要求の URL %s を製品の正規化が読む",url);
    if(r==HTTP_URL_TUPLE)note_fetch_site(j,j->final_url,j->target_origin);
}
/* 空にした緩衝へ append_fetch_metadata を呼び、出た頭の塊を返す。 */
static const char *metadata(struct job *j,const char *current){
    j->outgoing.length=0;if(j->outgoing.p)j->outgoing.p[0]=0;
    bool ok=append_fetch_metadata(j,current);
    checkf(ok,"append_fetch_metadata が %s で成功する",current);
    return j->outgoing.p?j->outgoing.p:"";
}
/* 製品の run_http の一段と同じく、visit の後に prepare_outgoing で頭を作る。 */
static const char *outgoing(struct job *j,const char *method){
    bool cross=!j->origin||strcmp(j->origin,j->target_origin)!=0;
    bool ok=prepare_outgoing(j,j->final_url,method,cross);
    checkf(ok,"prepare_outgoing が %s で成功する",j->final_url);
    return j->outgoing.p?j->outgoing.p:"";
}
/* 頭の塊の中の name の値（名前は大文字小文字を問わない）。無ければ NULL、二つ以上なら
   "<重複>"、CRLF で終わらない行があれば "<壊れた行>"。 */
static char found_value[1400];
static const char *field_value(const char *block,const char *name){
    size_t nl=strlen(name);unsigned count=0;
    for(const char *p=block;*p;){
        const char *end=strstr(p,"\r\n");
        if(!end)return "<壊れた行>";
        if((size_t)(end-p)>=nl+2&&!_strnicmp(p,name,nl)&&p[nl]==':'&&p[nl+1]==' '){
            snprintf(found_value,sizeof found_value,"%.*s",(int)(end-p-(ptrdiff_t)nl-2),p+nl+2);count++;
        }
        p=end+2;
    }
    return !count?NULL:count>1?"<重複>":found_value;
}
static bool field_is(const char *block,const char *name,const char *expected){
    const char *value=field_value(block,name);
    return expected?value&&!strcmp(value,expected):!value;
}
static bool has_fetch_metadata(const char *block){
    for(const char *p=block;*p;p++)if(!_strnicmp(p,"sec-fetch-",10))return true;
    return false;
}
/* Fetch：request の destination の一覧。"" の後は仕様の並び（RequestDestination と
   "serviceworker"・"webidentity"）。 */
static const char *const fetch_destinations[]={"","audio","audioworklet","document","embed","font","frame","iframe","image",
    "json","manifest","object","paintworklet","report","script","serviceworker","sharedworker","style","text","track",
    "video","webidentity","worker","xslt"};
/* Fetch の fetch の段：header list に Accept が無ければ、value は既定の全部の型の値で、
   destination ごとに最初に合う文があればそれ。 */
static const char *const document_accept="text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
static const char *spec_accept(const char *d){
    if(!strcmp(d,"document")||!strcmp(d,"frame")||!strcmp(d,"iframe"))return document_accept;
    if(!strcmp(d,"image"))return "image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
    if(!strcmp(d,"json"))return "application/json,*/*;q=0.5";
    if(!strcmp(d,"style"))return "text/css,*/*;q=0.1";
    if(!strcmp(d,"text"))return "text/plain,*/*;q=0.5";
    return "*/*";
}
/* Fetch：navigation request は destination が document・embed・frame・iframe・object の request。 */
static bool spec_navigation(const char *d){
    return !strcmp(d,"document")||!strcmp(d,"embed")||!strcmp(d,"frame")||!strcmp(d,"iframe")||!strcmp(d,"object");
}
/* 一つの要求の Accept-Language の頭。value は要求の wire の欄の値、prefill は先に緩衝にある頭。 */
static char language_header[2048];
static bool accept_language(const char *value,const char *prefill){
    struct job j={0};snprintf(j.accept_language,sizeof j.accept_language,"%s",value);
    bool ok=!prefill||header_text(&j,&j.outgoing,prefill);
    ok=ok&&append_accept_language(&j);
    snprintf(language_header,sizeof language_header,"%s",j.outgoing.p?j.outgoing.p:"");
    clear(&j);return ok;
}
/* 製品の run_one と同じ並びで、wire_request が作った bytes から Accept-Language の欄を読んで
   job に入れる：64 バイトの頭、referrer・最上位の文書・fetch の拡張の後に言語の拡張、URL・
   origin・referrer・最上位の文書の bytes の後に言語の bytes、その後に method・頭・本文・cookie。
   検査は製品と同じ（予約の欄は 0、長さは 1〜128、bytes は webnet_wire_language_valid）。
   run_one は channel から読むので切り出せず、ここは文書の並びの写し。全体の長さが合わなければ偽。 */
static size_t taken_language_at,taken_method_at;
static bool take_language(const unsigned char *tx,size_t len,struct job *j){
    struct webnet_wire_request h;if(!tx||len<sizeof h)return false;memcpy(&h,tx,sizeof h);
    size_t at=sizeof h;
    struct webnet_wire_referrer ref={0};struct webnet_wire_top_level top={0};struct webnet_wire_language lang={0};
    if(h.user_navigation&WEBNET_WIRE_REFERRER){memcpy(&ref,tx+at,sizeof ref);at+=sizeof ref;}
    if(h.user_navigation&WEBNET_WIRE_TOP_LEVEL){memcpy(&top,tx+at,sizeof top);at+=sizeof top;}
    if(h.user_navigation&WEBNET_WIRE_FETCH_METADATA)at+=sizeof(struct webnet_wire_fetch);
    if(h.user_navigation&WEBNET_WIRE_ACCEPT_LANGUAGE){
        memcpy(&lang,tx+at,sizeof lang);at+=sizeof lang;
        if(lang.reserved||!lang.length||lang.length>WEBNET_WIRE_ACCEPT_LANGUAGE_MAX)return false;
    }
    at+=(size_t)h.url_len+h.origin_len+ref.length+top.length;
    j->accept_language[0]=0;taken_language_at=at;
    if(lang.length){
        if(at+lang.length>len||!webnet_wire_language_valid((const char *)tx+at,lang.length))return false;
        memcpy(j->accept_language,tx+at,lang.length);j->accept_language[lang.length]=0;at+=lang.length;
    }
    taken_method_at=at;
    at+=(size_t)h.method_len+h.headers_len+h.body_len+h.cookie_len;
    return at==len;
}
/* 頭の塊の中に name の行がいくつあるか（名前は大文字小文字を問わない）。 */
static unsigned field_count(const char *block,const char *name){
    size_t nl=strlen(name);unsigned count=0;
    for(const char *p=block;p&&*p;){
        const char *end=strstr(p,"\r\n");if(!end)break;
        if((size_t)(end-p)>nl&&!_strnicmp(p,name,nl)&&p[nl]==':')count++;
        p=end+2;
    }
    return count;
}

/* 割り当ての失敗の注入：基準の回で数えた割り当てのそれぞれを一つずつ失敗させ、関数が
   false と誤りの文を返すこと、片付けの後に割り当てが残らないことを確かめる。 */
static unsigned fault_points;
static void fault_sweep(const char *name,void (*setup)(struct job *,int),bool (*run)(struct job *),int variants){
    for(int v=0;v<variants;v++){
        struct job j;int before=live_allocations;
        setup(&j,v);allocations=0;bool ok=run(&j);int needed=allocations;clear(&j);
        checkf(ok,"%s（変種 %d）の基準の回が成功する",name,v);
        for(int i=0;i<needed;i++){
            setup(&j,v);allocations=0;fail_allocation=i;ok=run(&j);fail_allocation=-1;fault_points++;
            checkf(!ok&&j.error[0],"%s（変種 %d）の %d 回目の割り当ての失敗は false と誤りの文になる",name,v,i);
            clear(&j);checkf(live_allocations==before,"%s（変種 %d）の %d 回目の割り当ての失敗の後に割り当てが残らない",name,v,i);
        }
    }
}
/* 欄に入る一番長い値（128 バイト）。 */
static char long_language[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX+1];
/* 先に置く頭の長さを変えて、Accept-Language の三つの断片のどこでも緩衝が伸びるようにする
   （緩衝の最初の大きさは 256、その次は 512）。 */
static const size_t language_prefills[]={0,40,100,112,120,200,236,240,250,255,256,300,360,366,380,480,494,496,500,510,511,512};
static void long_language_setup(struct job *j,int v){
    *j=(struct job){0};memcpy(j->accept_language,long_language,sizeof long_language);
    size_t n=language_prefills[v];if(!n)return;
    char fill[600];memset(fill,'c',n);memcpy(fill,"X-Fill: ",8);memcpy(fill+n-2,"\r\n",2);fill[n]=0;
    check(header_text(j,&j->outgoing,fill),"埋め草の頭を置ける");
}
static bool long_language_run(struct job *j){return append_accept_language(j);}
/* 先に v+156 バイトの頭を置き、Sec-Fetch-* の連なりのどの断片でも緩衝が伸びるようにする。 */
static void metadata_setup(struct job *j,int v){
    *j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION|WEBNET_FETCH_BROWSER_UI,WEBNET_WIRE_USER_NAVIGATION,"");
    visit(j,"https://example.test/");
    char fill[300];size_t n=(size_t)v+156;memset(fill,'a',n);memcpy(fill,"X-Fill: ",8);memcpy(fill+n-2,"\r\n",2);fill[n]=0;
    check(header_text(j,&j->outgoing,fill),"埋め草の頭を置ける");
}
static bool metadata_run(struct job *j){return append_fetch_metadata(j,j->final_url);}
/* 作者の頭の長さを変えて、prepare_outgoing の全部の段で緩衝が伸びるようにする。 */
static void outgoing_setup(struct job *j,int v){
    *j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"https://example.test/");
    memcpy(j->accept_language,long_language,sizeof long_language);
    char author[400];size_t n=(size_t)v*16+12;memset(author,'b',n);memcpy(author,"X-Author: ",10);memcpy(author+n-2,"\r\n",2);author[n]=0;
    free(j->headers);j->headers=strdup(author);
    visit(j,"https://other.test/");
}
static bool outgoing_run(struct job *j){return prepare_outgoing(j,j->final_url,"GET",true);}

int main(void){
    const unsigned destination_count=sizeof fetch_destinations/sizeof *fetch_destinations;
    /* 製品の destination の番号と名前が Fetch の一覧と同じであること。 */
    check(destination_count==WEBNET_DEST_COUNT,"destination の数が Fetch の一覧と同じ");
    for(unsigned d=0;d<destination_count&&d<WEBNET_DEST_COUNT;d++)
        checkf(!strcmp(webnet_destination_name((enum webnet_destination)d),fetch_destinations[d]),"destination %u の名前が Fetch の \"%s\"",d,fetch_destinations[d]);

    /* Fetch の fetch の段：destination ごとの Accept。navigation request の定義。 */
    for(unsigned d=0;d<WEBNET_DEST_COUNT;d++){
        const char *name=fetch_destinations[d];
        struct job j=request(WEBNET_RESOURCE,d,0,0,"https://example.test/");
        checkf(!strcmp(accept_value(&j),spec_accept(name)),"destination \"%s\" の既定の Accept は %s",name,spec_accept(name));
        checkf(navigation_request(&j)==spec_navigation(name),"destination \"%s\" が navigation request か（Fetch の定義）",name);
        clear(&j);
    }

    /* Accept-Language（Fetch の fetch の段 14）：webfetch は要求の wire の欄の値をそのまま一行にし、
       欄が無ければ頭を出さない。値を作る規則（q の重み）は webprefs の持ち物で、ここでは試さない。 */
    memset(long_language,'a',WEBNET_WIRE_ACCEPT_LANGUAGE_MAX);long_language[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX]=0;
    unsigned calls=prefs_calls;
    check(accept_language("ja",NULL)&&!strcmp(language_header,"Accept-Language: ja\r\n"),"欄の値を一行にする");
    check(accept_language("ja,en-US;q=0.9,en;q=0.8",NULL)&&!strcmp(language_header,"Accept-Language: ja,en-US;q=0.9,en;q=0.8\r\n"),
          "重みの付いた値も手を加えずに出す");
    check(accept_language("",NULL)&&!language_header[0],"欄が無ければ Accept-Language を出さない");
    check(accept_language("ja","Accept: */*\r\n")&&!strcmp(language_header,"Accept: */*\r\nAccept-Language: ja\r\n"),"前の頭を残して後ろへ足す");
    check(accept_language(long_language,NULL)&&strlen(language_header)==17+WEBNET_WIRE_ACCEPT_LANGUAGE_MAX+2,"128 バイトの値をそのまま出す");
    check(prefs_calls==calls,"webfetch は利用者の設定を読まない（webprefs を呼ばない）");

    /* 欄の値の検査（webnet_wire.h、窓の側と webfetch の両方が使う）：Fetch の CORS-safelisted
       request-header の Accept-Language の bytes（128 バイト以下）のうち [0-9A-Za-z,;=.-] だけ。
       頭を終わらせる bytes や別の頭を足す bytes は通さない。 */
    static const struct {const char *value;size_t length;bool valid;const char *label;} language_values[]={
        {"ja",2,true,"一つの tag"},
        {"ja,en-US;q=0.9,en;q=0.8",23,true,"重みの付いた一覧"},
        {"zh-Hant-TW;q=0.1",16,true,"三つの subtag"},
        {"",0,false,"空の値"},
        {"ja\r\nX-Evil: 1",14,false,"CR LF で頭を足す値"},
        {"ja\nX: 1",7,false,"LF だけ"},
        {"ja\r",3,false,"CR だけ"},
        {"ja\0en",5,false,"途中の NUL"},
        {"ja en",5,false,"空白（safelisted だが欄には入れない）"},
        {"*",1,false,"* （safelisted だが欄には入れない）"},
        {"ja:1",4,false,"コロン"},
        {"ja\"",3,false,"引用符"},
        {"ja\t",3,false,"タブ"},
        {"\xe6\x97\xa5\xe6\x9c\xac",6,false,"ASCII でない bytes"},
        {"ja_JP",5,false,"下線"},
    };
    for(size_t i=0;i<sizeof language_values/sizeof *language_values;i++)
        checkf(webnet_wire_language_valid(language_values[i].value,language_values[i].length)==language_values[i].valid,
               "欄の値の検査：%s は%s",language_values[i].label,language_values[i].valid?"通す":"拒む");
    check(webnet_wire_language_valid(long_language,WEBNET_WIRE_ACCEPT_LANGUAGE_MAX),"128 バイトは通す");
    char over[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX+2];memset(over,'a',sizeof over);
    check(!webnet_wire_language_valid(over,WEBNET_WIRE_ACCEPT_LANGUAGE_MAX+1),"129 バイトは拒む（CORS-safelisted の 128 バイト）");
    check(WEBPREFS_ACCEPT_LANGUAGE_MAX==WEBNET_WIRE_ACCEPT_LANGUAGE_MAX,"webprefs の上限と wire の欄の上限が同じ");

    /* 窓の側の request_language（webnet.c）：fetch の始まりに webprefs から一度だけ読み、壊れた値や
       失敗は欄なしにする（要求は続ける）。 */
    char chosen[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX+1];
    prefs_value="ja,en-US;q=0.9";prefs_result=0;calls=prefs_calls;request_language(chosen);
    check(!strcmp(chosen,"ja,en-US;q=0.9")&&prefs_calls==calls+1,"request_language は webprefs の値を一度だけ読む");
    prefs_value=long_language;request_language(chosen);
    check(!strcmp(chosen,long_language),"128 バイトの値を受ける");
    prefs_result=-N_ERR_TOO_LARGE;request_language(chosen);
    check(!chosen[0],"webprefs の失敗は欄なし");
    prefs_result=0;prefs_value="ja\r\nX-Evil: 1";request_language(chosen);
    check(!chosen[0],"webprefs が壊れた bytes を返しても欄に入れない");
    prefs_value="";request_language(chosen);
    check(!chosen[0],"空の値は欄なし");

    /* wire_request（webnet.c）：欄は fetch の拡張の後、bytes は最上位の文書の URL の後。要求の
       始まりの値が wire に固定され、後で設定が変わっても同じ要求の bytes は変わらない。 */
    {
        struct webnet net={0};
        struct webnet_request q={.kind=WEBNET_FETCH,.url="https://example.test/data",.origin="https://example.test/page",
            .method="GET",.headers="X-A: 1\r\n",.credentials=WEBNET_CREDENTIALS_OMIT,.referrer="https://example.test/page",
            .referrer_policy=WEBNET_REFERRER_STRICT_ORIGIN_CROSS,.top_level_url="https://example.test/",.destination=WEBNET_DEST_EMPTY};
        prefs_value="fr-CA,ja;q=0.9";request_language(chosen);
        size_t len=0;unsigned char *tx=wire_request(&net,&q,7,99,0,chosen,&len);
        struct webnet_wire_request h={0};if(tx)memcpy(&h,tx,sizeof h);
        check(tx&&(h.user_navigation&WEBNET_WIRE_ACCEPT_LANGUAGE)&&(h.user_navigation&WEBNET_WIRE_FETCH_METADATA)&&
              (h.user_navigation&WEBNET_WIRE_REFERRER)&&(h.user_navigation&WEBNET_WIRE_TOP_LEVEL),"wire に Accept-Language の印");
        check(tx&&!(h.user_navigation&~WEBNET_WIRE_REQUEST_FLAGS),"印は webfetch が受ける印の中");
        struct webnet_wire_language lang={0};
        if(tx)memcpy(&lang,tx+sizeof h+sizeof(struct webnet_wire_referrer)+sizeof(struct webnet_wire_top_level)+sizeof(struct webnet_wire_fetch),sizeof lang);
        check(lang.length==14&&lang.reserved==0,"言語の拡張は fetch の拡張の直後で、長さと予約の 0");
        struct job j={0};
        check(take_language(tx,len,&j),"文書どおりの並びで全体の長さが合う");
        check(!strcmp(j.accept_language,"fr-CA,ja;q=0.9"),"欄の bytes は始まりの値");
        check(tx&&!memcmp(tx+taken_language_at-strlen(q.top_level_url),q.top_level_url,strlen(q.top_level_url)),"言語の bytes の前は最上位の文書の URL");
        check(tx&&!memcmp(tx+taken_method_at,"GET",3),"言語の bytes の後は method");
        prefs_value="de";
        check(!strcmp(j.accept_language,"fr-CA,ja;q=0.9")&&tx&&!memcmp(tx+taken_language_at,"fr-CA,ja;q=0.9",14),
              "要求の後に設定が変わっても、その要求の値は始まりのまま");
        free(tx);clear(&j);
        size_t bare=0;unsigned char *plain=wire_request(&net,&q,8,99,0,"",&bare);
        struct webnet_wire_request ph={0};if(plain)memcpy(&ph,plain,sizeof ph);
        check(plain&&!(ph.user_navigation&WEBNET_WIRE_ACCEPT_LANGUAGE)&&bare+sizeof(struct webnet_wire_language)+14==len,
              "値が無ければ印も拡張も bytes も無い");
        check(take_language(plain,bare,&j)&&!j.accept_language[0],"欄なしの wire は Accept-Language なし");
        free(plain);clear(&j);
        size_t bad_len=0;
        check(!wire_request(&net,&q,9,99,0,"ja\r\nX-Evil: 1",&bad_len),"wire_request は不正な値の要求を作らない");
        char too_long_value[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX+2];memset(too_long_value,'a',sizeof too_long_value-1);too_long_value[sizeof too_long_value-1]=0;
        check(!wire_request(&net,&q,10,99,0,too_long_value,&bad_len),"wire_request は 129 バイトの値の要求を作らない");
        /* 二つの要求の間に設定が変わる：それぞれが始まりの値を持つ。 */
        prefs_value="ja";request_language(chosen);size_t la=0;unsigned char *a=wire_request(&net,&q,11,99,0,chosen,&la);
        prefs_value="en-US";request_language(chosen);size_t lb=0;unsigned char *b2=wire_request(&net,&q,12,99,0,chosen,&lb);
        struct job ja={0},jb={0};
        check(take_language(a,la,&ja)&&take_language(b2,lb,&jb)&&!strcmp(ja.accept_language,"ja")&&!strcmp(jb.accept_language,"en-US"),
              "設定の変わる前と後に始まった要求は、それぞれの始まりの値");
        free(a);free(b2);clear(&ja);clear(&jb);
        /* WebSocket の握手の要求にも付く（WebSockets の握手は fetch）。 */
        struct webnet_request ws={.kind=WEBNET_WEBSOCKET,.url="wss://www.example.test/socket",.origin="https://example.test/",
            .headers="Sec-WebSocket-Protocol: chat\r\n",.credentials=WEBNET_CREDENTIALS_OMIT,.top_level_url="https://example.test/"};
        prefs_value="ja,en;q=0.9";request_language(chosen);size_t wl=0;unsigned char *wt=wire_request(&net,&ws,13,99,0,chosen,&wl);
        struct job jw={0};
        check(take_language(wt,wl,&jw)&&!strcmp(jw.accept_language,"ja,en;q=0.9"),"WebSocket の握手の wire にも Accept-Language の欄");
        free(wt);clear(&jw);
        /* 不正な bytes の wire は webfetch の側の検査で拒む。 */
        prefs_value="ja";request_language(chosen);size_t tl=0;unsigned char *tampered=wire_request(&net,&q,14,99,0,chosen,&tl);
        struct job jt={0};
        if(tampered&&take_language(tampered,tl,&jt))tampered[taken_language_at]='\n';
        check(tampered&&!take_language(tampered,tl,&jt),"bytes を壊した wire は検査で拒む");
        if(tampered){struct webnet_wire_language bad={200,0};
            memcpy(tampered+sizeof h+sizeof(struct webnet_wire_referrer)+sizeof(struct webnet_wire_top_level)+sizeof(struct webnet_wire_fetch),&bad,sizeof bad);}
        check(tampered&&!take_language(tampered,tl,&jt),"128 バイトを超える長さの欄は拒む");
        if(tampered){struct webnet_wire_language bad={2,1};
            memcpy(tampered+sizeof h+sizeof(struct webnet_wire_referrer)+sizeof(struct webnet_wire_top_level)+sizeof(struct webnet_wire_fetch),&bad,sizeof bad);}
        check(tampered&&!take_language(tampered,tl,&jt),"予約の欄が 0 でなければ拒む");
        free(tampered);clear(&jt);
    }

    /* Fetch Metadata 2.1：Sec-Fetch-Dest は destination、空の文字列なら "empty"。 */
    for(unsigned d=0;d<WEBNET_DEST_COUNT;d++){
        const char *name=fetch_destinations[d];
        bool frame=!strcmp(name,"document")||!strcmp(name,"frame")||!strcmp(name,"iframe");
        struct job j=request(frame?WEBNET_NAVIGATION:WEBNET_RESOURCE,d,0,0,"https://example.test/");
        visit(&j,"https://example.test/r");
        const char *b=metadata(&j,j.final_url);
        checkf(field_is(b,"Sec-Fetch-Dest",*name?name:"empty"),"Sec-Fetch-Dest は destination \"%s\" を%s",name,*name?"そのまま出す":"\"empty\" にする");
        clear(&j);
    }

    /* Fetch Metadata 2.2：Sec-Fetch-Mode は request の mode。各要求の mode は HTML・Fetch・
       WebSockets・CSP の定義（WPT fetch/metadata の期待値と同じ）。 */
    static const struct {unsigned kind,destination,wire;const char *mode,*label;} modes[]={
        {WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_WIRE_USER_NAVIGATION,"navigate","最上位の文書の遷移は navigate"},
        {WEBNET_NAVIGATION,WEBNET_DEST_IFRAME,0,"navigate","iframe の遷移は navigate"},
        {WEBNET_NAVIGATION,WEBNET_DEST_FRAME,0,"navigate","frame の遷移は navigate"},
        {WEBNET_RESOURCE,WEBNET_DEST_EMBED,0,"navigate","embed は navigate（WPT embed.https.sub.tentative.html）"},
        {WEBNET_RESOURCE,WEBNET_DEST_OBJECT,0,"navigate","object は navigate（WPT object.https.sub.html）"},
        {WEBNET_FETCH,WEBNET_DEST_EMPTY,0,"cors","fetch() の既定の mode は cors"},
        {WEBNET_FETCH,WEBNET_DEST_EMPTY,WEBNET_WIRE_NO_CORS,"no-cors","fetch() の mode no-cors"},
        {WEBNET_FETCH,WEBNET_DEST_EMPTY,WEBNET_WIRE_SAME_ORIGIN,"same-origin","fetch() の mode same-origin"},
        {WEBNET_FETCH,WEBNET_DEST_WORKER,WEBNET_WIRE_SAME_ORIGIN,"same-origin","Worker の最上位の脚本は same-origin（WPT worker.https.sub.html）"},
        {WEBNET_FETCH,WEBNET_DEST_FONT,0,"cors","字体は cors（WPT css-font-face）"},
        {WEBNET_MODULE,WEBNET_DEST_SCRIPT,0,"cors","module script は cors"},
        {WEBNET_CLASSIC,WEBNET_DEST_SCRIPT,0,"no-cors","crossorigin の無い classic script は no-cors"},
        {WEBNET_RESOURCE,WEBNET_DEST_IMAGE,0,"no-cors","img は no-cors"},
        {WEBNET_RESOURCE,WEBNET_DEST_STYLE,0,"no-cors","crossorigin の無い stylesheet は no-cors"},
        {WEBNET_REPORT,WEBNET_DEST_REPORT,WEBNET_WIRE_REDIRECT_ERROR,"no-cors","CSP の報告は no-cors（WPT report.https.sub.html）"},
        {WEBNET_WEBSOCKET,WEBNET_DEST_EMPTY,0,"websocket","WebSocket の握手は websocket"},
    };
    for(size_t i=0;i<sizeof modes/sizeof *modes;i++){
        struct job j=request(modes[i].kind,modes[i].destination,0,modes[i].wire,"https://example.test/");
        visit(&j,"https://example.test/r");
        const char *b=metadata(&j,j.final_url);
        checkf(field_is(b,"Sec-Fetch-Mode",modes[i].mode),"Sec-Fetch-Mode：%s",modes[i].label);
        if(modes[i].kind==WEBNET_WEBSOCKET)check(field_is(b,"Sec-Fetch-Dest","empty"),"WebSocket の握手の destination は空なので empty");
        clear(&j);
    }

    /* Fetch Metadata 2.3：Sec-Fetch-Site は same-origin から始め、browser UI の遷移なら none。
       そうでなければ URL の一覧の各 URL について、request の origin と同じ origin なら次へ、
       そうでなければ cross-site にし、same site でなければ止め、same site なら same-site。 */
    static const struct {const char *document;unsigned kind,destination,flags,wire;const char *urls[5],*sites[5],*label;} sites[]={
        {"https://example.test/page",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://example.test/data"},{"same-origin"},"同じ origin は same-origin"},
        {"https://example.test:443/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://EXAMPLE.test/data"},{"same-origin"},"既定の port と大文字の host は同じ origin（URL の正規化）"},
        {"https://example.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://example.test:8443/data"},{"same-site"},"port だけ違う origin は same-site（same site は port を問わない）"},
        {"https://example.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://example.test./data"},{"cross-site"},"末尾の点の付いた同じ host は cross-site（WPT trailing-dot.https.sub.any.js）"},
        {"https://example.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://www.example.test./data"},{"cross-site"},"末尾の点の付いた same site の host も cross-site（WPT trailing-dot.https.sub.any.js）"},
        {"https://example.test/",WEBNET_RESOURCE,WEBNET_DEST_IMAGE,0,0,{"https://www.example.test/a.png"},{"same-site"},"同じ登録可能ドメインの別の host は same-site"},
        {"https://example.test/",WEBNET_RESOURCE,WEBNET_DEST_IMAGE,0,0,{"https://other.test/a.png"},{"cross-site"},"別の登録可能ドメインは cross-site"},
        {"https://alice.github.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://bob.github.test/"},{"cross-site"},"公開接尾辞の下の別の登録可能ドメインは cross-site"},
        {"http://example.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://example.test/"},{"cross-site"},"scheme の違う origin は same site でないので cross-site"},
        {"data:text/html,x",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://example.test/"},{"cross-site"},"opaque origin の要求は cross-site"},
        {"https://example.test/",WEBNET_NAVIGATION,WEBNET_DEST_IFRAME,0,0,
            {"https://example.test/r1","https://www.example.test/r2","https://other.test/r3","https://example.test/r4"},
            {"same-origin","same-site","cross-site","cross-site"},"リダイレクトの各段：一度 cross-site になれば元の origin へ戻っても cross-site（4.1 の例）"},
        {"https://example.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://www.example.test/r1","https://example.test/r2"},{"same-site","same-site"},
            "same-site の後に元の origin へ戻っても same-site"},
        {"https://example.test/",WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,{"https://example.test/a","http://example.test/b","https://example.test/c"},
            {"same-origin",NULL,"cross-site"},"信頼できない段も URL の一覧に入り、後の段を cross-site にする"},
        {"",WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_BROWSER_UI|WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,
            {"https://example.test/short","https://other.test/long"},{"none","none"},"browser UI で始めた遷移は none で、リダイレクトの後も none（4.1 の注）"},
        {"https://other.test/page",WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_BROWSER_UI|WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,
            {"https://example.test/"},{"none"},"文書を開いたまま address bar から遷移しても none"},
        {"https://example.test/",WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,
            {"https://other.test/"},{"cross-site"},"頁の中のリンクを押した遷移は none でなく関係のとおり（4.3）"},
        {"https://example.test/",WEBNET_WEBSOCKET,WEBNET_DEST_EMPTY,0,0,{"https://www.example.test/socket"},{"same-site"},
            "WebSocket の握手も文書の origin と URL の関係"},
    };
    for(size_t i=0;i<sizeof sites/sizeof *sites;i++){
        struct job j=request(sites[i].kind,sites[i].destination,sites[i].flags,sites[i].wire,sites[i].document);
        for(unsigned hop=0;hop<5&&sites[i].urls[hop];hop++){
            visit(&j,sites[i].urls[hop]);
            const char *b=metadata(&j,j.final_url);
            if(sites[i].sites[hop])checkf(field_is(b,"Sec-Fetch-Site",sites[i].sites[hop]),"Sec-Fetch-Site（%u 段目 %s）：%s",hop+1,sites[i].sites[hop],sites[i].label);
            else checkf(!has_fetch_metadata(b),"Sec-Fetch-*（%u 段目）：信頼できない URL には付けない",hop+1);
        }
        clear(&j);
    }

    /* Fetch Metadata 2.4：navigation request で user-activation が真のときだけ ?1。それ以外は出さない。 */
    static const struct {unsigned kind,destination,flags,wire;bool user;const char *label;} users[]={
        {WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,true,"利用者の起動のある最上位の遷移は ?1"},
        {WEBNET_NAVIGATION,WEBNET_DEST_IFRAME,WEBNET_FETCH_USER_ACTIVATION,0,true,"利用者の起動のある iframe の遷移は ?1"},
        {WEBNET_NAVIGATION,WEBNET_DEST_FRAME,WEBNET_FETCH_USER_ACTIVATION,0,true,"利用者の起動のある frame の遷移は ?1"},
        {WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION|WEBNET_FETCH_BROWSER_UI,WEBNET_WIRE_USER_NAVIGATION,true,"browser UI からの起動のある遷移は ?1"},
        {WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,0,WEBNET_WIRE_USER_NAVIGATION,false,"起動の無い遷移には出さない"},
        {WEBNET_NAVIGATION,WEBNET_DEST_IFRAME,0,0,false,"起動の無い iframe の遷移には出さない"},
        {WEBNET_RESOURCE,WEBNET_DEST_EMBED,0,0,false,"起動の無い embed には出さない（WPT embed の user は空）"},
        {WEBNET_RESOURCE,WEBNET_DEST_OBJECT,0,0,false,"起動の無い object には出さない"},
        {WEBNET_FETCH,WEBNET_DEST_EMPTY,WEBNET_FETCH_USER_ACTIVATION,0,false,"navigation request でない fetch() には起動があっても出さない"},
        {WEBNET_RESOURCE,WEBNET_DEST_IMAGE,WEBNET_FETCH_USER_ACTIVATION,0,false,"navigation request でない img には起動があっても出さない"},
        {WEBNET_CLASSIC,WEBNET_DEST_SCRIPT,WEBNET_FETCH_USER_ACTIVATION,0,false,"navigation request でない script には起動があっても出さない"},
    };
    for(size_t i=0;i<sizeof users/sizeof *users;i++){
        struct job j=request(users[i].kind,users[i].destination,users[i].flags,users[i].wire,"https://example.test/");
        visit(&j,"https://example.test/next");
        const char *b=metadata(&j,j.final_url);
        checkf(field_is(b,"Sec-Fetch-User",users[i].user?"?1":NULL),"Sec-Fetch-User：%s",users[i].label);
        clear(&j);
    }

    /* Fetch Metadata 3：current URL が potentially trustworthy URL でなければ何も足さない。 */
    static const char *const untrusted[]={"http://example.test/","http://127.evil.test/","http://10.0.0.1/","http://[::2]/"};
    static const char *const trusted[]={"https://example.test/","http://localhost:8080/","http://127.0.0.1/","http://127.255.255.255:80/","http://[::1]:8080/"};
    for(size_t i=0;i<sizeof untrusted/sizeof *untrusted;i++){
        struct job j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"https://example.test/");
        bool ok=header_text(&j,&j.outgoing,"X-Before: 1\r\n")&&append_fetch_metadata(&j,untrusted[i]);
        checkf(ok&&!strcmp(j.outgoing.p,"X-Before: 1\r\n"),"信頼できない %s には Sec-Fetch-* を一つも付けない",untrusted[i]);
        clear(&j);
    }
    for(size_t i=0;i<sizeof trusted/sizeof *trusted;i++){
        struct job j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"https://example.test/");
        bool ok=header_text(&j,&j.outgoing,"X-Before: 1\r\n")&&append_fetch_metadata(&j,trusted[i]);
        checkf(ok&&!strncmp(j.outgoing.p,"X-Before: 1\r\n",13)&&field_is(j.outgoing.p,"Sec-Fetch-Dest","document")&&
               field_is(j.outgoing.p,"Sec-Fetch-Mode","navigate")&&field_is(j.outgoing.p,"Sec-Fetch-User","?1"),
               "potentially trustworthy な %s には Sec-Fetch-* を付ける",trusted[i]);
        clear(&j);
    }

    /* Fetch Metadata 1.1 の例：頭の塊をそのまま比べる。 */
    struct job j=request(WEBNET_RESOURCE,WEBNET_DEST_IMAGE,0,0,"https://example.test/");visit(&j,"https://other.test/a.png");
    check(!strcmp(metadata(&j,j.final_url),"Sec-Fetch-Dest: image\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Site: cross-site\r\n"),"1.1 の例：別の site の picture の画像");
    clear(&j);
    j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"https://example.test/");visit(&j,"https://example.test/");
    check(!strcmp(metadata(&j,j.final_url),"Sec-Fetch-Dest: document\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Site: same-origin\r\nSec-Fetch-User: ?1\r\n"),
          "1.1 の例：頁の中のリンクを押した同じ origin の最上位の遷移");
    clear(&j);

    /* prepare_outgoing の一段：Fetch の fetch の段の Accept・Accept-Language は header list に
       無いときだけ足し（名前は大文字小文字を問わない）、Fetch Metadata は HTTP-network-or-cache の段で足す。 */
    wire_language="ja,en-US;q=0.9";
    j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_BROWSER_UI|WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"");
    visit(&j,"https://example.test/");
    const char *b=outgoing(&j,"GET");
    check(field_is(b,"Accept",document_accept),"address bar の遷移：Accept は文書の値");
    check(field_is(b,"Accept-Language","ja,en-US;q=0.9"),"address bar の遷移：Accept-Language は要求の欄の値（利用者の言語）");
    check(field_is(b,"Upgrade-Insecure-Requests","1"),"address bar の遷移：Upgrade-Insecure-Requests: 1");
    check(field_is(b,"Origin",NULL),"address bar の遷移：GET の遷移に Origin は無い");
    check(field_is(b,"Sec-Fetch-Dest","document")&&field_is(b,"Sec-Fetch-Mode","navigate")&&field_is(b,"Sec-Fetch-Site","none")&&field_is(b,"Sec-Fetch-User","?1"),
          "address bar の遷移：document・navigate・none・?1");
    clear(&j);
    j=request(WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page");visit(&j,"https://example.test/data");b=outgoing(&j,"GET");
    check(field_is(b,"Accept","*/*"),"fetch()：Accept は全部の型（WPT accept-header.any.js）");
    check(field_value(b,"Accept-Language")!=NULL&&strcmp(field_value(b,"Accept-Language"),"<重複>"),"fetch()：Accept-Language が一つある（WPT accept-header.any.js）");
    check(field_is(b,"Upgrade-Insecure-Requests",NULL),"fetch()：navigation request でないので Upgrade-Insecure-Requests は無い");
    check(field_is(b,"Origin","https://example.test"),"fetch()：CORS の要求に Origin");
    check(field_is(b,"Sec-Fetch-Dest","empty")&&field_is(b,"Sec-Fetch-Mode","cors")&&field_is(b,"Sec-Fetch-Site","same-origin")&&field_is(b,"Sec-Fetch-User",NULL),
          "fetch()：empty・cors・same-origin・User なし");
    clear(&j);
    j=request(WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page");free(j.headers);j.headers=strdup("Accept: custom/*\r\n");
    visit(&j,"https://example.test/data");b=outgoing(&j,"GET");
    check(field_is(b,"Accept","custom/*"),"作者の Accept があれば既定の値を足さない（WPT accept-header.any.js）");
    check(field_value(b,"Accept-Language")!=NULL,"作者の Accept は Accept-Language を消さない");
    clear(&j);
    j=request(WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page");free(j.headers);j.headers=strdup("accept-language: bzh\r\n");
    visit(&j,"https://example.test/data");b=outgoing(&j,"GET");
    check(field_is(b,"Accept-Language","bzh"),"作者の Accept-Language（小文字の名前）があれば利用者の言語を足さない（WPT accept-header.any.js）");
    check(field_is(b,"Accept","*/*"),"作者の Accept-Language は Accept を消さない");
    clear(&j);
    j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"https://example.test/");
    visit(&j,"http://example.test/plain");b=outgoing(&j,"GET");
    check(field_is(b,"Accept",document_accept)&&field_is(b,"Upgrade-Insecure-Requests","1"),"http の遷移にも Accept と Upgrade-Insecure-Requests は付く");
    check(!has_fetch_metadata(b),"http の遷移には Sec-Fetch-* を付けない");
    clear(&j);
    wire_language="";
    j=request(WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page");visit(&j,"https://example.test/data");b=outgoing(&j,"GET");
    check(field_is(b,"Accept-Language",NULL),"要求に欄が無ければ Accept-Language の頭を出さない");
    clear(&j);
    wire_language="ja";
    j=request(WEBNET_NAVIGATION,WEBNET_DEST_IFRAME,0,0,"https://example.test/");
    visit(&j,"https://example.test/frame");b=outgoing(&j,"GET");
    check(field_is(b,"Sec-Fetch-Site","same-origin")&&field_is(b,"Sec-Fetch-Dest","iframe"),"iframe の一段目は same-origin");
    visit(&j,"https://other.test/frame");b=outgoing(&j,"GET");
    check(field_is(b,"Sec-Fetch-Site","cross-site"),"iframe のリダイレクトの段は cross-site");
    check(field_is(b,"Accept",document_accept)&&field_is(b,"Accept-Language","ja")&&field_is(b,"Upgrade-Insecure-Requests","1")&&
          field_is(b,"Sec-Fetch-Dest","iframe")&&field_is(b,"Sec-Fetch-Mode","navigate"),"リダイレクトの段で作り直しても各頭は一つずつ");
    clear(&j);
    j=request(WEBNET_RESOURCE,WEBNET_DEST_IMAGE,0,0,"https://example.test/");visit(&j,"https://other.test/a.png");b=outgoing(&j,"GET");
    check(field_is(b,"Accept","image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5")&&field_is(b,"Upgrade-Insecure-Requests",NULL)&&
          field_is(b,"Sec-Fetch-Dest","image")&&field_is(b,"Sec-Fetch-Mode","no-cors")&&field_is(b,"Sec-Fetch-Site","cross-site"),
          "img：画像の Accept、Upgrade-Insecure-Requests なし、image・no-cors・cross-site");
    clear(&j);

    /* Fetch の fetch の段 14 は要求の header list に一度だけ足すので、リダイレクトのどの跳びでも同じ
       値が一つだけ行く。webfetch は跳びごとに頭を作り直すが、値は要求の欄から取る。 */
    static const struct {unsigned kind,destination,fetch_flags,wire;const char *document,*urls[4],*label;} hop_cases[]={
        {WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_BROWSER_UI|WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"",
            {"https://example.test/a","https://other.test/b","http://www.example.test/c","https://example.test/d"},"最上位の遷移"},
        {WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page",
            {"https://example.test/a","https://www.example.test/b","https://other.test/c",NULL},"fetch()"},
        {WEBNET_NAVIGATION,WEBNET_DEST_IFRAME,0,0,"https://example.test/",
            {"https://other.test/frame","https://example.test/frame",NULL,NULL},"iframe"},
        {WEBNET_RESOURCE,WEBNET_DEST_IMAGE,0,0,"https://example.test/",
            {"https://other.test/a.png","https://alice.github.test/b.png",NULL,NULL},"img"},
        {WEBNET_REPORT,WEBNET_DEST_REPORT,0,WEBNET_WIRE_REDIRECT_ERROR,"https://example.test/",
            {"https://other.test/report",NULL,NULL,NULL},"CSP の報告"},
    };
    calls=prefs_calls;
    for(size_t i=0;i<sizeof hop_cases/sizeof *hop_cases;i++){
        static const char *const values[]={"ja,en-US;q=0.9,en;q=0.8","de-CH"};
        for(unsigned v=0;v<2;v++){
            wire_language=values[v];
            j=request(hop_cases[i].kind,hop_cases[i].destination,hop_cases[i].fetch_flags,hop_cases[i].wire,hop_cases[i].document);
            for(unsigned hop=0;hop<4&&hop_cases[i].urls[hop];hop++){
                visit(&j,hop_cases[i].urls[hop]);b=outgoing(&j,"GET");
                checkf(field_count(b,"Accept-Language")==1&&field_is(b,"Accept-Language",values[v]),
                       "%s の %u 跳び目（%s）も要求の欄の値 %s が一つ",hop_cases[i].label,hop+1,hop_cases[i].urls[hop],values[v]);
            }
            clear(&j);
            /* 作者の値は全部の跳びで残り、利用者の値は足さない（WPT xhr/send-accept-language.htm の x-GameSpeak）。 */
            j=request(hop_cases[i].kind,hop_cases[i].destination,hop_cases[i].fetch_flags,hop_cases[i].wire,hop_cases[i].document);
            free(j.headers);j.headers=strdup(v?"Accept-Language: x-GameSpeak\r\n":"accept-language: bzh\r\n");
            for(unsigned hop=0;hop<4&&hop_cases[i].urls[hop];hop++){
                visit(&j,hop_cases[i].urls[hop]);b=outgoing(&j,"GET");
                checkf(field_count(b,"Accept-Language")==1&&field_is(b,"Accept-Language",v?"x-GameSpeak":"bzh"),
                       "%s の %u 跳び目：作者の Accept-Language が勝ち、利用者の値は足さない",hop_cases[i].label,hop+1);
            }
            clear(&j);
        }
    }
    check(prefs_calls==calls,"跳びの間に webfetch は利用者の設定を読まない");

    /* WebSocket の握手（製品の ws_handshake）：作者の欄は Sec-WebSocket-Protocol だけで、
       Accept-Language は要求の欄の値。 */
    {
        static const char *const values[]={"ja,en-US;q=0.9","",NULL};
        for(unsigned v=0;values[v];v++){
            wire_language=values[v];
            j=request(WEBNET_WEBSOCKET,WEBNET_DEST_EMPTY,0,0,"https://example.test/");
            free(j.headers);j.headers=strdup("Sec-WebSocket-Protocol: chat\r\n");
            struct url_owned u={0};
            check(url_parse_owned("https://www.example.test/socket",&u),"WebSocket の URL を製品の解析が読む");
            struct websocket ws={0};ws.j=&j;socket_sent[0]=0;
            bool ok=ws_handshake(&ws,"https://www.example.test/socket",&u);
            const char *fields=strstr(socket_sent,"\r\n");fields=fields?fields+2:"";
            checkf(!ok&&!strncmp(socket_sent,"GET /socket HTTP/1.1\r\n",22)&&!strcmp(j.error,"WebSocket server closed during the handshake"),
                   "握手の要求を送り、応答の無い stub で閉じたと分かる（値 \"%s\"）",values[v]);
            if(*values[v])check(field_count(fields,"Accept-Language")==1&&field_is(fields,"Accept-Language",values[v]),"WebSocket の握手にも要求の欄の Accept-Language が一つ");
            else check(field_count(fields,"Accept-Language")==0,"欄の無い WebSocket の握手には Accept-Language を出さない");
            check(field_is(fields,"Accept","*/*")&&field_is(fields,"Sec-Fetch-Mode","websocket"),"握手の Accept と Sec-Fetch-Mode");
            free(ws.in);url_owned_free(&u);clear(&j);
        }
        check(socket_records==0,"応答の無い握手は OPEN を記録しない");
    }

    /* CORS-preflight（製品の preflight）：Fetch の CORS-preflight fetch は fetch の段を通らないので
       Accept-Language を付けない。要求に欄があっても同じ。 */
    wire_language="ja,en-US;q=0.9";
    j=request(WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page");j.wire.deadline=UINT64_MAX;
    visit(&j,"https://other.test/data");
    preflight_sent[0]=0;unsigned before_requests=preflight_requests;
    bool preflighted=preflight(&j,j.final_url,"PUT","x-custom");
    const char *pf=strchr(preflight_sent,'\n');pf=pf?pf+1:"";
    check(!preflighted&&preflight_requests==before_requests+1&&!strncmp(preflight_sent,"OPTIONS https://other.test/data\n",32),"preflight の要求を送る");
    check(field_is(pf,"Access-Control-Request-Method","PUT")&&field_is(pf,"Access-Control-Request-Headers","x-custom")&&field_is(pf,"Origin","https://example.test"),
          "preflight の Origin と Access-Control-Request-*");
    check(field_count(pf,"Accept-Language")==0,"preflight には Accept-Language を付けない");
    clear(&j);
    wire_language="";

    /* 割り当ての失敗の注入。 */
    check(accept_language(long_language,NULL),"128 バイトの値の基準");
    fault_sweep("長い Accept-Language",long_language_setup,long_language_run,(int)(sizeof language_prefills/sizeof *language_prefills));
    fault_sweep("Sec-Fetch-* の連なり",metadata_setup,metadata_run,100);
    fault_sweep("prepare_outgoing の一段",outgoing_setup,outgoing_run,16);

    check(unknown_site_hosts==0,"same site の表が試験に出る host をすべて持つ");
    check(cookie_reads==0,"credentials omit の試験では cookie を読まない");
    check(live_allocations==0,"試験の終わりに割り当てが残らない");
    printf("Fetch の Accept・Accept-Language と Fetch Metadata の試験 %u 件（割り当ての失敗の注入 %u 点）、失敗 %u 件\n",checks,fault_points,failed);
    return failed?1:0;
}
