/* Continuous author ResizeObserver feedback must not monopolize task selection.
 * Slow native ticks reproduce an observer that is always due; timers/rAF/posted
 * tasks must still create and update a real native Canvas2D bitmap. */
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <string.h>

static int checks,failures,errors;static bool done;
static void log_line(void *unused,int level,const char *text){
    if(!strncmp(text,"OK ",3)){checks++;puts(text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;puts(text);}
    else if(!strcmp(text,"OBSERVER-FAIRNESS-DONE"))done=true;
    else if(level>=2){errors++;printf("ERROR %s\n",text);}
}
static const char page[]=
    "<!doctype html><style>html,body{margin:0}#watched{width:100px;height:40px}</style><div id='watched'></div><script>"
    "let observing=true,observerCount=0,timerCount=0,postedCount=0,rafCount=0;let timerDuring=false,postedDuring=false,rafDuring=false;"
    "const watched=document.querySelector('#watched'),channel=new MessageChannel();"
    "function check(n,v){console.log((v?'OK ':'FAIL ')+n);}"
    "channel.port1.onmessage=()=>{postedCount++;postedDuring=observing&&observerCount>0;};"
    "const ro=new ResizeObserver(()=>{observerCount++;queueMicrotask(()=>{watched.style.width=(100+observerCount)+'px';});channel.port2.postMessage(observerCount);});ro.observe(watched);"
    "function draw(){rafCount++;rafDuring=observing&&observerCount>0;let canvas=document.querySelector('#native-hero');"
    "if(!canvas){canvas=document.createElement('canvas');canvas.id='native-hero';canvas.width=4;canvas.height=4;document.body.appendChild(canvas);}"
    "const context=canvas.getContext('2d');context.fillStyle='#00ff00';context.fillRect(0,0,4,4);if(rafCount<6)requestAnimationFrame(draw);}requestAnimationFrame(draw);"
    "const timer=setInterval(()=>{timerCount++;timerDuring=observing&&observerCount>0;"
    "if(observerCount<6||rafCount<6||timerCount<6||postedCount<6)return;"
    "check('continuous-observer-runs',observerCount>=6);check('ordinary-timer-progress',timerCount>=6);check('posted-task-progress',postedCount>=6);check('animation-frame-progress',rafCount>=6);"
    "check('timer-during-observer-feedback',timerDuring);check('posted-during-observer-feedback',postedDuring);check('raf-during-observer-feedback',rafDuring);"
    "const canvas=document.querySelector('#native-hero');check('raf-created-native-canvas',canvas instanceof HTMLCanvasElement);"
    "const pixel=canvas.getContext('2d').getImageData(1,1,1,1).data;check('raf-native-canvas-pixel',pixel[0]===0&&pixel[1]===255&&pixel[2]===0&&pixel[3]===255);"
    "check('observer-actual-geometry-update',watched.getBoundingClientRect().width===100+observerCount);"
    "ro.disconnect();observing=false;clearInterval(timer);channel.port1.close();channel.port2.close();"
    "const before=observerCount;setTimeout(()=>{check('disconnected-observer-stops',observerCount===before);console.log('OBSERVER-FAIRNESS-DONE');},0);},25);"
    "</script>";
int main(void){
    struct web_host host={.console=log_line,.js_task_budget_ms=5000};
    web_doc *doc=web_live(page,sizeof page-1,"https://observer-fairness.test/","utf-8",&host);if(!doc)return 1;
    web_layout(doc,400,300);uint64_t deadline=uptime_ms()+5000;
    while(!done&&uptime_ms()<deadline){web_tick(doc,uptime_ms());msleep(20);}
    if(!done||checks!=11||errors){printf("FAIL observer fairness completion done=%d checks=%d errors=%d\n",done,checks,errors);failures++;}
    web_free(doc);printf("observerfairnesstest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
