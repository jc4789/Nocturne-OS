#define main nocturne_jstest_main
#include "jstest.c"
#undef main
int main(void) {
    external_case("js_messaging_cases.js", ";runMessagingCases().then(n=>{console.log('Message checks '+n);check('message-count',n>=38);mark('api-done');},e=>{console.log('FAIL messaging '+e+' '+e.stack);mark('api-done');});", BASE);
    test_check("messaging-js-count-reached",has_mark(&fixture,"message-count"));
    printf("messagingtest: native channel, transfer and window-message cases completed\n");
    char *lifecycle=script_page("let turns=0,loaded=false;const loop=new MessageChannel();loop.port2.onmessage=()=>{if(++turns<100)loop.port1.postMessage(0);};loop.port1.postMessage(0);"
        "addEventListener('load',()=>{loaded=true;check('posted-does-not-starve-load',turns<100);mark('posted-load');});");
    if(lifecycle&&open_case(lifecycle,false)) {test_check("posted-lifecycle",pump("posted-load",5000));close_case();}free(lifecycle);
    char *quota=script_page("const buffer=new ArrayBuffer(8);let queued=0;try{for(;queued<4096;queued++)postMessage(0,'*');}catch(e){}"
        "check('posted-native-quota',queued===4096);let rejected=false;try{postMessage(buffer,'*',[buffer]);}catch(e){rejected=e instanceof RangeError;}"
        "check('posted-quota-retains-transfer',rejected&&buffer.byteLength===8);mark('posted-quota');");
    if(quota&&open_case(quota,false)) {test_check("posted-quota-completion",pump("posted-quota",10000));close_case();}free(quota);
    char *page=script_page("postMessage('must-not-run','*');const c=new MessageChannel();c.port2.onmessage=()=>mark('old-port-fired');c.port1.postMessage(1);mark('posted-ready');");
    if(page && open_case(page,false)) {
        test_check("posted-before-navigation",pump("posted-ready",5000));close_case();
    }
    free(page);
    page=script_page("check('message-fresh-context',typeof c==='undefined');mark('fresh-posted');");
    if(page && open_case(page,false)) {test_check("posted-fresh",pump("fresh-posted",5000));test_check("posted-no-leak",pump(NULL,5000)&&!has_mark(&fixture,"old-port-fired"));close_case();}
    free(page);
    printf("messagingtest: %d checks, %d failed\n",total,failed);return failed!=0;
}
