#include <nocturne.h>
#include <stdio.h>
#include <string.h>
#include "browser_events.h"
static int checks,failed;
static void check(const char *name,bool pass){checks++;if(!pass){failed++;printf("FAIL browserdeferred %s\n",name);}}
int main(void){
    struct gui_event q[8]={{.type=EV_MOUSE_MOVE,.x=10,.y=20}},e={.type=EV_MOUSE_MOVE,.x=40,.y=50};
    check("empty queue cannot merge",!browser_coalesce_hover(NULL,0,&e,true));
    check("consecutive hover merges",browser_coalesce_hover(q,1,&e,true));
    check("latest complete native position retained",q[0].x==40&&q[0].y==50&&q[0].buttons==0);
    int types[]={EV_MOUSE_DOWN,EV_MOUSE_UP,EV_KEY,EV_WHEEL,EV_UNFOCUS,EV_CLOSE,EV_RESIZE};
    for(unsigned i=0;i<sizeof types/sizeof *types;i++){
        q[0].type=types[i];struct gui_event before=q[0];
        check("non-move FIFO boundary retained",!browser_coalesce_hover(q,1,&e,true)&&!memcmp(q,&before,sizeof before));
    }
    q[0]=(struct gui_event){.type=EV_MOUSE_MOVE,.buttons=1};
    check("pressed prior movement retained",!browser_coalesce_hover(q,1,&e,true));
    q[0].buttons=0;e.buttons=1;check("new pressed drag retained",!browser_coalesce_hover(q,1,&e,true));
    e.buttons=0;e.mods=NMOD_SHIFT;check("modifier transition retained",!browser_coalesce_hover(q,1,&e,true));
    q[0].mods=NMOD_SHIFT;check("unchanged modifiers may merge",browser_coalesce_hover(q,1,&e,true));
    q[1]=(struct gui_event){.type=EV_MOUSE_DOWN,.x=71};q[2]=e;e.x=99;
    check("only tail hover changes",browser_coalesce_hover(q,3,&e,true)&&q[1].type==EV_MOUSE_DOWN&&q[1].x==71&&q[2].x==99);
    e.type=EV_MOUSE_UP;struct gui_event before=q[2];
    check("release never replaces hover",!browser_coalesce_hover(q,3,&e,true)&&!memcmp(&q[2],&before,sizeof before));
    e.type=EV_MOUSE_MOVE;q[0]=e;
    for(unsigned i=0;i<sizeof types/sizeof *types;i++){
        bool eligible=true;struct gui_event consumed={.type=types[i],.key=NKEY_F12};
        before=q[0];browser_hover_boundary(&eligible,&consumed);
        check("consumed chrome boundary forbids merge",!eligible&&!browser_coalesce_hover(q,1,&e,eligible)&&!memcmp(q,&before,sizeof before));
    }
    bool eligible=true;browser_hover_boundary(&eligible,&e);
    check("ordinary hover retains continuity",eligible);
    e.buttons=1;browser_hover_boundary(&eligible,&e);check("consumed drag invalidates continuity",!eligible);
    check("pressed event cannot establish hover tail",!browser_hover_tail(&e));
    e.buttons=0;check("new queued hover establishes continuity",browser_hover_tail(&e));
    printf("browserdeferred: %d checks, %d failed\n",checks,failed);return failed!=0;
}
