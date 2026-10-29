/* Exact production CSS/radius/cache/gfx routines; serial host dispatcher only. */
#include <time.h>
static int checks,failed;
static void check(bool value,const char *name){checks++;if(!value){failed++;printf("FAIL radius-host %s\n",name);}}
static uint32_t pixels[256*160];
static uint32_t pixel(int x,int y){return pixels[y*256+x]&0xffffffu;}
static void reset(canvas_t *canvas){gfx_init(canvas,pixels,256,160,256);gfx_fill(canvas,0,0,256,160,0xffffffffu);}
/* The descriptor radius_clip_push gives a paint at the cache's own position. */
static struct gfx_mask_clip shape_clip(const struct radius_cache *cache){
    const struct radius_shape *s=radius_shape_mask(cache->shape,false);
    if(!s)return (struct gfx_mask_clip){0};
    return (struct gfx_mask_clip){.alpha=radius_shape_alpha(s),.x=cache->left,.y=cache->top,.w=s->width,.h=s->height,.pitch=s->width,
                                  .opaque=s->mask,.repeat_at=s->repeat_at,.repeat_extra=s->repeat_extra};
}
/* Every pixel of a box tall enough to repeat rows reads the coverage of its own pixel. */
static bool exact_shape(web_doc *doc,struct radius_cache **cache,canvas_t *canvas,float x,float y,float w,float h,struct border_radii radii){
    if(!radius_cache_prepare(doc,cache,x,y,w,h,radii))return false;
    struct gfx_mask_clip mask=shape_clip(*cache);reset(canvas);canvas->mask_clip=&mask;
    bool exact=mask.repeat_extra>0;
    for(int yy=(int)floorf(y);exact&&yy<(int)ceilf(y+h)+1;yy++)for(int xx=(int)floorf(x);xx<(int)ceilf(x+w)+1;xx++)
        exact&=gfx_clip_alpha(canvas,xx,yy)==(uint8_t)(255*corners_coverage(xx+.5f,yy+.5f,x,y,w,h,&radii)+.5f);
    canvas->mask_clip=NULL;return exact;
}
static int mask_performance(void){
    const int width=1920,height=1080,frames=8;size_t count=(size_t)width*height;
    uint32_t *slow=calloc(count,sizeof *slow),*fast=calloc(count,sizeof *fast),*source=malloc(count*sizeof *source);
    check(slow&&fast&&source,"mask performance buffers");if(!slow||!fast||!source){free(slow);free(fast);free(source);return 1;}
    for(size_t i=0;i<count;i++)source[i]=0xff000000u|((uint32_t)i*37&0xffffffu);
    style_t style={.border_radius=4};web_doc doc={.width=width,.height=height};struct radius_cache *cache=NULL;
    check(radius_cache_prepare(&doc,&cache,0,0,width,height,used_radii(&style,width,height)),"actual 1080p retained mask");
    if(!cache){free(slow);free(fast);free(source);return 1;}
    struct gfx_mask_clip masks[4];for(int i=0;i<4;i++){masks[i]=shape_clip(cache);masks[i].opaque=NULL;masks[i].next=i?&masks[i-1]:NULL;}
    image_t image={.w=width,.h=height,.px=source};canvas_t canvas;gfx_init(&canvas,slow,width,height,width);canvas.mask_clip=&masks[3];
    clock_t start=clock();for(int i=0;i<frames;i++){gfx_fill(&canvas,0,0,width,height,0xffffffffu);image_draw(&canvas,&image,0,0);}clock_t old=clock();
    for(int i=0;i<4;i++)masks[i].opaque=shape_clip(cache).opaque;
    gfx_init(&canvas,fast,width,height,width);canvas.mask_clip=&masks[3];
    for(int i=0;i<frames;i++){gfx_fill(&canvas,0,0,width,height,0xffffffffu);image_draw(&canvas,&image,0,0);}clock_t optimized=clock();
    check(!memcmp(slow,fast,count*sizeof *slow),"all 1080p pixels match generic chain reference after repeated frames");
    printf("mask-host-performance: %d frames x %dx%d x 4 ancestor masks, generic %.3f ms, opaque spans %.3f ms (host only)\n",frames,width,height,(double)(old-start)*1000/CLOCKS_PER_SEC,(double)(optimized-old)*1000/CLOCKS_PER_SEC);
    web_radius_cache_free(&cache);free(slow);free(fast);free(source);printf("mask-host: %d checks, %d failures\n",checks,failed);return failed!=0;
}
static int image_association(void){
    style_t parent,style;css_style_init(&parent,NULL);
    parent.bg_image="https://image.test/parent.svg";parent.bg_img=13;
    /* the mask layers' group: its images carry their document images (css_image's img) */
    struct css_image parent_mask_image={.kind=CSS_IMAGE_URL,.url="https://image.test/parent-mask.svg",.img=14};
    const struct css_image *parent_mask_list=&parent_mask_image;
    struct css_mask parent_mask=css_mask_initial;parent_mask.image=(struct css_layer_images){&parent_mask_list,1};
    parent.mask=&parent_mask;
    css_style_init(&style,&parent);
    check(!style.bg_image&&!style.bg_img&&!css_mask_layers_set(css_style_mask(&style)),"non-inherited image values do not borrow registered parent handles");
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
    check(css_apply(mask,"inherit",7,&cx)&&css_style_mask(&style)->image.n==1&&css_style_mask(&style)->image.v[0]==&parent_mask_image&&
          css_style_mask(&style)->image.v[0]->img==14,"explicit inherited mask keeps its real association");
    memset(property_bits,0,sizeof property_bits);
    check(css_apply(mask,url,strlen(url),&cx)&&css_style_mask(&style)->image.n==1&&css_style_mask(&style)->image.v[0]&&
          !css_style_mask(&style)->image.v[0]->img,"replacement mask URL invalidates prior association");
    memset(property_bits,0,sizeof property_bits);
    check(css_apply(mask,"none",4,&cx)&&!css_mask_layers_set(css_style_mask(&style)),"mask none cannot retain a native icon handle");
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
    struct gfx_mask_clip mask=shape_clip(cache);canvas.mask_clip=&mask;
    const struct radius_shape *shape=cache->shape;
    bool proof=mask.opaque!=NULL;for(size_t yy=0;proof&&yy<shape->rows;yy++)for(int xx=mask.opaque[yy].x0;xx<mask.opaque[yy].x1;xx++)proof&=mask.alpha[yy*(size_t)mask.pitch+xx]==255;
    check(proof,"retained row spans prove every bypassed alpha is actually 255");
    gfx_fill(&canvas,50,50,80,40,0xffff0000u);check(pixel(50,50)==0xffffff&&pixel(90,70)==0xff0000,"opaque fill respects ellipse mask");
    uint32_t source_pixels[80*40];for(unsigned i=0;i<80*40;i++)source_pixels[i]=0xff0000ffu;
    canvas_t source;gfx_init(&source,source_pixels,80,40,80);gfx_blit_alpha(&canvas,50,50,&source,0,0,80,40);
    check(pixel(50,50)==0xffffff&&pixel(90,70)==0x0000ff,"image alpha blit uses same rounded geometry");
    image_t bitmap={.w=80,.h=40,.px=source_pixels};image_draw(&canvas,&bitmap,50,50);
    check(pixel(50,50)==0xffffff&&pixel(90,70)==0x0000ff,"actual decoded-image direct-row path cannot bypass retained mask");
    uint32_t reference_pixels[256*160];mask.opaque=NULL;reset(&canvas);canvas.mask_clip=&mask;
    gfx_fill_blend(&canvas,50,50,80,40,0x802233aau);image_draw(&canvas,&bitmap,50,50);memcpy(reference_pixels,pixels,sizeof pixels);
    mask.opaque=shape->mask;reset(&canvas);canvas.mask_clip=&mask;
    gfx_fill_blend(&canvas,50,50,80,40,0x802233aau);image_draw(&canvas,&bitmap,50,50);
    check(!memcmp(reference_pixels,pixels,sizeof pixels),"opaque fill/image span path exactly matches generic A8 corner blending");
    uint8_t zero[1]={0};struct gfx_mask_clip parent={.alpha=zero,.x=90,.y=70,.w=1,.h=1,.pitch=1};mask.next=&parent;
    gfx_fill_blend(&canvas,90,70,1,1,0xff00ff00u);check(pixel(90,70)==0x0000ff,"nested immutable mask intersections do not escape parent");
    gfx_noclip(&canvas);check(!canvas.mask_clip,"noclip clears borrowed descriptors");
    check(radius_cache_prepare(&doc,&cache,50,50,80,1000000,radii)&&shape_clip(cache).alpha&&cache->shape->height==1000000&&cache->shape->rows<=2*(20+2)+2,
          "a tall box stores its corner rows and one middle row");
    struct border_radii round={.x={12,12,12,12},.y={12,12,12,12}};
    check(exact_shape(&doc,&cache,&canvas,50.75f,40.25f,80,110,round),"circular corners through the repeated middle rows are the shape's own coverage");
    check(exact_shape(&doc,&cache,&canvas,50.25f,50.5f,80,100,radii),"elliptical corners through the repeated middle rows are the shape's own coverage");
    struct radius_cache *alike=NULL;
    check(radius_cache_prepare(&doc,&alike,170.25f,20.5f,80,100,radii)&&alike->shape==cache->shape,"like boxes share one shape");
    shape=cache->shape;check(radius_cache_prepare(&doc,&cache,51.25f,52.5f,80,100,radii)&&cache->shape==shape&&cache->left==51,"a box moved by whole pixels keeps its mask");
    web_radius_cache_free(&alike);check(cache->shape==shape&&shape_clip(cache).alpha,"a shape outlives a cache that let it go");
    check(radius_cache_prepare(&doc,&cache,50,50,80,40,radii),"back to the first shape");
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
