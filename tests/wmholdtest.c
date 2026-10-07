/* 実28窓を保持する診断負荷。MediaPlayer/HMP/音声は親harnessが所有する。 */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>

#define WINDOWS 28
#define HOLD_MS 40000
static uint32_t color(unsigned id) { return RGB(23+id*7,41+id*5,67+id*3); }

int main(void) {
    window_t *wins[WINDOWS]={0};
    uint32_t *pixels=NULL;
    int sw,sh,gw,gh,result=1;
    if (screen_size(&sw,&sh)<0 || sw<640 || sh<400 || screen_grab(NULL,0,&gw,&gh)<0) return 1;
    pixels=malloc((size_t)gw*gh*4);
    if (!pixels) return 1;
    int width=MIN(sw-190,900),height=MIN(sh-90,600);
    for (unsigned i=0;i<WINDOWS;i++) {
        wins[i]=win_open(width,height,"WM診断: HMP title drag / real shared RAM",0);
        if (!wins[i]) { printf("wmholdtest: FAIL capacity=%u\n",i); goto cleanup; }
        win_move(wins[i],40+(int)(i%4)*19,30+(int)(i%5)*13);
        gfx_fill(&wins[i]->c,0,0,width,height,color(i));
        win_update(wins[i]);
    }
    msleep(200);
    if (screen_grab(pixels,(size_t)gw*gh*4,&gw,&gh)<0) goto cleanup;
    int x=40+27%4*19,y=30+27%5*13;
    if (pixels[(size_t)(y+28+90)*gw+x+1+80]!=color(27)) {
        printf("wmholdtest: FAIL initial client pixel\n"); goto cleanup;
    }
    printf("wmholdtest: READY windows=28 hold_ms=%u title_x=%d title_y=%d client_x=%d client_y=%d client_w=%d client_h=%d screen_w=%d screen_h=%d\n",
           (unsigned)HOLD_MS,x+width/2,y+14,x+1,y+28,width,height,gw,gh);
    /* 保持中はuserを眠らせる。dummy忙しいloop/追加damage/serial出力なし。 */
    msleep(HOLD_MS);
    if (screen_grab(pixels,(size_t)gw*gh*4,&gw,&gh)<0) goto cleanup;
    unsigned found=0;
    for (size_t i=0;i<(size_t)gw*gh;i++) if (pixels[i]==color(27)) found++;
    if (found<64) { printf("wmholdtest: FAIL final client not visible\n"); goto cleanup; }
    printf("wmholdtest: HOLD_END windows=28 visible_pixels=%u\n",found);
    result=0;
cleanup:
    for (unsigned i=0;i<WINDOWS;i++) if (wins[i]) win_close(wins[i]);
    free(pixels);
    printf(result ? "wmholdtest: FAIL cleaned partial workload\n" : "wmholdtest: PASS destroy after hold\n");
    return result;
}
