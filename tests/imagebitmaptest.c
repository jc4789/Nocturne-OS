/* ImageBitmap's real web_live/codec/raster seam and independent pixel owner. */
#include <nocturne.h>
#include <web.h>
#include "js_canvas.h"
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,failures,errors,js_checks,diagnostics,seen[5];
static bool done,off_done,sanitized=true,cancel_decode;
static uint64_t image_request;
static const char *source_types[]={"Blob","ImageData","HTMLCanvasElement","HTMLImageElement","ImageBitmap"};
/* The browser uses an exact-sized JS_NewRuntime2 allocator. CONFIG_NOCTURNE's
   default JS_NewRuntime usable_size is zero and cannot measure external pixel
   payloads. Match the real size/count/limit contract, not a mock bitmap. */
union bitmap_allocation_header { size_t size;long double alignment; };
struct bitmap_allocation_state {size_t live,quota_failures,failed_size,max_successful_request;};
static void *bitmap_malloc(JSMallocState *s,size_t size){
    struct bitmap_allocation_state *a=s->opaque;
    if(!size||size>SIZE_MAX-sizeof(union bitmap_allocation_header))return NULL;
    size_t total=size+sizeof(union bitmap_allocation_header);
    if(total>SIZE_MAX-s->malloc_size||s->malloc_size+total>s->malloc_limit){a->quota_failures++;a->failed_size=size;return NULL;}
    union bitmap_allocation_header *h=malloc(total);if(!h)return NULL;
    h->size=size;a->live++;s->malloc_count++;s->malloc_size+=total;
    if(size>a->max_successful_request)a->max_successful_request=size;return h+1;
}
static void bitmap_free(JSMallocState *s,void *ptr){
    if(!ptr)return;union bitmap_allocation_header *h=(union bitmap_allocation_header *)ptr-1;
    ((struct bitmap_allocation_state *)s->opaque)->live--;s->malloc_count--;s->malloc_size-=h->size+sizeof *h;free(h);
}
static void *bitmap_realloc(JSMallocState *s,void *ptr,size_t size){
    if(!ptr)return bitmap_malloc(s,size);if(!size){bitmap_free(s,ptr);return NULL;}
    if(size>SIZE_MAX-sizeof(union bitmap_allocation_header))return NULL;
    union bitmap_allocation_header *old=(union bitmap_allocation_header *)ptr-1;
    size_t base=s->malloc_size-old->size;
    if(size>SIZE_MAX-base||base+size>s->malloc_limit){struct bitmap_allocation_state *a=s->opaque;a->quota_failures++;a->failed_size=size;return NULL;}
    union bitmap_allocation_header *next=malloc(sizeof *next+size);if(!next)return NULL;
    memcpy(next+1,old+1,old->size<size?old->size:size);next->size=size;s->malloc_size=base+size;free(old);
    struct bitmap_allocation_state *a=s->opaque;if(size>a->max_successful_request)a->max_successful_request=size;return next+1;
}
static size_t bitmap_usable(const void *ptr){return ptr?((const union bitmap_allocation_header *)ptr-1)->size:0;}
static const JSMallocFunctions bitmap_allocator={bitmap_malloc,bitmap_free,bitmap_realloc,bitmap_usable};
static void check(const char *name,bool pass){checks++;printf("%s bitmap-native %s\n",pass?"OK":"FAIL",name);if(!pass)failures++;}
static void log_line(void *unused,int level,const char *message){
    if(!strncmp(message,"OK bitmap ",10)){checks++;js_checks++;puts(message);}
    else if(!strncmp(message,"FAIL bitmap ",12)){checks++;js_checks++;failures++;puts(message);}
    else if(!strncmp(message,"BITMAP-DONE ",12)){done=true;puts(message);}
    else if(!strcmp(message,"BITMAP-OFF-DONE"))off_done=true;
    else if(!strncmp(message,"ImageBitmap decoded: ",21)){
        diagnostics++;puts(message);bool known=false;
        for(unsigned i=0;i<5;i++){char prefix[80];snprintf(prefix,sizeof prefix,"source=%s width=",source_types[i]);if(strstr(message,prefix)){seen[i]++;known=true;}}
        if(!known||strstr(message,"http")||strstr(message,"private")||strstr(message,"?")||strstr(message,"invalid"))sanitized=false;
    }else if(level>=2){errors++;printf("ERROR bitmap %s\n",message);}
    fflush(stdout);
}
static bool request(void *unused,const struct web_request *r){
    if(r->kind==WEB_RESOURCE_IMAGE && !strcmp(r->url,"https://bitmap.test/fixture.png"))image_request=r->id;
    return true;
}
static bool checkpoint(void *unused){return !cancel_decode;}
static void exception_clear(JSContext *ctx){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
static bool exception_named(JSContext *ctx,const char *name){
    JSValue error=JS_GetException(ctx),value=JS_GetPropertyStr(ctx,error,"name");const char *text=JS_ToCString(ctx,value);
    bool matched=text&&!strcmp(text,name);JS_FreeCString(ctx,text);JS_FreeValue(ctx,value);JS_FreeValue(ctx,error);return matched;
}
static JSValue create_native(JSContext *ctx,web_doc *d,int kind,JSValue source,const double *options,JSValue dimensions){
    JSValue args[]={JS_NewString(ctx,"create"),JS_NewInt32(ctx,kind),source,JS_NewArrayBufferCopy(ctx,(const uint8_t *)options,9*sizeof(double)),dimensions};
    JSValue result=web_image_bitmap_native(ctx,d,NULL,5,args);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[3]);return result;
}
static void close_native(JSContext *ctx,JSValue token){
    JSValue args[]={JS_NewString(ctx,"close"),token};JSValue result=web_image_bitmap_native(ctx,NULL,NULL,2,args);
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,args[0]);
}
static void native_lifetimes(void){
    struct bitmap_allocation_state allocation={0};
    JSRuntime *rt=JS_NewRuntime2(&bitmap_allocator,&allocation);JSContext *ctx=rt?JS_NewContext(rt):NULL;
    check("independent runtime allocated",ctx!=NULL);if(!ctx){if(rt)JS_FreeRuntime(rt);return;}
    const uint8_t rgba[4]={17,31,47,255};const double dimensions[3]={1,1,0},normal[9]={0,0,0,0,0,0,0,0,1};
    JSValue data=JS_NewArrayBufferCopy(ctx,rgba,sizeof rgba),shape=JS_NewArrayBufferCopy(ctx,(const uint8_t *)dimensions,sizeof dimensions);
    web_doc *owner=calloc(1,sizeof *owner);JSValue token=create_native(ctx,owner,1,data,normal,shape);
    unsigned w=0,h=0;check("real opaque pixel owner",!JS_IsException(token)&&web_image_bitmap_dimensions(token,&w,&h)&&w==1&&h==1);
    free(owner);owner=NULL;
    check("token survives document arena lifetime",web_image_bitmap_dimensions(token,&w,&h)&&w==1&&h==1);
    close_native(ctx,token);close_native(ctx,token);
    check("close then finalizer is not double-free",web_image_bitmap_dimensions(token,&w,&h)&&!w&&!h);
    JS_FreeValue(ctx,token);JS_RunGC(rt);
    const double big[9]={0,0,0,0,0,256,256,0,0};
    /* Warm the registered class above, and create private argument buffers
       before the measured output. QuickJS, not a document quota, owns pixels. */
    JSMemoryUsage before,allocated,closed,freed;JS_ComputeMemoryUsage(rt,&before);
    token=create_native(ctx,NULL,1,data,big,shape);JS_ComputeMemoryUsage(rt,&allocated);
    check("pixels charged to JS allocator",!JS_IsException(token)&&allocated.malloc_size>=before.malloc_size+256*256*4);
    close_native(ctx,token);JS_ComputeMemoryUsage(rt,&closed);
    check("close releases pixel allocation",allocated.malloc_size>=closed.malloc_size+256*256*4);
    JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    check("finalizer releases opaque owner",freed.malloc_size==before.malloc_size);
    token=create_native(ctx,NULL,1,data,big,shape);JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    check("unclosed finalizer releases pixels",freed.malloc_size==before.malloc_size);
    /* Fault injection is bounded by the real allocator; no 16-MiB production
       limit is invented or reinstated to make this negative case pass. */
    JS_SetMemoryLimit(rt,(size_t)before.malloc_size+512);
    token=create_native(ctx,NULL,1,data,big,shape);JS_SetMemoryLimit(rt,SIZE_MAX);
    check("allocator OOM becomes exception",JS_IsException(token)&&JS_HasException(ctx));
    exception_clear(ctx);JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    check("OOM does not leak partial pixels",freed.malloc_size==before.malloc_size);
    const double oversized[9]={0,0,0,0,0,4294967295.0,2,0,0};token=create_native(ctx,NULL,1,data,oversized,shape);
    check("native dimension overflow rejects",JS_IsException(token)&&exception_named(ctx,"RangeError"));JS_FreeValue(ctx,token);
    /* This local document is not shared with a helper. Keep the fault setup
       portable to the guest TCC, which has no GNU atomic builtins. */
    web_doc cancelled={0};cancelled.native_cancelled=true;
    token=create_native(ctx,&cancelled,1,data,normal,shape);
    check("cancelled snapshot rejects before borrowing",JS_IsException(token)&&exception_named(ctx,"InternalError"));JS_FreeValue(ctx,token);
    uint32_t *large=calloc(128u*128u,sizeof *large);uint8_t *png=NULL;size_t png_size=0;
    bool encoded=large&&png_encode_rgba(large,128,128,128,&png,&png_size)==0&&png_size>=16384;free(large);
    struct web_host host={.script_checkpoint=checkpoint,.js_task_budget_ms=5000};
    web_doc *decode_doc=web_live("<!doctype html>",15,"https://bitmap-cancel.test/","utf-8",&host);
    for(unsigned i=0;decode_doc&&i<8;i++){web_tick(decode_doc,uptime_ms());msleep(1);}
    check("actual helper-decode source prepared",encoded&&decode_doc!=NULL);
    JSValue blob=encoded?JS_NewArrayBufferCopy(ctx,png,png_size):JS_UNDEFINED;free(png);
    if(decode_doc)decode_doc->native_checkpoint_count=62;
    cancel_decode=true;token=create_native(ctx,decode_doc,0,blob,normal,JS_UNDEFINED);cancel_decode=false;
    check("decode join cancellation discards bitmap",decode_doc&&decode_doc->native_cancelled&&JS_IsException(token)&&exception_named(ctx,"InternalError"));
    JS_FreeValue(ctx,token);JS_FreeValue(ctx,blob);if(decode_doc)web_free(decode_doc);
    JS_FreeValue(ctx,data);JS_FreeValue(ctx,shape);JS_FreeContext(ctx);JS_FreeRuntime(rt);
}

/* Only this repaired heap/allocator boundary runs after the original suite.
   Already successful JS/raster/decoder/cancellation cases are not repeated. */
static int heap_contract(void){
    struct bitmap_allocation_state allocation={0};
    JSRuntime *rt=JS_NewRuntime2(&bitmap_allocator,&allocation);JSContext *ctx=rt?JS_NewContext(rt):NULL;
    check("heap exact-sized runtime allocated",ctx!=NULL);if(!ctx){if(rt)JS_FreeRuntime(rt);return 1;}
    const uint8_t rgba[4]={17,31,47,255};const double dimensions[3]={1,1,0},normal[9]={0,0,0,0,0,0,0,0,1},big[9]={0,0,0,0,0,256,256,0,0};
    const size_t pixel_bytes=256u*256u*4u;
    JSValue data=JS_NewArrayBufferCopy(ctx,rgba,sizeof rgba),shape=JS_NewArrayBufferCopy(ctx,(const uint8_t *)dimensions,sizeof dimensions);
    JSValue token=create_native(ctx,NULL,1,data,normal,shape);unsigned w=0,h=0;
    check("heap real snapshot and class warmed",!JS_IsException(token)&&web_image_bitmap_dimensions(token,&w,&h)&&w==1&&h==1);
    JS_FreeValue(ctx,token);exception_clear(ctx);JS_RunGC(rt);
    JSMemoryUsage before,allocated,closed,freed;JS_ComputeMemoryUsage(rt,&before);
    allocation.max_successful_request=0;
    token=create_native(ctx,NULL,1,data,big,shape);JS_ComputeMemoryUsage(rt,&allocated);
    size_t successful_request=allocation.max_successful_request;
    printf("BITMAP-HEAP before=%lu allocated=%lu pixel_bytes=%lu\n",(unsigned long)before.malloc_size,(unsigned long)allocated.malloc_size,(unsigned long)pixel_bytes);
    check("heap pixel bytes actually charged",!JS_IsException(token)&&allocated.malloc_size>=before.malloc_size+(int64_t)pixel_bytes);
    close_native(ctx,token);JS_ComputeMemoryUsage(rt,&closed);
    printf("BITMAP-HEAP closed=%lu released=%lu\n",(unsigned long)closed.malloc_size,(unsigned long)(allocated.malloc_size-closed.malloc_size));
    check("heap close releases actual pixel bytes",allocated.malloc_size>=closed.malloc_size+(int64_t)pixel_bytes);
    JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    printf("BITMAP-HEAP finalized=%lu baseline=%lu\n",(unsigned long)freed.malloc_size,(unsigned long)before.malloc_size);
    check("heap finalizer frees opaque owner",freed.malloc_size==before.malloc_size);
    token=create_native(ctx,NULL,1,data,big,shape);JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    printf("BITMAP-HEAP unclosed_finalized=%lu\n",(unsigned long)freed.malloc_size);
    check("heap unclosed finalizer frees pixels",freed.malloc_size==before.malloc_size);
    allocation.quota_failures=allocation.failed_size=0;
    /* Leave room for argument/error objects, but not the genuine pixel array.
       Confirm the rejected allocation's exact size so this cannot pass merely
       because an earlier transform packet ran out of memory. */
    JS_SetMemoryLimit(rt,(size_t)before.malloc_size+4096);
    token=create_native(ctx,NULL,1,data,big,shape);JS_SetMemoryLimit(rt,SIZE_MAX);
    printf("BITMAP-HEAP oom_failures=%lu failed_size=%lu successful_request=%lu payload=%lu\n",(unsigned long)allocation.quota_failures,(unsigned long)allocation.failed_size,(unsigned long)successful_request,(unsigned long)pixel_bytes);
    check("heap actual pixel allocator OOM rejects",JS_IsException(token)&&JS_HasException(ctx)&&allocation.quota_failures>0&&successful_request>=pixel_bytes&&allocation.failed_size==successful_request);
    exception_clear(ctx);JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    printf("BITMAP-HEAP after_oom=%lu baseline=%lu\n",(unsigned long)freed.malloc_size,(unsigned long)before.malloc_size);
    check("heap partial OOM leaves no allocation",freed.malloc_size==before.malloc_size);
    JS_FreeValue(ctx,data);JS_FreeValue(ctx,shape);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("BITMAP-HEAP live_after_runtime=%lu\n",(unsigned long)allocation.live);
    check("heap all runtime owners freed",allocation.live==0);
    printf("imagebitmapheap: %d checks, %d failed\n",checks,failures);return failures!=0;
}

/* The first sized-heap guest established all other lifetimes. Re-run only the
   corrected OOM request boundary, its cleanup and complete runtime teardown. */
static int heap_oom_contract(void){
    struct bitmap_allocation_state allocation={0};
    JSRuntime *rt=JS_NewRuntime2(&bitmap_allocator,&allocation);JSContext *ctx=rt?JS_NewContext(rt):NULL;
    if(!ctx){if(rt)JS_FreeRuntime(rt);puts("imagebitmapheapoom: runtime allocation failed");return 1;}
    const uint8_t rgba[4]={17,31,47,255};const double dimensions[3]={1,1,0},normal[9]={0,0,0,0,0,0,0,0,1},big[9]={0,0,0,0,0,256,256,0,0};
    const size_t payload=256u*256u*4u;
    JSValue data=JS_NewArrayBufferCopy(ctx,rgba,sizeof rgba),shape=JS_NewArrayBufferCopy(ctx,(const uint8_t *)dimensions,sizeof dimensions);
    JSValue token=create_native(ctx,NULL,1,data,normal,shape);JS_FreeValue(ctx,token);exception_clear(ctx);JS_RunGC(rt);
    JSMemoryUsage before,freed;JS_ComputeMemoryUsage(rt,&before);
    /* Observe the exact callback request for this same successful snapshot.
       QuickJS may add its own block header before invoking the backend. This
       is measured, never encoded as a magic +8 or a loose expected range. */
    allocation.max_successful_request=0;
    token=create_native(ctx,NULL,1,data,big,shape);
    unsigned w=0,h=0;bool usable=!JS_IsException(token)&&web_image_bitmap_dimensions(token,&w,&h)&&w==256&&h==256;
    size_t successful_request=allocation.max_successful_request;
    JS_FreeValue(ctx,token);exception_clear(ctx);JS_RunGC(rt);
    allocation.quota_failures=allocation.failed_size=0;
    JS_SetMemoryLimit(rt,(size_t)before.malloc_size+4096);
    token=create_native(ctx,NULL,1,data,big,shape);JS_SetMemoryLimit(rt,SIZE_MAX);
    printf("BITMAP-HEAP-OOM successful_request=%lu payload=%lu failures=%lu failed_request=%lu\n",(unsigned long)successful_request,(unsigned long)payload,(unsigned long)allocation.quota_failures,(unsigned long)allocation.failed_size);
    check("OOM matches measured successful pixel request",usable&&successful_request>=payload&&JS_IsException(token)&&JS_HasException(ctx)&&allocation.quota_failures>0&&allocation.failed_size==successful_request);
    exception_clear(ctx);JS_FreeValue(ctx,token);JS_RunGC(rt);JS_ComputeMemoryUsage(rt,&freed);
    printf("BITMAP-HEAP-OOM cleanup=%lu baseline=%lu\n",(unsigned long)freed.malloc_size,(unsigned long)before.malloc_size);
    check("OOM cleanup restores actual sized baseline",freed.malloc_size==before.malloc_size);
    JS_FreeValue(ctx,data);JS_FreeValue(ctx,shape);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("BITMAP-HEAP-OOM live_after_runtime=%lu\n",(unsigned long)allocation.live);
    check("OOM runtime frees every allocator owner",allocation.live==0);
    printf("imagebitmapheapoom: %d checks, %d failed\n",checks,failures);return failures!=0;
}

int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--heap-oom"))return heap_oom_contract();
    if(argc>1&&!strcmp(argv[1],"--heap"))return heap_contract();
    native_lifetimes();
    FILE *file=fopen("/data/tests/js_image_bitmap_cases.js","rb");if(!file){puts("imagebitmap: fixture missing");return 1;}
    fseek(file,0,SEEK_END);long length=ftell(file);fseek(file,0,SEEK_SET);char *source=length>0?malloc((size_t)length+1):NULL;
    if(!source||fread(source,1,(size_t)length,file)!=(size_t)length){fclose(file);free(source);return 1;}fclose(file);source[length]=0;
    const uint32_t pixels[]={0xffff0000,0x8000ff00,0xff0000ff,0x00ffffff};uint8_t *png=NULL;size_t png_size=0;
    check("finite native PNG transport prepared",png_encode_rgba(pixels,2,2,2,&png,&png_size)==0);
    struct web_host host={.console=log_line,.request=request,.debug_js=true,.js_task_budget_ms=5000};
    const char page[]="<!doctype html><body></body>";
    web_doc *d=web_live(page,sizeof page-1,"https://bitmap.test/private?private=never-logged","utf-8",&host);
    check("real web_live document allocated",d!=NULL);
    if(d){
        for(unsigned i=0;i<8;i++){web_tick(d,uptime_ms());msleep(1);}web_layout(d,128,128);
        check("new JS boundary evaluated",web_console_eval(d,source,(size_t)length));
        uint64_t deadline=uptime_ms()+12000;
        while(!done&&uptime_ms()<deadline){
            web_tick(d,uptime_ms());
            if(image_request){struct web_response response={.status=200,.body=(char *)png,.body_len=png_size};strcpy(response.url,"https://bitmap.test/fixture.png");web_resource_loaded(d,image_request,&response);image_request=0;}
            msleep(1);
        }
        check("bounded Promise/task completion",done);
        check("no runtime exception",errors==0);
        check("complete new JS boundary",js_checks==84);
        bool once=diagnostics==5;for(unsigned i=0;i<5;i++)once&=seen[i]==1;
        check("debug actual-success source types once",once);
        check("debug logs only type and dimensions",sanitized);
        web_free(d);
    }
    free(png);free(source);
    host.debug_js=false;host.request=NULL;int before=diagnostics;
    d=web_live("<!doctype html>",15,"https://bitmap-off.test/","utf-8",&host);
    check("debug-off document allocated",d!=NULL);
    if(d){
        for(unsigned i=0;i<8;i++){web_tick(d,uptime_ms());msleep(1);}
        const char script[]="createImageBitmap(new ImageData(1,1)).then(bitmap=>{bitmap.close();console.log('BITMAP-OFF-DONE');});";
        check("debug-off native creation started",web_console_eval(d,script,sizeof script-1));
        uint64_t deadline=uptime_ms()+1000;while(!off_done&&uptime_ms()<deadline){web_tick(d,uptime_ms());msleep(1);}
        check("debug-off creates with zero diagnostic logs",off_done&&diagnostics==before);web_free(d);
    }
    printf("imagebitmap: %d checks, %d failed (JS %d)\n",checks,failures,js_checks);
    return failures!=0;
}
