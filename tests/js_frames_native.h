/* Include in jstest.c after its fixture helpers, before main. */
static int frame_origin_requests;
static bool frame_test_request(void *opaque,const struct web_request *r){
    if(r->kind==WEB_RESOURCE_FETCH && strstr(r->url,"frame-api")) {
        test_check("frame-child-initiating-origin",r->origin && !strcmp(r->origin,"http://other.test/frame-cross.html"));
        frame_origin_requests++;
    }
    if(r->kind==WEB_RESOURCE_FRAME && strstr(r->url,"frame-green.html?next"))
        test_check("frame-link-initiating-origin",r->origin && !strcmp(r->origin,"http://fixture.test/frame-red.html"));
    return request(opaque,r);
}
static void frame_test_deliver(void){
    uint64_t now=uptime_ms();
    for(int i=0;i<SLOTS;i++)if(fixture.queue[i].used && fixture.queue[i].due<=now &&
       (strstr(fixture.queue[i].url,"/frame-cross.html") || strstr(fixture.queue[i].url,"/frame-api") ||
        strstr(fixture.queue[i].url,"/frame-red.html") || strstr(fixture.queue[i].url,"/frame-green.html"))){
        struct queued q=fixture.queue[i];fixture.queue[i].used=false;
        struct web_response *r=calloc(1,sizeof *r);test_check("frame-response-allocation",r!=NULL);if(!r)continue;
        r->status=200;snprintf(r->url,sizeof r->url,"%s",q.url);strcpy(r->headers,"Content-Type: text/html\r\n");
        const char *body=strstr(q.url,"/frame-api")?"ok":strstr(q.url,"/frame-red.html")?
            "<!doctype html><body style='margin:0;background:#ff0000'>"
            "<input id='field' style='position:absolute;left:10px;top:20px;width:100px;height:20px'>"
            "<a id='jump' target='green' href='/frame-green.html?next' style='position:absolute;display:block;left:10px;top:60px;width:100px;height:20px' "
            "onclick=\"document._clickX=event.clientX;console.log('OK frame-child-native-click');\">next</a>"
            "<a id='script-top' href='/forbidden-top.html' target='_top'></a>"
            "<script>document.getElementById('script-top').click();console.log('OK frame-red-ready');</script>":
            strstr(q.url,"/frame-green.html")?(strstr(q.url,"?next")?
            "<!doctype html><body style='margin:0;background:#00ff00'><script>console.log('OK frame-green-next');</script>":
            "<!doctype html><body style='margin:0;background:#00ff00'><script>console.log('OK frame-green-ready');</script>"):
            "<!doctype html><body><script>"
            "try{void parent.document;console.log('FAIL cross-child-parent-document');}catch(e){console.log(e.name==='SecurityError'?'OK cross-child-parent-denied':'FAIL cross-child-parent-error');}"
            "fetch('http://other.test/frame-api').then(()=>console.log('OK cross-child-fetch'));"
            "const crossChannel=new BroadcastChannel('frame-realms');crossChannel.postMessage('forbidden-cross-origin');"
            "</script>cross-origin";
        r->body=strdup(body);r->body_len=strlen(body);
        web_resource_loaded(fixture.doc,q.id,r);web_response_free(r);free(r);fixture.completions++;
    }
}
static void test_frame_ui(void){
    free_marks(&fixture);
    memset(&fixture,0,sizeof fixture);
    struct web_host host={.opaque=&fixture,.request=frame_test_request,.cancel=cancel,.console=receive_log,.scroll=scroll_position,.navigate=navigate};
    const char *page="<!doctype html><html><head></head><frameset cols='25%,*'><frame name='red' src='/frame-red.html'><frame name='green' src='/frame-green.html'></frameset></html>";
    fixture.doc=web_live(page,strlen(page),BASE,"utf-8",&host);test_check("frameset-live-context",fixture.doc!=NULL);if(!fixture.doc)return;
    uint64_t end=uptime_ms()+5000;
    while(uptime_ms()<end && (!has_mark(&fixture,"frame-red-ready") || !has_mark(&fixture,"frame-green-ready"))){frame_test_deliver();step(&fixture);msleep(1);}
    test_check("frameset-two-child-documents",has_mark(&fixture,"frame-red-ready") && has_mark(&fixture,"frame-green-ready"));
    step(&fixture);
    test_check("frameset-native-red-paint",(pixels[300*VW+100]&0xffffff)==0xff0000);
    test_check("frameset-native-green-paint",(pixels[300*VW+500]&0xffffff)==0x00ff00);
    test_check("script-child-target-top-denied",fixture.navigations==0);
    web_node *input=web_node_at(fixture.doc,20,25);test_check("frame-input-hit",input!=NULL);
    if(input){
        web_focus(fixture.doc,input);test_check("frame-input-focus",web_focused(fixture.doc)==input);
        struct gui_event key={.key='Q'};test_check("frame-input-native-key",web_key(fixture.doc,&key)==1);
        const char *value=web_control_value(input);test_check("frame-input-native-value",value && !strcmp(value,"Q"));
        int x,y,w,h;test_check("frame-input-top-rect",web_node_rect(fixture.doc,input,&x,&y,&w,&h) && x<30 && y<35 && w>80);
    }
    web_node *link=web_node_at(fixture.doc,12,65),*anchor=web_link_activation_anchor(fixture.doc,link);
    struct web_hit hit={0};test_check("frame-child-link-hit",anchor && web_link_action(fixture.doc,anchor,&hit) && hit.kind==WEB_HIT_LINK);
    if(anchor && hit.href){
        struct web_event click={.type="click",.bubbles=true,.cancelable=true,.x=12,.y=65};
        test_check("frame-native-click-dispatch",web_dispatch(fixture.doc,link,&click));
        test_check("frame-native-click-child-realm",has_mark(&fixture,"frame-child-native-click"));
        const char *probe="console.log(frames[0].document._clickX===12?'OK frame-local-click-coordinate':'FAIL frame-local-click-coordinate');";
        test_check("frame-native-coordinate-eval",web_console_eval(fixture.doc,probe,strlen(probe)));
        test_check("frame-local-click-coordinate",has_mark(&fixture,"frame-local-click-coordinate"));
        test_check("frame-named-native-navigation",web_frame_navigate(fixture.doc,anchor,hit.href));
        end=uptime_ms()+5000;
        while(uptime_ms()<end && !has_mark(&fixture,"frame-green-next")){frame_test_deliver();step(&fixture);msleep(1);}
        test_check("frame-named-navigation-loaded",has_mark(&fixture,"frame-green-next"));
        test_check("frame-native-link-no-top-navigation",fixture.navigations==0);
    }
    test_check("frame-ui-no-errors",fixture.errors==0 && fixture.js_failures==0);close_case();
}
static void test_frames(void){
    FILE *file=fopen("/data/tests/js_frames_cases.js","rb");test_check("frame-case-source",file!=NULL);if(!file)return;
    fseek(file,0,SEEK_END);long n=ftell(file);rewind(file);
    char *source=n>=0 && n<65536?malloc((size_t)n+512):NULL;test_check("frame-case-memory",source!=NULL);
    if(!source){fclose(file);return;}size_t got=fread(source,1,(size_t)n,file);fclose(file);source[got]=0;
    strcat(source,";runFrameCases().then(n=>{check('frame-api-count',n>=40);mark('frames-done');},e=>{console.log('FAIL frames '+e+' '+e.stack);mark('frames-done');});");
    char *page=script_page(source);free(source);test_check("frame-case-page",page!=NULL);if(!page)return;
    free_marks(&fixture);memset(&fixture,0,sizeof fixture);frame_origin_requests=0;
    struct web_host host={.opaque=&fixture,.request=frame_test_request,.cancel=cancel,.console=receive_log,.scroll=scroll_position,.navigate=navigate};
    fixture.doc=web_live(page,strlen(page),BASE,"utf-8",&host);free(page);test_check("frame-live-context",fixture.doc!=NULL);if(!fixture.doc)return;
    uint64_t end=uptime_ms()+15000;
    while(uptime_ms()<end && !has_mark(&fixture,"frames-done")){frame_test_deliver();step(&fixture);msleep(1);}
    if(fixture.errors || fixture.js_failures){
        printf("frames診断: load段階 ");for(unsigned i=1;i<=5;i++){char marker[32];snprintf(marker,sizeof marker,"frame-load-step-%u",i);printf("%u=%s ",i,has_mark(&fixture,marker)?"完了":"未完了");}printf("\n");fflush(stdout);
    }
    test_check("frames-finished",has_mark(&fixture,"frames-done"));test_check("frame-initiating-origin-reached",frame_origin_requests==1);
    test_check("cross-child-parent-denied",has_mark(&fixture,"cross-child-parent-denied"));
    test_check("cross-child-fetch",has_mark(&fixture,"cross-child-fetch"));
    test_check("frame-window-method-overrides",has_mark(&fixture,"frame-window-method-overrides"));
    test_check("frame-saved-method-navigation-guard",has_mark(&fixture,"frame-saved-method-navigation-guard"));
    test_check("frame-saved-method-detach-guard",has_mark(&fixture,"frame-saved-method-detach-guard"));
    test_check("frame-no-errors",fixture.errors==0 && fixture.js_failures==0);
    close_case();test_check("frame-free-cancels-outstanding",queued_count(&fixture)==0);
    test_frame_ui();
}
