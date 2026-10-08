/* Real web_live -> QuickJS -> native bitmap -> web_paint, not a JS-only mock. */
#include "nocturne.h"
#include "web.h"
#include "gpu.h"
#include <stdio.h>
#include <string.h>

static int checks,failures,errors; static bool done;
static uint32_t pixels[128*128];
static uint64_t image_request;
static bool require_blit,blit_measured;
static struct n_gpu_info blit_before;
/* Finite transport fixture, decoded by the actual image codec. No fake pixels
   or external network/site acceptance: completion only queues native work. */
static const char svg[]="<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'><rect width='1' height='1' fill='#ff0000'/><rect x='1' width='1' height='1' fill='#0000ff'/></svg>";
static bool request_image(void *unused,const struct web_request *r) {
    if (r->kind!=WEB_RESOURCE_IMAGE) return false;
    if (!strcmp(r->url,"https://canvas.test/fixture.svg")) image_request=r->id;
    return true;
}
static void log_line(void *unused,int level,const char *text) {
    if (!strcmp(text,"CANVAS-BLIT-BEGIN")) gpu_info(&blit_before);
    else if (!strcmp(text,"CANVAS-BLIT-END")) {
        struct n_gpu_info after={0};gpu_info(&after); checks++;
        bool available=(blit_before.capabilities&N_GPU_CAP_BLIT)!=0;
        blit_measured=available && after.submissions==blit_before.submissions+1 && after.failures==blit_before.failures;
        if ((available && !blit_measured) || (require_blit && !blit_measured)) {
            printf("FAIL canvas drawImage-native-blit\n"); failures++;
        }
    } else if (!strncmp(text,"OK canvas ",10)) checks++;
    else if (!strncmp(text,"FAIL canvas ",12)) { checks++;failures++;printf("%s\n",text); }
    else if (!strcmp(text,"CANVAS-DONE")) done=true;
    else if (level==2) { errors++;printf("Canvas JS error: %s\n",text); }
}
static const char page[]="<!doctype html><style>body{margin:0}</style><canvas id=c width=128 height=128></canvas><script>"
    "function ck(n,v){console.log((v?'OK canvas ':'FAIL canvas ')+n)}"
    "const c=document.getElementById('c'),ctx=c.getContext('2d');"
    "ck('identity',c instanceof HTMLCanvasElement&&ctx instanceof CanvasRenderingContext2D&&ctx.canvas===c&&ctx===c.getContext('2d'));"
    "ck('not-fake-webgl',c.getContext('webgl')===null&&c.getContext('webgl2')===null);"
    "ck('dimensions',c.width===128&&c.height===128);"
    "ctx.fillStyle='#ff0000';ctx.fillRect(0,0,128,128);let p=ctx.getImageData(3,4,1,1).data;ck('fill-pixel',p[0]===255&&p[1]===0&&p[2]===0&&p[3]===255);"
    "ctx.clearRect(0,0,4,4);p=ctx.getImageData(1,1,1,1).data;ck('clear-alpha',p[3]===0);"
    "ctx.fillStyle='rgba(0,0,255,0.5)';ctx.fillRect(0,0,4,4);p=ctx.getImageData(1,1,1,1).data;ck('straight-alpha',p[0]===0&&p[2]===255&&p[3]>=127&&p[3]<=128);"
    "ctx.globalAlpha=0.5;ctx.fillStyle='#00ff00';ctx.fillRect(8,8,4,4);p=ctx.getImageData(9,9,1,1).data;ck('source-over',p[0]>=126&&p[0]<=129&&p[1]>=127&&p[1]<=129&&p[3]===255);"
    "ctx.save();ctx.globalAlpha=1;ctx.translate(20,20);ctx.fillStyle='#ffffff';ctx.fillRect(0,0,4,4);ctx.restore();p=ctx.getImageData(21,21,1,1).data;ck('translation',p[0]===255&&p[1]===255&&p[2]===255);ck('saved-state',ctx.globalAlpha===0.5&&ctx.fillStyle==='#00ff00');"
    "ctx.globalAlpha=1;ctx.beginPath();ctx.moveTo(30,30);ctx.lineTo(50,30);ctx.lineTo(40,50);ctx.closePath();ctx.fill();p=ctx.getImageData(40,35,1,1).data;ck('path-pixel',p[1]===255&&p[0]===0);"
    "const im=new ImageData(2,2);im.data.set([7,9,11,13]);ctx.putImageData(im,60,60);p=ctx.getImageData(60,60,1,1).data;ck('image-data',p[0]===7&&p[1]===9&&p[2]===11&&p[3]===13);"
    "p=ctx.getImageData(-1,-1,1,1).data;ck('read-outside',p.every(x=>x===0));"
    "c.width=c.width;p=ctx.getImageData(60,60,1,1).data;ck('reset-same-width',p.every(x=>x===0)&&ctx.fillStyle==='#000000'&&ctx.globalAlpha===1);"
    "c.setAttribute('height','128');ck('attribute-reset',ctx.fillStyle==='#000000');"
    "ctx.fillStyle='#123456';c.setAttributeNS('urn:canvas-test','width','1');ck('namespace-dimension-isolation',c.width===128&&ctx.fillStyle==='#123456');"
    "c.setAttributeNS(null,'WIDTH','2');ck('exact-case-dimension-isolation',c.width===128&&ctx.fillStyle==='#123456');"
    "c.setAttributeNS(null,'width','128');ck('null-namespace-reset',c.width===128&&ctx.fillStyle==='#000000');"
    "ctx.fillStyle='#123456';ctx.fillRect(1,1,2,2);ctx.reset();ck('explicit-reset',ctx.fillStyle==='#000000'&&ctx.getImageData(1,1,1,1).data.every(x=>x===0));"
    "const clone=c.cloneNode();ck('clone-no-bitmap',clone.getContext('2d').getImageData(0,0,1,1).data.every(x=>x===0));"
    "const zero=document.createElement('canvas');zero.width=0;ck('zero-canvas',zero.width===0&&zero.getContext('2d')!==null);"
    "const big=document.createElement('canvas');big.width=65536;big.height=65536;ck('memory-bound',big.getContext('2d')===null);"
    "let caught=false;try{CanvasRenderingContext2D.prototype.fillRect.call({},0,0,1,1)}catch(e){caught=e instanceof TypeError}ck('native-brand',caught);"
    "const s=document.createElement('canvas');s.width=4;s.height=1;const sx=s.getContext('2d'),source=new ImageData(new Uint8ClampedArray([255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255]),4,1);sx.putImageData(source,0,0);"
    "const q=document.createElement('canvas');q.width=4;q.height=2;const qx=q.getContext('2d');qx.imageSmoothingEnabled=false;"
    "console.log('CANVAS-BLIT-BEGIN');qx.drawImage(s,0,0);console.log('CANVAS-BLIT-END');p=qx.getImageData(2,0,1,1).data;ck('draw-three',p[2]===255&&p[3]===255);"
    "qx.clearRect(0,0,4,2);qx.drawImage(s,0,0,2,1);p=qx.getImageData(0,0,2,1).data;ck('draw-five',p[1]===255&&p[4]===255&&p[5]===255&&p[6]===255);"
    "qx.clearRect(0,0,4,2);qx.drawImage(s,-1,0,4,1,0,0,4,1);p=qx.getImageData(0,0,4,1).data;ck('draw-crop-proportional',p[3]===0&&p[4]===255&&p[9]===255&&p[14]===255);"
    "qx.clearRect(0,0,4,2);qx.drawImage(s,3,0,-3,1,3,0,-3,1);p=qx.getImageData(0,0,3,1).data;ck('draw-negative-no-flip',p[0]===255&&p[5]===255&&p[10]===255);"
    "sx.imageSmoothingEnabled=false;sx.drawImage(s,0,0,3,1,1,0,3,1);p=sx.getImageData(0,0,4,1).data;ck('draw-self-snapshot',p[0]===255&&p[4]===255&&p[9]===255&&p[14]===255);sx.putImageData(source,0,0);"
    "q.height=4;qx.imageSmoothingEnabled=false;qx.setTransform(2,0,0,2,0,0);qx.drawImage(s,1,0,2,1,0,1,2,1);p=qx.getImageData(0,2,4,2).data;ck('draw-nearest-axis-scale',p[1]===255&&p[5]===255&&p[10]===255&&p[14]===255&&p[17]===255&&p[30]===255&&qx.getImageData(0,1,1,1).data[3]===0);q.height=2;"
    "qx.fillStyle='#ffffff';qx.fillRect(0,0,4,2);qx.setTransform(-1,0,0,1,4,1);qx.globalAlpha=0.5;qx.drawImage(s,0,0);p=qx.getImageData(1,1,1,1).data;ck('draw-transform-alpha',p[0]>=126&&p[0]<=128&&p[1]>=126&&p[1]<=128&&p[2]===255&&p[3]===255&&qx.globalCompositeOperation==='source-over');qx.reset();"
    "const edge=document.createElement('canvas');edge.width=2;edge.height=1;edge.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray([255,0,0,255,0,0,255,0]),2,1),0,0);qx.drawImage(edge,0,0,4,1);p=qx.getImageData(1,0,1,1).data;ck('draw-premultiplied-smoothing',p[0]===255&&p[2]===0&&p[3]>=190&&p[3]<=192);"
    "const dataimg=new Image();dataimg.src='data:image/svg+xml,'+encodeURIComponent(\"<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'><rect width='2' height='1' fill='#00ff00'/></svg>\");qx.reset();qx.drawImage(dataimg,0,0);p=qx.getImageData(0,0,1,1).data;ck('draw-decoded-image',dataimg.naturalWidth===2&&p[1]===255&&p[3]===255);"
    "const pending=new Image();pending.src='https://canvas.test/pending.svg';qx.drawImage(pending,0,0);p=qx.getImageData(0,0,1,1).data;ck('draw-pending-no-op',p[1]===255&&p[3]===255);"
    "const broken=new Image();broken.src='data:image/png,broken';caught=false;try{qx.drawImage(broken,0,0)}catch(e){caught=e instanceof DOMException&&e.name==='InvalidStateError'}ck('draw-broken-state',caught);"
    "caught=false;try{qx.drawImage(zero,0,0)}catch(e){caught=e instanceof DOMException&&e.name==='InvalidStateError'}ck('draw-empty-canvas-state',caught);"
    "caught=false;try{qx.drawImage({width:2,height:1},0,0)}catch(e){caught=e instanceof TypeError}ck('draw-source-brand',caught);"
    "const quota=Array.from({length:3},()=>{const b=document.createElement('canvas');b.width=1024;b.height=1024;b.getContext('2d');return b});caught=false;try{quota[0].getContext('2d').drawImage(quota[0],0,0,1,1,0,0,1,1)}catch(e){caught=e instanceof RangeError}ck('draw-snapshot-quota',caught);quota[1].width=0;quota[2].width=0;quota[0].getContext('2d').drawImage(quota[0],0,0,1,1,0,0,1,1);ck('draw-snapshot-release',quota[0].getContext('2d').getImageData(0,0,1,1).data.every(x=>x===0));quota[0].width=0;"
    "function finish(){ctx.fillStyle='#e02040';ctx.fillRect(0,0,128,128);console.log('CANVAS-DONE')}"
    "const httpimg=new Image();httpimg.onload=()=>{qx.reset();qx.drawImage(httpimg,0,0);caught=false;try{qx.getImageData(0,0,1,1)}catch(e){caught=e instanceof DOMException&&e.name==='SecurityError'}ck('draw-http-taint',caught);"
    "const taintcopy=document.createElement('canvas'),tx=taintcopy.getContext('2d');tx.drawImage(q,0,0);caught=false;try{tx.getImageData(0,0,1,1)}catch(e){caught=e instanceof DOMException&&e.name==='SecurityError'}ck('draw-taint-propagation',caught);"
    "qx.clearRect(0,0,4,2);qx.putImageData(new ImageData(1,1),0,0);caught=false;try{qx.getImageData(0,0,1,1)}catch(e){caught=e.name==='SecurityError'}ck('draw-clear-does-not-untaint',caught);"
    "q.width=q.width;ck('draw-size-reset-untaints',qx.getImageData(0,0,1,1).data.every(x=>x===0));tx.reset();ck('draw-reset-untaints',tx.getImageData(0,0,1,1).data.every(x=>x===0));finish()};httpimg.onerror=()=>{ck('draw-http-fixture',false);finish()};httpimg.src='https://canvas.test/fixture.svg';</script>";
int main(int argc,char **argv) {
    for (int i=1;i<argc;i++) if (!strcmp(argv[i],"--require-blit")) require_blit=true;
    struct n_gpu_info before={0},after={0}; gpu_info(&before);
    struct web_host host={.console=log_line,.request=request_image};
    web_doc *doc=web_live(page,strlen(page),"https://canvas.test/","utf-8",&host);
    if (!doc) return 1;
    web_layout(doc,128,128);
    uint64_t deadline=uptime_ms()+6000;
    while (!done && uptime_ms()<deadline) {
        web_tick(doc,uptime_ms());
        if (image_request) {
            struct web_response r={.status=200,.body=(char *)svg,.body_len=sizeof svg-1};
            strcpy(r.url,"https://canvas.test/fixture.svg");
            web_resource_loaded(doc,image_request,&r); image_request=0;
        }
        msleep(1);
    }
    canvas_t c;gfx_init(&c,pixels,128,128,128);web_layout(doc,128,128);web_paint(doc,&c,0,0,128,128,0,0);
    bool painted=pixels[16*128+16]==0xffe02040;
    if (!painted) { printf("FAIL canvas native-paint got=%x\n",pixels[16*128+16]);failures++; }
    gpu_info(&after);
    if ((before.capabilities&N_GPU_CAP_CLEAR) && after.submissions<=before.submissions) { printf("FAIL canvas GPU-not-used\n");failures++; }
    web_free(doc);
    if (!done || checks<43 || errors) failures++;
    printf("canvastest: %d checks, %d failed, %d errors, painted=%d GPU submissions=%lu\n",checks,failures,errors,painted,(unsigned long)(after.submissions-before.submissions));
    printf("Canvas drawImage native blit=%u required=%u\n",(unsigned)blit_measured,(unsigned)require_blit);
    return failures!=0;
}
