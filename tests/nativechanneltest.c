/* Real Nocturne descriptor/IPC path, including an independently spawned
 * service process. This is supporting evidence, not browser/site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <nocturne.h>
static int checks,failed;
static void check(int ok,const char *name) {checks++;if(!ok){failed++;printf("FAIL native-channel: %s (errno %d)\n",name,errno);}}
static int wait_read(int fd,void *data,size_t size) {
    unsigned char *p=data;size_t at=0;
    while(at<size) {
        ssize_t n=read(fd,p+at,size-at);
        if(n>0){at+=(size_t)n;continue;}
        if(n<0 && errno==EAGAIN){struct n_pollfd pollfd={fd,N_POLLIN,0};if(poll(&pollfd,1,5000)>0)continue;}
        return -1;
    }
    return 0;
}
static int server(void) {
    struct n_pollfd p={0,N_POLLIN,0};if(poll(&p,1,5000)<=0)return 21;
    int fd=n_channel_accept(0);if(fd<0)return 22;
    char text[4];int result=wait_read(fd,text,sizeof text);
    if(!result && !memcmp(text,"ping",4))result=write(fd,"pong",4)==4?0:-1;
    close(0);close(fd);return result?23:0;
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"server"))return server();
    char key[128],other[128];snprintf(key,sizeof key,"native-channel-test:%d",getpid());
    snprintf(other,sizeof other,"%s:other",key);
    int listener=n_channel_listen(key),duplicate=-1;
    check(listener>=0,"atomic listener create");if(listener<0)return 1;
    check(n_channel_listen(key)<0 && errno==EEXIST,"exact existing name rejected");
    check(n_channel_connect(other)<0 && errno==ENOENT,"full identity distinguishes suffix");
    check(n_channel_accept(listener)<0 && errno==EAGAIN,"empty accept is nonblocking");
    duplicate=dup(listener);check(duplicate>=0,"listener duplicate owns namespace");close(listener);listener=duplicate;
    int client=n_channel_connect(key),peer=n_channel_accept(listener);
    check(client>=0 && peer>=0,"duplex connect and accept");
    if(client<0 || peer<0){close(client);close(peer);close(listener);return 1;}
    size_t bytes=32781;unsigned char *input=malloc(bytes),*output=malloc(bytes);
    check(input && output,"transfer storage");
    if(input && output) {
        for(size_t i=0;i<bytes;i++)input[i]=(unsigned char)(i*31+7);
        check(write(client,input,bytes)==(ssize_t)bytes,"segmented dynamic write exceeds former quantum");
        check(wait_read(peer,output,bytes)==0 && !memcmp(input,output,bytes),"segmented transfer retains byte order");
        check(write(peer,"ok",2)==2 && wait_read(client,output,2)==0 && !memcmp(output,"ok",2),"reverse duplex direction");
    }
    free(input);free(output);close(peer);
    struct n_pollfd closed={client,N_POLLIN,0};char byte;
    check(poll(&closed,1,0)==1 && (closed.revents&N_POLLHUP),"peer close wakes and reports hangup");
    check(read(client,&byte,1)==0,"closed peer yields EOF");
    check(write(client,"x",1)<0 && errno==EPIPE,"closed peer rejects write");close(client);
    client=n_channel_connect(key);check(client>=0,"pending peer can connect");close(listener);
    check(read(client,&byte,1)==0 && write(client,"x",1)<0 && errno==EPIPE,"listener death closes pending peers");close(client);
    listener=n_channel_listen(key);check(listener>=0,"last close releases name immediately");
    enum {COUNT=96};int clients[COUNT],peers[COUNT];struct n_pollfd fds[COUNT];
    memset(clients,0xff,sizeof clients);memset(peers,0xff,sizeof peers);
    bool connected=true;
    for(unsigned i=0;i<COUNT;i++) {
        clients[i]=n_channel_connect(key);peers[i]=n_channel_accept(listener);
        if(clients[i]<0 || peers[i]<0){connected=false;break;}
        fds[i]=(struct n_pollfd){clients[i],N_POLLIN,0};
    }
    check(connected,"96 clients grow existing process descriptor table");
    if(connected) {
        check(poll(fds,COUNT,0)==0,"poll supports more than 64 descriptors without false readiness");
        for(unsigned i=0;i<COUNT;i++){char value=(char)i;check(write(peers[i],&value,1)==1,"each client direction remains independent");}
        check(poll(fds,COUNT,0)==COUNT,"large poll snapshot reports every ready client");
        for(unsigned i=0;i<COUNT;i++)check(read(clients[i],&byte,1)==1 && (unsigned char)byte==i,"per-client byte routing");
        int high=dup2(clients[0],700);check(high==700,"explicit high fd grows representation without truncation");
        close(clients[0]);clients[0]=-1;
        check(write(peers[0],"h",1)==1 && read(high,&byte,1)==1 && byte=='h',"high duplicate retains channel lifetime");close(high);
    }
    for(unsigned i=0;i<COUNT;i++){if(clients[i]>=0)close(clients[i]);if(peers[i]>=0)close(peers[i]);}
    int nullfd=open("/dev/null",O_RDWR);int map[3]={listener,nullfd,nullfd};char *args[]={argv[0],"server",NULL};
    int pid=spawn(argv[0],args,map,SPAWN_DETACH);check(pid>0,"detached service inherits native listener");
    if(nullfd>=0)close(nullfd);close(listener);
    if(pid>0) {
        client=n_channel_connect(key);check(client>=0,"another process owns listener after creator closes");
        char response[4];bool ok=client>=0 && write(client,"ping",4)==4 && !wait_read(client,response,4) && !memcmp(response,"pong",4);
        check(ok,"independent process handles duplex exchange");
        if(ok){struct n_pollfd p={client,N_POLLIN,0};check(poll(&p,1,5000)>0 && read(client,&byte,1)==0,"service process closes client on completion");}
        else kill(pid);
        if(client>=0)close(client);
        listener=n_channel_listen(key);check(listener>=0,"service exit returns namespace ownership");if(listener>=0)close(listener);
    }
    printf("nativechanneltest: %d checks, %d failures\n",checks,failed);return failed!=0;
}
