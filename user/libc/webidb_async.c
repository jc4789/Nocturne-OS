/* Native GUI-side IndexedDB transport. The persistent engine lives only in
 * browserstorageworker; this module never waits on it. One message channel
 * carries each command and its result as one message apiece. Only one
 * command is outstanding, so a send always finds the worker's queue empty. */
#include <webidb_async.h>
#include <nocturne.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define STORAGE_WORKER N_FOLDER_SYSTEM "\\browserstorageworker"

struct idb_packet {
    struct idb_packet *next;
    struct webidb_wire_command command;
    uint8_t *body; /* room for the command, then length payload bytes */
    size_t length;
    bool sent;
};
struct webidb_async {
    struct webidb_async *next;
    n_handle app, channel;
    uint32_t serial;
    bool failed, ready;
    struct idb_packet *first, *last;
    struct webidb_wire_result response;
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
    if(db->channel>=0)n_handle_close(db->channel);
    db->channel=-1;
    if(db->app>=0)n_app_stop(db->app,0);
}
static bool reap(webidb_async *db) {
    if(db->app>=0){
        if(n_app_result(db->app,0,NULL)==-N_ERR_NOT_READY)return false;
        n_handle_close(db->app);db->app=-1;
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
    n_handle pair[2]={-1,-1};
    if(n_channel_pair(pair)<0){stop(db);return false;}
    /* The worker's diagnostics go to the page owner's stream, else the system log. */
    const char *arguments[]={"browserstorageworker"};int32_t handles[]={pair[1],pair[1],2};
    struct n_launch launch={.arguments=arguments,.argument_count=1,.handles=handles,.handle_count=3};
    n_handle app=n_app_launch(STORAGE_WORKER,&launch);
    if(app==-N_ERR_NO_HANDLE){handles[2]=-1;app=n_app_launch(STORAGE_WORKER,&launch);}
    n_handle_close(pair[1]);
    db->channel=pair[0];
    if(app<0){stop(db);return false;}
    db->app=app;
    return true;
}
webidb_async *webidb_async_create(void) {
    webidb_async_background();
    webidb_async *db=calloc(1,sizeof *db);
    if(db)db->app=db->channel=-1;
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
    p->body=malloc(sizeof p->command+total);
    if(!p->body){free(p);return 0;}
    size_t at=sizeof p->command;
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
    if(db->app<0&&!spawn_broker(db)){packet_free(p);return 0;}
    if(db->last)db->last->next=p;
    else db->first=p;
    db->last=p;
    return p->command.sequence;
}
void webidb_async_pump(webidb_async *db) {
    if(!db||db->failed||db->ready||!db->first)return;
    struct idb_packet *p=db->first;
    if(!p->sent){
        memcpy(p->body,&p->command,sizeof p->command);
        if(n_channel_send(db->channel,p->body,sizeof p->command+p->length,-1)<0){stop(db);return;}
        p->sent=true;
    }
    n_result size=n_channel_receive(db->channel,NULL,0,N_CHANNEL_PEEK,NULL);
    if(size==-N_ERR_NOT_READY)return;
    struct webidb_wire_result *r=&db->response;
    uint8_t *bytes=size>=(n_result)sizeof *r?malloc((size_t)size):NULL;
    if(!bytes||n_channel_receive(db->channel,bytes,(size_t)size,0,NULL)!=size){free(bytes);stop(db);return;}
    memcpy(r,bytes,sizeof *r);
    if(r->magic!=WEB_IDB_WIRE_MAGIC||r->operation!=p->command.operation||
       r->sequence!=p->command.sequence||r->status>WEB_IDB_MEMORY||
       r->payload_length!=(size_t)size-sizeof *r||r->count>SIZE_MAX){free(bytes);stop(db);return;}
    /* A pending BEGIN carries a present (possibly empty) body. */
    if(r->payload_length||(r->operation==WEB_IDB_BEGIN&&r->pending)){
        memmove(bytes,bytes+sizeof *r,(size_t)r->payload_length);db->body=bytes;
    }else free(bytes);
    db->ready=true;
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
    db->ready=false;
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
