/* Nocturne native IndexedDB I/O owner. It executes real transaction engine
 * operations off the page task; protocol input never contains a file path. */
#include <webidb_async.h>
#include <nocturne.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* The browser's channel: one command per message in, one result per message out. */
#define CHANNEL 0
/* The next whole command message, waiting for it; NULL once the browser has gone. */
static uint8_t *receive_command(size_t *length) {
    n_result size=n_channel_receive(CHANNEL,NULL,0,N_CHANNEL_WAIT|N_CHANNEL_PEEK,NULL);
    if(size<(n_result)sizeof(struct webidb_wire_command))return NULL;
    uint8_t *bytes=malloc((size_t)size);
    if(!bytes||n_channel_receive(CHANNEL,bytes,(size_t)size,0,NULL)!=size){free(bytes);return NULL;}
    *length=(size_t)size;return bytes;
}
static bool send_result(const struct webidb_wire_result *result,const uint8_t *output,size_t length) {
    if(length>SIZE_MAX-sizeof *result)return false;
    uint8_t *message=malloc(sizeof *result+length);
    if(!message)return false;
    memcpy(message,result,sizeof *result);if(length)memcpy(message+sizeof *result,output,length);
    bool sent=n_channel_send(CHANNEL,message,sizeof *result+length,-1)>=0;
    free(message);return sent;
}
static bool add(size_t *total,uint64_t length) {
    if(length>SIZE_MAX-*total)return false;
    *total+=(size_t)length;return true;
}
static void put64(uint8_t *bytes,uint64_t value) {
    for(unsigned i=0;i<8;i++)bytes[i]=(uint8_t)(value>>(8*i));
}
static char *string_copy(const uint8_t *bytes,size_t length) {
    if(length==SIZE_MAX)return NULL;
    char *s=malloc(length+1);
    if(s){memcpy(s,bytes,length);s[length]=0;}
    return s;
}
static bool catalog_pack(struct webidb_database *items,size_t count,uint8_t **bytes,size_t *length) {
    size_t total=0;
    for(size_t i=0;i<count;i++)if(!add(&total,16)||!add(&total,items[i].name_length))return false;
    uint8_t *data=malloc(total?total:1);
    if(!data)return false;
    size_t at=0;
    for(size_t i=0;i<count;i++){
        put64(data+at,items[i].name_length);put64(data+at+8,items[i].version);at+=16;
        memcpy(data+at,items[i].name,items[i].name_length);at+=items[i].name_length;
    }
    *bytes=data;*length=total;return true;
}
int main(void) {
    webidb *engine=webidb_create();
    if(!engine)return 1;
    struct webidb_wire_command command;
    size_t message_length=0;uint8_t *message;
    while((message=receive_command(&message_length))){
        size_t total=0;
        memcpy(&command,message,sizeof command);
        if(command.magic!=WEB_IDB_WIRE_MAGIC||!command.sequence||
           command.operation<WEB_IDB_BEGIN||command.operation>WEB_IDB_FIRED||
           command.reference_count>SIZE_MAX/64||
           !add(&total,command.origin_length)||!add(&total,command.name_length)||
           !add(&total,command.payload_length)||!add(&total,command.reference_count*64)||
           total!=message_length-sizeof command){free(message);break;}
        uint8_t *body=message+sizeof command;
        char *origin=string_copy(body,(size_t)command.origin_length);
        char *name=string_copy(body+command.origin_length,(size_t)command.name_length);
        uint8_t *payload=body+command.origin_length+command.name_length;
        struct webidb_wire_result result={
            .magic=WEB_IDB_WIRE_MAGIC,.operation=command.operation,.sequence=command.sequence,.status=WEB_IDB_IO
        };
        uint8_t *output=NULL;size_t output_length=0;
        if(!origin||!name){result.status=WEB_IDB_MEMORY;goto reply;}
        if(memchr(origin,0,(size_t)command.origin_length)){result.status=WEB_IDB_SECURITY;goto reply;}
        switch(command.operation){
        case WEB_IDB_BEGIN:
            result.status=webidb_begin(engine,origin,name,(size_t)command.name_length,&result.token,&output,&output_length);
            result.pending=output!=NULL;break;
        case WEB_IDB_STAGE: {
            char reference[65];
            result.status=webidb_stage(engine,command.token,payload,(size_t)command.payload_length,reference);
            if(result.status==WEB_IDB_OK){
                output=(uint8_t*)string_copy((uint8_t*)reference,64);
                if(output)output_length=64;
                else result.status=WEB_IDB_MEMORY;
            }
            break;
        }
        case WEB_IDB_VALUE: {
            if(command.payload_length!=64)break;
            char reference[65];memcpy(reference,payload,64);reference[64]=0;
            result.status=webidb_value(engine,command.token,reference,&output,&output_length);break;
        }
        case WEB_IDB_COMMIT: {
            size_t count=(size_t)command.reference_count;
            if(count>SIZE_MAX/65||count>SIZE_MAX/sizeof(char*)){result.status=WEB_IDB_MEMORY;break;}
            char **references=count?malloc(count*sizeof *references):NULL;
            char *strings=count?malloc(count*65):NULL;
            if(count&&(!references||!strings)){free(references);free(strings);result.status=WEB_IDB_MEMORY;break;}
            const uint8_t *source=payload+command.payload_length;
            for(size_t i=0;i<count;i++){
                references[i]=strings+i*65;
                memcpy(references[i],source+i*64,64);
                references[i][64]=0;
            }
            result.status=webidb_commit(engine,command.token,payload,(size_t)command.payload_length,(const char *const*)references,count,command.version);
            free(strings);free(references);break;
        }
        case WEB_IDB_ABORT:
            result.status=webidb_abort(engine,command.token);break;
        case WEB_IDB_DATABASES: {
            struct webidb_database *items=NULL;size_t count=0;
            result.status=webidb_databases(engine,origin,&items,&count);
            if(result.status==WEB_IDB_OK){
                if(catalog_pack(items,count,&output,&output_length))result.count=count;
                else result.status=WEB_IDB_MEMORY;
            }
            webidb_databases_free(items,count);break;
        }
        case WEB_IDB_CONNECTIONS: {
            size_t count=0,unfired=0;
            result.status=webidb_connections(engine,command.token,command.version,&count,&unfired);
            result.count=count;result.pending=unfired>UINT32_MAX?UINT32_MAX:(uint32_t)unfired;break;
        }
        case WEB_IDB_CONNECT:
            result.status=webidb_connect(engine,command.token,command.version,&result.token);break;
        case WEB_IDB_POLL: {
            int pending=0;
            result.status=webidb_poll(engine,command.token,&pending,&result.version);result.pending=pending;break;
        }
        case WEB_IDB_DISCONNECT:
            result.status=webidb_disconnect(engine,command.token);break;
        case WEB_IDB_FORGET:
            result.status=webidb_forget_connection(engine,command.token);break;
        case WEB_IDB_FIRED:
            result.status=webidb_fired(engine,command.token);break;
        case WEB_IDB_VALUES: {
            size_t count=(size_t)command.reference_count,capacity=0;
            const uint8_t *source=payload+command.payload_length;
            result.status=WEB_IDB_OK;
            for(size_t i=0;i<count;i++){
                char reference[65];memcpy(reference,source+i*64,64);reference[64]=0;
                uint8_t *value=NULL;size_t length=0;
                result.status=webidb_value(engine,command.token,reference,&value,&length);
                if(result.status!=WEB_IDB_OK){free(value);break;}
                if(output_length>SIZE_MAX-8||length>SIZE_MAX-output_length-8){
                    free(value);result.status=WEB_IDB_MEMORY;break;
                }
                size_t needed=output_length+8+length;
                if(needed>capacity){
                    size_t grown=capacity?capacity:4096;
                    while(grown<needed){
                        if(grown>SIZE_MAX/2){grown=needed;break;}
                        grown*=2;
                    }
                    uint8_t *more=realloc(output,grown);
                    if(!more){free(value);result.status=WEB_IDB_MEMORY;break;}
                    output=more;capacity=grown;
                }
                put64(output+output_length,length);
                output_length+=8;
                memcpy(output+output_length,value,length);
                output_length+=length;
                free(value);
            }
            if(result.status==WEB_IDB_OK)result.count=count;
            else {free(output);output=NULL;output_length=0;}
            break;
        }
        }
reply:
        result.payload_length=output_length;
        bool sent=send_result(&result,output,output_length);
        free(output);
        free(name);free(origin);free(message);
        if(!sent)break;
    }
    webidb_free(engine);return 0;
}
