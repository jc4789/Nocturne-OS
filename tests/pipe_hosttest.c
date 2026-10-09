/* Exact production pipe functions; host IRQ/scheduler shims are supporting
 * checks, never Nocturne throughput or real-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pipe_host_shim/fs/vfs.h"
static unsigned checks,failed,irq_depth,polls,waits;
static void check(bool ok,const char *name) { checks++;if(!ok){failed++;fprintf(stderr,"FAIL %s\n",name);} }
uint64_t irq_save(void) { unsigned old=irq_depth;irq_depth++;return old; }
void irq_restore(uint64_t old) { irq_depth=(unsigned)old; }
struct task fake_task;
struct task *current_task=&fake_task;
static void (*wait_hook)(struct wait_queue *);
void wq_wait(struct wait_queue *q) { check(irq_depth>0,"wait condition is IRQ protected");waits++;if(!wait_hook){fprintf(stderr,"unexpected wait\n");exit(2);}wait_hook(q); }
void wq_wake_all(struct wait_queue *q) { check(irq_depth>0,"peer wake is IRQ protected");q->wakes++; }
void poll_notify(void) { check(irq_depth>0,"poll wake is IRQ protected");polls++; }
size_t strlcpy(char *d,const char *s,size_t n) { size_t z=strlen(s);if(n){size_t k=z<n-1?z:n-1;memcpy(d,s,k);d[k]=0;}return z; }
struct file *vfs_open_vnode(struct vnode *v,int flags) { struct file *f=calloc(1,sizeof *f);if(f){f->vn=v;f->flags=flags;}return f; }
/* Use the kernel's rep movsb rather than host CRT memcpy in both variants. */
static void *pipe_copy(void *d,const void *s,size_t n) { void *out=d;__asm__ volatile("rep movsb" : "+D"(d),"+S"(s),"+c"(n) : : "memory");return out; }
#define memcpy pipe_copy
#ifdef PIPE_TEST_BASELINE
#include "../build/goal-20261009/pipe-before.c"
#else
#include "../kernel/src/fs/pipe.c"
#endif
#undef memcpy
static struct vnode *hook_v;
static struct file *hook_rd,*hook_wr;
static uint8_t input[PIPE_SIZE*2+13],output[PIPE_SIZE*2+13];
static uint32_t random_state=0x172391a7;
static uint32_t random_u32(void) { uint32_t x=random_state;x^=x<<13;x^=x>>17;x^=x<<5;return random_state=x; }
static void supply(struct wait_queue *q) { (void)q;wait_hook=NULL;check(pipe_write(hook_v,hook_wr,input,0,7)==7,"blocked reader supplied"); }
static void drain(struct wait_queue *q) { (void)q;wait_hook=NULL;check(pipe_read(hook_v,hook_rd,output,0,PIPE_SIZE)==PIPE_SIZE,"blocked writer drained"); }
static void reset(struct pipe *p) { memset(p,0,sizeof *p);p->readers=p->writers=1; }
int main(void) {
    struct pipe p;reset(&p);struct vnode v={.priv=&p};struct file rd={.flags=O_RDONLY|O_NONBLOCK},wr={.flags=O_WRONLY|O_NONBLOCK};
    hook_v=&v;hook_rd=&rd;hook_wr=&wr;
    for(size_t i=0;i<sizeof input;i++)input[i]=(uint8_t)(i*73+19);
    check(pipe_read(&v,&rd,output,0,1)==-EAGAIN,"empty nonblocking read");
    check(pipe_write(&v,&wr,input,0,sizeof input)==PIPE_SIZE,"nonblocking full partial write");
    check(!pipe_can_write(&v,&wr)&&pipe_can_read(&v,&rd),"full readiness");
    check(pipe_write(&v,&wr,input,0,1)==-EAGAIN,"full nonblocking write");
    check(pipe_read(&v,&rd,output,0,91)==91&&!memcmp(input,output,91),"short read bytes");
    check(pipe_write(&v,&wr,input+PIPE_SIZE,0,91)==91,"wrapped tail write");
    check(pipe_read(&v,&rd,output,0,sizeof output)==PIPE_SIZE,"wrapped read length");
    check(!memcmp(output,input+91,PIPE_SIZE-91)&&!memcmp(output+PIPE_SIZE-91,input+PIPE_SIZE,91),"wrapped read order");
    check(p.count==0&&pipe_can_write(&v,&wr)&&!pipe_can_read(&v,&rd),"empty readiness");
    p.writers=0;check(pipe_read(&v,&rd,output,0,1)==0&&pipe_can_read(&v,&rd),"closed writer EOF readiness");
    reset(&p);p.readers=0;check(pipe_write(&v,&wr,input,0,1)==-EPIPE&&pipe_can_write(&v,&wr),"closed reader broken pipe readiness");
    reset(&p);rd.flags=O_RDONLY;wait_hook=supply;check(pipe_read(&v,&rd,output,0,7)==7&&!memcmp(input,output,7),"blocking reader retry");
    reset(&p);wr.flags=O_WRONLY;check(pipe_write(&v,&wr,input,0,PIPE_SIZE)==PIPE_SIZE,"blocking fill");wait_hook=drain;
    check(pipe_write(&v,&wr,input,0,13)==13&&p.count==13,"blocking writer retry");
    reset(&p);fake_task.killed=true;check(pipe_read(&v,&rd,output,0,1)==-EINTR,"killed blocked reader");
    p.count=PIPE_SIZE;check(pipe_write(&v,&wr,input,0,1)==-EINTR,"killed blocked writer");fake_task.killed=false;
    reset(&p);unsigned rp=p.rq.wakes,wp=p.wq.wakes,pp=polls;pipe_close(&v,&rd);check(p.readers==0&&p.rq.wakes==rp+1&&p.wq.wakes==wp+1&&polls==pp+1,"close wakes both peer queues and poll");
    check(waits==2&&irq_depth==0,"bounded waits and IRQ restore");
    /* Finite reference FIFO exercises many independently wrapped head/tail
       positions and request lengths, including greater than pipe capacity. */
    reset(&p);rd.flags=O_RDONLY|O_NONBLOCK;wr.flags=O_WRONLY|O_NONBLOCK;
    uint8_t model[PIPE_SIZE];size_t used=0;
    for(unsigned i=0;i<10000;i++){
        size_t want=1+random_u32()%(sizeof input);
        if(random_u32()&1){
            size_t take=want<PIPE_SIZE-used?want:PIPE_SIZE-used;
            for(size_t j=0;j<want;j++)input[j]=(uint8_t)(j*31+i*13+(i>>8));
            int64_t got=pipe_write(&v,&wr,input,0,want);
            check(got==(take?(int64_t)take:-EAGAIN),"random partial write length");
            memcpy(model+used,input,take);used+=take;
        }else{
            size_t take=want<used?want:used;int64_t got=pipe_read(&v,&rd,output,0,want);
            check(got==(take?(int64_t)take:-EAGAIN),"random short read length");
            check(!memcmp(output,model,take),"random FIFO byte order");
            memmove(model,model+take,used-take);used-=take;
        }
        check(p.count==used&&p.head<PIPE_SIZE&&p.tail<PIPE_SIZE,"random ring invariant");
    }
    /* 320 MiB roundtrips, beyond coarse CRT clock resolution. No speed
       threshold: this is host CPU evidence only, not WHPX/YouTube timing. */
    reset(&p);rd.flags=O_RDONLY|O_NONBLOCK;wr.flags=O_WRONLY|O_NONBLOCK;
    clock_t begin=clock();for(unsigned i=0;i<20480;i++){if(pipe_write(&v,&wr,input,0,PIPE_SIZE)!=PIPE_SIZE||pipe_read(&v,&rd,output,0,PIPE_SIZE)!=PIPE_SIZE)return 3;}
    printf("pipe host %u checks / %u failed; 320 MiB roundtrips %.3f ms; %u poll wakes\n",checks,failed,(double)(clock()-begin)*1000/CLOCKS_PER_SEC,polls);
    return failed?1:0;
}
