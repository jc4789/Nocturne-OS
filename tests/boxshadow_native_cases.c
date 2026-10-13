/* Exact production shadow functions above; host dispatcher is serial only. */
static int checks,failed;
static void check(bool value,const char *name){checks++;if(!value){failed++;printf("FAIL shadow-host %s\n",name);}}
static uint32_t pixels[256*160];
static void reset(canvas_t *canvas){gfx_init(canvas,pixels,256,160,256);gfx_fill(canvas,0,0,256,160,0xffffffffu);}
static uint32_t pixel(int x,int y){return pixels[y*256+x]&0xffffffu;}
int main(void){
    web_doc doc={.width=256,.height=160};float borders[4]={0};struct shadow_cache *cache=NULL;canvas_t canvas;reset(&canvas);
    struct pctx context={.d=&doc,.c=&canvas};
    struct css_box_shadow shadow={.spread=1,.color=0x14000000u};style_t style={.box_shadow=&shadow,.border_radius=20};
    check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders),"actual spread cache construction");
    paint_shadows(&context,cache,50,50,false);
    check(pixel(49,70)==0xebebeb,"alpha spread outline is not omitted");check(pixel(100,70)==0xffffff,"transparent interior knockout");
    check(pixel(49,49)==0xffffff,"rounded spread retains curved corner");
    struct shadow_cache *retained=cache;
    check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders)&&cache==retained,"geometry cache retained");
    shadow.color=0xff00ff00u;check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders)&&cache==retained,"tint mutation does not rebuild mask");
    reset(&canvas);paint_shadows(&context,cache,50,50,false);
    check(pixel(49,70)==0x02ff02,"new tint uses retained immutable curved-edge mask with its original coverage");
    shadow=(struct css_box_shadow){.x=4,.color=0xffff0000u,.inset=true};style.border_radius=0;
    check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders),"inset source hole cache");reset(&canvas);paint_shadows(&context,cache,50,50,true);
    check(pixel(51,70)==0xff0000&&pixel(100,70)==0xffffff&&pixel(49,70)==0xffffff,"inset padding clipping/offset/knockout");
    struct css_box_shadow lower={.spread=10,.color=0xff0000ffu};shadow=(struct css_box_shadow){.spread=5,.color=0xffff0000u,.next=&lower};
    check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders),"multiple shadows cache");reset(&canvas);paint_shadows(&context,cache,50,50,false);
    check(pixel(46,70)==0xff0000&&pixel(41,70)==0x0000ff,"first shadow on top of later one");
    shadow=(struct css_box_shadow){.blur=20,.color=0xff000000u};check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders),"separable Gaussian exact functions");
    reset(&canvas);paint_shadows(&context,cache,50,50,false);
    unsigned edge=(pixel(49,70)>>16)&255,far=(pixel(30,70)>>16)&255;
    check(edge>100&&edge<180&&far>edge&&far<255,"Gaussian half-plane alpha and monotonic falloff");
    shadow=(struct css_box_shadow){.spread=2,.color=0xffff0000u};check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,1000000,borders),"tall author geometry renders without full-height allocation");
    check(cache->clipped&&cache->masks[0].height<200,"viewport convolution halo bounds actual mask bytes");
    doc.view_y=100000;check(shadow_cache_prepare(&doc,&cache,&style,50,50,100,1000000,borders),"new visible cropped slice cached");
    reset(&canvas);paint_shadows(&context,cache,50,-99950,false);check(pixel(49,70)==0xff0000,"cropped source stays correct after scroll");
    retained=cache;shadow.blur=3;fail_allocate=1;
    check(!shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders)&&cache==retained,"actual cache allocation failure preserves previous geometry");
    fail_allocate=0;cancelled=true;doc.native_cancelled=false;
    check(!shadow_cache_prepare(&doc,&cache,&style,50,50,100,40,borders)&&cache==retained&&doc.native_cancelled,"native cancellation releases partial new masks");
    cancelled=false;doc.native_cancelled=false;web_shadow_cache_free(&cache);check(cache==NULL,"cache explicit retirement frees ownership");
    printf("boxshadow-host: %d checks, %d failures\n",checks,failed);return failed!=0;
}
