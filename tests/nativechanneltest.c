/* Real Nocturne named rendezvous and message channels, including a service in
 * another app. This is supporting evidence, not browser/site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
static int checks,failed;
static void check(bool ok,const char *name) {checks++;if(!ok){failed++;printf("FAIL native-channel: %s\n",name);}}
/* The interest bits of h that are ready within ms, or 0. */
static uint32_t ready(n_handle h,uint32_t interest,int64_t ms) {
    n_handle q=n_events_create();if(q<0)return 0;
    struct n_event e={0};
    n_result n=n_events_watch(q,h,interest,1)<0?-1:n_events_wait(q,&e,1,ms);
    n_handle_close(q);return n==1?e.events:0;
}
static int server(void) {
    if(!(ready(0,N_EVENT_RECEIVABLE,5000)&N_EVENT_RECEIVABLE))return 21;
    n_handle h=n_channel_accept(0);if(h<0)return 22;
    char text[8];n_result n=n_channel_receive(h,text,sizeof text,N_CHANNEL_WAIT,NULL);
    int result=n==4&&!memcmp(text,"ping",4)&&n_channel_send(h,"pong",4,-1)==4?0:23;
    n_handle_close(0);n_handle_close(h);return result;
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"server"))return server();
    char key[128],other[160];snprintf(key,sizeof key,"native-channel-test:%lld",(long long)n_app_self());
    snprintf(other,sizeof other,"%s:other",key);
    n_handle listener=n_channel_listen(key);
    check(listener>=0,"atomic listener create");if(listener<0)return 1;
    check(n_channel_listen(key)==-N_ERR_EXISTS,"exact existing name rejected");
    check(n_channel_connect(other)==-N_ERR_NOT_FOUND,"full identity distinguishes suffix");
    check(n_channel_accept(listener)==-N_ERR_NOT_READY,"empty accept does not wait");
    check(!ready(listener,N_EVENT_RECEIVABLE,0),"idle listener is not receivable");
    n_handle client=n_channel_connect(key);
    check(client>=0 && (ready(listener,N_EVENT_RECEIVABLE,0)&N_EVENT_RECEIVABLE),"connection waits in the listener");
    n_handle peer=n_channel_accept(listener);
    check(client>=0 && peer>=0,"connect and accept");
    if(client<0 || peer<0){n_handle_close(client);n_handle_close(peer);n_handle_close(listener);return 1;}
    size_t bytes=3u*1024u*1024u+17u;unsigned char *input=malloc(bytes),*output=malloc(bytes);
    check(input && output,"transfer storage");
    if(input && output) {
        for(size_t i=0;i<bytes;i++)input[i]=(unsigned char)(i*31+7);
        check(n_channel_send(client,input,bytes,-1)==(n_result)bytes,"a message larger than the flow limit enters an empty queue");
        check(n_channel_send(client,"",0,-1)==0,"empty message");
        check(n_channel_receive(peer,NULL,0,N_CHANNEL_PEEK,NULL)==(n_result)bytes,"peek reports the whole message");
        check(n_channel_receive(peer,output,bytes-1,0,NULL)==-N_ERR_TOO_LARGE,"short buffer leaves the message queued");
        check(n_channel_receive(peer,output,bytes,0,NULL)==(n_result)bytes && !memcmp(input,output,bytes),"one message keeps its bytes and boundary");
        check(n_channel_receive(peer,output,bytes,0,NULL)==0,"empty message keeps its boundary");
        check(n_channel_send(peer,"ok",2,-1)==2 && n_channel_receive(client,output,bytes,N_CHANNEL_WAIT,NULL)==2 && !memcmp(output,"ok",2),"reverse direction");
        check(n_channel_send(client,"header-then-rest",16,-1)==16 && n_channel_receive(peer,output,6,N_CHANNEL_DISCARD,NULL)==16 &&
              !memcmp(output,"header",6) && n_channel_receive(peer,output,bytes,0,NULL)==-N_ERR_NOT_READY,"discard copies a prefix and drops the rest");
    }
    free(input);free(output);n_handle_close(peer);
    check((ready(client,N_EVENT_CLOSED,0)&N_EVENT_CLOSED)!=0,"peer close reports closed");
    char byte;
    check(n_channel_receive(client,&byte,1,0,NULL)==-N_ERR_CLOSED,"closed peer has nothing more");
    check(n_channel_send(client,"x",1,-1)==-N_ERR_CLOSED,"closed peer rejects sending");n_handle_close(client);
    client=n_channel_connect(key);check(client>=0,"pending peer can connect");n_handle_close(listener);
    check(n_channel_receive(client,&byte,1,N_CHANNEL_WAIT,NULL)==-N_ERR_CLOSED,"listener close closes pending peers");n_handle_close(client);
    listener=n_channel_listen(key);check(listener>=0,"closing the listener releases the name");
    enum {COUNT=96};n_handle clients[COUNT],peers[COUNT];
    memset(clients,0xff,sizeof clients);memset(peers,0xff,sizeof peers);
    bool connected=true;
    for(unsigned i=0;i<COUNT;i++) {
        clients[i]=n_channel_connect(key);peers[i]=n_channel_accept(listener);
        if(clients[i]<0 || peers[i]<0){connected=false;break;}
    }
    check(connected,"96 clients");
    if(connected) {
        n_handle q=n_events_create();bool watched=q>=0;
        for(unsigned i=0;i<COUNT&&watched;i++)watched=n_events_watch(q,clients[i],N_EVENT_RECEIVABLE,i)>=0;
        struct n_event events[COUNT];
        check(watched && n_events_wait(q,events,COUNT,0)==0,"many watched channels without false readiness");
        for(unsigned i=0;i<COUNT;i++){char value=(char)i;check(n_channel_send(peers[i],&value,1,-1)==1,"each client direction remains independent");}
        check(watched && n_events_wait(q,events,COUNT,0)==COUNT,"one wait reports every ready client");
        for(unsigned i=0;i<COUNT;i++)check(n_channel_receive(clients[i],&byte,1,0,NULL)==1 && (unsigned char)byte==i,"per-client routing");
        if(q>=0)n_handle_close(q);
    }
    for(unsigned i=0;i<COUNT;i++){if(clients[i]>=0)n_handle_close(clients[i]);if(peers[i]>=0)n_handle_close(peers[i]);}
    const char *args[]={argv[0],"server"};const n_handle handles[]={listener};
    struct n_launch launch={.arguments=args,.argument_count=2,.handles=handles,.handle_count=1};
    n_handle app=n_app_launch(argv[0],&launch);check(app>=0,"a service app receives the listener");
    n_handle_close(listener);
    if(app>=0) {
        client=n_channel_connect(key);check(client>=0,"another app owns the listener after its creator closes it");
        char response[8];bool ok=client>=0 && n_channel_send(client,"ping",4,-1)==4 &&
            n_channel_receive(client,response,sizeof response,N_CHANNEL_WAIT,NULL)==4 && !memcmp(response,"pong",4);
        check(ok,"service app answers one message");
        if(ok)check(n_channel_receive(client,&byte,1,N_CHANNEL_WAIT,NULL)==-N_ERR_CLOSED,"service app closes its end when done");
        int status=-1;
        if(!ok)n_app_stop(app,0);
        check(n_app_result(app,5000,&status)>=0 && (!ok || status==0),"service app ends");
        n_handle_close(app);
        if(client>=0)n_handle_close(client);
        listener=n_channel_listen(key);check(listener>=0,"the service ending returns the name");if(listener>=0)n_handle_close(listener);
    }
    printf("nativechanneltest: %d checks, %d failures\n",checks,failed);return failed!=0;
}
