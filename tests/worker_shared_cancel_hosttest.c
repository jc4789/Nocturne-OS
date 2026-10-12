/* The product's native-only interrupt path. Endpoint readiness is modeled;
 * an actual QuickJS infinite task exercises cancellation, never site/QEMU. */
#define WORKER_STREAM_MODEL
#define WORKER_CANCEL_MODEL
#define SHARED_SERVER_SUPPORT_ONLY
#include "sharedworker_server_hosttest.c"
static bool live_fd[256],connect_during_scan;
static int pending[128];static unsigned pending_at,pending_count,polls,accepts,reads,writes;
ssize_t read(int fd,void*p,size_t n){(void)fd;(void)p;(void)n;reads++;errno=EAGAIN;return -1;}
ssize_t write(int fd,const void*p,size_t n){(void)fd;(void)p;writes++;return n;}
int n_channel_accept(int fd){(void)fd;if(!pending_count){errno=EAGAIN;return -1;}accepts++;int result=pending[pending_at++];pending_count--;return result;}
int poll(struct n_pollfd*p,int n,int timeout){(void)timeout;polls++;int ready=0;for(int i=0;i<n;i++){p[i].revents=0;if(p[i].fd==0){if(pending_count)p[i].revents|=N_POLLIN;}else if(p[i].fd>0&&p[i].fd<256&&!live_fd[p[i].fd])p[i].revents|=N_POLLHUP;if(p[i].revents)ready++;}
 if(connect_during_scan&&n&&p[0].fd!=0){connect_during_scan=false;pending[0]=100;pending_at=0;pending_count=1;live_fd[100]=true;}return ready;
}
static void reset(struct worker*w){while(w->clients){struct client*c=w->clients;w->clients=c->next;free(c);}memset(w,0,sizeof*w);w->shared=true;w->until=UINT64_MAX;memset(live_fd,0,sizeof live_fd);pending_at=pending_count=polls=accepts=reads=writes=0;connect_during_scan=false;}
static void client(struct worker*w,int fd){struct client*c=calloc(1,sizeof*c);c->fd=fd;c->active=true;c->next=w->clients;w->clients=c;w->had_client=true;}
static int checkpoint(struct worker*w){w->interrupt_ticks=15;return interrupt(NULL,w);}
int main(void){struct worker w={0};reset(&w);check("initial-shared-bootstrap-without-client-is-not-killed",checkpoint(&w)==0&&!polls&&!w.closed);
 client(&w,8);live_fd[8]=true;w.interrupt_ticks=0;for(unsigned i=0;i<15;i++)interrupt(NULL,&w);check("no-native-poll-on-every-bytecode-interrupt",polls==0);check("live-client-protects-native-task",interrupt(NULL,&w)==0&&!w.closed&&polls>0&&!reads&&!writes);
 reset(&w);for(int fd=8;fd<88;fd++)client(&w,fd);check("all-hup-clients-over-one-stack-batch-cancel-task",checkpoint(&w)==1&&w.closed&&!reads&&!writes);
 reset(&w);client(&w,8);pending[0]=100;pending_count=1;live_fd[100]=true;check("live-pending-client-is-retained-without-deserialization",checkpoint(&w)==0&&!w.closed&&accepts==1&&w.clients->fd==100&&!w.clients->established&&!reads&&!writes);
 reset(&w);client(&w,8);for(unsigned i=0;i<96;i++)pending[i]=100+(int)i;pending_count=96;bool bounded=true;int stopped=0;for(unsigned i=0;i<6;i++){unsigned before=accepts;stopped=checkpoint(&w);if(accepts-before>16)bounded=false;if(i<5&&stopped)bounded=false;}check("dead-unaccepted-clients-drain-in-finite-native-quanta",bounded&&stopped==1&&w.closed&&accepts==96&&!pending_count&&!reads&&!writes);
 reset(&w);client(&w,8);connect_during_scan=true;check("new-pending-owner-during-scan-prevents-premature-close",checkpoint(&w)==0&&!w.closed&&pending_count==1&&checkpoint(&w)==0&&accepts==1&&w.clients->fd==100&&!reads&&!writes);
 reset(&w);client(&w,8);JSRuntime*rt=JS_NewRuntime();JSContext*ctx=JS_NewContext(rt);JS_SetInterruptHandler(rt,interrupt,&w);JSValue result=JS_Eval(ctx,"for(;;){}",9,"actual-pure-cpu-worker-task",JS_EVAL_TYPE_GLOBAL);check("actual-endless-quickjs-task-exits-on-last-client-eof",JS_IsException(result)&&w.closed&&polls>0&&!reads&&!writes);if(JS_HasException(ctx))JS_FreeValue(ctx,JS_GetException(ctx));JS_FreeValue(ctx,result);JS_FreeContext(ctx);JS_FreeRuntime(rt);
 reset(&w);printf("RESULT %u checks %u failures\n",checks,failures);return failures?1:0;}
