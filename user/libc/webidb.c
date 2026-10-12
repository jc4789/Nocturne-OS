/* IndexedDB manifests contain schema/keys/index keys and immutable payload
 * references, not video segment bytes. Updating one value writes that payload
 * once; commit writes only metadata. Two checksum-protected manifest slots
 * retain crash fallback, and payload GC keeps both committed generations. */
#include <webidb.h>
#include <nocturne.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <bearssl.h>
#define IDB_ROOT "/data/browser/indexeddb"
#define IDB_HEAD 64u
#define IDB_CATALOG_HEAD 120u
struct idb_refs {uint8_t *data;size_t count;};
struct idb_stage {struct idb_stage *next;uint8_t digest[32];};
struct idb_tx {struct idb_tx *next;uint32_t token;int lease,slot;uint64_t generation;char path[160];char *origin,*name;size_t name_length;struct idb_refs current,old;struct idb_stage *staged;};
struct idb_client {struct idb_client *next;uint32_t handle;uint64_t identity;size_t offset;int fd;};
struct webidb {struct idb_tx *active;struct idb_client *clients;uint32_t serial;};
static uint32_t client_serial;
static void hash(const void *bytes,size_t n,uint8_t out[32]){br_sha256_context h;br_sha256_init(&h);br_sha256_update(&h,bytes,n);br_sha256_out(&h,out);}
static uint64_t u64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static int directory(const char *path){struct n_stat st;if(!stat(path,&st))return st.type==2;if(errno!=ENOENT)return 0;return !mkdir(path,0)||(!stat(path,&st)&&st.type==2);}
static int io(int fd,uint8_t *bytes,size_t n,int writing){size_t at=0;while(at<n){size_t step=n-at;if(step>INT_MAX)step=INT_MAX;ssize_t done=writing?write(fd,bytes+at,step):read(fd,bytes+at,step);if(done<=0)return 0;at+=(size_t)done;}return 1;}
static void hex(const uint8_t digest[32],char text[65]){for(unsigned i=0;i<32;i++)snprintf(text+i*2,3,"%02x",digest[i]);}
static int unhex(const char *text,uint8_t digest[32]){if(!text||strlen(text)!=64)return 0;for(unsigned i=0;i<32;i++){unsigned v=0;for(unsigned j=0;j<2;j++){char c=text[i*2+j];int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;if(n<0)return 0;v=v*16+(unsigned)n;}digest[i]=(uint8_t)v;}return 1;}
static struct idb_tx *find(webidb *db,uint32_t token){struct idb_tx *p=db?db->active:NULL;while(p&&p->token!=token)p=p->next;return p;}
static void payload_path(struct idb_tx *tx,const uint8_t digest[32],char path[240]){char text[65];hex(digest,text);snprintf(path,240,"%s.v%s",tx->path,text);}
static int contains(const struct idb_refs *r,const uint8_t digest[32]){size_t lo=0,hi=r->count;while(lo<hi){size_t at=lo+(hi-lo)/2;int c=memcmp(r->data+at*32,digest,32);if(c<0)lo=at+1;else hi=at;}return lo<r->count&&!memcmp(r->data+lo*32,digest,32);}
static int digest_compare(const void *a,const void *b){return memcmp(a,b,32);}
static void collect(struct idb_tx *tx,const struct idb_refs *r,const struct idb_refs *a,const struct idb_refs *b){for(size_t i=0;i<r->count;i++){const uint8_t *d=r->data+i*32;if(!contains(a,d)&&!contains(b,d)){char path[240];payload_path(tx,d,path);unlink(path);}}}
static int load(const char *base,int slot,uint8_t **bytes,size_t *length,uint64_t *generation,struct idb_refs *refs){
    char path[168];snprintf(path,sizeof path,"%s.%d",base,slot);struct n_stat st;if(stat(path,&st)<0)return errno==ENOENT?0:-1;
    if(st.size<IDB_HEAD||st.size>SIZE_MAX||st.size>LONG_MAX)return -1;
    size_t n=(size_t)st.size;uint8_t *data=malloc(n);if(!data)return -2;int fd=open(path,O_RDONLY),ok=fd>=0&&io(fd,data,n,0);if(fd>=0)close(fd);
    uint8_t digest[32];size_t count=0,offset=0,base_offset=IDB_HEAD,field=40;
    if(ok&&n>=IDB_CATALOG_HEAD&&!memcmp(data,"NIDB0003",8)){field=72;uint64_t on=u64(data+96),nn=u64(data+104);ok=on<=n-IDB_CATALOG_HEAD&&nn<=n-IDB_CATALOG_HEAD-on;if(ok){base_offset=IDB_CATALOG_HEAD+(size_t)on+(size_t)nn;hash(data+72,base_offset-72,digest);ok=!memcmp(digest,data+40,32);}}
    else if(ok)ok=!memcmp(data,"NIDB0002",8);
    if(ok){hash(data+field,n-field,digest);uint64_t c=u64(data+field+16);ok=!memcmp(data+8,digest,32)&&u64(data+field)>0&&c<=(n-base_offset)/32;count=(size_t)c;offset=base_offset+count*32;if(ok)ok=u64(data+field+8)==n-offset;for(size_t i=1;ok&&i<count;i++)ok=memcmp(data+base_offset+(i-1)*32,data+base_offset+i*32,32)<0;}
    if(!ok){free(data);return -1;}refs->data=count?malloc(count*32):NULL;if(count&&!refs->data){free(data);return -2;}if(count)memcpy(refs->data,data+base_offset,count*32);refs->count=count;*generation=u64(data+field);*length=n-offset;memmove(data,data+offset,*length);*bytes=data;return 1;
}
static int pending_clear(struct idb_tx *tx){char path[176];snprintf(path,sizeof path,"%s.pending",tx->path);struct n_stat st;if(stat(path,&st)<0)return errno==ENOENT?WEB_IDB_OK:WEB_IDB_IO;if(st.size==0)return WEB_IDB_OK;int fd=open(path,O_WRONLY|O_TRUNC);if(fd<0)return WEB_IDB_IO;return close(fd)<0?WEB_IDB_IO:WEB_IDB_OK;}
static int pending_recover(struct idb_tx *tx){char path[176];snprintf(path,sizeof path,"%s.pending",tx->path);struct n_stat st;if(stat(path,&st)<0)return errno==ENOENT?WEB_IDB_OK:WEB_IDB_IO;if(st.size>SIZE_MAX||st.size>LONG_MAX)return WEB_IDB_IO;size_t n=(size_t)st.size;uint8_t *data=malloc(n?n:1);if(!data)return WEB_IDB_MEMORY;int fd=open(path,O_RDONLY),ok=fd>=0&&io(fd,data,n,0);if(fd>=0)close(fd);if(!ok){free(data);return WEB_IDB_IO;}struct idb_refs pending={data,n/32};collect(tx,&pending,&tx->current,&tx->old);free(data);return pending_clear(tx);}
static int pending_append(struct idb_tx *tx,const uint8_t digest[32]){char path[176];snprintf(path,sizeof path,"%s.pending",tx->path);int fd=open(path,O_WRONLY|O_CREAT|O_APPEND);if(fd<0)return WEB_IDB_IO;int ok=io(fd,(uint8_t *)digest,32,1);if(close(fd)<0)ok=0;return ok?WEB_IDB_OK:WEB_IDB_IO;}
webidb *webidb_create(void){return calloc(1,sizeof(webidb));}
void webidb_free(webidb *db){if(!db)return;while(db->active)webidb_abort(db,db->active->token);while(db->clients){struct idb_client *c=db->clients;webidb_forget_connection(db,c->handle);if(db->clients==c){db->clients=c->next;close(c->fd);free(c);}}free(db);}
int webidb_begin(webidb *db,const char *origin,const char *name,size_t name_length,uint32_t *token,uint8_t **bytes,size_t *length){
    *bytes=NULL;*length=0;*token=0;if(!db||!origin||!name||(strncmp(origin,"http://",7)&&strncmp(origin,"https://",8)))return WEB_IDB_SECURITY;
    if(!directory("/data/browser")||!directory(IDB_ROOT))return WEB_IDB_IO;
    br_sha256_context h;br_sha256_init(&h);uint8_t len[8],digest[32];put64(len,strlen(origin));br_sha256_update(&h,len,8);br_sha256_update(&h,origin,strlen(origin));put64(len,name_length);br_sha256_update(&h,len,8);br_sha256_update(&h,name,name_length);br_sha256_out(&h,digest);
    struct idb_tx *tx=calloc(1,sizeof *tx);if(!tx)return WEB_IDB_MEMORY;char text[65],lease[168];hex(digest,text);snprintf(tx->path,sizeof tx->path,"%s/%s",IDB_ROOT,text);snprintf(lease,sizeof lease,"%s.lock",tx->path);tx->lease=open(lease,O_RDWR|O_CREAT);if(tx->lease<0){free(tx);return WEB_IDB_IO;}
    if(fcntl(tx->lease,F_NTRYLOCK)<0){int busy=errno==EAGAIN;close(tx->lease);free(tx);return busy?WEB_IDB_BUSY:WEB_IDB_IO;}
    uint8_t *a=NULL,*b=NULL;size_t an=0,bn=0;uint64_t ag=0,bg=0;struct idb_refs arx={0},brx={0};int ar=load(tx->path,0,&a,&an,&ag,&arx),br=load(tx->path,1,&b,&bn,&bg,&brx);
    if(ar==-2||br==-2||((ar<0||br<0)&&ar!=1&&br!=1)){free(a);free(b);free(arx.data);free(brx.data);close(tx->lease);free(tx);return ar==-2||br==-2?WEB_IDB_MEMORY:WEB_IDB_IO;}
    if(ar==1&&(br!=1||ag>=bg)){*bytes=a;*length=an;free(b);tx->slot=0;tx->generation=ag;tx->current=arx;tx->old=brx;}
    else if(br==1){*bytes=b;*length=bn;free(a);tx->slot=1;tx->generation=bg;tx->current=brx;tx->old=arx;}else tx->slot=1;
    int recovered=pending_recover(tx);tx->origin=strdup(origin);tx->name=name_length<SIZE_MAX?malloc(name_length+1):NULL;if(tx->name){memcpy(tx->name,name,name_length);tx->name[name_length]=0;tx->name_length=name_length;}if(!tx->origin||!tx->name)recovered=WEB_IDB_MEMORY;if(recovered){free(*bytes);*bytes=NULL;*length=0;free(tx->origin);free(tx->name);free(tx->current.data);free(tx->old.data);close(tx->lease);free(tx);return recovered;}do{if(++db->serial==0)++db->serial;tx->token=db->serial;if(!find(db,tx->token))break;}while(1);tx->next=db->active;db->active=tx;*token=tx->token;return WEB_IDB_OK;
}
int webidb_value(webidb *db,uint32_t token,const char *reference,uint8_t **bytes,size_t *length){
    *bytes=NULL;*length=0;struct idb_tx *tx=find(db,token);uint8_t digest[32];if(!tx||!unhex(reference,digest))return WEB_IDB_IO;
    int permitted=contains(&tx->current,digest);for(struct idb_stage *p=tx->staged;!permitted&&p;p=p->next)permitted=!memcmp(p->digest,digest,32);if(!permitted)return WEB_IDB_SECURITY;
    char path[240];payload_path(tx,digest,path);struct n_stat st;if(stat(path,&st)<0||st.size>SIZE_MAX||st.size>LONG_MAX)return WEB_IDB_IO;size_t n=(size_t)st.size;uint8_t *data=malloc(n?n:1);if(!data)return WEB_IDB_MEMORY;int fd=open(path,O_RDONLY),ok=fd>=0&&io(fd,data,n,0);if(fd>=0)close(fd);uint8_t actual[32];if(ok){hash(data,n,actual);ok=!memcmp(actual,digest,32);}if(!ok){free(data);return WEB_IDB_IO;}*bytes=data;*length=n;return WEB_IDB_OK;
}
int webidb_stage(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,char reference[65]){
    struct idb_tx *tx=find(db,token);if(!tx||length>LONG_MAX)return WEB_IDB_IO;uint8_t digest[32];hash(bytes,length,digest);hex(digest,reference);
    if(contains(&tx->current,digest))return WEB_IDB_OK;
    for(struct idb_stage *p=tx->staged;p;p=p->next)if(!memcmp(p->digest,digest,32))return WEB_IDB_OK;
    struct idb_stage *p=malloc(sizeof *p);if(!p)return WEB_IDB_MEMORY;char path[240];payload_path(tx,digest,path);if(!contains(&tx->old,digest)){int journal=pending_append(tx,digest);if(journal){free(p);return journal;}int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC);int ok=fd>=0&&io(fd,(uint8_t *)bytes,length,1);if(fd>=0&&close(fd)<0)ok=0;if(!ok){unlink(path);free(p);return WEB_IDB_IO;}}memcpy(p->digest,digest,32);p->next=tx->staged;tx->staged=p;return WEB_IDB_OK;
}
int webidb_abort(webidb *db,uint32_t token){if(!db)return WEB_IDB_IO;struct idb_tx **p=&db->active;while(*p&&(*p)->token!=token)p=&(*p)->next;if(!*p)return WEB_IDB_IO;struct idb_tx *tx=*p;*p=tx->next;while(tx->staged){struct idb_stage *s=tx->staged;tx->staged=s->next;if(!contains(&tx->current,s->digest)&&!contains(&tx->old,s->digest)){char path[240];payload_path(tx,s->digest,path);unlink(path);}free(s);}/* A failed clear is retried by next begin's recovery; it cannot invalidate a commit. */pending_clear(tx);free(tx->origin);free(tx->name);free(tx->current.data);free(tx->old.data);close(tx->lease);free(tx);return WEB_IDB_OK;}
int webidb_commit(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,const char *const *references,size_t count,uint64_t version){
    struct idb_tx *tx=find(db,token);if(!tx||tx->generation==UINT64_MAX)return WEB_IDB_IO;size_t on=strlen(tx->origin);if(on>SIZE_MAX-IDB_CATALOG_HEAD||tx->name_length>SIZE_MAX-IDB_CATALOG_HEAD-on)return WEB_IDB_MEMORY;size_t head=IDB_CATALOG_HEAD+on+tx->name_length;if(head>(size_t)LONG_MAX||count>(SIZE_MAX-head)/32||count>((size_t)LONG_MAX-head)/32||length>SIZE_MAX-head-count*32||length>(size_t)LONG_MAX-head-count*32)return WEB_IDB_MEMORY;
    struct idb_refs live={count?malloc(count*32):NULL,count};if(count&&!live.data)return WEB_IDB_MEMORY;for(size_t i=0;i<count;i++)if(!unhex(references[i],live.data+i*32)){free(live.data);return WEB_IDB_IO;}if(count)qsort(live.data,count,32,digest_compare);size_t unique=0;for(size_t i=0;i<count;i++)if(!unique||memcmp(live.data+(unique-1)*32,live.data+i*32,32)){memmove(live.data+unique*32,live.data+i*32,32);unique++;}live.count=unique;
    for(size_t i=0;i<unique;i++){const uint8_t *digest=live.data+i*32;int present=contains(&tx->current,digest)||contains(&tx->old,digest);for(struct idb_stage *p=tx->staged;!present&&p;p=p->next)present=!memcmp(p->digest,digest,32);if(!present){free(live.data);return WEB_IDB_SECURITY;}}
    size_t offset=head+unique*32,n=offset+length;uint8_t *data=malloc(n);if(!data){free(live.data);return WEB_IDB_MEMORY;}memcpy(data,"NIDB0003",8);put64(data+72,tx->generation+1);put64(data+80,length);put64(data+88,unique);put64(data+96,on);put64(data+104,tx->name_length);put64(data+112,version);memcpy(data+IDB_CATALOG_HEAD,tx->origin,on);memcpy(data+IDB_CATALOG_HEAD+on,tx->name,tx->name_length);hash(data+72,head-72,data+40);if(unique)memcpy(data+head,live.data,unique*32);if(length)memcpy(data+offset,bytes,length);hash(data+72,n-72,data+8);char path[168];snprintf(path,sizeof path,"%s.%d",tx->path,1-tx->slot);int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC),ok=fd>=0&&io(fd,data,n,1);if(fd>=0&&close(fd)<0)ok=0;free(data);if(!ok){unlink(path);free(live.data);return WEB_IDB_IO;}
    collect(tx,&tx->old,&live,&tx->current);free(tx->old.data);tx->old=tx->current;tx->current=live;return webidb_abort(db,token);
}

static int catalog(const char *base,int slot,const char *origin,struct webidb_database *item,uint64_t *generation){
    char path[168];snprintf(path,sizeof path,"%s.%d",base,slot);struct n_stat st;if(stat(path,&st)<0)return errno==ENOENT?0:-1;if(st.size<IDB_CATALOG_HEAD)return 0;
    uint8_t head[IDB_CATALOG_HEAD];int fd=open(path,O_RDONLY);if(fd<0)return -1;if(!io(fd,head,sizeof head,0)){close(fd);return -1;}if(memcmp(head,"NIDB0003",8)){close(fd);return 0;}
    uint64_t on=u64(head+96),nn=u64(head+104),refs=u64(head+88),meta=u64(head+80);uint64_t remaining=st.size-IDB_CATALOG_HEAD;
    if(on>remaining||nn>remaining-on||refs>(remaining-on-nn)/32||meta!=remaining-on-nn-refs*32||on!=strlen(origin)||nn>SIZE_MAX-1||on>SIZE_MAX-IDB_CATALOG_HEAD||nn>SIZE_MAX-IDB_CATALOG_HEAD-on){close(fd);return -1;}
    size_t n=IDB_CATALOG_HEAD+(size_t)on+(size_t)nn;uint8_t *data=malloc(n);if(!data){close(fd);return -2;}memcpy(data,head,sizeof head);int ok=io(fd,data+IDB_CATALOG_HEAD,n-IDB_CATALOG_HEAD,0);uint8_t digest[32];if(ok){hash(data+72,n-72,digest);ok=!memcmp(digest,data+40,32)&&u64(data+72)>0;}if(ok&&memcmp(data+IDB_CATALOG_HEAD,origin,(size_t)on)){close(fd);free(data);return 0;}
    /* Validate the metadata manifest in bounded chunks, never opening a
     * .v payload file. A complete-size but damaged slot cannot advertise a
     * newer catalog than the valid crash-fallback generation. */
    if(ok){br_sha256_context h;br_sha256_init(&h);br_sha256_update(&h,data+72,n-72);uint8_t chunk[4096];uint64_t remaining=st.size-n;while(ok&&remaining){size_t step=remaining>sizeof chunk?sizeof chunk:(size_t)remaining;ok=io(fd,chunk,step,0);if(ok){br_sha256_update(&h,chunk,step);remaining-=step;}}br_sha256_out(&h,digest);if(ok)ok=!memcmp(digest,data+8,32);}close(fd);if(!ok){free(data);return -1;}
    /* Bind catalog names back to their private hashed filename, preventing
     * a malformed header from becoming a cross-origin database alias. */
    br_sha256_context h;br_sha256_init(&h);uint8_t len[8];put64(len,on);br_sha256_update(&h,len,8);br_sha256_update(&h,origin,(size_t)on);put64(len,nn);br_sha256_update(&h,len,8);br_sha256_update(&h,data+IDB_CATALOG_HEAD+on,(size_t)nn);br_sha256_out(&h,digest);char text[65];hex(digest,text);const char *basename=strrchr(base,'/');if(!basename||strcmp(basename+1,text)){free(data);return -1;}
    item->name=malloc((size_t)nn+1);if(!item->name){free(data);return -2;}memcpy(item->name,data+IDB_CATALOG_HEAD+on,(size_t)nn);item->name[nn]=0;item->name_length=(size_t)nn;item->version=u64(data+112);*generation=u64(data+72);free(data);return 1;
}
void webidb_databases_free(struct webidb_database *items,size_t count){for(size_t i=0;i<count;i++)free(items[i].name);free(items);}
int webidb_databases(webidb *db,const char *origin,struct webidb_database **items,size_t *count){
    *items=NULL;*count=0;if(!db||!origin||(strncmp(origin,"http://",7)&&strncmp(origin,"https://",8)))return WEB_IDB_SECURITY;
    int fd=open(IDB_ROOT,O_RDONLY|O_DIRECTORY);if(fd<0)return errno==ENOENT?WEB_IDB_OK:WEB_IDB_IO;struct n_dirent entry;int result=WEB_IDB_OK;
    for(int index=0;;index++){if(index==INT_MAX){result=WEB_IDB_MEMORY;break;}int found=readdir(fd,index,&entry);if(found<0){result=WEB_IDB_IO;break;}if(!found)break;size_t len=strlen(entry.name);if(len!=66||entry.name[64]!='.'||(entry.name[65]!='0'&&entry.name[65]!='1'))continue;char text[65];memcpy(text,entry.name,64);text[64]=0;uint8_t digest[32];if(!unhex(text,digest))continue;
        char base[160],lock[168];snprintf(base,sizeof base,"%s/%s",IDB_ROOT,text);snprintf(lock,sizeof lock,"%s.lock",base);int lease=open(lock,O_RDWR|O_CREAT);if(lease<0){result=WEB_IDB_IO;break;}if(fcntl(lease,F_NTRYLOCK)<0){result=errno==EAGAIN?WEB_IDB_BUSY:WEB_IDB_IO;close(lease);break;}
        struct webidb_database a={0},b={0};uint64_t ag=0,bg=0;int ar=catalog(base,0,origin,&a,&ag),br=catalog(base,1,origin,&b,&bg);close(lease);if(ar==-2||br==-2){free(a.name);free(b.name);result=WEB_IDB_MEMORY;break;}struct webidb_database chosen={0};if(ar==1&&(br!=1||ag>=bg)){chosen=a;free(b.name);}else if(br==1){chosen=b;free(a.name);}else{free(a.name);free(b.name);continue;}if(!chosen.version){free(chosen.name);continue;}int duplicate=0;for(size_t i=0;i<*count;i++)if((*items)[i].name_length==chosen.name_length&&!memcmp((*items)[i].name,chosen.name,chosen.name_length)){duplicate=1;break;}if(duplicate){free(chosen.name);continue;}if(*count>=SIZE_MAX/sizeof **items){free(chosen.name);result=WEB_IDB_MEMORY;break;}struct webidb_database *more=realloc(*items,(*count+1)*sizeof **items);if(!more){free(chosen.name);result=WEB_IDB_MEMORY;break;}*items=more;(*items)[(*count)++]=chosen;
        if(index==INT_MAX){result=WEB_IDB_MEMORY;break;}
    }
    close(fd);if(result){webidb_databases_free(*items,*count);*items=NULL;*count=0;}return result;
}

/* RAM-root registry, so connection leases/notifications never survive reboot.
 * Process start times prevent PID reuse from resurrecting dead connections. */
#define CLIENT_BYTES 48u
struct client_table {uint8_t *rows;size_t count;struct n_procinfo *processes;int process_count;};
static int clients_open(struct idb_tx *tx){if(!directory("/tmp/browser-indexeddb"))return -1;const char *base=strrchr(tx->path,'/');char path[160];snprintf(path,sizeof path,"/tmp/browser-indexeddb/%s.clients",base?base+1:tx->path);return open(path,O_RDWR|O_CREAT);}
static int clients_lock(int fd){if(fcntl(fd,F_NTRYLOCK)<0)return errno==EAGAIN?WEB_IDB_BUSY:WEB_IDB_IO;return WEB_IDB_OK;}
static int processes(struct client_table *t){int capacity=32;for(;;){t->processes=malloc((size_t)capacity*sizeof *t->processes);if(!t->processes)return WEB_IDB_MEMORY;int n=proclist(t->processes,capacity);if(n<0){free(t->processes);t->processes=NULL;return WEB_IDB_IO;}if(n<capacity){t->process_count=n;return WEB_IDB_OK;}free(t->processes);t->processes=NULL;if(capacity>INT_MAX/2)return WEB_IDB_MEMORY;capacity*=2;}}
static int client_alive(struct client_table *t,uint8_t *row){for(int i=0;i<t->process_count;i++)if(t->processes[i].pid==(int)u64(row+8)&&t->processes[i].start_ms==u64(row+16)&&t->processes[i].state!=3)return 1;return 0;}
static void table_free(struct client_table *t){free(t->rows);free(t->processes);memset(t,0,sizeof *t);}
static int table_read(int fd,struct client_table *t){struct n_stat st;if(fstat(fd,&st)<0||st.size%CLIENT_BYTES||st.size>SIZE_MAX-CLIENT_BYTES)return WEB_IDB_IO;size_t n=(size_t)st.size;t->rows=calloc(1,n+CLIENT_BYTES);if(!t->rows)return WEB_IDB_MEMORY;t->count=n/CLIENT_BYTES;if(lseek(fd,0,SEEK_SET)<0||!io(fd,t->rows,n,0)){table_free(t);return WEB_IDB_IO;}int status=processes(t);if(status){table_free(t);return status;}for(size_t i=0;i<t->count;i++){uint8_t *row=t->rows+i*CLIENT_BYTES;if(u64(row)&&!client_alive(t,row))memset(row,0,CLIENT_BYTES);}return WEB_IDB_OK;}
static int table_write(int fd,struct client_table *t){return lseek(fd,0,SEEK_SET)>=0&&io(fd,t->rows,t->count*CLIENT_BYTES,1)?WEB_IDB_OK:WEB_IDB_IO;}
int webidb_connections(webidb *db,uint32_t token,uint64_t version,size_t *count){*count=0;struct idb_tx *tx=find(db,token);if(!tx)return WEB_IDB_IO;int fd=clients_open(tx);if(fd<0)return WEB_IDB_IO;int status=clients_lock(fd);struct client_table t={0};if(!status)status=table_read(fd,&t);if(!status){for(size_t i=0;i<t.count;i++){uint8_t *row=t.rows+i*CLIENT_BYTES;if(u64(row)){(*count)++;if(!u64(row+40)||u64(row+32)!=version){put64(row+32,version);put64(row+40,1);}}}status=table_write(fd,&t);}table_free(&t);close(fd);return status;}
int webidb_connect(webidb *db,uint32_t token,uint64_t version,uint32_t *connection){*connection=0;struct idb_tx *tx=find(db,token);if(!tx)return WEB_IDB_IO;int fd=clients_open(tx);if(fd<0)return WEB_IDB_IO;int status=clients_lock(fd);struct client_table t={0};if(!status)status=table_read(fd,&t);struct idb_client *c=NULL;
    if(!status){c=calloc(1,sizeof *c);if(!c)status=WEB_IDB_MEMORY;else{uint32_t serial=__atomic_add_fetch(&client_serial,1,__ATOMIC_RELAXED);if(!serial)serial=__atomic_add_fetch(&client_serial,1,__ATOMIC_RELAXED);c->handle=serial;c->identity=(uint64_t)(uint32_t)getpid()<<32|serial;c->fd=fd;size_t i=0;while(i<t.count&&u64(t.rows+i*CLIENT_BYTES))i++;if(i==t.count)t.count++;c->offset=i*CLIENT_BYTES;uint8_t *row=t.rows+c->offset;put64(row,c->identity);put64(row+8,(uint32_t)getpid());uint64_t start=0;for(int j=0;j<t.process_count;j++)if(t.processes[j].pid==getpid())start=t.processes[j].start_ms;put64(row+16,start);put64(row+24,version);status=table_write(fd,&t);}}
    table_free(&t);if(status){if(c)free(c);close(fd);return status;}fcntl(fd,F_NUNLOCK);c->next=db->clients;db->clients=c;*connection=c->handle;return WEB_IDB_OK;
}
int webidb_poll(webidb *db,uint32_t connection,int *pending,uint64_t *version){*pending=0;*version=0;struct idb_client *c=db?db->clients:NULL;while(c&&c->handle!=connection)c=c->next;if(!c)return WEB_IDB_IO;int status=clients_lock(c->fd);if(status)return status;uint8_t row[CLIENT_BYTES];if(lseek(c->fd,c->offset,SEEK_SET)<0||!io(c->fd,row,sizeof row,0)||u64(row)!=c->identity)status=WEB_IDB_IO;else if(u64(row+40)==1){*pending=1;*version=u64(row+32);put64(row+40,2);if(lseek(c->fd,c->offset,SEEK_SET)<0||!io(c->fd,row,sizeof row,1))status=WEB_IDB_IO;}fcntl(c->fd,F_NUNLOCK);return status;}
int webidb_disconnect(webidb *db,uint32_t connection){if(!db)return WEB_IDB_IO;struct idb_client **p=&db->clients;while(*p&&(*p)->handle!=connection)p=&(*p)->next;if(!*p)return WEB_IDB_IO;struct idb_client *c=*p;int status=clients_lock(c->fd);if(status)return status;uint8_t row[CLIENT_BYTES];if(lseek(c->fd,c->offset,SEEK_SET)<0||!io(c->fd,row,sizeof row,0)||u64(row)!=c->identity)status=WEB_IDB_IO;else{memset(row,0,sizeof row);if(lseek(c->fd,c->offset,SEEK_SET)<0||!io(c->fd,row,sizeof row,1))status=WEB_IDB_IO;}fcntl(c->fd,F_NUNLOCK);*p=c->next;close(c->fd);free(c);return status;}
int webidb_forget_connection(webidb *db,uint32_t connection){
    int result=webidb_disconnect(db,connection);if(result!=WEB_IDB_BUSY)return result;
    struct idb_client *c=db?db->clients:NULL;while(c&&c->handle!=connection)c=c->next;if(!c)return WEB_IDB_IO;
    /* This lease covers RAM-root row I/O only, never author code or a
     * transaction. An unpublished OOM connection cannot be left for JS to close. */
    if(fcntl(c->fd,F_NLOCK)<0)return WEB_IDB_IO;
    return webidb_disconnect(db,connection);
}
