/* Real window/shared-buffer/compositor route, not an offscreen-only fixture. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t color(unsigned id,int x,int y) {
    return RGB((id*29+x/9)&255,(id*53+y/7)&255,(id*71+(x+y)/11)&255);
}
static void paint(window_t *w,unsigned id) {
    for (int y=0;y<w->h;y++) for (int x=0;x<w->w;x++)
        w->c.px[(size_t)y*w->c.pitch+x]=color(id,x,y);
    win_update(w);
}
static int check_client(uint32_t *screen,int pitch,int sw,int sh,
                        unsigned id,int x,int y) {
    const int offsets[][2]={{80,90},{113,131},{157,179}};
    if (screen_grab(screen,(size_t)sw*sh*4,&sw,&sh)<0) return 1;
    for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        int px=offsets[i][0],py=offsets[i][1];
        uint32_t actual=screen[(size_t)(y+28+py)*pitch+x+1+px];
        if (actual!=color(id,px,py)) {
            printf("wmramtest: client mismatch id=%u xy=%d,%d got=%x expected=%x\n",
                   id,px,py,actual,color(id,px,py));return 1;
        }
    }
    return 0;
}
int main(int argc,char **argv) {
    bool many=argc==2 && !strcmp(argv[1],"--many");
    if (argc>1 && !many) return 1;
    int sw,sh;
    if (screen_size(&sw,&sh)<0 || sw<640 || sh<400) return 1;
    /* screen_size excludes the 40px taskbar; grab reports the complete screen. */
    int gw,gh;if (screen_grab(NULL,0,&gw,&gh)<0) return 1;
    uint32_t *pixels=malloc((size_t)gw*gh*4);
    if (!pixels) return 1;
    struct n_cpuinfo before={0},after={0};cpu_info(&before);
    /* MAX_FDS is 32: stdio uses three; leave one slot spare. The separate
       kernel fixture covers 64 layers, not this single-process GUI workload. */
    window_t *wins[28]={0};
    unsigned count=many ? 28 : 16;
    int result=1;
    int width=sw-190,height=sh-90;
    if (width>900) width=900;if (height>600) height=600;
    for (unsigned i=0;i<count;i++) {
        wins[i]=win_open(width,height,"RAM compositor: real overlapping windows",WIN_RESIZABLE);
        if (!wins[i]) { printf("wmramtest: FAIL capacity at %u windows\n",i);goto cleanup; }
        win_move(wins[i],40+(int)(i%4)*19,30+(int)(i%5)*13);
        paint(wins[i],i+1);
    }
    msleep(150);
    if (many) {
        int last=(int)count-1;
        if (check_client(pixels,gw,gw,gh,count,40+(last%4)*19,30+(last%5)*13)) goto cleanup;
        printf("wmramtest: many-stage PASS real-windows=28 client-pixels=3\n");
        for (unsigned i=count;i>16;) { --i;win_close(wins[i]);wins[i]=NULL; }
        msleep(150);
    }
    int x=40+3*19,y=30;
    if (check_client(pixels,gw,gw,gh,16,x,y)) goto cleanup;
    if (many) printf("wmramtest: many-stage PASS after-drop=16 client-pixels=3\n");
    for (unsigned round=0;round<12;round++) {
        x=35+(int)round*7;y=33+(int)(round%4)*9;
        win_move(wins[15],x,y);
        win_set_title(wins[15],round&1 ? "RAM CPU / moving, immutable title snapshot" : "RAM CPU / overlap and shadow");
        paint(wins[15],16);
        msleep(35);
        if (check_client(pixels,gw,gw,gh,16,x,y)) goto cleanup;
    }
    /* Reallocation frees the previous source immediately after join. */
    long address=__syscall(SYS_WIN_RESIZE,wins[15]->fd,width-37,height-29,0,0);
    if (address<0) goto cleanup;
    wins[15]->w=width-37;wins[15]->h=height-29;
    gfx_init(&wins[15]->c,(uint32_t *)address,wins[15]->w,wins[15]->h,wins[15]->w);
    paint(wins[15],16);msleep(100);
    if (check_client(pixels,gw,gw,gh,16,x,y)) goto cleanup;
    cpu_info(&after);
    if (after.worker_cpus && after.parallel_jobs<=before.parallel_jobs) {
        printf("wmramtest: workers did not execute real compositor work\n");goto cleanup;
    }
    printf("wmramtest: PASS real-windows=16 move=12 resize=1 client-pixels=42 CPUs=%u jobs=%lu chunks=%lu\n",
           after.online_cpus,(unsigned long)(after.parallel_jobs-before.parallel_jobs),
           (unsigned long)(after.worker_chunks-before.worker_chunks));
    result=0;
cleanup:
    for (unsigned i=0;i<28;i++) if (wins[i]) win_close(wins[i]);
    msleep(50);free(pixels);
    printf(result ? "wmramtest: FAIL cleaned partial workload\n" : "wmramtest: PASS destroy after synchronous joins\n");
    return result;
}
