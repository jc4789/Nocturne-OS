/* Native GUI-side IndexedDB transport. The persistent engine lives only in
 * browserstorageworker; this module does bounded, nonblocking pipe work. */
#include <webidb_async.h>
#include <nocturne.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct idb_packet {
    struct idb_packet *next;
    struct webidb_wire_command command;
    uint8_t *body;
    size_t length, position;
};
struct webidb_async {
    struct webidb_async *next;
    int pid, input, output;
    uint32_t serial;
    bool failed, ready;
    struct idb_packet *first, *last;
    struct webidb_wire_result response;
    size_t header_position, body_position;
    uint8_t *body;
};
static webidb_async *retired;

static bool add_length(size_t *total,size_t add) {
    if(add>SIZE_MAX-*total)return false;
    *total+=add;return true;
}
static void packet_free(struct idb_packet *p) {
    if(!p)return;
    free(p->body);free(p);
}
static void stop(webidb_async *db) {
    if(db->failed)return;
    db->failed=true;
    if(db->input>=0)close(db->input);
    if(db->output>=0)close(db->output);
    db->input=db->output=-1;
    if(db->pid>0)kill(db->pid);
}
static bool reap(webidb_async *db) {
    if(db->pid>0){
        int result=waitpid(db->pid,NULL,WNOHANG);
        if(result!=db->pid&&!(result<0&&errno==ECHILD))return false;
        db->pid=0;
    }
    return true;
}
void webidb_async_background(void) {
    webidb_async **link=&retired;
    while(*link){
        webidb_async *db=*link;
        if(!reap(db)){link=&db->next;continue;}
        *link=db->next;free(db);
    }
}
static bool spawn_broker(webidb_async *db) {
    int input[2]={-1,-1},output[2]={-1,-1};
    if(pipe(input)<0||pipe(output)<0)goto failed;
    int map[3]={input[0],output[1],2};
    char *arguments[]={"browserstorageworker",NULL};
    db->pid=spawn("/bin/browserstorageworker",arguments,map,0);
    close(input[0]);input[0]=-1;
    close(output[1]);output[1]=-1;
    if(db->pid<0){db->pid=0;goto failed;}
    db->input=input[1];input[1]=-1;
    db->output=output[0];output[0]=-1;
    if(fcntl(db->input,F_SETFL,O_NONBLOCK)<0||fcntl(db->output,F_SETFL,O_NONBLOCK)<0){
        stop(db);return false;
    }
    return true;
failed:
    for(unsigned i=0;i<2;i++){
        if(input[i]>=0)close(input[i]);
        if(output[i]>=0)close(output[i]);
    }
    stop(db);return false;
}
webidb_async *webidb_async_create(void) {
    webidb_async_background();
    webidb_async *db=calloc(1,sizeof *db);
    if(db)db->input=db->output=-1;
    return db;
}
uint32_t webidb_async_submit(webidb_async *db,const struct webidb_async_request *request) {
    if(!db||db->failed||!request||request->operation<WEB_IDB_BEGIN||request->operation>WEB_IDB_VALUES)return 0;
    size_t origin_length=request->origin?strlen(request->origin):0;
    size_t name_length=request->name?request->name_length:0;
    size_t value_length=request->operation==WEB_IDB_VALUE?64:request->length;
    size_t total=0;
    if((value_length&&!request->bytes&&request->operation!=WEB_IDB_VALUE)||
       (request->operation==WEB_IDB_VALUE&&(!request->reference||strlen(request->reference)!=64))||
       request->reference_count>SIZE_MAX/64||
       !add_length(&total,origin_length)||!add_length(&total,name_length)||
       !add_length(&total,value_length)||!add_length(&total,request->reference_count*64)||
       total>SIZE_MAX-sizeof(struct webidb_wire_command))return 0;
    struct idb_packet *p=calloc(1,sizeof *p);
    if(!p)return 0;
    p->body=malloc(total?total:1);
    if(!p->body){free(p);return 0;}
    size_t at=0;
    if(origin_length){
        memcpy(p->body+at,request->origin,origin_length);at+=origin_length;
    }
    if(name_length){
        memcpy(p->body+at,request->name,name_length);at+=name_length;
    }
    if(value_length){
        const void *source=request->operation==WEB_IDB_VALUE?
            (const void*)request->reference:request->bytes;
        memcpy(p->body+at,source,value_length);at+=value_length;
    }
    for(size_t i=0;i<request->reference_count;i++){
        if(!request->references||!request->references[i]||strlen(request->references[i])!=64){packet_free(p);return 0;}
        memcpy(p->body+at,request->references[i],64);at+=64;
    }
    do{db->serial++;}while(!db->serial);
    p->command=(struct webidb_wire_command){
        .magic=WEB_IDB_WIRE_MAGIC,.operation=request->operation,.sequence=db->serial,
        .token=request->token,.version=request->version,.origin_length=origin_length,
        .name_length=name_length,.payload_length=value_length,.reference_count=request->reference_count
    };
    p->length=total;
    if(!db->pid&&!spawn_broker(db)){packet_free(p);return 0;}
    if(db->last)db->last->next=p;
    else db->first=p;
    db->last=p;
    return p->command.sequence;
}
static bool transient(void) {return errno==EAGAIN||errno==EINTR;}
void webidb_async_pump(webidb_async *db) {
    if(!db||db->failed||db->ready||!db->first)return;
    struct idb_packet *p=db->first;
    size_t budget=65536;
    while(budget&&p->position<sizeof p->command+p->length){
        bool header=p->position<sizeof p->command;
        size_t at=header?p->position:p->position-sizeof p->command;
        size_t available=header?sizeof p->command-at:p->length-at;
        size_t step=available<budget?available:budget;
        const uint8_t *bytes=header?(const uint8_t*)&p->command:p->body;
        ssize_t done=write(db->input,bytes+at,step);
        if(done<0&&transient())break;
        if(done<=0){stop(db);return;}
        p->position+=(size_t)done;budget-=(size_t)done;
    }
    budget=65536;
    while(budget){
        bool header=db->header_position<sizeof db->response;
        size_t left=header?sizeof db->response-db->header_position:(size_t)db->response.payload_length-db->body_position;
        if(!left){db->ready=true;return;}
        size_t step=left<budget?left:budget;
        uint8_t *bytes=header?(uint8_t*)&db->response:db->body;
        size_t at=header?db->header_position:db->body_position;
        ssize_t done=read(db->output,bytes+at,step);
        if(done<0&&transient())return;
        if(done<=0){stop(db);return;}
        budget-=(size_t)done;
        if(header){
            db->header_position+=(size_t)done;
            if(db->header_position==sizeof db->response){
                struct webidb_wire_result *r=&db->response;
                if(r->magic!=WEB_IDB_WIRE_MAGIC||r->operation!=p->command.operation||
                   r->sequence!=p->command.sequence||r->status>WEB_IDB_MEMORY||
                   r->payload_length>SIZE_MAX||r->count>SIZE_MAX){stop(db);return;}
                if(r->payload_length||(r->operation==WEB_IDB_BEGIN&&r->pending)){
                    db->body=malloc(r->payload_length?(size_t)r->payload_length:1);
                    if(!db->body){stop(db);return;}
                }
            }
        }else db->body_position+=(size_t)done;
    }
    if(db->header_position==sizeof db->response&&db->body_position==db->response.payload_length)db->ready=true;
}
static uint64_t read64(const uint8_t *bytes) {
    uint64_t value=0;
    for(unsigned i=0;i<8;i++)value|=(uint64_t)bytes[i]<<(8*i);
    return value;
}
void webidb_async_result_free(struct webidb_async_result *r) {
    if(!r)return;
    free(r->bytes);
    for(size_t i=0;i<r->item_count;i++)free(r->items[i].name);
    free(r->items);memset(r,0,sizeof *r);
}
bool webidb_async_take(webidb_async *db,struct webidb_async_result *r) {
    if(!db||!r||!db->first||(!db->ready&&!db->failed))return false;
    struct idb_packet *p=db->first;
    memset(r,0,sizeof *r);r->operation=p->command.operation;r->sequence=p->command.sequence;
    r->status=db->failed?WEB_IDB_IO:(int)db->response.status;
    if(!db->failed){
        r->token=db->response.token;
        r->pending=db->response.pending;
        r->version=db->response.version;
        r->count=(size_t)db->response.count;
        if(r->operation==WEB_IDB_STAGE&&r->status==WEB_IDB_OK){
            if(db->response.payload_length!=64){r->status=WEB_IDB_IO;stop(db);}
            else {memcpy(r->reference,db->body,64);r->reference[64]=0;}
        }else if(r->operation==WEB_IDB_DATABASES&&r->status==WEB_IDB_OK){
            size_t count=r->count,at=0,n=(size_t)db->response.payload_length;
            if(count>SIZE_MAX/sizeof *r->items)r->status=WEB_IDB_MEMORY;
            else if(count&&!(r->items=calloc(count,sizeof *r->items)))r->status=WEB_IDB_MEMORY;
            for(size_t i=0;r->status==WEB_IDB_OK&&i<count;i++){
                if(n-at<16){r->status=WEB_IDB_IO;break;}
                uint64_t name_length=read64(db->body+at),version=read64(db->body+at+8);at+=16;
                if(name_length>n-at||name_length>=SIZE_MAX){r->status=WEB_IDB_IO;break;}
                struct webidb_database *item=&r->items[i];
                item->name=malloc((size_t)name_length+1);
                if(!item->name){r->status=WEB_IDB_MEMORY;break;}
                memcpy(item->name,db->body+at,(size_t)name_length);item->name[name_length]=0;
                item->name_length=(size_t)name_length;item->version=version;
                r->item_count++;at+=(size_t)name_length;
            }
            if(r->status==WEB_IDB_OK&&at!=n)r->status=WEB_IDB_IO;
        }else{
            r->bytes=db->body;
            db->body=NULL;
            r->length=(size_t)db->response.payload_length;
        }
    }
    free(db->body);db->body=NULL;
    db->header_position=db->body_position=0;db->ready=false;
    memset(&db->response,0,sizeof db->response);
    db->first=p->next;if(!db->first)db->last=NULL;
    packet_free(p);return true;
}
bool webidb_async_pending(const webidb_async *db) {return db&&db->first;}
bool webidb_async_ready(const webidb_async *db) {return db&&db->first&&(db->ready||db->failed);}
bool webidb_async_failed(const webidb_async *db) {return db&&db->failed;}
void webidb_async_free(webidb_async *db) {
    if(!db)return;
    stop(db);
    while(db->first){
        struct idb_packet *p=db->first;
        db->first=p->next;packet_free(p);
    }
    free(db->body);db->body=NULL;
    if(reap(db))free(db);
    else {db->next=retired;retired=db;}
}
