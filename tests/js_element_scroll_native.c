/* Focused real web_live/QuickJS/paint/hit supplement. No prior suite reruns. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main
static void scroll_paint_hit(void){
    const char *source="globalThis.scrollBox=document.createElement('div');"
        "scrollBox.style.cssText='width:100px;height:60px;border:2px solid black;overflow:auto;position:relative';"
        "for(const [id,x,color] of [['red',0,'rgb(255,0,0)'],['green',120,'rgb(0,255,0)']]){"
        "const a=document.createElement('a');a.id=id;a.href='/dir/'+id;"
        "a.style.cssText='position:absolute;left:'+x+'px;top:0;width:100px;height:50px;background:'+color;scrollBox.appendChild(a);}"
        "document.body.appendChild(scrollBox);mark('scroll-paint-ready');";
    char *page=script_page(source);test_check("scroll-paint-page",page!=NULL);if(!page)return;
    if(open_case(page,false)){
        test_check("scroll-paint-ready",pump("scroll-paint-ready",3000));
        web_node *red=web_node_at(fixture.doc,12,32);struct web_hit before={0};
        test_check("scroll-red-pixel",(pixels[32*VW+12]&0xffffff)==0xff0000);
        test_check("scroll-red-native-hit",web_hit_test(fixture.doc,12,32,&before)&&before.node==red&&before.href&&strstr(before.href,"/dir/red"));
        int rx,ry,rw,rh;test_check("scroll-red-native-rect",web_node_rect(fixture.doc,red,&rx,&ry,&rw,&rh));
        const char *move="scrollBox.scrollTo(120,0);mark('scroll-paint-moved');";
        test_check("scroll-native-eval",web_console_eval(fixture.doc,move,strlen(move)));
        test_check("scroll-native-moved",pump("scroll-paint-moved",1000));
        /* Paint-only dirty is intentionally separate from the old harness's
           layout dirty path. Consume it and use the ordinary public painter. */
        bool paint=web_paint_dirty(fixture.doc);test_check("scroll-paint-only-notification",paint);
        canvas_t c;gfx_init(&c,pixels,VW,VH,VW);web_paint(fixture.doc,&c,0,0,VW,VH,0,0);
        web_node *green=web_node_at(fixture.doc,12,32);struct web_hit after={0};
        test_check("scroll-green-pixel",(pixels[32*VW+12]&0xffffff)==0x00ff00);
        test_check("scroll-green-native-hit",green!=red&&web_hit_test(fixture.doc,12,32,&after)&&after.node==green&&after.href&&strstr(after.href,"/dir/green"));
        int x,y,w,h;test_check("scroll-native-rect-displacement",web_node_rect(fixture.doc,red,&x,&y,&w,&h)&&x==rx-120&&y==ry);
        test_check("scroll-green-rect-matches-hit",web_node_rect(fixture.doc,green,&x,&y,&w,&h)&&x==rx&&y==ry);
        struct web_hit clipped={0};test_check("scroll-link-outside-clipped",!web_hit_test(fixture.doc,112,32,&clipped));
        test_check("scroll-pixel-outside-clipped",(pixels[32*VW+112]&0xffffff)==0xffffff);
        test_check("scroll-native-no-JS-errors",fixture.errors==0&&fixture.js_failures==0);
        close_case();
    }
    free(page);
}
int main(int argc,char **argv){
    (void)argc;(void)argv;
    external_case("js_element_scroll_cases.js",";runElementScrollCases().then(n=>{console.log('Element scroll API checks '+n);check('element-scroll-count',n>=35);mark('api-done');},e=>{console.log('FAIL element scroll '+e+' '+e.stack);mark('api-done');});",BASE);
    scroll_paint_hit();
    printf("js_element_scroll_native: %d checks, %d failed\n",total,failed);return failed!=0;
}
