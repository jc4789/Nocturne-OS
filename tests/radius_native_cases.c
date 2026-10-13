/* Exact production CSS/radius/cache/gfx routines; serial host dispatcher only. */
#include <time.h>
static int checks,failed;
static void check(bool value,const char *name){checks++;if(!value){failed++;printf("FAIL radius-host %s\n",name);}}
static uint32_t pixels[256*160];
static uint32_t pixel(int x,int y){return pixels[y*256+x]&0xffffffu;}
static void reset(canvas_t *canvas){gfx_init(canvas,pixels,256,160,256);gfx_fill(canvas,0,0,256,160,0xffffffffu);}
static int mask_performance(void){
    const int width=1920,height=1080,frames=8;size_t count=(size_t)width*height;
    uint32_t *slow=calloc(count,sizeof *slow),*fast=calloc(count,sizeof *fast),*source=malloc(count*sizeof *source);
    check(slow&&fast&&source,"mask performance buffers");if(!slow||!fast||!source){free(slow);free(fast);free(source);return 1;}
    for(size_t i=0;i<count;i++)source[i]=0xff000000u|((uint32_t)i*37&0xffffffu);
    style_t style={.border_radius=4};web_doc doc={.width=width,.height=height};struct radius_cache *cache=NULL;
    check(radius_cache_prepare(&doc,&cache,0,0,width,height,used_radii(&style,width,height)),"actual 1080p retained mask");
    if(!cache){free(slow);free(fast);free(source);return 1;}
    struct gfx_mask_clip masks[4];for(int i=0;i<4;i++)masks[i]=(struct gfx_mask_clip){.alpha=cache->alpha,.x=cache->left,.y=cache->top,.w=cache->width,.h=cache->height,.pitch=cache->width,.next=i?&masks[i-1]:NULL};
    image_t image={.w=width,.h=height,.px=source};canvas_t canvas;gfx_init(&canvas,slow,width,height,width);canvas.mask_clip=&masks[3];
    clock_t start=clock();for(int i=0;i<frames;i++){gfx_fill(&canvas,0,0,width,height,0xffffffffu);image_draw(&canvas,&image,0,0);}clock_t old=clock();
    for(int i=0;i<4;i++)masks[i].opaque=cache->opaque;
    gfx_init(&canvas,fast,width,height,width);canvas.mask_clip=&masks[3];
    for(int i=0;i<frames;i++){gfx_fill(&canvas,0,0,width,height,0xffffffffu);image_draw(&canvas,&image,0,0);}clock_t optimized=clock();
    check(!memcmp(slow,fast,count*sizeof *slow),"all 1080p pixels match generic chain reference after repeated frames");
    printf("mask-host-performance: %d frames x %dx%d x 4 ancestor masks, generic %.3f ms, opaque spans %.3f ms (host only)\n",frames,width,height,(double)(old-start)*1000/CLOCKS_PER_SEC,(double)(optimized-old)*1000/CLOCKS_PER_SEC);
    web_radius_cache_free(&cache);free(slow);free(fast);free(source);printf("mask-host: %d checks, %d failures\n",checks,failed);return failed!=0;
}
static int image_association(void){
    style_t parent,style;css_style_init(&parent,NULL);
    parent.bg_image="https://image.test/parent.svg";parent.bg_img=13;
    parent.mask_image="https://image.test/parent-mask.svg";parent.mask_img=14;
    css_style_init(&style,&parent);
    check(!style.bg_image&&!style.bg_img&&!style.mask_image&&!style.mask_img,"non-inherited image values do not borrow registered parent handles");
    arena_t arena={0};uint8_t property_bits[css_prop_count()];memset(property_bits,0,sizeof property_bits);
    struct cx cx={.s=&style,.parent=&parent,.em=16,.rem=16,.vw=800,.vh=600,.a=&arena,.set=property_bits};
    const struct propdef *bg=css_prop_lookup("background-image",16),*mask=css_prop_lookup("mask-image",10);
    check(css_apply(bg,"inherit",7,&cx)&&style.bg_image==parent.bg_image&&style.bg_img==13,"explicit inherited URL keeps the corresponding parent image handle");
    memset(property_bits,0,sizeof property_bits);
    const char *url="url(https://image.test/child.svg)";
    check(css_apply(bg,url,strlen(url),&cx)&&style.bg_image&&!style.bg_img,"replacement image URL invalidates the old native association");
    style.bg_img=19;memset(property_bits,0,sizeof property_bits);
    const char *gradient="linear-gradient(red,blue)";
    check(css_apply(bg,gradient,strlen(gradient),&cx)&&style.has_grad&&style.gradient&&!style.bg_image&&!style.bg_img,"gradient cannot accidentally paint the old decoded URL over itself");
    memset(property_bits,0,sizeof property_bits);
    check(css_apply(bg,url,strlen(url),&cx)&&!style.gradient&&!style.has_grad&&!style.bg_img,"URL replacement removes prior gradient representation");
    style.bg_img=21;memset(property_bits,0,sizeof property_bits);
    check(css_apply(bg,"none",4,&cx)&&!style.bg_image&&!style.bg_img&&!style.gradient&&!style.has_grad,"background none clears the registered handle too");
    memset(property_bits,0,sizeof property_bits);
    check(css_apply(mask,"inherit",7,&cx)&&style.mask_image==parent.mask_image&&style.mask_img==14,"explicit inherited mask keeps its real association");
    memset(property_bits,0,sizeof property_bits);
    check(css_apply(mask,url,strlen(url),&cx)&&style.mask_image&&!style.mask_img,"replacement mask URL invalidates prior association");
    style.mask_img=22;memset(property_bits,0,sizeof property_bits);
    check(css_apply(mask,"none",4,&cx)&&!style.mask_image&&!style.mask_img,"mask none cannot retain a native icon handle");
    ar_free(&arena);printf("image-association-host: %d checks, %d failures\n",checks,failed);return failed!=0;
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--mask-performance"))return mask_performance();
    if(argc>1&&!strcmp(argv[1],"--image-association"))return image_association();
    struct css_corner_radius corner={.axis={{.kind=LK_LEN,.pct=50},{.kind=LK_LEN,.pct=50}}};
    style_t style={.corner_radius={corner,corner,corner,corner}};
    struct border_radii radii=used_radii(&style,80,40);
    check(radii.x[0]==40&&radii.y[0]==20,"percentage radii resolve against distinct width/height axes");
    check(corners_coverage(51,51,50,50,80,40,&radii)==0&&corners_coverage(90,70,50,50,80,40,&radii)==1,"ellipse knockout and middle");
    corner.axis[0].pct=100;corner.axis[1].pct=100;for(int i=0;i<4;i++)style.corner_radius[i]=corner;radii=used_radii(&style,80,40);
    check(radii.x[0]==40&&radii.y[0]==20,"one common overlap factor scales every axis");
    struct css_corner_radius square={.axis={{.kind=LK_LEN},{.kind=LK_LEN}}};style.corner_radius[2]=square;
    radii=used_radii(&style,80,40);check(radii.x[2]==0&&radii.y[2]==0&&corners_coverage(129,89,50,50,80,40,&radii)==1,"per-corner square survives other round corners");
    corner.axis[0].pct=corner.axis[1].pct=50;for(int i=0;i<4;i++)style.corner_radius[i]=corner;radii=used_radii(&style,80,40);
    float borders[4]={2,4,6,8};struct border_radii inner=inset_radii(radii,borders,68,32);
    check(inner.x[0]==32&&inner.y[0]==18&&inner.x[2]==36&&inner.y[2]==14,"inner corners subtract adjacent horizontal and vertical borders");
    web_doc doc={.width=256,.height=160};struct radius_cache *cache=NULL;canvas_t canvas;reset(&canvas);
    check(radius_cache_prepare(&doc,&cache,50,50,80,40,radii),"retained real A8 geometry construction");
    struct radius_cache *retained=cache;check(radius_cache_prepare(&doc,&cache,50,50,80,40,radii)&&cache==retained,"unchanged radius cache identity");
    struct gfx_mask_clip mask={.alpha=cache->alpha,.x=cache->left,.y=cache->top,.w=cache->width,.h=cache->height,.pitch=cache->width,.opaque=cache->opaque};canvas.mask_clip=&mask;
    bool proof=cache->opaque!=NULL;for(int yy=0;proof&&yy<cache->height;yy++)for(int xx=cache->opaque[yy].x0;xx<cache->opaque[yy].x1;xx++)proof&=cache->alpha[(size_t)yy*cache->width+xx]==255;
    check(proof,"retained row spans prove every bypassed alpha is actually 255");
    gfx_fill(&canvas,50,50,80,40,0xffff0000u);check(pixel(50,50)==0xffffff&&pixel(90,70)==0xff0000,"opaque fill respects ellipse mask");
    uint32_t source_pixels[80*40];for(unsigned i=0;i<80*40;i++)source_pixels[i]=0xff0000ffu;
    canvas_t source;gfx_init(&source,source_pixels,80,40,80);gfx_blit_alpha(&canvas,50,50,&source,0,0,80,40);
    check(pixel(50,50)==0xffffff&&pixel(90,70)==0x0000ff,"image alpha blit uses same rounded geometry");
    image_t bitmap={.w=80,.h=40,.px=source_pixels};image_draw(&canvas,&bitmap,50,50);
    check(pixel(50,50)==0xffffff&&pixel(90,70)==0x0000ff,"actual decoded-image direct-row path cannot bypass retained mask");
    uint32_t reference_pixels[256*160];mask.opaque=NULL;reset(&canvas);canvas.mask_clip=&mask;
    gfx_fill_blend(&canvas,50,50,80,40,0x802233aau);image_draw(&canvas,&bitmap,50,50);memcpy(reference_pixels,pixels,sizeof pixels);
    mask.opaque=cache->opaque;reset(&canvas);canvas.mask_clip=&mask;
    gfx_fill_blend(&canvas,50,50,80,40,0x802233aau);image_draw(&canvas,&bitmap,50,50);
    check(!memcmp(reference_pixels,pixels,sizeof pixels),"opaque fill/image span path exactly matches generic A8 corner blending");
    uint8_t zero[1]={0};struct gfx_mask_clip parent={.alpha=zero,.x=90,.y=70,.w=1,.h=1,.pitch=1};mask.next=&parent;
    gfx_fill_blend(&canvas,90,70,1,1,0xff00ff00u);check(pixel(90,70)==0x0000ff,"nested immutable mask intersections do not escape parent");
    gfx_noclip(&canvas);check(!canvas.mask_clip,"noclip clears borrowed descriptors");
    check(radius_cache_prepare(&doc,&cache,50,50,80,1000000,radii)&&cache->clipped&&cache->height<=160,"large author geometry allocates visible mask bytes only");
    doc.view_y=100000;check(radius_cache_prepare(&doc,&cache,50,50,80,1000000,radii),"scroll builds newly exposed rounded slice");
    retained=cache;fail_allocate=1;check(!radius_cache_prepare(&doc,&cache,50,50,81,40,radii)&&cache==retained,"allocation failure preserves old published mask");fail_allocate=0;
    cancelled=true;doc.native_cancelled=false;check(!radius_cache_prepare(&doc,&cache,50,50,81,40,radii)&&cache==retained,"cancel releases new mask without replacing old one");cancelled=false;doc.native_cancelled=false;
    web_radius_cache_free(&cache);check(!cache,"radius cache explicit owner retirement");
    struct css_box_shadow shadow={.spread=1,.color=0xff0000ffu};style.box_shadow=&shadow;
    struct shadow_cache *fragment=NULL;float no_borders[4]={0};doc.view_y=0;
    struct border_radii fragment_radii=used_radii(&style,80,40);
    fragment_radii.x[0]=fragment_radii.y[0]=fragment_radii.x[3]=fragment_radii.y[3]=0;
    check(shadow_cache_prepare_radii(&doc,&fragment,&style,50,50,80,40,no_borders,fragment_radii)&&
        fragment->radii.x[0]==0&&fragment->radii.y[0]==0&&fragment->radii.x[1]==40,
        "inline sliced shadow consumes the same open-edge geometry as background clip");
    web_shadow_cache_free(&fragment);
    printf("radius-host: %d checks, %d failures\n",checks,failed);return failed!=0;
}
