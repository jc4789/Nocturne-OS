/* Real indexed window surfaces, release notification and retained screen-grab
   path. Arguments choose the workload and visual observation duration. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static bool wait_frame(window_t *win, unsigned submitted) {
    struct gui_event event;
    while (win_event(win,&event,-1)>0) {
        if (event.type==EV_CLOSE) return false;
        if (event.type==EV_FRAME)
            return event.key==submitted && event.x==(int)(submitted^1);
    }
    return false;
}

int main(int argc,char **argv) {
    if (argc!=3) return 1;
    unsigned long frames=strtoul(argv[1],NULL,10),hold=strtoul(argv[2],NULL,10);
    if (!frames) return 1;
    int sw,sh,gw,gh;
    if (screen_size(&sw,&sh)<0 || screen_grab(NULL,0,&gw,&gh)<0) return 1;
    window_t *win=win_open(sw/2,sh/2,"二重バッファ: 解放通知・画素確認",WIN_DOUBLE_BUFFER);
    if (!win) return 1;
    win_move(win,sw/4,sh/4);
    uint32_t *screen=malloc((size_t)gw*gh*sizeof *screen);
    int result=1;
    if (!screen) goto done;
    uint64_t start=uptime_ms();
    for (unsigned long frame=0;frame<frames;frame++) {
        if (win_begin_frame(win)<0) goto done;
        unsigned submitted=win->buffer_index;
        uint32_t color=RGB(frame&255,(frame>>8)&255,255-(frame&255));
        gfx_fill(&win->c,0,0,win->w,win->h,color);
        win_update(win);
        if (win->frame_acquired || win->c.px || !wait_frame(win,submitted)) goto done;
        /* A retained front must never be accepted as a new writable frame. */
        if (__syscall(SYS_WIN_PRESENT,win->handle,0,0,0,submitted+1)!=-EBUSY ||
            __syscall(SYS_WIN_PRESENT,win->handle,0,0,0,WIN_BUFFER_COUNT+1)!=-EINVAL ||
            __syscall(SYS_WIN_PRESENT,win->handle,0,0,0,0)!=-EINVAL) goto done;
        if (win_begin_frame(win)<0) goto done;
        /* Unsubmitted writes to the other surface must not change scanout or
           SYS_SCREEN_GRAB. This is the BSP/AP mutable-pixel failure control. */
        gfx_fill(&win->c,0,0,win->w,win->h,RGB(0,255,0));
        if (screen_grab(screen,(size_t)gw*gh*sizeof *screen,&gw,&gh)<0) goto done;
        int x=sw/4+1+win->w/3,y=sh/4+28+win->h/3;
        if (screen[(size_t)y*gw+x]!=color) {
            printf("wmframetest: FAIL frame=%lu got=%x expected=%x\n",frame,screen[(size_t)y*gw+x],color);
            goto done;
        }
    }
    /* The library reallocates both mappings on EV_RESIZE and must not hand
       the caller a batched release belonging to the retired surfaces. */
    for (unsigned restored=0;restored<2;restored++) {
        if (win_fullscreen(win,!restored)<0) goto done;
        struct gui_event event;
        do {
            if (win_event(win,&event,-1)<=0 || event.type==EV_CLOSE) goto done;
        } while (event.type!=EV_RESIZE);
        if (win_begin_frame(win)<0) goto done;
        unsigned submitted=win->buffer_index;
        uint32_t color=restored?RGB(24,96,180):RGB(180,96,24);
        gfx_fill(&win->c,0,0,win->w,win->h,color);
        win_update(win);
        if (!wait_frame(win,submitted) || screen_grab(screen,(size_t)gw*gh*sizeof *screen,&gw,&gh)<0) goto done;
        int x=restored?sw/4+1+win->w/3:gw/3;
        int y=restored?sh/4+28+win->h/3:gh/3;
        if (screen[(size_t)y*gw+x]!=color) goto done;
    }
    printf("wmframetest: PASS fullscreen-resize-restore mappings=both\n");
    /* MAP index one is legal before index zero: its alias must also retire
       when resize allocates replacement surfaces. */
    long fd=__syscall(SYS_WIN_CREATE,sw/4,sh/4,(long)"副バッファ先行割当",WIN_DOUBLE_BUFFER|WIN_NO_DECOR,0);
    if (fd<0) goto done;
    long second=__syscall(SYS_WIN_MAP,fd,1,0,0,0);
    long resized=second>0?__syscall(SYS_WIN_RESIZE,fd,sw/3,sh/3,0,0):-1;
    n_handle_close((n_handle)fd);
    if (resized<0) goto done;
    printf("wmframetest: PASS second-only-map resize destroy\n");
    printf("wmframetest: PASS frames=%lu elapsed_ms=%lu indexed-copy-free=1 releases=checked other-buffer=isolated screen-grab=correct\n",frames,(unsigned long)(uptime_ms()-start));
    printf("wmframetest: READY visual hold_ms=%lu\n",hold);
    msleep(hold);
    result=0;
done:
    free(screen);win_close(win);
    printf(result?"wmframetest: FAIL\n":"wmframetest: PASS destroy\n");
    return result;
}
