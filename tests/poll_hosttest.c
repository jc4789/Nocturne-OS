/* Exact production VFS/pipe/ramfs functions. Host scheduler, allocation and
   user-permission shims test ownership and copy boundaries, not guest timing,
   hardware IRQ delivery, sys_poll sleeping or real-site throughput. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "poll_host_shim/fs/vfs.h"

static unsigned checks, failed, irq_depth, live_allocations;
static bool fail_next_allocation;
static struct task fake_task;
struct task *current_task = &fake_task;
static uintptr_t readable_start, writable_start;
static size_t readable_bytes, writable_bytes, largest_validation;

static void check(bool good, const char *name) {
    checks++;
    if (!good) { failed++; fprintf(stderr, "FAIL %s\n", name); }
}
uint64_t irq_save(void) { unsigned old=irq_depth;irq_depth++;return old; }
void irq_restore(uint64_t flags) { irq_depth=(unsigned)flags; }
void wq_wait(struct wait_queue *q) { (void)q;check(false,"unexpected blocking host wait");exit(2); }
void wq_wake_all(struct wait_queue *q) {
    check(irq_depth != 0,"wake is IRQ protected");
    if (q->head) { q->wakes++;q->head=NULL; }
}
void *kmalloc(size_t n) {
    if (fail_next_allocation) { fail_next_allocation=false;return NULL; }
    void *p=malloc(n);if(p)live_allocations++;return p;
}
void *kzalloc(size_t n) { void *p=kmalloc(n);if(p)memset(p,0,n);return p; }
void *krealloc(void *p,size_t n) { if(!p)return kmalloc(n);return realloc(p,n); }
void kfree(void *p) { if(p){live_allocations--;free(p);} }
int64_t time_now(void) { return 0; }
size_t strlcpy(char *dst,const char *src,size_t n) {
    size_t len=strlen(src);if(n){size_t take=MIN(len,n-1);memcpy(dst,src,take);dst[take]=0;}return len;
}
int ksnprintf(char *dst,size_t n,const char *fmt,...) {
    va_list args;va_start(args,fmt);int r=vsnprintf(dst,n,fmt,args);va_end(args);return r;
}
static bool valid_span(const void *p,size_t n,uintptr_t start,size_t bytes) {
    largest_validation=MAX(largest_validation,n);
    uintptr_t address=(uintptr_t)p;
    return !n || (address>=start && address-start<=bytes && n<=bytes-(address-start));
}
bool user_ok(const void *p,size_t n) { return valid_span(p,n,readable_start,readable_bytes); }
bool user_ok_w(void *p,size_t n) { return valid_span(p,n,writable_start,writable_bytes); }

#include "../kernel/src/fs/vfs.c"
#include "../kernel/src/fs/pipe.c"
#include "../kernel/src/fs/ramfs.c"

static void enroll(struct poll_wait *wait,struct file *f) {
    uint64_t flags=irq_save();vfs_poll_file(wait,f);irq_restore(flags);
}
static void notification_and_lifetime(void) {
    unsigned before=live_allocations;
    struct file *ar=NULL,*aw=NULL,*br=NULL,*bw=NULL;
    int a=pipe_create(&ar,&aw),b=pipe_create(&br,&bw);
    check(a==0 && b==0,"two independent providers");
    if(a || b){vfs_close(ar);vfs_close(aw);vfs_close(br);vfs_close(bw);return;}
    struct poll_wait first={0},second={0};
    enroll(&first,ar);enroll(&second,br);
    first.wq.head=second.wq.head=&fake_task;
    char payload[]="bytes";
    uint64_t observed=first.generation;
    check(vfs_write(aw,payload,sizeof payload)==sizeof payload,"production write notifies registered reader");
    check(first.generation!=observed && first.wq.wakes==1,"event after enrollment survives readiness-to-sleep seam");
    check(second.generation==0 && second.wq.wakes==0,"unrelated vnode remains asleep");
    char out[sizeof payload];
    check(vfs_read(ar,out,sizeof out)==sizeof out && !memcmp(out,payload,sizeof out),"ready bytes survive notification");
    struct file *duplicate=vfs_dup(aw);
    observed=first.generation;vfs_close(aw);
    check(first.generation==observed && !pipe_hangup(ar->vn,ar),"duplicate preserves writer ownership");
    vfs_close(duplicate);
    check(first.generation!=observed && pipe_hangup(ar->vn,ar),"last writer close wakes and reports hangup");
    struct vnode *pinned=ar->vn;
    vfs_close(ar);
    check(pinned->refs==1 && pinned->pollers!=NULL,"registration pins vnode but not open endpoint");
    vfs_poll_cleanup(&first);
    check(first.entries==NULL,"cleanup detaches all call-owned entries");
    vfs_poll_cleanup(&second);vfs_close(br);vfs_close(bw);
    check(live_allocations==before,"close and cleanup reclaim every owner");
}
static void enrollment_boundaries(void) {
    struct file *rd,*wr;check(pipe_create(&rd,&wr)==0,"enrollment provider");
    char byte='a';check(vfs_write(wr,&byte,1)==1,"event before enrollment");
    struct poll_wait wait={0};
    uint64_t flags=irq_save();
    enroll(&wait,rd);
    check(vfs_can_read(rd),"already-arrived event visible at enrollment readiness check");
    irq_restore(flags);
    struct poll_wait duplicate={0};enroll(&duplicate,rd);enroll(&duplicate,rd);
    vfs_poll_cleanup(&wait);
    wait=(struct poll_wait){0};enroll(&wait,rd);
    vfs_poll_cleanup(&duplicate);vfs_poll_cleanup(&wait);
    check(rd->vn->pollers==NULL,"interleaved and duplicate source registrations unlink correctly");
    fail_next_allocation=true;enroll(&wait,rd);
    check(wait.error==-ENOMEM && wait.entries==NULL,"registration allocation failure is explicit and leaves no owner");
    vfs_poll_cleanup(&wait);vfs_close(rd);vfs_close(wr);
}
static void channel_notifications(void) {
    unsigned before=live_allocations;
    struct file *listener,*client,*peer;const char key[]="poll-boundary";
    check(channel_listen(key,sizeof key,&listener)==0,"native listener");
    struct poll_wait accept_wait={0};enroll(&accept_wait,listener);
    check(channel_connect(key,sizeof key,&client)==0,"native client enqueued");
    check(accept_wait.generation!=0 && vfs_can_read(listener),"connect notifies only registered listener source");
    check(channel_accept(listener,&peer)==0,"accept production peer");
    struct poll_wait receive_wait={0};enroll(&receive_wait,client);
    char byte='c';uint64_t generation=receive_wait.generation;
    check(vfs_write(peer,&byte,1)==1 && receive_wait.generation!=generation && vfs_can_read(client),"underlying receive pipe notifies channel wrapper");
    generation=receive_wait.generation;vfs_close(peer);
    check(receive_wait.generation!=generation && channel_hangup(client->vn,client),"either directional close notifies channel hangup");
    vfs_poll_cleanup(&accept_wait);vfs_close(listener);vfs_close(client);
    vfs_poll_cleanup(&receive_wait);
    check(live_allocations==before,"listener and directional subscriptions release without stale pointers");
}
static void actual_transfer_validation(void) {
    struct file *rd,*wr;check(pipe_create(&rd,&wr)==0,"copy provider");
    char bytes[]="data",out[sizeof bytes];
    writable_start=(uintptr_t)out;writable_bytes=sizeof out;largest_validation=0;
    check(vfs_write(wr,bytes,sizeof bytes)==sizeof bytes,"queued short transfer");
    check(vfs_read_user(rd,out,SIZE_MAX)==sizeof bytes && largest_validation==sizeof bytes,
          "read validates available bytes, not oversized requested span");
    check(!memcmp(out,bytes,sizeof bytes),"short user copy content");
    check(vfs_write(wr,bytes,sizeof bytes)==sizeof bytes,"fault path bytes queued");
    writable_bytes=sizeof out-1;
    check(vfs_read_user(rd,out,sizeof out)==-EFAULT && ((struct pipe *)rd->vn->priv)->count==sizeof bytes,
          "unwritable output fails before consuming bytes");
    writable_bytes=sizeof out;check(vfs_read_user(rd,out,sizeof out)==sizeof out,"fault retry recovers unchanged bytes");
    readable_start=(uintptr_t)bytes;readable_bytes=sizeof bytes-1;
    check(vfs_write_user(wr,bytes,sizeof bytes)==-EFAULT && ((struct pipe *)rd->vn->priv)->count==0,
          "unreadable input fails before publishing bytes");
    readable_bytes=sizeof bytes;largest_validation=0;
    check(vfs_write_user(wr,bytes,sizeof bytes)==sizeof bytes && largest_validation==sizeof bytes,"user write validates committed input");
    vfs_close(rd);vfs_close(wr);
    struct vnode *v=ramfs_new_node("short",VT_FILE);v->unlinked=true;
    struct file *f=vfs_open_vnode(v,O_RDWR);vnode_unref(v);
    check(vfs_write(f,bytes,sizeof bytes)==sizeof bytes && vfs_seek(f,0,0)==0,"regular file payload");
    largest_validation=0;
    check(vfs_read_user(f,out,SIZE_MAX)==sizeof out && largest_validation==sizeof out,"regular file validates and passes EOF-bounded request");
    check(vfs_read_user(f,(void *)1,SIZE_MAX)==0,"EOF does not validate untouched user pages");
    vfs_close(f);
}
int main(void) {
    notification_and_lifetime();enrollment_boundaries();channel_notifications();actual_transfer_validation();
    check(irq_depth==0 && live_allocations==0,"all IRQ guards and heap owners are released");
    printf("poll / transfer host: %u checks, %u failed\n",checks,failed);
    return failed ? 1 : 0;
}
