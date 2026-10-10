/* Real web_live/frame transport lifecycle for the deliberately bounded
 * foreign-origin, allow-same-origin + allow-scripts sandbox implementation. */
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webi.h"
#include "frame.h"

static int checks,failures,errors,navigations,auxiliary,policy_logs;
static bool policy_private=true,child_timer,child_promise,child_fetch,child_message;
static struct {uint64_t id;int kind;char url[256];bool used;} queued[32];
static web_doc *top;
static void check(const char *name,bool value){checks++;if(!value)failures++;printf("%s %s\n",value?"OK":"FAIL",name);}
static void receive(void *unused,int level,const char *text){
    if(!strncmp(text,"OK ",3)){checks++;puts(text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;puts(text);}
    else if(!strcmp(text,"CHILD-TIMER"))child_timer=true;
    else if(!strcmp(text,"CHILD-PROMISE"))child_promise=true;
    else if(!strcmp(text,"CHILD-FETCH"))child_fetch=true;
    else if(!strcmp(text,"CHILD-MESSAGE"))child_message=true;
    else if(!strncmp(text,"Frame sandbox policy:",21)){
        policy_logs++;if(strstr(text,"private-path")||strstr(text,"private-query")||strstr(text,"private-token"))policy_private=false;
        puts(text);
    }else if(level>=2){errors++;printf("ERROR %s\n",text);}
}
static bool request(void *unused,const struct web_request *r){
    for(unsigned i=0;i<32;i++)if(!queued[i].used){queued[i].id=r->id;queued[i].kind=r->kind;
        snprintf(queued[i].url,sizeof queued[i].url,"%s",r->url);queued[i].used=true;return true;}
    return false;
}
static void cancel(void *unused,uint64_t id){for(unsigned i=0;i<32;i++)if(queued[i].id==id)queued[i].used=false;}
static void navigate(void *unused,const char *url,const char *post){navigations++;}
static void navigate_form(void *unused,const char *url,const void *body,size_t length,const char *type,const char *target){
    if(target&&!strcmp(target,"_blank"))auxiliary++;else navigations++;
}
static node_t *find_id(web_doc *d,const char *id){
    for(node_t *n=d->root;n;){if(n->type==N_ELEM && node_attr(n,"id") && !strcmp(node_attr(n,"id"),id))return n;
        if(n->first)n=n->first;else{while(n!=d->root&&!n->next)n=n->parent;if(n==d->root)break;n=n->next;}}
    return NULL;
}
static struct web_frame *frame(const char *id){node_t *n=find_id(top,id);return n?web_frame_find(top,n):NULL;}
static void pump(void){for(int i=0;i<8;i++){web_tick(top,uptime_ms());msleep(1);}}
static bool deliver(const char *needle,const char *url,const char *extra,const char *body){
    for(unsigned i=0;i<32;i++)if(queued[i].used&&strstr(queued[i].url,needle)){
        struct web_response r={.status=200};web_response_set_url(&r,url?url:queued[i].url);
        snprintf(r.headers,sizeof r.headers,"Content-Type: %s\r\n%s",queued[i].kind==WEB_RESOURCE_FETCH?"text/plain":"text/html",extra?extra:"");
        r.body=strdup(body);r.body_len=strlen(body);uint64_t id=queued[i].id;queued[i].used=false;
        web_resource_loaded(top,id,&r);web_response_free(&r);pump();return true;
    }
    return false;
}
static bool pending(const char *needle){for(unsigned i=0;i<32;i++)if(queued[i].used&&strstr(queued[i].url,needle))return true;return false;}
static void eval(web_doc *d,const char *code){check("native-console-evaluation",web_console_eval(d,code,strlen(code)));}

static const char page[]=
    "<!doctype html><body><iframe id='plain' src='/plain'></iframe>"
    "<iframe id='safe' sandbox='allow-same-origin allow-scripts private-token' src='https://foreign.test/private-path/start?private-query'></iframe>"
    "<iframe id='grants' sandbox='allow-same-origin allow-scripts allow-forms allow-downloads allow-popups allow-popups-to-escape-sandbox allow-top-navigation-by-user-activation' src='https://grant.test/grants'></iframe>"
    "<iframe id='opaque' sandbox='allow-scripts' src='https://opaque.test/opaque'></iframe>"
    "<iframe id='noscripts' sandbox='allow-same-origin' src='https://noscripts.test/noscripts'></iframe>"
    "<iframe id='srcdoc' sandbox='allow-same-origin allow-scripts' srcdoc='<script>console.log(1)</script>'></iframe>"
    "<iframe id='same' sandbox='allow-same-origin allow-scripts' src='/same'></iframe>"
    "<iframe id='blank' sandbox='allow-same-origin allow-scripts' src='about:blank'></iframe>"
    "<iframe id='redirect' sandbox='allow-same-origin allow-scripts' src='https://foreign.test/redirect'></iframe>"
    "<iframe id='csp' sandbox='allow-same-origin allow-scripts' src='https://foreign.test/csp'></iframe>"
    "<iframe id='cspcomma' sandbox='allow-same-origin allow-scripts' src='https://foreign.test/cspcomma'></iframe>"
    "<script>addEventListener('message',e=>{if(e.origin==='https://foreign.test'&&e.data==='sandbox-child')console.log('CHILD-MESSAGE');});</script>";
static const char child[]=
    "<!doctype html><form id='form' action='/submit'><button id='button'>submit</button></form>"
    "<a id='top' target='_top' href='https://root.test/top'>top</a><a id='blank' target='_blank' href='https://destination.test/'>blank</a>"
    "<a id='download' download href='https://destination.test/file'>download</a>"
    "<dialog id='dialog' open><form id='dialog-form' method='dialog'><button>close</button></form></dialog>"
    "<iframe id='nested' sandbox='allow-same-origin allow-scripts allow-forms allow-popups allow-popups-to-escape-sandbox allow-downloads' src='https://nested.test/nested'></iframe>"
    "<iframe id='ancestor' src='https://root.test/ancestor'></iframe>"
    "<script>function ck(n,v){console.log((v?'OK ':'FAIL ')+n);}"
    "function denied(f){try{f();return false;}catch(e){return /SecurityError/.test(e.name+' '+e.message);}}"
    "ck('foreign-author-script-executed',location.origin==='https://foreign.test');"
    "ck('foreign-parent-document-denied',denied(()=>parent.document));"
    "ck('foreign-borrowed-parent-function-denied',denied(()=>parent.setTimeout(()=>{},0)));"
    "ck('foreign-parent-location-denied',denied(()=>parent.location.assign('https://root.test/escape')));"
    "ck('foreign-top-location-denied',denied(()=>top.location.replace('https://root.test/escape')));"
    "ck('sandbox-domain-write-denied',denied(()=>{document.domain='foreign.test';}));"
    "document.title='sandbox-native';ck('foreign-own-dom-allowed',document.title==='sandbox-native');"
    "let submits=0;const form=document.querySelector('#form');form.addEventListener('submit',()=>submits++);"
    "form.requestSubmit();document.querySelector('#button').click();form.submit();"
    "ck('sandbox-form-events-aborted',submits===0);"
    "document.querySelector('#dialog-form').requestSubmit();ck('sandbox-dialog-form-remains-open',document.querySelector('#dialog').open);"
    "ck('unsupported-modals-not-exposed',['alert','confirm','prompt','print'].every(k=>typeof globalThis[k]==='undefined'));"
    "ck('unsupported-popup-not-exposed',typeof open==='undefined');"
    "ck('unsupported-locks-not-exposed',typeof document.documentElement.requestPointerLock==='undefined');"
    "setTimeout(()=>console.log('CHILD-TIMER'),0);Promise.resolve().then(()=>console.log('CHILD-PROMISE'));"
    "fetch('/api').then(()=>console.log('CHILD-FETCH'));parent.postMessage('sandbox-child','https://root.test');"
    "document.querySelector('#top').click();document.querySelector('#blank').click();document.querySelector('#download').click();"
    "</script>";
static const char grant_page[]=
    "<!doctype html><form id='form' action='/submit'><button>submit</button></form>"
    "<a id='top' target='_top' href='https://root.test/allowed'>top</a><a id='blank' target='_blank' href='https://destination.test/allowed'>blank</a>";

int main(void){
    char known[161];uint32_t flags=sandbox_parse(NULL,known,sizeof known);check("absent-policy-unrestricted",flags==0&&!known[0]);
    check("empty-policy-opaque-blocked",!sandbox_supported(sandbox_parse("",known,sizeof known)));
    flags=sandbox_parse("ALLOW-SAME-ORIGIN\tallow-scripts\nallow-scripts unknown-private-token",known,sizeof known);
    check("ascii-token-case-and-dedup",sandbox_supported(flags)&&!strcmp(known,"allow-same-origin allow-scripts"));
    check("missing-scripts-blocked",!sandbox_supported(sandbox_parse("allow-same-origin",known,sizeof known)));
    check("missing-same-origin-blocked",!sandbox_supported(sandbox_parse("allow-scripts",known,sizeof known)));
    check("unactivated-top-grant-outside-subset",!sandbox_supported(sandbox_parse("allow-same-origin allow-scripts allow-top-navigation",known,sizeof known)));
    char overlong[4100];memset(overlong,' ',4097);strcpy(overlong+4097,"x");check("overlong-policy-fail-closed",!sandbox_supported(sandbox_parse(overlong,known,sizeof known)));
    struct web_host host={.request=request,.cancel=cancel,.console=receive,.navigate=navigate,.navigate_form=navigate_form,.debug_js=true,.js_task_budget_ms=5000};
    top=web_live(page,sizeof page-1,"https://root.test/","utf-8",&host);check("native-live-root",top!=NULL);if(!top)return 1;
    web_layout(top,640,480);pump();
    struct web_frame *safe=frame("safe"),*grant=frame("grants");
    check("foreign-navigation-requested",safe&&pending("/start"));
    check("initial-blank-scripts-disabled",safe&&safe->document&&(safe->document->sandbox_flags&SB_SCRIPTS)&&!safe->document->js);
    check("initial-blank-console-disabled",safe&&safe->document&&!web_console_eval(safe->document,"1",1));
    eval(top,"function ck(n,v){console.log((v?'OK ':'FAIL ')+n);}function denied(f){try{f();return false;}catch(e){return /SecurityError/.test(e.name+' '+e.message);}}const f=document.querySelector('#safe');ck('initial-contentdocument-not-exported',f.contentDocument===null);ck('initial-Function-not-exported',denied(()=>f.contentWindow.Function));ck('initial-eval-not-exported',denied(()=>f.contentWindow.eval));ck('initial-opener-null',f.contentWindow.opener===null);document.domain='root.test';ck('nosandbox-domain-same-host-preserved',document.domain==='root.test');");
    check("opaque-profile-no-network",!pending("/opaque"));check("script-disabled-profile-no-network",!pending("/noscripts"));
    check("srcdoc-profile-fail-closed",frame("srcdoc")&&frame("srcdoc")->failed&&!frame("srcdoc")->document);
    check("authored-blank-profile-fail-closed",frame("blank")&&frame("blank")->failed&&!frame("blank")->document);
    check("plain-response-delivered",deliver("/plain",NULL,NULL,"<!doctype html><script>console.log('OK nosandbox-author-script');</script>"));
    check("foreign-response-delivered",deliver("/start",NULL,NULL,child));
    check("grants-response-delivered",deliver("/grants",NULL,NULL,grant_page));
    check("same-origin-response-delivered",deliver("/same",NULL,NULL,"<script>console.log('FAIL same-origin-author-ran');</script>"));
    check("same-origin-active-script-still-disabled",frame("same")&&frame("same")->document&&(frame("same")->document->sandbox_flags&SB_SCRIPTS));
    check("redirect-response-delivered",deliver("/redirect","https://root.test/redirected",NULL,"<script>console.log('FAIL redirect-author-ran');</script>"));
    check("same-origin-redirect-fail-closed",frame("redirect")&&frame("redirect")->failed&&(frame("redirect")->document->sandbox_flags&SB_SCRIPTS));
    check("csp-response-delivered",deliver("/csp","https://foreign.test/csp","Content-Security-Policy: default-src 'self'; sandbox allow-scripts allow-same-origin\r\n","<script>console.log('FAIL csp-author-ran');</script>"));
    check("csp-sandbox-fail-closed",frame("csp")&&frame("csp")->failed);
    check("csp-comma-response-delivered",deliver("/cspcomma","https://foreign.test/cspcomma","Content-Security-Policy: default-src 'self', sandbox allow-scripts allow-same-origin\r\n","<script>console.log('FAIL cspcomma-author-ran');</script>"));
    check("csp-comma-sandbox-fail-closed",frame("cspcomma")&&frame("cspcomma")->failed);
    check("nested-response-delivered",deliver("/nested",NULL,NULL,"<!doctype html><form id='form'><button>submit</button></form>"));
    check("ancestor-response-delivered",deliver("/ancestor",NULL,NULL,"<script>console.log('FAIL ancestor-author-ran');</script>"));
    check("own-fetch-response-delivered",deliver("/api",NULL,NULL,"ok"));
    pump();check("own-timer-progress",child_timer);check("own-promise-progress",child_promise);check("own-fetch-progress",child_fetch);check("cross-origin-postmessage-progress",child_message);
    web_doc *active=safe?safe->document:NULL,*granted=grant?grant->document:NULL;
    check("active-flags-created-before-script",active&&!(active->sandbox_flags&SB_SCRIPTS)&&(active->sandbox_flags&SB_FORMS));
    check("active-foreign-origin-preserved",active&&!strcmp(web_effective_url(active),"https://foreign.test/private-path/start?private-query"));
    check("forms-default-denied",active&&!web_sandbox_forms_allowed(active));
    check("native-ui-forms-denied",active&&!web_sandbox_form_submission_allowed(top,find_id(active,"form")));
    struct web_form_request form_request={0};check("common-form-encoding-denied",active&&!web_submit_request(active,find_id(active,"form"),&form_request));web_submit_request_free(&form_request);
    check("dialog-method-forms-denied",active&&web_js_dialog_submit(active,find_id(active,"dialog-form"))==-1);
    check("granted-forms-native-ui-allowed",granted&&web_sandbox_form_submission_allowed(top,find_id(granted,"form")));
    check("granted-form-real-encoding",granted&&web_submit_request(granted,find_id(granted,"form"),&form_request));web_submit_request_free(&form_request);
    struct web_doc unrelated={.live=true};check("unrelated-ui-document-denied",active&&!web_sandbox_form_submission_allowed(&unrelated,find_id(active,"form")));
    struct web_frame *nested=active?web_frame_find(active,find_id(active,"nested")):NULL,*ancestor=active?web_frame_find(active,find_id(active,"ancestor")):NULL;
    check("nested-parent-flags-inherited",nested&&nested->document&&(nested->document->sandbox_flags&SB_FORMS)&&(nested->document->sandbox_flags&SB_POPUPS));
    check("nested-ancestor-origin-fail-closed",ancestor&&ancestor->failed&&(ancestor->document->sandbox_flags&SB_SCRIPTS));
    check("self-navigation-allowed",active&&web_sandbox_navigation_allowed(active,active,false));
    check("descendant-navigation-allowed",active&&nested&&web_sandbox_navigation_allowed(active,nested->document,false));
    check("top-without-activation-denied",active&&!web_sandbox_navigation_allowed(active,top,false));
    check("top-default-even-trusted-denied",active&&!web_sandbox_navigation_allowed(active,top,true));
    check("sibling-navigation-denied",active&&granted&&!web_sandbox_navigation_allowed(active,granted,true));
    check("nested-parent-navigation-denied",nested&&!web_sandbox_navigation_allowed(nested->document,active,true));
    check("explicit-top-untrusted-denied",granted&&!web_sandbox_navigation_allowed(granted,top,false));
    check("explicit-top-trusted-allowed",granted&&web_sandbox_navigation_allowed(granted,top,true));
    check("auxiliary-default-denied",active&&!web_sandbox_auxiliary_allowed(active));
    check("auxiliary-explicit-escape-allowed",granted&&web_sandbox_auxiliary_allowed(granted));
    check("default-top-native-link-consumed",active&&web_frame_navigate(top,find_id(active,"top"),"https://root.test/denied"));
    check("default-popup-native-link-consumed",active&&web_frame_navigate(top,find_id(active,"blank"),"https://destination.test/denied"));
    check("default-download-native-link-consumed",active&&web_frame_navigate(top,find_id(active,"download"),"https://destination.test/file"));
    check("restricted-actions-no-host-escape",navigations==0&&auxiliary==0);
    check("explicit-top-native-host-fallback",granted&&!web_frame_navigate(top,find_id(granted,"top"),"https://root.test/allowed"));
    check("explicit-popup-native-link",granted&&web_frame_navigate(top,find_id(granted,"blank"),"https://destination.test/allowed"));
    check("explicit-popup-one-host-window",auxiliary==1);
    struct web_event click={.type="click",.bubbles=true,.cancelable=true,.synthetic=true};
    if(granted)web_dispatch(granted,find_id(granted,"top"),&click);
    check("synthetic-event-no-activation",granted&&granted->sandbox_activation_until==0);
    click.synthetic=false;if(granted)web_dispatch(granted,find_id(granted,"top"),&click);
    check("native-trusted-event-activation",granted&&granted->sandbox_activation_until>uptime_ms());
    uint32_t saved=active?active->sandbox_flags:0;
    check("sandbox-attribute-removal-saved",safe&&doc_node_attr(top,safe->element,"sandbox",NULL));pump();
    check("active-policy-immutable-after-attribute-change",active&&safe->document==active&&active->sandbox_flags==saved&&!web_sandbox_forms_allowed(active));
    check("next-navigation-source-change",safe&&doc_node_attr(top,safe->element,"src","https://foreign.test/next"));pump();
    check("pending-policy-does-not-relax-old-document",active&&safe->pending_sandbox_flags==0&&active->sandbox_flags==saved);
    check("new-navigation-response-delivered",deliver("/next",NULL,NULL,"<!doctype html><form id='form'><button>submit</button></form>"));
    check("new-document-uses-new-policy",safe&&safe->document!=active&&safe->document->sandbox_flags==0&&web_sandbox_forms_allowed(safe->document));
    check("retired-document-cannot-run",active&&!active->live&&!web_console_eval(active,"1",1));
    check("retired-document-cannot-submit",active&&!web_sandbox_form_submission_allowed(top,find_id(active,"form")));
    check("native-policy-snapshot-redacts-author-secrets",policy_logs>=8&&policy_private);
    check("no-unexpected-runtime-error",errors==0);
    web_free(top);printf("sandboxprofiletest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
