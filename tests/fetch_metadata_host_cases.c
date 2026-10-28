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
/* 要求を作る。document は要求を始めた文書の URL（"" は文書なし）。j->origin は製品の
   run_http と同じく http_url で導く（data: などは opaque で NULL のまま）。 */
static struct job request(unsigned kind,unsigned destination,unsigned fetch_flags,unsigned wire_flags,const char *document){
    struct job j={0};
    j.wire.kind=kind;j.wire.user_navigation=wire_flags;
    j.fetch.destination=destination;j.fetch.flags=fetch_flags;
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
/* 一つの要求の Accept-Language の頭。prefill は先に緩衝にある頭。 */
static char language_header[2048];
static bool accept_language(const char *list,const char *prefill){
    struct job j={0};languages_value=list;
    bool ok=!prefill||header_text(&j,&j.outgoing,prefill);
    ok=ok&&append_accept_language(&j);
    snprintf(language_header,sizeof language_header,"%s",j.outgoing.p?j.outgoing.p:"");
    clear(&j);return ok;
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
static char long_languages[1024];
static void long_language_setup(struct job *j,int v){(void)v;*j=(struct job){0};languages_value=long_languages;}
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
    languages_value=long_languages;
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

    /* Accept-Language：一番目は q なし、以後 0.9, 0.8 …、10 番目から後は 0.1。言語が無ければ頭を出さない。 */
    check(accept_language("ja",NULL)&&!strcmp(language_header,"Accept-Language: ja\r\n"),"言語が一つなら q なし");
    check(accept_language("ja,en-US,en",NULL)&&!strcmp(language_header,"Accept-Language: ja,en-US;q=0.9,en;q=0.8\r\n"),"二番目から重みが 0.1 ずつ下がる");
    check(accept_language("ja,en-US,en,fr,de,es,it,pt-BR,ko,zh-CN,ru,nl",NULL)&&!strcmp(language_header,
        "Accept-Language: ja,en-US;q=0.9,en;q=0.8,fr;q=0.7,de;q=0.6,es;q=0.5,it;q=0.4,pt-BR;q=0.3,ko;q=0.2,zh-CN;q=0.1,ru;q=0.1,nl;q=0.1\r\n"),
        "10 番目は 0.1、11 番目から後も 0.1");
    check(accept_language("",NULL)&&!language_header[0],"言語が無ければ Accept-Language を出さない");
    check(accept_language("ja,,en,",NULL)&&!strcmp(language_header,"Accept-Language: ja,en;q=0.9\r\n"),"空の項目は言語に数えない");
    check(accept_language("ja","Accept: */*\r\n")&&!strcmp(language_header,"Accept: */*\r\nAccept-Language: ja\r\n"),"前の頭を残して後ろへ足す");
    char too_long[1100];memset(too_long,'x',sizeof too_long-1);too_long[sizeof too_long-1]=0;
    unsigned calls=languages_calls;
    check(accept_language(too_long,NULL)&&!language_header[0],"言語の一覧を読めないときは壊れた頭を出さず、要求は続ける");
    check(languages_calls==calls+1,"言語の一覧は n_user_languages から読む");

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
    languages_value="ja,en-US";
    j=request(WEBNET_NAVIGATION,WEBNET_DEST_DOCUMENT,WEBNET_FETCH_BROWSER_UI|WEBNET_FETCH_USER_ACTIVATION,WEBNET_WIRE_USER_NAVIGATION,"");
    visit(&j,"https://example.test/");
    const char *b=outgoing(&j,"GET");
    check(field_is(b,"Accept",document_accept),"address bar の遷移：Accept は文書の値");
    check(field_is(b,"Accept-Language","ja,en-US;q=0.9"),"address bar の遷移：Accept-Language は利用者の言語");
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
    languages_value="";
    j=request(WEBNET_FETCH,WEBNET_DEST_EMPTY,0,0,"https://example.test/page");visit(&j,"https://example.test/data");b=outgoing(&j,"GET");
    check(field_is(b,"Accept-Language",NULL),"利用者の言語が無ければ Accept-Language の頭を出さない");
    clear(&j);
    languages_value="ja";
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

    /* 割り当ての失敗の注入。 */
    size_t at=0;
    for(unsigned i=0;i<80;i++)at+=(size_t)snprintf(long_languages+at,sizeof long_languages-at,"%sab-C%02u",i?",":"",i);
    check(accept_language(long_languages,NULL),"長い言語の一覧の基準");
    unsigned tenth=0;for(const char *p=language_header;(p=strstr(p,";q=0.1"));p++)tenth++;
    check(tenth==71&&strstr(language_header,"Accept-Language: ab-C00,ab-C01;q=0.9,")==language_header,"長い一覧でも 10 番目から後はすべて 0.1");
    fault_sweep("長い Accept-Language",long_language_setup,long_language_run,1);
    fault_sweep("Sec-Fetch-* の連なり",metadata_setup,metadata_run,100);
    fault_sweep("prepare_outgoing の一段",outgoing_setup,outgoing_run,16);

    check(unknown_site_hosts==0,"same site の表が試験に出る host をすべて持つ");
    check(cookie_reads==0,"credentials omit の試験では cookie を読まない");
    check(live_allocations==0,"試験の終わりに割り当てが残らない");
    printf("Fetch の Accept・Accept-Language と Fetch Metadata の試験 %u 件（割り当ての失敗の注入 %u 点）、失敗 %u 件\n",checks,fault_points,failed);
    return failed?1:0;
}
