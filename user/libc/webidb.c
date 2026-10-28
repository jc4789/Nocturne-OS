/* IndexedDB manifests contain schema/keys/index keys and immutable payload
 * references, not video segment bytes. Updating one value writes that payload
 * once; commit writes only metadata. Two checksum-protected manifest slots
 * retain crash fallback, and payload GC keeps both committed generations. */
#include <webidb.h>
#include <webstorage.h>
#include <weburl.h>
#include <nocturne.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <bearssl.h>
#include "webidb_journal.h"
/* The databases live in indexeddb in the browser's profile folder; false when it cannot be found. */
static bool idb_root(char root[WEBIDB_BASE_MAX]){
    const char *profile=web_profile_folder(false);
    return profile&&n_path_join(profile,"indexeddb",root,WEBIDB_BASE_MAX)>=0;
}
#define IDB_HEAD 64u
#define IDB_CATALOG_HEAD 120u
struct idb_refs {uint8_t *data;size_t count;};
struct idb_stage {struct idb_stage *next;uint8_t digest[32];};
struct idb_tx {
    struct idb_tx *next;
    uint32_t token;
    n_handle lease;
    int slot;
    uint64_t generation;
    char path[WEBIDB_BASE_MAX];
    char *origin,*name;
    size_t name_length;
    struct idb_refs current,old;
    struct idb_stage *staged;
};
struct idb_client {struct idb_client *next;uint32_t handle;uint64_t identity;size_t offset;n_handle table;};
struct webidb {struct idb_tx *active;struct idb_client *clients;uint32_t serial;};
static uint32_t client_serial;
static void hash(const void *bytes,size_t n,uint8_t out[32]){br_sha256_context h;br_sha256_init(&h);br_sha256_update(&h,bytes,n);br_sha256_out(&h,out);}
static uint64_t u64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static int directory(const char *path){struct n_entry e;n_result r=n_entry_info(path,&e);if(r>=0)return e.kind==N_KIND_FOLDER;if(r!=-N_ERR_NOT_FOUND)return 0;r=n_folder_create(path);return (r>=0||r==-N_ERR_EXISTS)&&n_entry_info(path,&e)>=0&&e.kind==N_KIND_FOLDER;}
static int io(n_handle h,uint64_t at,uint8_t *bytes,size_t n,int writing){size_t done=0;while(done<n){n_result r=writing?n_document_write_at(h,at+done,bytes+done,n-done):n_document_read_at(h,at+done,bytes+done,n-done);if(r<=0||(uint64_t)r>n-done)return 0;done+=(size_t)r;}return 1;}
/* The document at path, created empty when absent, open for reading and writing. */
static n_handle document(const char *path){n_handle h=n_document_open(path,N_DOCUMENT_EDIT);if(h!=-N_ERR_NOT_FOUND)return h;h=n_document_open(path,N_DOCUMENT_CREATE);return h==-N_ERR_EXISTS?n_document_open(path,N_DOCUMENT_EDIT):h;}
/* The whole document in a malloc buffer: 0, or a negative n_result. */
static n_result read_whole(const char *path,uint8_t **out,size_t *length){*out=NULL;*length=0;n_handle h=n_document_open(path,N_DOCUMENT_READ);if(h<0)return h;n_result size=n_document_size(h);uint8_t *data=size>=0&&(uint64_t)size<SIZE_MAX?malloc(size?(size_t)size:1):NULL;if(size>=0&&!data)size=-N_ERR_NO_MEMORY;if(size>=0&&!io(h,0,data,(size_t)size,0))size=-N_ERR_IO;n_handle_close(h);if(size<0){free(data);return size;}*out=data;*length=(size_t)size;return 0;}
static void hex(const uint8_t digest[32],char text[65]){for(unsigned i=0;i<32;i++)snprintf(text+i*2,3,"%02x",digest[i]);}
static int unhex(const char *text,uint8_t digest[32]){if(!text||strlen(text)!=64)return 0;for(unsigned i=0;i<32;i++){unsigned v=0;for(unsigned j=0;j<2;j++){char c=text[i*2+j];int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;if(n<0)return 0;v=v*16+(unsigned)n;}digest[i]=(uint8_t)v;}return 1;}
static struct idb_tx *find(webidb *db,uint32_t token){struct idb_tx *p=db?db->active:NULL;while(p&&p->token!=token)p=p->next;return p;}
static void payload_path(struct idb_tx *tx,const uint8_t digest[32],char path[WEBIDB_FILE_MAX]){
    char text[65];hex(digest,text);
    snprintf(path,WEBIDB_FILE_MAX,"%s.values\\%.2s\\%s",tx->path,text,text);
}
static void legacy_payload_path(struct idb_tx *tx,const uint8_t digest[32],char path[WEBIDB_FILE_MAX]){
    char text[65];hex(digest,text);snprintf(path,WEBIDB_FILE_MAX,"%s.v%s",tx->path,text);
}
static int payload_directory(struct idb_tx *tx,const uint8_t digest[32]){
    char text[65],path[WEBIDB_FILE_MAX];hex(digest,text);
    snprintf(path,sizeof path,"%s.values",tx->path);if(!directory(path))return 0;
    snprintf(path,sizeof path,"%s.values\\%.2s",tx->path,text);return directory(path);
}
static int contains(const struct idb_refs *r,const uint8_t digest[32]){size_t lo=0,hi=r->count;while(lo<hi){size_t at=lo+(hi-lo)/2;int c=memcmp(r->data+at*32,digest,32);if(c<0)lo=at+1;else hi=at;}return lo<r->count&&!memcmp(r->data+lo*32,digest,32);}
static int digest_compare(const void *a,const void *b){return memcmp(a,b,32);}
static void collect(struct idb_tx *tx,const struct idb_refs *r,const struct idb_refs *a,const struct idb_refs *b){
    for(size_t i=0;i<r->count;i++){
        const uint8_t *digest=r->data+i*32;
        if(contains(a,digest)||contains(b,digest))continue;
        char path[WEBIDB_FILE_MAX];payload_path(tx,digest,path);n_entry_remove(path);
        legacy_payload_path(tx,digest,path);n_entry_remove(path);
    }
}
static int load(const char *base,int slot,uint8_t **bytes,size_t *length,uint64_t *generation,struct idb_refs *refs){
    uint8_t *data=NULL;size_t n=0;
    int loaded=webidb_journal_read(base,slot,&data,&n);
    if(loaded!=1)return loaded;
    int ok=n>=IDB_HEAD;
    uint8_t digest[32];size_t count=0,offset=0,base_offset=IDB_HEAD,field=40;
    if(ok&&n>=IDB_CATALOG_HEAD&&!memcmp(data,"NIDB0003",8)){
        field=72;
        uint64_t on=u64(data+96),nn=u64(data+104);
        ok=on<=n-IDB_CATALOG_HEAD&&nn<=n-IDB_CATALOG_HEAD-on;
        if(ok){
            base_offset=IDB_CATALOG_HEAD+(size_t)on+(size_t)nn;
            hash(data+72,base_offset-72,digest);
            ok=!memcmp(digest,data+40,32);
        }
    }
    else if(ok)ok=!memcmp(data,"NIDB0002",8);
    if(ok){
        hash(data+field,n-field,digest);
        uint64_t c=u64(data+field+16);
        ok=!memcmp(data+8,digest,32)&&u64(data+field)>0&&c<=(n-base_offset)/32;
        count=(size_t)c;offset=base_offset+count*32;
        if(ok)ok=u64(data+field+8)==n-offset;
        for(size_t i=1;ok&&i<count;i++)
            ok=memcmp(data+base_offset+(i-1)*32,data+base_offset+i*32,32)<0;
    }
    if(!ok){free(data);return -1;}
    refs->data=count?malloc(count*32):NULL;
    if(count&&!refs->data){free(data);return -2;}
    if(count)memcpy(refs->data,data+base_offset,count*32);
    refs->count=count;
    *generation=u64(data+field);
    *length=n-offset;
    memmove(data,data+offset,*length);
    *bytes=data;
    return 1;
}
static int pending_clear(struct idb_tx *tx){
    char path[WEBIDB_FILE_MAX];snprintf(path,sizeof path,"%s.pending",tx->path);
    n_handle h=n_document_open(path,N_DOCUMENT_EDIT);
    if(h==-N_ERR_NOT_FOUND)return WEB_IDB_OK;
    if(h<0)return WEB_IDB_IO;
    n_result size=n_document_size(h);
    int ok=size==0||(size>0&&n_document_set_size(h,0)>=0);
    return n_handle_close(h)<0||!ok?WEB_IDB_IO:WEB_IDB_OK;
}
static int pending_recover(struct idb_tx *tx){
    char path[WEBIDB_FILE_MAX];snprintf(path,sizeof path,"%s.pending",tx->path);
    uint8_t *data;size_t n;
    n_result loaded=read_whole(path,&data,&n);
    if(loaded==-N_ERR_NOT_FOUND)return WEB_IDB_OK;
    if(loaded<0)return loaded==-N_ERR_NO_MEMORY?WEB_IDB_MEMORY:WEB_IDB_IO;
    struct idb_refs pending={data,n/32};
    collect(tx,&pending,&tx->current,&tx->old);
    free(data);
    return pending_clear(tx);
}
static int pending_append(struct idb_tx *tx,const uint8_t digest[32]){
    char path[WEBIDB_FILE_MAX];snprintf(path,sizeof path,"%s.pending",tx->path);
    return n_document_append(path,digest,32)>=0?WEB_IDB_OK:WEB_IDB_IO;
}
webidb *webidb_create(void){return calloc(1,sizeof(webidb));}
void webidb_free(webidb *db){
    if(!db)return;
    while(db->active)webidb_abort(db,db->active->token);
    while(db->clients){struct idb_client *c=db->clients;webidb_forget_connection(db,c->handle);if(db->clients==c){db->clients=c->next;n_handle_close(c->table);free(c);}}
    webidb_journal_forget();free(db);
}
int webidb_begin(webidb *db,const char *origin,const char *name,size_t name_length,uint32_t *token,uint8_t **bytes,size_t *length){
    *bytes=NULL;
    *length=0;
    *token=0;
    if(!db||!name||!web_origin_http(origin))
        return WEB_IDB_SECURITY;
    char root[WEBIDB_BASE_MAX];
    if(!idb_root(root)||!directory(root))return WEB_IDB_IO;
    br_sha256_context h;
    uint8_t len[8],digest[32];
    br_sha256_init(&h);
    put64(len,strlen(origin));
    br_sha256_update(&h,len,8);
    br_sha256_update(&h,origin,strlen(origin));
    put64(len,name_length);
    br_sha256_update(&h,len,8);
    br_sha256_update(&h,name,name_length);
    br_sha256_out(&h,digest);
    struct idb_tx *tx=calloc(1,sizeof *tx);if(!tx)return WEB_IDB_MEMORY;
    char text[65],prefix[3],lease[WEBIDB_FILE_MAX],legacy[WEBIDB_BASE_MAX],probe[WEBIDB_FILE_MAX],shard[WEBIDB_BASE_MAX];hex(digest,text);
    snprintf(prefix,sizeof prefix,"%.2s",text);
    if(n_path_join(root,text,legacy,sizeof legacy)<0){free(tx);return WEB_IDB_IO;}
    struct n_entry existing;bool old_layout=false;
    for(unsigned slot=0;slot<2;slot++){snprintf(probe,sizeof probe,"%s.%u",legacy,slot);if(n_entry_info(probe,&existing)>=0)old_layout=true;}
    if(old_layout)snprintf(tx->path,sizeof tx->path,"%s",legacy);
    else{
        if(n_path_join(root,prefix,shard,sizeof shard)<0||!directory(shard)||n_path_join(shard,text,tx->path,sizeof tx->path)<0){free(tx);return WEB_IDB_IO;}
    }
    /* Nocturne vnode leases are same-process reentrant. Serialize conflicting
       transactions inside this broker too, not just between helper processes. */
    for(struct idb_tx *active=db->active;active;active=active->next)
        if(!strcmp(active->path,tx->path)){free(tx);return WEB_IDB_BUSY;}
    snprintf(lease,sizeof lease,"%s.lock",tx->path);tx->lease=document(lease);
    if(tx->lease<0){free(tx);return WEB_IDB_IO;}
    {n_result claimed=n_document_claim(tx->lease,false);if(claimed<0){n_handle_close(tx->lease);free(tx);return claimed==-N_ERR_NOT_READY?WEB_IDB_BUSY:WEB_IDB_IO;}}
    uint8_t *a=NULL,*b=NULL;
    size_t an=0,bn=0;
    uint64_t ag=0,bg=0;
    struct idb_refs arx={0},brx={0};
    int ar=load(tx->path,0,&a,&an,&ag,&arx);
    int br=load(tx->path,1,&b,&bn,&bg,&brx);
    if(ar==-2||br==-2||((ar<0||br<0)&&ar!=1&&br!=1)){
        free(a);free(b);
        free(arx.data);free(brx.data);
        n_handle_close(tx->lease);free(tx);
        return ar==-2||br==-2?WEB_IDB_MEMORY:WEB_IDB_IO;
    }
    if(ar==1&&(br!=1||ag>=bg)){
        *bytes=a;*length=an;free(b);
        tx->slot=0;tx->generation=ag;
        tx->current=arx;tx->old=brx;
    }else if(br==1){
        *bytes=b;*length=bn;free(a);
        tx->slot=1;tx->generation=bg;
        tx->current=brx;tx->old=arx;
    }else tx->slot=1;

    int recovered=pending_recover(tx);
    tx->origin=strdup(origin);
    tx->name=name_length<SIZE_MAX?malloc(name_length+1):NULL;
    if(tx->name){
        memcpy(tx->name,name,name_length);
        tx->name[name_length]=0;tx->name_length=name_length;
    }
    if(!tx->origin||!tx->name)recovered=WEB_IDB_MEMORY;
    if(recovered){
        free(*bytes);*bytes=NULL;*length=0;
        free(tx->origin);free(tx->name);
        free(tx->current.data);free(tx->old.data);
        n_handle_close(tx->lease);free(tx);
        return recovered;
    }
    do{
        if(++db->serial==0)++db->serial;
        tx->token=db->serial;
        if(!find(db,tx->token))break;
    }while(1);
    tx->next=db->active;db->active=tx;
    *token=tx->token;
    return WEB_IDB_OK;
}
int webidb_value(webidb *db,uint32_t token,const char *reference,uint8_t **bytes,size_t *length){
    *bytes=NULL;*length=0;struct idb_tx *tx=find(db,token);uint8_t digest[32];if(!tx||!unhex(reference,digest))return WEB_IDB_IO;
    int permitted=contains(&tx->current,digest);for(struct idb_stage *p=tx->staged;!permitted&&p;p=p->next)permitted=!memcmp(p->digest,digest,32);if(!permitted)return WEB_IDB_SECURITY;
    char path[WEBIDB_FILE_MAX];payload_path(tx,digest,path);uint8_t *data;size_t n;
    n_result loaded=read_whole(path,&data,&n);
    if(loaded==-N_ERR_NOT_FOUND){legacy_payload_path(tx,digest,path);loaded=read_whole(path,&data,&n);}
    if(loaded<0)return loaded==-N_ERR_NO_MEMORY?WEB_IDB_MEMORY:WEB_IDB_IO;
    uint8_t actual[32];hash(data,n,actual);
    if(memcmp(actual,digest,32)){free(data);return WEB_IDB_IO;}*bytes=data;*length=n;return WEB_IDB_OK;
}
int webidb_stage(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,char reference[65]){
    struct idb_tx *tx=find(db,token);
    if(!tx||length>LONG_MAX)return WEB_IDB_IO;
    uint8_t digest[32];
    hash(bytes,length,digest);hex(digest,reference);
    if(contains(&tx->current,digest))return WEB_IDB_OK;
    for(struct idb_stage *p=tx->staged;p;p=p->next)if(!memcmp(p->digest,digest,32))return WEB_IDB_OK;
    struct idb_stage *p=malloc(sizeof *p);if(!p)return WEB_IDB_MEMORY;
    char path[WEBIDB_FILE_MAX];payload_path(tx,digest,path);
    if(!contains(&tx->old,digest)){
        if(!payload_directory(tx,digest)){free(p);return WEB_IDB_IO;}
        int journal=pending_append(tx,digest);if(journal){free(p);return journal;}
        /* Swapped in whole: a failed save never leaves a torn payload. */
        if(n_document_save(path,bytes,length)<0){free(p);return WEB_IDB_IO;}
    }
    memcpy(p->digest,digest,32);
    p->next=tx->staged;tx->staged=p;
    return WEB_IDB_OK;
}
/* The staged payloads at the end of a transaction (or of a commit that keeps the lease): the ones
   neither generation references are removed, and the journal of the transaction's payloads is
   cleared (failed clearing is retried by the next begin; it cannot invalidate a commit). */
static void staged_drop(struct idb_tx *tx){
    while(tx->staged){
        struct idb_stage *s=tx->staged;
        tx->staged=s->next;
        if(!contains(&tx->current,s->digest)&&!contains(&tx->old,s->digest)){
            char path[WEBIDB_FILE_MAX];payload_path(tx,s->digest,path);n_entry_remove(path);
        }
        free(s);
    }
    pending_clear(tx);
}
int webidb_abort(webidb *db,uint32_t token){
    if(!db)return WEB_IDB_IO;
    struct idb_tx **p=&db->active;
    while(*p&&(*p)->token!=token)p=&(*p)->next;
    if(!*p)return WEB_IDB_IO;
    struct idb_tx *tx=*p;
    *p=tx->next;
    staged_drop(tx);
    free(tx->origin);free(tx->name);
    free(tx->current.data);free(tx->old.data);
    n_handle_close(tx->lease);free(tx);
    return WEB_IDB_OK;
}
/* The manifest of the next generation written in the other slot, every reference current, old
   or staged; payloads neither generation keeps are collected. */
static int manifest_write(struct idb_tx *tx,const uint8_t *bytes,size_t length,const char *const *references,size_t count,uint64_t version){
    if(tx->generation==UINT64_MAX)return WEB_IDB_IO;
    size_t on=strlen(tx->origin);
    if(on>SIZE_MAX-IDB_CATALOG_HEAD||tx->name_length>SIZE_MAX-IDB_CATALOG_HEAD-on)
        return WEB_IDB_MEMORY;
    size_t head=IDB_CATALOG_HEAD+on+tx->name_length;
    if(head>(size_t)LONG_MAX||count>(SIZE_MAX-head)/32||
       count>((size_t)LONG_MAX-head)/32||length>SIZE_MAX-head-count*32||
       length>(size_t)LONG_MAX-head-count*32)return WEB_IDB_MEMORY;

    struct idb_refs live={count?malloc(count*32):NULL,count};
    if(count&&!live.data)return WEB_IDB_MEMORY;
    for(size_t i=0;i<count;i++)if(!unhex(references[i],live.data+i*32)){
        free(live.data);return WEB_IDB_IO;
    }
    if(count)qsort(live.data,count,32,digest_compare);
    size_t unique=0;
    for(size_t i=0;i<count;i++){
        if(!unique||memcmp(live.data+(unique-1)*32,live.data+i*32,32)){
            memmove(live.data+unique*32,live.data+i*32,32);
            unique++;
        }
    }
    live.count=unique;
    for(size_t i=0;i<unique;i++){
        const uint8_t *digest=live.data+i*32;
        int present=contains(&tx->current,digest)||contains(&tx->old,digest);
        for(struct idb_stage *p=tx->staged;!present&&p;p=p->next)
            present=!memcmp(p->digest,digest,32);
        if(!present){free(live.data);return WEB_IDB_SECURITY;}
    }
    size_t offset=head+unique*32,n=offset+length;
    uint8_t *data=malloc(n);if(!data){free(live.data);return WEB_IDB_MEMORY;}
    memcpy(data,"NIDB0003",8);put64(data+72,tx->generation+1);put64(data+80,length);put64(data+88,unique);
    put64(data+96,on);put64(data+104,tx->name_length);put64(data+112,version);
    memcpy(data+IDB_CATALOG_HEAD,tx->origin,on);memcpy(data+IDB_CATALOG_HEAD+on,tx->name,tx->name_length);
    hash(data+72,head-72,data+40);
    if(unique)memcpy(data+head,live.data,unique*32);
    if(length)memcpy(data+offset,bytes,length);
    hash(data+72,n-72,data+8);
    int written=webidb_journal_write(tx->path,1-tx->slot,data,n);
    free(data);
    if(written!=1){free(live.data);return written==-2?WEB_IDB_MEMORY:WEB_IDB_IO;}
    collect(tx,&tx->old,&live,&tx->current);
    free(tx->old.data);
    tx->old=tx->current;
    tx->current=live;
    tx->slot=1-tx->slot;tx->generation++;
    return WEB_IDB_OK;
}
int webidb_commit(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,const char *const *references,size_t count,uint64_t version){
    struct idb_tx *tx=find(db,token);
    if(!tx)return WEB_IDB_IO;
    int status=manifest_write(tx,bytes,length,references,count,version);
    return status?status:webidb_abort(db,token);
}
/* The same commit with the lease kept: the token stays the database's, at the generation just
   written, for the transactions the realm schedules after this one. */
int webidb_save(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,const char *const *references,size_t count,uint64_t version){
    struct idb_tx *tx=find(db,token);
    if(!tx)return WEB_IDB_IO;
    int status=manifest_write(tx,bytes,length,references,count,version);
    if(!status)staged_drop(tx);
    return status;
}

/* A slot's catalog: the database's name and version, when it is origin's (origin NULL: any, its
   origin then malloc'd in *named). 1, 0 missing or another origin's, -1 invalid, -2 no memory. */
static int catalog(const char *base,int slot,const char *origin,struct webidb_database *item,uint64_t *generation,char **named){
    uint8_t *data=NULL;size_t size=0;
    int loaded=webidb_journal_read(base,slot,&data,&size);if(loaded!=1)return loaded;
    if(size<IDB_CATALOG_HEAD||memcmp(data,"NIDB0003",8)){free(data);return 0;}
    uint64_t on=u64(data+96),nn=u64(data+104),refs=u64(data+88),meta=u64(data+80);
    size_t remaining=size-IDB_CATALOG_HEAD;
    if(on>remaining||nn>remaining-on||refs>(remaining-on-nn)/32||meta!=remaining-on-nn-refs*32||
       (origin&&on!=strlen(origin))||nn>SIZE_MAX-1){free(data);return -1;}
    size_t n=IDB_CATALOG_HEAD+(size_t)on+(size_t)nn;uint8_t digest[32];
    hash(data+72,n-72,digest);
    int ok=!memcmp(digest,data+40,32)&&u64(data+72)>0;
    if(ok&&origin&&memcmp(data+IDB_CATALOG_HEAD,origin,(size_t)on)){free(data);return 0;}
    /* Validate reconstructed metadata and both checksums, never opening an
       immutable value payload. Warm catalog checks reuse the broker cache. */
    if(ok){hash(data+72,size-72,digest);ok=!memcmp(digest,data+8,32);}
    if(!ok){free(data);return -1;}
    /* Bind catalog names back to their private hashed filename, preventing
     * a malformed header from becoming a cross-origin database alias. */
    br_sha256_context h;br_sha256_init(&h);uint8_t len[8];put64(len,on);br_sha256_update(&h,len,8);br_sha256_update(&h,data+IDB_CATALOG_HEAD,(size_t)on);put64(len,nn);br_sha256_update(&h,len,8);br_sha256_update(&h,data+IDB_CATALOG_HEAD+on,(size_t)nn);br_sha256_out(&h,digest);char text[65];hex(digest,text);if(strcmp(n_path_name(base),text)){free(data);return -1;}
    item->name=malloc((size_t)nn+1);if(!item->name){free(data);return -2;}
    if(named){
        if(!(*named=malloc((size_t)on+1))){free(item->name);item->name=NULL;free(data);return -2;}
        memcpy(*named,data+IDB_CATALOG_HEAD,(size_t)on);(*named)[on]=0;
    }
    memcpy(item->name,data+IDB_CATALOG_HEAD+on,(size_t)nn);item->name[nn]=0;
    item->name_length=(size_t)nn;item->version=u64(data+112);*generation=u64(data+72);free(data);return 1;
}
void webidb_databases_free(struct webidb_database *items,size_t count){for(size_t i=0;i<count;i++)free(items[i].name);free(items);}
static bool shard_name(const char *name){return strlen(name)==2&&strspn(name,"0123456789abcdef")==2;}
static int catalog_scan(const char *directory_path,const char *origin,struct webidb_database **items,size_t *count,bool shards){
    struct n_entry *entries=NULL;n_result listed=n_folder_entries(directory_path,&entries);
    if(listed<0)return listed==-N_ERR_NOT_FOUND?WEB_IDB_OK:listed==-N_ERR_NO_MEMORY?WEB_IDB_MEMORY:WEB_IDB_IO;
    int result=WEB_IDB_OK;
    for(n_result index=0;index<listed;index++){
        const struct n_entry *entry=&entries[index];
        if(shards&&entry->kind==N_KIND_FOLDER&&shard_name(entry->name)){
            char child[WEBIDB_BASE_MAX];if(n_path_join(directory_path,entry->name,child,sizeof child)<0)continue;
            result=catalog_scan(child,origin,items,count,false);if(result)break;continue;
        }
        size_t len=strlen(entry->name);
        if(len!=66||entry->name[64]!='.'||(entry->name[65]!='0'&&entry->name[65]!='1'))continue;
        char text[65];memcpy(text,entry->name,64);text[64]=0;uint8_t digest[32];if(!unhex(text,digest))continue;
        char base[WEBIDB_BASE_MAX],lock[WEBIDB_FILE_MAX];if(n_path_join(directory_path,text,base,sizeof base)<0)continue;snprintf(lock,sizeof lock,"%s.lock",base);
        n_handle lease=document(lock);if(lease<0){result=WEB_IDB_IO;break;}
        n_result claimed=n_document_claim(lease,false);
        if(claimed<0){result=claimed==-N_ERR_NOT_READY?WEB_IDB_BUSY:WEB_IDB_IO;n_handle_close(lease);break;}
        struct webidb_database a={0},b={0};uint64_t ag=0,bg=0;int ar=catalog(base,0,origin,&a,&ag,NULL),br=catalog(base,1,origin,&b,&bg,NULL);n_handle_close(lease);if(ar==-2||br==-2){free(a.name);free(b.name);result=WEB_IDB_MEMORY;break;}struct webidb_database chosen={0};if(ar==1&&(br!=1||ag>=bg)){chosen=a;free(b.name);}else if(br==1){chosen=b;free(a.name);}else{free(a.name);free(b.name);continue;}if(!chosen.version){free(chosen.name);continue;}int duplicate=0;for(size_t i=0;i<*count;i++)if((*items)[i].name_length==chosen.name_length&&!memcmp((*items)[i].name,chosen.name,chosen.name_length)){duplicate=1;break;}if(duplicate){free(chosen.name);continue;}if(*count>=SIZE_MAX/sizeof **items){free(chosen.name);result=WEB_IDB_MEMORY;break;}struct webidb_database *more=realloc(*items,(*count+1)*sizeof **items);if(!more){free(chosen.name);result=WEB_IDB_MEMORY;break;}*items=more;(*items)[(*count)++]=chosen;
    }
    free(entries);if(result){webidb_databases_free(*items,*count);*items=NULL;*count=0;}return result;
}
int webidb_databases(webidb *db,const char *origin,struct webidb_database **items,size_t *count){
    *items=NULL;*count=0;
    if(!db||!web_origin_http(origin))return WEB_IDB_SECURITY;
    char root[WEBIDB_BASE_MAX];
    return idb_root(root)?catalog_scan(root,origin,items,count,true):WEB_IDB_IO;
}

/* The Settings window's view of the databases, whatever their origin. A database's files hang
   from its base (folder\<digest>): the slots .0 and .1, .journal0 and .journal1, .lock, .pending,
   the payloads in .values (two levels) and the older .v<digest>; its catalog names its origin. */
static int files_tree(const char *path,bool remove,uint64_t *bytes){
    struct n_entry *entries=NULL;n_result listed=n_folder_entries(path,&entries);
    if(listed<0)return listed==-N_ERR_NOT_FOUND?WEB_IDB_OK:listed==-N_ERR_NO_MEMORY?WEB_IDB_MEMORY:WEB_IDB_IO;
    int result=WEB_IDB_OK;
    for(n_result i=0;i<listed&&!result;i++){
        char child[WEBIDB_FILE_MAX];
        if(n_path_join(path,entries[i].name,child,sizeof child)<0){result=WEB_IDB_IO;break;}
        if(entries[i].kind==N_KIND_FOLDER)result=files_tree(child,remove,bytes);
        else{*bytes+=entries[i].size;if(remove&&n_entry_remove(child)<0)result=WEB_IDB_IO;}
    }
    free(entries);
    if(!result&&remove&&n_entry_remove(path)<0)result=WEB_IDB_IO;
    return result;
}
struct idb_visit {
    void (*each)(void *opaque,const char *origin,uint64_t bytes);
    bool (*match)(void *opaque,const char *origin);
    void *opaque;
    bool busy;
};
/* The origin the newer readable slot of base names, malloc'd ("" when neither can be read). */
static int base_origin(const char *base,char **origin){
    struct webidb_database a={0},b={0};uint64_t ag=0,bg=0;char *ao=NULL,*bo=NULL;
    int ar=catalog(base,0,NULL,&a,&ag,&ao),br=catalog(base,1,NULL,&b,&bg,&bo);
    free(a.name);free(b.name);
    if(ar==-2||br==-2){free(ao);free(bo);return WEB_IDB_MEMORY;}
    if(ar==1&&(br!=1||ag>=bg)){*origin=ao;free(bo);}
    else if(br==1){*origin=bo;free(ao);}
    else{free(ao);free(bo);*origin=calloc(1,1);}
    return *origin?WEB_IDB_OK:WEB_IDB_MEMORY;
}
static void clients_force(const char *text);
/* One database: its bytes, and with remove its files (under its lease: a transaction now leaves it). */
static int database_visit(const char *folder,const struct n_entry *entries,n_result listed,const char *text,struct idb_visit *v,bool remove){
    char base[WEBIDB_BASE_MAX];if(n_path_join(folder,text,base,sizeof base)<0)return WEB_IDB_IO;
    char *origin=NULL;int result=base_origin(base,&origin);if(result)return result;
    if(v->match&&!v->match(v->opaque,origin)){free(origin);return WEB_IDB_OK;}
    n_handle lease=-1;
    if(remove){
        char lock[WEBIDB_FILE_MAX];snprintf(lock,sizeof lock,"%s.lock",base);
        lease=document(lock);
        n_result claimed=lease<0?lease:n_document_claim(lease,false);
        if(claimed<0){if(lease>=0)n_handle_close(lease);free(origin);if(claimed!=-N_ERR_NOT_READY)return WEB_IDB_IO;v->busy=true;return WEB_IDB_OK;}
    }
    uint64_t bytes=0;
    for(n_result i=0;i<listed&&!result;i++){
        const struct n_entry *e=&entries[i];
        if(strncmp(e->name,text,64)||e->name[64]!='.')continue;
        char path[WEBIDB_FILE_MAX];if(n_path_join(folder,e->name,path,sizeof path)<0){result=WEB_IDB_IO;break;}
        if(e->kind==N_KIND_FOLDER)result=files_tree(path,remove,&bytes);
        else{bytes+=e->size;if(remove&&strcmp(e->name+64,".lock")&&n_entry_remove(path)<0)result=WEB_IDB_IO;}
    }
    /* Its open connections, of every process, are closed forcibly (IndexedDB "close a database
       connection" with forced: their transactions abort, then close fires at them). */
    if(remove&&!result)clients_force(text);
    /* The lease document stays (empty): a transaction that opened it meanwhile would otherwise
       hold a removed document while the next one claims a new one. */
    if(lease>=0){n_document_release(lease);n_handle_close(lease);}
    if(!result&&v->each&&(*origin||bytes))v->each(v->opaque,origin,bytes); /* not a lease alone */
    free(origin);return result;
}
static int databases_in(const char *folder,struct idb_visit *v,bool shards,bool remove){
    struct n_entry *entries=NULL;n_result listed=n_folder_entries(folder,&entries);
    if(listed<0)return listed==-N_ERR_NOT_FOUND?WEB_IDB_OK:listed==-N_ERR_NO_MEMORY?WEB_IDB_MEMORY:WEB_IDB_IO;
    int result=WEB_IDB_OK;
    for(n_result i=0;i<listed&&!result;i++){
        const struct n_entry *e=&entries[i];
        if(shards&&e->kind==N_KIND_FOLDER&&shard_name(e->name)){
            char child[WEBIDB_BASE_MAX];if(n_path_join(folder,e->name,child,sizeof child)<0)continue;
            result=databases_in(child,v,false,remove);continue;
        }
        char text[65];uint8_t digest[32];
        if(strlen(e->name)<66||e->name[64]!='.')continue;
        memcpy(text,e->name,64);text[64]=0;if(!unhex(text,digest))continue;
        bool first=true; /* each database once, at its first entry */
        for(n_result k=0;k<i&&first;k++)first=strncmp(entries[k].name,text,64)||entries[k].name[64]!='.';
        if(first)result=database_visit(folder,entries,listed,text,v,remove);
    }
    free(entries);return result;
}
static int databases_visit(struct idb_visit *v,bool remove){
    char root[WEBIDB_BASE_MAX];if(!idb_root(root))return WEB_IDB_IO;
    int result=databases_in(root,v,true,remove);
    return result?result:v->busy?WEB_IDB_BUSY:WEB_IDB_OK;
}
int webidb_usage(void (*each)(void *opaque,const char *origin,uint64_t bytes),void *opaque){
    struct idb_visit v={.each=each,.opaque=opaque};
    return each?databases_visit(&v,false):WEB_IDB_OK;
}
int webidb_clear(bool (*match)(void *opaque,const char *origin),void *opaque){
    struct idb_visit v={.match=match,.opaque=opaque};
    return databases_visit(&v,true);
}

/* Registry in indexeddb in the profile's temporary folder, so connection leases/notifications never
 * survive a restart. Process start times prevent PID reuse from resurrecting dead connections. */
#define CLIENT_BYTES 48u
struct client_table {uint8_t *rows;size_t count;struct n_procinfo *processes;int process_count;};
static n_handle clients_open(struct idb_tx *tx){
    const char *profile=web_profile_folder(true);char folder[WEBIDB_BASE_MAX],leaf[80],path[WEBIDB_BASE_MAX];
    if(!profile||n_path_join(profile,"indexeddb",folder,sizeof folder)<0||!directory(folder))return -1;
    snprintf(leaf,sizeof leaf,"%s.clients",n_path_name(tx->path));
    return n_path_join(folder,leaf,path,sizeof path)<0?-1:document(path);
}
static int clients_lock(n_handle table){n_result claimed=n_document_claim(table,false);return claimed>=0?WEB_IDB_OK:claimed==-N_ERR_NOT_READY?WEB_IDB_BUSY:WEB_IDB_IO;}
static int processes(struct client_table *t){int capacity=32;for(;;){t->processes=malloc((size_t)capacity*sizeof *t->processes);if(!t->processes)return WEB_IDB_MEMORY;int n=proclist(t->processes,capacity);if(n<0){free(t->processes);t->processes=NULL;return WEB_IDB_IO;}if(n<capacity){t->process_count=n;return WEB_IDB_OK;}free(t->processes);t->processes=NULL;if(capacity>INT_MAX/2)return WEB_IDB_MEMORY;capacity*=2;}}
static int client_alive(struct client_table *t,uint8_t *row){for(int i=0;i<t->process_count;i++)if(t->processes[i].pid==(int)u64(row+8)&&t->processes[i].start_ms==u64(row+16)&&t->processes[i].state!=3)return 1;return 0;}
static void table_free(struct client_table *t){free(t->rows);free(t->processes);memset(t,0,sizeof *t);}
static int table_read(n_handle table,struct client_table *t){n_result size=n_document_size(table);if(size<0||size%CLIENT_BYTES||(uint64_t)size>SIZE_MAX-CLIENT_BYTES)return WEB_IDB_IO;size_t n=(size_t)size;t->rows=calloc(1,n+CLIENT_BYTES);if(!t->rows)return WEB_IDB_MEMORY;t->count=n/CLIENT_BYTES;if(!io(table,0,t->rows,n,0)){table_free(t);return WEB_IDB_IO;}int status=processes(t);if(status){table_free(t);return status;}for(size_t i=0;i<t->count;i++){uint8_t *row=t->rows+i*CLIENT_BYTES;if(u64(row)&&!client_alive(t,row))memset(row,0,CLIENT_BYTES);}return WEB_IDB_OK;}
static int table_write(n_handle table,struct client_table *t){return io(table,0,t->rows,t->count*CLIENT_BYTES,1)?WEB_IDB_OK:WEB_IDB_IO;}
/* A row: identity, pid, process start, version, desired version, and its versionchange to the
 * desired version: CLIENT_NONE, CLIENT_MARKED (by connections), CLIENT_DELIVERED (by poll), or
 * CLIENT_FIRED (by fired). CLIENT_FORCED: its database was cleared (clients_force); its poll
 * delivers the forced close once, and it is then close pending (CLIENT_FIRED: it is still open,
 * and needs no versionchange; its desired version is none an opener asks for).
 * CLIENT_GHOST: a free row (identity 0) left by a connection that closed after its versionchange
 * to the desired version was fired while it was open. The opener that asked for that version had
 * to fire blocked at its request ("If any of the connections in openConnections are still not
 * closed" once every versionchange was fired), however soon the close came; the ghost is that
 * evidence, counted once by its connections and then cleared. A ghost of another desired version
 * is stale (its opener has gone on or gone) and is cleared; connect never reuses a ghost. */
enum {CLIENT_NONE,CLIENT_MARKED,CLIENT_DELIVERED,CLIENT_FIRED,CLIENT_FORCED,CLIENT_GHOST};
int webidb_connections(webidb *db,uint32_t token,uint64_t version,size_t *count,size_t *unfired){*count=0;*unfired=0;struct idb_tx *tx=find(db,token);if(!tx)return WEB_IDB_IO;n_handle table=clients_open(tx);if(table<0)return WEB_IDB_IO;int status=clients_lock(table);struct client_table t={0};if(!status)status=table_read(table,&t);if(!status){size_t ghosts=0;
        for(size_t i=0;i<t.count;i++){uint8_t *row=t.rows+i*CLIENT_BYTES;
            if(!u64(row)){if(u64(row+40)==CLIENT_GHOST){if(u64(row+32)==version)ghosts++;else memset(row,0,CLIENT_BYTES);}continue;}
            (*count)++;if(u64(row+40)==CLIENT_FORCED)continue;if(u64(row+40)==CLIENT_NONE||u64(row+32)!=version){put64(row+32,version);put64(row+40,CLIENT_MARKED);}if(u64(row+40)!=CLIENT_FIRED)(*unfired)++;}
        /* The ghosts count as open until every versionchange was fired; then once, cleared. */
        *count+=ghosts;
        if(!*unfired)for(size_t i=0;ghosts&&i<t.count;i++){uint8_t *row=t.rows+i*CLIENT_BYTES;if(!u64(row)&&u64(row+40)==CLIENT_GHOST&&u64(row+32)==version)memset(row,0,CLIENT_BYTES);}
        status=table_write(table,&t);}table_free(&t);n_handle_close(table);return status;}
static void clients_force(const char *text){
    const char *profile=web_profile_folder(true);char folder[WEBIDB_BASE_MAX],leaf[80],path[WEBIDB_BASE_MAX];
    if(!profile||n_path_join(profile,"indexeddb",folder,sizeof folder)<0)return;
    snprintf(leaf,sizeof leaf,"%s.clients",text);
    if(n_path_join(folder,leaf,path,sizeof path)<0)return;
    n_handle table=n_document_open(path,N_DOCUMENT_EDIT);if(table<0)return; /* none ever connected */
    /* Row I/O only (as webidb_forget_connection): the clearing waits for it, never for author code. */
    if(n_document_claim(table,true)<0){n_handle_close(table);return;}
    struct client_table t={0};
    if(!table_read(table,&t)){bool forced=false;for(size_t i=0;i<t.count;i++){uint8_t *row=t.rows+i*CLIENT_BYTES;if(u64(row)){put64(row+40,CLIENT_FORCED);forced=true;}}if(forced)table_write(table,&t);}
    table_free(&t);n_handle_close(table);
}
int webidb_fired(webidb *db,uint32_t connection){struct idb_client *c=db?db->clients:NULL;while(c&&c->handle!=connection)c=c->next;if(!c)return WEB_IDB_IO;int status=clients_lock(c->table);if(status)return status;uint8_t row[CLIENT_BYTES];if(!io(c->table,c->offset,row,sizeof row,0)||u64(row)!=c->identity)status=WEB_IDB_IO;else if(u64(row+40)==CLIENT_DELIVERED){put64(row+40,CLIENT_FIRED);if(!io(c->table,c->offset,row,sizeof row,1))status=WEB_IDB_IO;}n_document_release(c->table);return status;}
int webidb_connect(webidb *db,uint32_t token,uint64_t version,uint32_t *connection){*connection=0;struct idb_tx *tx=find(db,token);if(!tx)return WEB_IDB_IO;n_handle table=clients_open(tx);if(table<0)return WEB_IDB_IO;int status=clients_lock(table);struct client_table t={0};if(!status)status=table_read(table,&t);struct idb_client *c=NULL;
    if(!status){c=calloc(1,sizeof *c);if(!c)status=WEB_IDB_MEMORY;else{uint32_t serial=__atomic_add_fetch(&client_serial,1,__ATOMIC_RELAXED);if(!serial)serial=__atomic_add_fetch(&client_serial,1,__ATOMIC_RELAXED);c->handle=serial;c->identity=(uint64_t)(uint32_t)n_app_self()<<32|serial;c->table=table;size_t i=0;while(i<t.count&&(u64(t.rows+i*CLIENT_BYTES)||u64(t.rows+i*CLIENT_BYTES+40)==CLIENT_GHOST))i++;if(i==t.count)t.count++;c->offset=i*CLIENT_BYTES;uint8_t *row=t.rows+c->offset;put64(row,c->identity);put64(row+8,(uint32_t)n_app_self());uint64_t start=0;for(int j=0;j<t.process_count;j++)if(t.processes[j].pid==(int)n_app_self())start=t.processes[j].start_ms;put64(row+16,start);put64(row+24,version);status=table_write(table,&t);}}
    table_free(&t);if(status){if(c)free(c);n_handle_close(table);return status;}n_document_release(table);c->next=db->clients;db->clients=c;*connection=c->handle;return WEB_IDB_OK;
}
int webidb_poll(webidb *db,uint32_t connection,int *pending,uint64_t *version){*pending=0;*version=0;struct idb_client *c=db?db->clients:NULL;while(c&&c->handle!=connection)c=c->next;if(!c)return WEB_IDB_IO;int status=clients_lock(c->table);if(status)return status;uint8_t row[CLIENT_BYTES];if(!io(c->table,c->offset,row,sizeof row,0)||u64(row)!=c->identity)status=WEB_IDB_IO;else if(u64(row+40)==CLIENT_FORCED){*pending=2;put64(row+32,UINT64_MAX);put64(row+40,CLIENT_FIRED);if(!io(c->table,c->offset,row,sizeof row,1))status=WEB_IDB_IO;}else if(u64(row+40)==CLIENT_MARKED){*pending=1;*version=u64(row+32);put64(row+40,CLIENT_DELIVERED);if(!io(c->table,c->offset,row,sizeof row,1))status=WEB_IDB_IO;}n_document_release(c->table);return status;}
int webidb_disconnect(webidb *db,uint32_t connection){if(!db)return WEB_IDB_IO;struct idb_client **p=&db->clients;while(*p&&(*p)->handle!=connection)p=&(*p)->next;if(!*p)return WEB_IDB_IO;struct idb_client *c=*p;int status=clients_lock(c->table);if(status)return status;uint8_t row[CLIENT_BYTES];if(!io(c->table,c->offset,row,sizeof row,0)||u64(row)!=c->identity)status=WEB_IDB_IO;else{uint64_t desired=u64(row+32);int ghost=u64(row+40)==CLIENT_FIRED;memset(row,0,sizeof row);if(ghost){put64(row+32,desired);put64(row+40,CLIENT_GHOST);}if(!io(c->table,c->offset,row,sizeof row,1))status=WEB_IDB_IO;}n_document_release(c->table);*p=c->next;n_handle_close(c->table);free(c);return status;}
int webidb_forget_connection(webidb *db,uint32_t connection){
    int result=webidb_disconnect(db,connection);if(result!=WEB_IDB_BUSY)return result;
    struct idb_client *c=db?db->clients:NULL;while(c&&c->handle!=connection)c=c->next;if(!c)return WEB_IDB_IO;
    /* This lease covers temporary-folder row I/O only, never author code or a
     * transaction. An unpublished OOM connection cannot be left for JS to close. */
    if(n_document_claim(c->table,true)<0)return WEB_IDB_IO;
    return webidb_disconnect(db,connection);
}
