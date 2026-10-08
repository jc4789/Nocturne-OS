/* A real native GUI consumer. Status distinguishes verified virgl from RAM. */
#include "nocturne.h"
#include "gpu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    window_t *win=win_open(400,400,"Nocturne 3D",WIN_CENTER);
    if (!win) { puts("No GUI display");return 1; }
    struct n_gpu_info info={0};gpu_info(&info);
    struct n_gpu_batch r={.width=400,.height=400,.triangle_count=4,.clear_argb=RGB(22,20,42)};
    unsigned colors[3]={RGB(250,90,130),RGB(80,230,150),RGB(90,130,250)};
    while (!win->closed) {
        double angle=uptime_ms()/1800.0;
        for (int t=0;t<4;t++) for (int i=0;i<3;i++) {
            double a=angle*(t&1?-1:1)+i*M_PI*2/3;
            int x=(t&1)?23000:-23000, y=(t&2)?23000:-23000;
            r.vertex[t*3+i]=(struct n_gpu_vertex){x+(int)(cos(a)*19000),y+(int)(sin(a)*19000),0,65536,colors[i]};
        }
        int backend=gfx_render3d_batch(&win->c,&r);
        const char *name=backend==N_GPU_BACKEND_VIRGL?"virgl verified batched 3D":"CPU software 3D fallback";
        gfx_text(&win->c,12,12,name,RGB(240,235,250),0,FONT_SMALL);
        gfx_text(&win->c,12,34,"4 triangles / one frame; not WebGL",RGB(170,165,190),0,FONT_SMALL);
        win_update(win);
        struct gui_event event;if (win_event(win,&event,33)>0 && event.type==EV_KEY && event.key==NKEY_ESC) break;
    }
    win_close(win);return 0;
}
