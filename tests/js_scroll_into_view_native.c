/* New scrollIntoView only, no repeated Element scrolling suite. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main
static void into_viewport(void *opaque,int x,int y){struct fixture *f=opaque;f->scroll_x=x;f->scroll_y=y;}
static void test_into_viewport(void){
    const char *source="const filler=document.createElement('div');filler.style.height='1600px';document.body.appendChild(filler);"
        "globalThis.intoViewport=document.createElement('a');intoViewport.href='/dir/into-viewport';"
        "intoViewport.style.cssText='position:absolute;top:1000px;left:0;width:50px;height:50px;background:rgb(0,255,0)';"
        "document.body.appendChild(intoViewport);mark('into-viewport-ready');";
    char *page=script_page(source);test_check("into-viewport-page",page!=NULL);if(!page)return;
    memset(&fixture,0,sizeof fixture);
    struct web_host host={.opaque=&fixture,.request=request,.cancel=cancel,.sync_load=sync_load,.console=receive_log,
        .scroll=scroll_position,.scroll_to=into_viewport};
    fixture.doc=web_live(page,strlen(page),BASE,"utf-8",&host);
    test_check("into-viewport-document",fixture.doc!=NULL);
    if(fixture.doc){
        test_check("into-viewport-ready",pump("into-viewport-ready",3000));
        const char *move="intoViewport.scrollIntoView({block:'start',inline:'nearest'});"
            "check('into-viewport-actual-JS-rect',intoViewport.getBoundingClientRect().top===0);mark('into-viewport-moved');";
        test_check("into-viewport-eval",web_console_eval(fixture.doc,move,strlen(move)));
        test_check("into-viewport-host-position",fixture.scroll_y==1000);
        test_check("into-viewport-moved",pump("into-viewport-moved",1000));
        test_check("into-viewport-JS-rect",has_mark(&fixture,"into-viewport-actual-JS-rect"));
        canvas_t c;gfx_init(&c,pixels,VW,VH,VW);web_paint(fixture.doc,&c,0,0,VW,VH,fixture.scroll_x,fixture.scroll_y);
        test_check("into-viewport-actual-pixel",(pixels[10*VW+10]&0xffffff)==0x00ff00);
        struct web_hit hit={0};test_check("into-viewport-pixel-hit",web_hit_test(fixture.doc,10,10+fixture.scroll_y,&hit)&&hit.href&&strstr(hit.href,"/dir/into-viewport"));
        test_check("into-viewport-no-errors",fixture.errors==0&&fixture.js_failures==0);
        close_case();
    }
    free(page);
}
int main(int argc,char **argv){
    (void)argc;(void)argv;
    external_case("js_scroll_into_view_cases.js",";runScrollIntoViewCases().then(n=>{console.log('Scroll into view API checks '+n);check('into-view-count',n>=25);mark('api-done');},e=>{console.log('FAIL into view '+e+' '+e.stack);mark('api-done');});",BASE);
    test_into_viewport();
    printf("js_scroll_into_view_native: %d checks, %d failed\n",total,failed);return failed!=0;
}
