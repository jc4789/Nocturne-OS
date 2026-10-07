/* Real web_live -> QuickJS -> native bitmap -> web_paint, not a JS-only mock. */
#include "nocturne.h"
#include "web.h"
#include "gpu.h"
#include <stdio.h>
#include <string.h>

static int checks,failures,errors; static bool done;
static uint32_t pixels[128*128];
static void log_line(void *unused,int level,const char *text) {
    if (!strncmp(text,"OK canvas ",10)) checks++;
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
    "ctx.fillStyle='#e02040';ctx.fillRect(0,0,128,128);console.log('CANVAS-DONE');</script>";
int main(void) {
    struct n_gpu_info before={0},after={0}; gpu_info(&before);
    struct web_host host={.console=log_line};
    web_doc *doc=web_live(page,strlen(page),"https://canvas.test/","utf-8",&host);
    if (!doc) return 1;
    web_layout(doc,128,128);
    uint64_t deadline=uptime_ms()+6000;
    while (!done && uptime_ms()<deadline) { web_tick(doc,uptime_ms());msleep(1); }
    canvas_t c;gfx_init(&c,pixels,128,128,128);web_layout(doc,128,128);web_paint(doc,&c,0,0,128,128,0,0);
    bool painted=pixels[16*128+16]==0xffe02040;
    if (!painted) { printf("FAIL canvas native-paint got=%x\n",pixels[16*128+16]);failures++; }
    gpu_info(&after);
    if ((before.capabilities&N_GPU_CAP_CLEAR) && after.submissions<=before.submissions) { printf("FAIL canvas GPU-not-used\n");failures++; }
    web_free(doc);
    if (!done || checks<22 || errors) failures++;
    printf("canvastest: %d checks, %d failed, %d errors, painted=%d GPU submissions=%lu\n",checks,failures,errors,painted,(unsigned long)(after.submissions-before.submissions));
    return failures!=0;
}
