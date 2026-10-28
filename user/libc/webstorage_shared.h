/* Native shared localStorage. The fixed leased control file is in the shared root only.
 * Two checksummed headers commit an append, never an unvalidated tail. A
 * checkpoint replaces consumed deltas; live document event cursors prohibit
 * compaction of undelivered notifications. Persistent slots are unchanged. */
#define STORAGE_CONTROL_BYTES 256u
#define STORAGE_READER_BYTES 48u
/* Storage events not yet taken, not stored data: one origin's shared journal, and the queue of one
 * tab's sessionStorage events, hold at most this many bytes of them (twice the largest change the
 * quota allows: its key, old and new value at up to 3 bytes a unit). Past it a change waits for
 * the documents that have not read their events (WEB_STORAGE_QUOTA) instead of dropping them. */
#define STORAGE_EVENT_BYTES (8u * WEB_STORAGE_QUOTA_BYTES)
/* The longest host a clear names: an origin is under 2048 bytes. */
#define STORAGE_DOMAIN_MAX 2047u
struct storage_notification {
    struct storage_notification *next;
    struct web_storage_result data;
    size_t bytes;
};
struct storage_source {
    struct storage_source *next;
    struct storage_area *area;
    uint64_t token, reader, cursor;
    struct storage_notification *events, *tail;
};
static bool storage_io_at(n_handle h,uint64_t at,void *bytes,size_t n,bool writing) {
    size_t done=0;
    while(done<n){
        n_result size=writing?n_document_write_at(h,at+done,(char *)bytes+done,n-done):
                              n_document_read_at(h,at+done,(char *)bytes+done,n-done);
        if(size<=0 || (uint64_t)size>n-done)return false;
        done+=(size_t)size;
    }
    return true;
}
/* The document at path, created empty when absent, open for reading and writing. */
static n_handle storage_document(const char *path) {
    n_handle h=n_document_open(path,N_DOCUMENT_EDIT);
    if(h!=-N_ERR_NOT_FOUND)return h;
    h=n_document_open(path,N_DOCUMENT_CREATE);
    return h==-N_ERR_EXISTS?n_document_open(path,N_DOCUMENT_EDIT):h;
}
static void storage_control_pack(struct storage_control *h,uint8_t b[128]) {
    memset(b,0,128);memcpy(b,"NWSHARE1",8);
    storage_put64(b+40,h->serial);storage_put64(b+48,h->epoch);storage_put64(b+56,h->revision);
    storage_put64(b+64,h->end);storage_put64(b+72,h->checkpoint_revision);storage_put64(b+80,h->persisted);
    storage_put64(b+88,h->generation);storage_put32(b+96,(uint32_t)h->checkpoint_slot);storage_put32(b+100,(uint32_t)h->slot);
    storage_put64(b+104,h->dirty_since);storage_put64(b+112,h->changed_at);
    storage_hash(b+40,88,b+8);
}
static bool storage_control_unpack(uint8_t b[128],struct storage_control *h) {
    uint8_t hash[32];storage_hash(b+40,88,hash);
    if(memcmp(b,"NWSHARE1",8)||memcmp(b+8,hash,32))return false;
    h->serial=storage_u64(b+40);h->epoch=storage_u64(b+48);h->revision=storage_u64(b+56);
    h->end=storage_u64(b+64);h->checkpoint_revision=storage_u64(b+72);h->persisted=storage_u64(b+80);
    h->generation=storage_u64(b+88);h->checkpoint_slot=(int)storage_u32(b+96);h->slot=(int)storage_u32(b+100);
    h->dirty_since=storage_u64(b+104);h->changed_at=storage_u64(b+112);
    return h->serial && h->epoch && h->end>=STORAGE_CONTROL_BYTES &&
        h->end<=STORAGE_CONTROL_BYTES+STORAGE_EVENT_BYTES && h->checkpoint_revision<=h->revision &&
        h->persisted<=h->revision && h->checkpoint_slot>=0 && h->checkpoint_slot<=1 && h->slot>=-1 && h->slot<=1;
}
static int storage_control_read(struct storage_area *a,struct storage_control *h) {
    uint8_t bytes[STORAGE_CONTROL_BYTES]={0};size_t done=0;
    while(done<sizeof bytes){
        n_result size=n_document_read_at(a->shared,done,bytes+done,sizeof bytes-done);
        if(size<0 || (uint64_t)size>sizeof bytes-done)goto failed;
        if(!size)break;
        done+=(size_t)size;
    }
    if(!done){a->shared_control_cached=false;return 0;}
    if(a->shared_control_cached && !memcmp(bytes,a->shared_control_bytes,sizeof bytes)){
        *h=a->shared_control;return 1;
    }
    struct storage_control other;bool found=false;
    for(unsigned i=0;i<2;i++)if(done>=(i+1u)*128 && storage_control_unpack(bytes+i*128,&other)){
        if(!found || other.serial>h->serial)*h=other;found=true;
    }
    if(!found)goto failed;
    memcpy(a->shared_control_bytes,bytes,sizeof bytes);a->shared_control=*h;a->shared_control_cached=true;
    return 1;
failed:
    a->shared_control_cached=false;return -1;
}
static bool storage_control_commit(struct storage_area *a,struct storage_control *h) {
    if(h->serial==UINT64_MAX)return false;
    h->serial++;uint8_t b[128];storage_control_pack(h,b);
    size_t offset=(h->serial&1u)*128;
    if(!storage_io_at(a->shared,offset,b,128,true)){a->shared_control_cached=false;return false;}
    if(a->shared_control_cached){memcpy(a->shared_control_bytes+offset,b,128);a->shared_control=*h;}
    return true;
}
static struct n_procinfo *storage_processes(int *count) {
    int capacity=32;
    for(;;){
        struct n_procinfo *list=malloc((size_t)capacity*sizeof *list);if(!list)return NULL;
        int n=proclist(list,capacity);
        if(n<0){free(list);return NULL;}
        if(n<capacity){*count=n;return list;}
        free(list);if(capacity>INT_MAX/2)return NULL;capacity*=2;
    }
}
static uint64_t storage_process_start(void) {
    int count=0;struct n_procinfo *list=storage_processes(&count);uint64_t start=0;
    for(int i=0;list && i<count;i++)if(list[i].pid==(int)n_app_self()){start=list[i].start_ms;break;}
    free(list);return start;
}
static bool storage_reader_alive(const uint8_t row[48],struct n_procinfo *list,int count) {
    for(int i=0;i<count;i++)if(list[i].pid==(int)storage_u64(row+8) &&
        list[i].start_ms==storage_u64(row+16) && list[i].state!=3)return true;
    return false;
}
static n_handle storage_readers_open(struct storage_area *a) {
    char path[STORAGE_PATH];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,3,path);
    return storage_document(path);
}
static bool storage_reader_register(struct storage_area *a,uint64_t token,bool holder,
                                    const struct storage_control *h,uint64_t *offset) {
    n_handle readers=storage_readers_open(a);if(readers<0)return false;
    n_result size=n_document_size(readers);bool ok=size>=0 && size%STORAGE_READER_BYTES==0;
    int count=0;struct n_procinfo *list=ok?storage_processes(&count):NULL;
    if(!list)ok=false;
    uint64_t vacant=UINT64_MAX;uint8_t row[48];bool found=false;
    for(uint64_t at=0;ok && at<(uint64_t)size;at+=48){
        if(!storage_io_at(readers,at,row,48,false)){ok=false;break;}
        if(storage_u64(row) && !storage_reader_alive(row,list,count)){
            memset(row,0,48);if(!storage_io_at(readers,at,row,48,true)){ok=false;break;}
        }
        if(storage_u64(row)==token && storage_u64(row+40)==(uint64_t)holder){*offset=at;found=true;break;}
        if(!storage_u64(row) && vacant==UINT64_MAX)vacant=at;
    }
    if(ok && !found){
        *offset=vacant==UINT64_MAX?(uint64_t)size:vacant;memset(row,0,48);
        storage_put64(row,token);storage_put64(row+8,(uint32_t)n_app_self());storage_put64(row+16,storage_process_start());
        storage_put64(row+24,h->epoch);storage_put64(row+32,h->end);storage_put64(row+40,holder);
        ok=storage_io_at(readers,*offset,row,48,true);
    }
    free(list);if(n_handle_close(readers)<0)ok=false;return ok;
}
static bool storage_reader_update(struct storage_area *a,uint64_t offset,uint64_t token,
                                  uint64_t epoch,uint64_t cursor,bool remove) {
    n_handle readers=storage_readers_open(a);if(readers<0)return false;uint8_t row[48];
    bool ok=storage_io_at(readers,offset,row,48,false) && storage_u64(row)==token && !storage_u64(row+40);
    if(ok){
        if(remove)memset(row,0,48);else{storage_put64(row+24,epoch);storage_put64(row+32,cursor);}
        ok=storage_io_at(readers,offset,row,48,true);
    }
    if(n_handle_close(readers)<0)ok=false;return ok;
}
static bool storage_checkpoint_allowed(struct storage_area *a,const struct storage_control *h) {
    n_handle readers=storage_readers_open(a);if(readers<0)return false;
    n_result size=n_document_size(readers);bool ok=size>=0 && size%48==0;int count=0;
    struct n_procinfo *list=ok?storage_processes(&count):NULL;if(!list)ok=false;
    uint8_t row[48];
    for(uint64_t at=0;ok && at<(uint64_t)size;at+=48){
        if(!storage_io_at(readers,at,row,48,false)){ok=false;break;}
        if(!storage_u64(row))continue;
        if(!storage_reader_alive(row,list,count)){
            memset(row,0,48);if(!storage_io_at(readers,at,row,48,true))ok=false;continue;
        }
        if(!storage_u64(row+40) && (storage_u64(row+24)!=h->epoch || storage_u64(row+32)<h->end))ok=false;
    }
    free(list);if(n_handle_close(readers)<0)ok=false;return ok;
}
static int storage_checkpoint(struct storage_area *a,struct storage_control *h) {
    if(h->epoch==UINT64_MAX || h->revision==UINT64_MAX)return WEB_STORAGE_IO;
    struct storage_area snapshot=*a;struct web_storage_result ignored={0};
    snapshot.slot=h->checkpoint_slot;snapshot.generation=h->revision;snapshot.unavailable=false;
    int result=storage_save_at(WEBSTORAGE_SHARED_ROOT,&snapshot,&ignored);if(result)return result;
    h->checkpoint_slot=snapshot.slot;h->checkpoint_revision=h->revision;h->epoch++;h->end=STORAGE_CONTROL_BYTES;
    if(!storage_control_commit(a,h))return WEB_STORAGE_IO;
    a->shared_epoch=h->epoch;a->shared_cursor=h->end;
    /* The committed checkpoint is sufficient for recovery. Truncate only
     * the delta document, retaining the leased control and every open handle.
     * A crash before truncation leaves an ignored tail; after it, the committed
     * checkpoint is complete. Every operation owns the same control lease. */
    return n_document_set_size(a->shared_log,0)>=0?WEB_STORAGE_OK:WEB_STORAGE_IO;
}
/* Opens a's shared documents when they are not open and takes their lease. *present: 1 with the
 * control read into h, 0 when no window made the shared area since the start (h zero). On an
 * error no lease is held. */
static int storage_shared_claim(struct storage_area *a,struct storage_control *h,int *present) {
    if(a->shared<0){
        if(!storage_dirs_at(WEBSTORAGE_SHARED_ROOT))return WEB_STORAGE_IO;
        char path[STORAGE_PATH];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,2,path);
        a->shared=storage_document(path);if(a->shared<0)return WEB_STORAGE_IO;
    }
    if(a->shared_log<0){
        char path[STORAGE_PATH];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,4,path);
        a->shared_log=storage_document(path);if(a->shared_log<0)return WEB_STORAGE_IO;
    }
    if(n_document_claim(a->shared,true)<0)return WEB_STORAGE_IO;
    memset(h,0,sizeof *h);*present=storage_control_read(a,h);
    if(*present<0){n_document_release(a->shared);return WEB_STORAGE_IO;}
    return WEB_STORAGE_OK;
}
/* Under the lease, when no window made the shared area since the start: the newer valid saved
 * slot becomes a's map, never an empty map over an invalid one. h starts from it. */
static int storage_seed(struct storage_area *a,struct storage_control *h) {
    struct storage_area *snapshot=NULL,*other=NULL;int result=WEB_STORAGE_OK;
    int ar=storage_read(a->origin,0,&snapshot),br=storage_read(a->origin,1,&other);
    if(other && (!snapshot || other->generation>snapshot->generation)){storage_area_free(snapshot);snapshot=other;other=NULL;}
    storage_area_free(other);
    if(!snapshot && (ar<0 || br<0))result=WEB_STORAGE_IO;
    if(snapshot){
        storage_map_take(a,snapshot);a->slot=snapshot->slot;a->generation=snapshot->generation;
        storage_area_free(snapshot);
    }
    h->checkpoint_slot=-1;h->slot=a->slot;h->generation=a->generation;
    return result;
}
static int storage_shared_lock(webstorage *s,struct storage_area *a,struct storage_control *h) {
    int present=0,result=storage_shared_claim(a,h,&present);if(result)return result;
    /* A shared area wins over an older disk snapshot. Only seed from disk
     * when the leased area is first created after boot. */
    if(!present){result=storage_seed(a,h);if(!result)result=storage_checkpoint(a,h);}
    if(!result && !a->shared_holder){uint64_t holder;
        if(!storage_reader_register(a,s->identity,true,h,&holder))result=WEB_STORAGE_IO;
        else a->shared_holder=true;
    }
    if(result)n_document_release(a->shared);
    return result;
}
static int storage_record_read(struct storage_area *a,uint64_t at,uint64_t end,uint8_t **out) {
    uint8_t header[48];if(at<STORAGE_CONTROL_BYTES || end-at<48 || !storage_io_at(a->shared_log,at-STORAGE_CONTROL_BYTES,header,48,false))return WEB_STORAGE_IO;
    uint32_t n=storage_u32(header),op=storage_u32(header+4),kn=storage_u32(header+24),old=storage_u32(header+28),value=storage_u32(header+32),url=storage_u32(header+36);
    if(n<48 || n>end-at || (uint64_t)kn+old+value+url!=n-48 || op<WEB_STORAGE_SET || op>WEB_STORAGE_CLEAR)return WEB_STORAGE_IO;
    uint8_t *bytes=malloc(n);if(!bytes)return WEB_STORAGE_QUOTA;
    memcpy(bytes,header,48);if(!storage_io_at(a->shared_log,at+48-STORAGE_CONTROL_BYTES,bytes+48,n-48,false)){free(bytes);return WEB_STORAGE_IO;}
    *out=bytes;return WEB_STORAGE_OK;
}
static int storage_shared_sync(struct storage_area *a,const struct storage_control *h) {
    if(a->shared_epoch!=h->epoch){
        struct storage_area *snapshot=NULL;
        if(storage_read_at(WEBSTORAGE_SHARED_ROOT,a->origin,h->checkpoint_slot,&snapshot)!=1)return WEB_STORAGE_IO;
        if(snapshot->generation!=h->checkpoint_revision+1){storage_area_free(snapshot);return WEB_STORAGE_IO;}
        storage_map_take(a,snapshot);storage_area_free(snapshot);
        a->shared_epoch=h->epoch;a->shared_cursor=STORAGE_CONTROL_BYTES;
    }
    if(a->shared_cursor>h->end)return WEB_STORAGE_IO;
    while(a->shared_cursor<h->end){
        uint8_t *record=NULL;int result=storage_record_read(a,a->shared_cursor,h->end,&record);if(result)return result;
        uint32_t kn=storage_u32(record+24),old=storage_u32(record+28),vn=storage_u32(record+32);
        struct web_storage_request r={.kind=WEB_STORAGE_LOCAL,.operation=(int)storage_u32(record+4),
            .key=(char *)record+48,.key_len=kn,.value=(char *)record+48+kn+old,.value_len=vn};
        struct web_storage_result ignored;result=storage_area_apply(a,&r,&ignored);
        if(!result)a->shared_cursor+=storage_u32(record);free(record);if(result)return result;
    }
    a->generation=h->generation;a->slot=h->slot;a->dirty=h->revision!=h->persisted;
    a->dirty_since=h->dirty_since;a->changed_at=h->changed_at;a->unavailable=false;
    return WEB_STORAGE_OK;
}
static struct storage_source *storage_source_find(webstorage *s,struct storage_area *a,uint64_t token) {
    for(struct storage_source *p=s->sources;p;p=p->next)if(p->area==a && p->token==token)return p;
    return NULL;
}
static int storage_source_add(webstorage *s,struct storage_area *a,uint64_t token,const struct storage_control *h) {
    if(!token || storage_source_find(s,a,token))return WEB_STORAGE_OK;
    struct storage_source *p=calloc(1,sizeof *p);if(!p)return WEB_STORAGE_QUOTA;
    p->area=a;p->token=token;
    if(a->kind==WEB_STORAGE_LOCAL && !storage_reader_register(a,token,false,h,&p->reader)){free(p);return WEB_STORAGE_IO;}
    p->next=s->sources;s->sources=p;a->sources++;return WEB_STORAGE_OK;
}
void webstorage_result_free(struct web_storage_result *out) {
    free(out->text);free(out->event_key);free(out->old_value);free(out->new_value);free(out->url);memset(out,0,sizeof *out);
}
static int storage_event_decode(const uint8_t *record,struct web_storage_result *out) {
    size_t kn=storage_u32(record+24),on=storage_u32(record+28),vn=storage_u32(record+32),un=storage_u32(record+36);unsigned flags=storage_u32(record+40);
    out->event=true;out->event_key_len=kn;out->old_len=on;out->new_len=vn;
    if(flags&1)out->event_key=storage_copy((char *)record+48,kn);
    if(flags&2)out->old_value=storage_copy((char *)record+48+kn,on);
    if(flags&4)out->new_value=storage_copy((char *)record+48+kn+on,vn);
    out->url=storage_copy((char *)record+48+kn+on+vn,un);
    if(((flags&1)&&!out->event_key)||((flags&2)&&!out->old_value)||((flags&4)&&!out->new_value)||!out->url){webstorage_result_free(out);return WEB_STORAGE_QUOTA;}
    return WEB_STORAGE_OK;
}
static int storage_event_poll(struct storage_area *a,struct storage_source *p,const struct storage_control *h,
                              const struct web_storage_request *r,struct web_storage_result *out) {
    n_handle readers=storage_readers_open(a);uint8_t row[48];
    bool ok=readers>=0 && storage_io_at(readers,p->reader,row,48,false) && storage_u64(row)==p->token;
    if(readers>=0)n_handle_close(readers);if(!ok)return WEB_STORAGE_IO;
    uint64_t epoch=storage_u64(row+24),cursor=storage_u64(row+32),stored=cursor;
    if(epoch!=h->epoch){epoch=h->epoch;cursor=STORAGE_CONTROL_BYTES;stored=UINT64_MAX;}
    if(r->operation==WEB_STORAGE_ACK){
        if(r->cursor<cursor || r->cursor>h->end)return WEB_STORAGE_IO;
        return storage_reader_update(a,p->reader,p->token,epoch,r->cursor,false)?WEB_STORAGE_OK:WEB_STORAGE_IO;
    }
    while(cursor<h->end){
        uint8_t *record=NULL;int result=storage_record_read(a,cursor,h->end,&record);if(result)return result;
        uint64_t next=cursor+storage_u32(record);
        if(storage_u64(record+16)!=p->token){
            result=storage_event_decode(record,out);free(record);if(result)return result;
            out->cursor=next;return WEB_STORAGE_OK;
        }
        free(record);cursor=next;
    }
    /* An idle poll leaves the reader row as it found it: no write, no second open. */
    if(cursor==stored)return WEB_STORAGE_OK;
    return storage_reader_update(a,p->reader,p->token,epoch,cursor,false)?WEB_STORAGE_OK:WEB_STORAGE_IO;
}
/* Session storage belongs to one tab (top-level traversable): its other live realms of the same
 * origin, and only those, receive queued events (HTML's broadcast). Allocate every recipient
 * before changing RAM. */
static int storage_session_prepare(struct storage_area *a,const struct web_storage_request *r,
                                    const char *old,size_t old_len,bool existed,struct storage_notification **pending,
                                    const struct storage_source *sources) {
    size_t kn=r->operation==WEB_STORAGE_CLEAR?0:r->key_len,vn=r->operation==WEB_STORAGE_SET?r->value_len:0;
    size_t un=r->url?strlen(r->url):0,bytes=sizeof(struct storage_notification)+kn+old_len+vn+un+4;
    size_t total=a->space->event_bytes;
    for(const struct storage_source *p=sources;p;p=p->next)if(p->area==a && p->token!=r->source){
        if(bytes>STORAGE_EVENT_BYTES-total)return WEB_STORAGE_QUOTA;
        struct storage_notification *e=calloc(1,sizeof *e);if(!e)return WEB_STORAGE_QUOTA;
        e->data.event=true;e->data.event_key_len=kn;e->data.old_len=old_len;e->data.new_len=vn;
        e->data.cursor=p->token;e->bytes=bytes;
        if(r->operation!=WEB_STORAGE_CLEAR)e->data.event_key=storage_copy(r->key,kn);
        if(existed)e->data.old_value=storage_copy(old,old_len);
        if(r->operation==WEB_STORAGE_SET)e->data.new_value=storage_copy(r->value,vn);
        e->data.url=storage_copy(r->url?r->url:"",un);
        e->next=*pending;*pending=e;total+=bytes;
        if((r->operation!=WEB_STORAGE_CLEAR&&!e->data.event_key)||(existed&&!e->data.old_value)||
           (r->operation==WEB_STORAGE_SET&&!e->data.new_value)||!e->data.url)return WEB_STORAGE_QUOTA;
    }
    return WEB_STORAGE_OK;
}
static void storage_session_discard(struct storage_notification *pending) {
    while(pending){struct storage_notification *e=pending;pending=e->next;webstorage_result_free(&e->data);free(e);}
}
static void storage_session_commit(webstorage *s,struct storage_area *a,struct storage_notification *pending) {
    while(pending){struct storage_notification *e=pending;pending=e->next;
        struct storage_source *p=storage_source_find(s,a,e->data.cursor);e->next=NULL;
        e->data.cursor=++p->cursor;if(p->tail)p->tail->next=e;else p->events=e;p->tail=e;a->space->event_bytes+=e->bytes;
    }
}
static int storage_session_poll(struct storage_source *p,const struct web_storage_request *r,
                                struct web_storage_result *out) {
    if(!p || !p->events)return WEB_STORAGE_OK;
    struct storage_notification *e=p->events;
    if(r->operation==WEB_STORAGE_ACK){
        if(r->cursor!=e->data.cursor)return WEB_STORAGE_IO;
        p->events=e->next;if(!p->events)p->tail=NULL;p->area->space->event_bytes-=e->bytes;
        webstorage_result_free(&e->data);free(e);return WEB_STORAGE_OK;
    }
    out->event=true;out->cursor=e->data.cursor;out->event_key_len=e->data.event_key_len;
    out->old_len=e->data.old_len;out->new_len=e->data.new_len;
    if(e->data.event_key)out->event_key=storage_copy(e->data.event_key,out->event_key_len);
    if(e->data.old_value)out->old_value=storage_copy(e->data.old_value,out->old_len);
    if(e->data.new_value)out->new_value=storage_copy(e->data.new_value,out->new_len);
    out->url=storage_copy(e->data.url,strlen(e->data.url));
    if((e->data.event_key&&!out->event_key)||(e->data.old_value&&!out->old_value)||
       (e->data.new_value&&!out->new_value)||!out->url){webstorage_result_free(out);return WEB_STORAGE_QUOTA;}
    return WEB_STORAGE_OK;
}
static int storage_delta_publish(struct storage_area *a,struct storage_control *h,const struct web_storage_request *r,
                                  const char *old,size_t old_len,bool existed) {
    size_t kn=r->operation==WEB_STORAGE_CLEAR?0:r->key_len,vn=r->operation==WEB_STORAGE_SET?r->value_len:0;
    size_t url_len=r->url?strlen(r->url):0,n=48+kn+old_len+vn+url_len;
    if(n>STORAGE_EVENT_BYTES || h->end>STORAGE_CONTROL_BYTES+STORAGE_EVENT_BYTES-n || h->revision==UINT64_MAX)return WEB_STORAGE_QUOTA;
    uint8_t *record=malloc(n);if(!record)return WEB_STORAGE_QUOTA;memset(record,0,48);
    storage_put32(record,(uint32_t)n);storage_put32(record+4,r->operation);storage_put64(record+8,h->revision+1);storage_put64(record+16,r->source);
    storage_put32(record+24,kn);storage_put32(record+28,old_len);storage_put32(record+32,vn);storage_put32(record+36,url_len);
    storage_put32(record+40,(r->operation!=WEB_STORAGE_CLEAR?1:0)|(existed?2:0)|(r->operation==WEB_STORAGE_SET?4:0));
    size_t at=48;if(kn){memcpy(record+at,r->key,kn);at+=kn;}if(old_len){memcpy(record+at,old,old_len);at+=old_len;}
    if(vn){memcpy(record+at,r->value,vn);at+=vn;}if(url_len)memcpy(record+at,r->url,url_len);
    bool ok=storage_io_at(a->shared_log,h->end-STORAGE_CONTROL_BYTES,record,n,true);free(record);if(!ok)return WEB_STORAGE_IO;
    if(h->revision==h->persisted)h->dirty_since=n_clock_uptime_ms();h->changed_at=n_clock_uptime_ms();h->revision++;h->end+=n;
    if(!storage_control_commit(a,h))return WEB_STORAGE_IO;
    a->shared_epoch=h->epoch;a->shared_cursor=h->end;return WEB_STORAGE_OK;
}
static int storage_shared_flush(webstorage *s,struct storage_area *a,struct web_storage_result *out) {
    if(a->kind!=WEB_STORAGE_LOCAL)return WEB_STORAGE_OK;
    struct storage_control h;int result=storage_shared_lock(s,a,&h);if(result)return result;
    result=storage_shared_sync(a,&h);
    if(!result && h.revision!=h.persisted){
        result=storage_save(a,out);
        if(!result){h.persisted=h.revision;h.generation=a->generation;h.slot=a->slot;
            if(!storage_control_commit(a,&h))result=WEB_STORAGE_IO;}
    }
    n_document_release(a->shared);return result;
}
static void storage_source_drop(webstorage *s,struct storage_source **link) {
    struct storage_source *p=*link;
    if(p->area->kind==WEB_STORAGE_LOCAL){struct storage_control h;
        if(!storage_shared_lock(s,p->area,&h)){storage_reader_update(p->area,p->reader,p->token,0,0,true);n_document_release(p->area->shared);}}
    /* Only a sessionStorage source queues events in RAM (localStorage ones are in the journal). */
    while(p->events){struct storage_notification *e=p->events;p->events=e->next;if(p->area->space)p->area->space->event_bytes-=e->bytes;webstorage_result_free(&e->data);free(e);}
    p->area->sources--;*link=p->next;free(p);
}
static void storage_sources_free(webstorage *s) {
    while(s->sources)storage_source_drop(s,&s->sources);
    /* Holder rows are reclaimed at the next open/checkpoint if a process exits.
       No path is removed while another app can still hold it open. */
    for(struct storage_area *a=s->areas;a;a=a->next)if(a->shared>=0){
        struct storage_control h;if(storage_shared_lock(s,a,&h))continue;
        n_handle readers=storage_readers_open(a);n_result size=readers>=0?n_document_size(readers):-1;uint8_t row[48];
        for(n_result at=0;at+48<=size;at+=48)
            if(storage_io_at(readers,at,row,48,false) && storage_u64(row)==s->identity && storage_u64(row+40)){
                memset(row,0,48);storage_io_at(readers,at,row,48,true);break;
            }
        if(readers>=0)n_handle_close(readers);n_document_release(a->shared);
    }
}
void webstorage_session_close(webstorage *s,uint64_t session) {
    struct storage_space **link=s?&s->spaces:NULL;
    while(link && *link && (*link)->id!=session)link=&(*link)->next;
    if(!link || !*link)return;
    struct storage_space *p=*link;
    for(struct storage_source **q=&s->sources;*q;)if((*q)->area->space==p)storage_source_drop(s,q);else q=&(*q)->next;
    *link=p->next;storage_space_free(p);
    /* Its documents are gone: localStorage areas only they used leave once idle. */
    storage_sweep(s);
}
int webstorage_access(webstorage *s,uint64_t session,const char *origin,const struct web_storage_request *r,struct web_storage_result *out) {
    if(!out)return WEB_STORAGE_IO;memset(out,0,sizeof *out);
    if(!s || !r)return WEB_STORAGE_IO;
    if(r->operation==WEB_STORAGE_RELEASE){
        for(struct storage_source **p=&s->sources;*p;)if((*p)->token==r->source)storage_source_drop(s,p);else p=&(*p)->next;
        return WEB_STORAGE_OK;
    }
    if(!storage_origin(origin) || (r->kind!=WEB_STORAGE_LOCAL && r->kind!=WEB_STORAGE_SESSION))return WEB_STORAGE_SECURITY;
    if(r->operation<0 || r->operation>WEB_STORAGE_RELEASE)return WEB_STORAGE_IO;
    /* The host names the tab; a closed or unknown one has no session storage shed. */
    struct storage_space *space=NULL;
    if(r->kind==WEB_STORAGE_SESSION && !(space=storage_space_find(s,session)))return WEB_STORAGE_SECURITY;
    size_t ku=0,vu=0;
    if((r->operation==WEB_STORAGE_GET || r->operation==WEB_STORAGE_SET || r->operation==WEB_STORAGE_REMOVE) &&
        (r->key_len>STORAGE_FILE_MAX || !storage_units(r->key,r->key_len,&ku)))return WEB_STORAGE_QUOTA;
    if(r->operation==WEB_STORAGE_SET && (r->value_len>STORAGE_FILE_MAX || !storage_units(r->value,r->value_len,&vu)))return WEB_STORAGE_QUOTA;
    struct storage_area *a=NULL;int result=storage_get_area(s,space,origin,&a);if(result)return result;
    a->touched=true;
    /* A Window getter checks storage eligibility, not the current key map.
     * After its first successful registration, repeated getters need neither
     * a shared lease nor a disk/head read. Actual reads, mutations and event
     * polls below still synchronize every time, including within one task. */
    if(r->operation==WEB_STORAGE_CHECK && (a->kind==WEB_STORAGE_SESSION || a->shared_holder) &&
       (!r->source || storage_source_find(s,a,r->source)))return WEB_STORAGE_OK;
    struct storage_control h={0};bool locked=a->kind==WEB_STORAGE_LOCAL;
    if(locked){result=storage_shared_lock(s,a,&h);if(result)return result;result=storage_shared_sync(a,&h);if(result)goto done;}
    result=storage_source_add(s,a,r->source,&h);if(result)goto done;
    if(r->operation==WEB_STORAGE_SUBSCRIBE){result=WEB_STORAGE_OK;goto done;}
    struct storage_source *source=storage_source_find(s,a,r->source);
    if(r->operation==WEB_STORAGE_POLL || r->operation==WEB_STORAGE_ACK){
        result=locked?(source?storage_event_poll(a,source,&h,r,out):WEB_STORAGE_IO):storage_session_poll(source,r,out);goto done;
    }
    bool mutation=r->operation>=WEB_STORAGE_SET && r->operation<=WEB_STORAGE_CLEAR;
    struct storage_entry *e=mutation && r->operation!=WEB_STORAGE_CLEAR?storage_find(a,r->key,r->key_len):NULL;
    bool changed=mutation && !((r->operation==WEB_STORAGE_REMOVE && !e)||(r->operation==WEB_STORAGE_CLEAR && !a->count)||
        (r->operation==WEB_STORAGE_SET && e && e->vn==r->value_len && (!e->vn || !memcmp(e->value,r->value,e->vn))));
    if(!changed){result=storage_area_apply(a,r,out);goto done;}
    size_t newunits=r->operation==WEB_STORAGE_CLEAR?0:a->units-(e?e->units:0)+(r->operation==WEB_STORAGE_SET?ku+vu:0);
    if(newunits>STORAGE_QUOTA_UNITS){result=WEB_STORAGE_QUOTA;goto done;}
    struct storage_entry *prepared=NULL;
    if(r->operation==WEB_STORAGE_SET){
        prepared=calloc(1,sizeof *prepared);
        if(prepared){prepared->key=storage_copy(r->key,r->key_len);prepared->value=storage_copy(r->value,r->value_len);
            prepared->kn=r->key_len;prepared->vn=r->value_len;prepared->units=ku+vu;}
        if(!prepared || !prepared->key || !prepared->value || (!e && !storage_index_room(a))){storage_entry_free(prepared);result=WEB_STORAGE_QUOTA;goto done;}
    }
    struct storage_notification *pending=NULL;
    size_t checkpoint_bytes=a->units*3+a->count*8+56;
    if(checkpoint_bytes<65536)checkpoint_bytes=65536;
    if(locked && h.end-STORAGE_CONTROL_BYTES>checkpoint_bytes){
        if(storage_checkpoint_allowed(a,&h)){result=storage_checkpoint(a,&h);if(result)goto mutation_done;}
    }
    if(locked)result=storage_delta_publish(a,&h,r,e?e->value:NULL,e?e->vn:0,e!=NULL);
    else result=storage_session_prepare(a,r,e?e->value:NULL,e?e->vn:0,e!=NULL,&pending,s->sources);
    if(!result){
        if(r->operation==WEB_STORAGE_CLEAR)storage_map_clear(a);
        else if(prepared){storage_link(a,prepared,e);prepared=NULL;storage_entry_free(e);}
        else{storage_unlink(a,e);storage_entry_free(e);}
        a->units=newunits;
        if(locked){a->dirty=true;a->dirty_since=h.dirty_since;a->changed_at=h.changed_at;}
        else{storage_session_commit(s,a,pending);pending=NULL;}
    }
mutation_done:
    storage_entry_free(prepared);storage_session_discard(pending);
done:
    if(locked)n_document_release(a->shared);
    return result;
}

/* ---- The user agent's clears and the settings' measure ---- */

/* A host a clear may name: what an origin's host may hold. */
static bool storage_domain(const char *d) {
    size_t n=d?strlen(d):0;
    if(!n || n>STORAGE_DOMAIN_MAX)return false;
    for(const unsigned char *p=(const unsigned char *)d;*p;p++)if(*p<=32 || *p>=127 || strchr("/\\@?#%",*p))return false;
    return true;
}
/* Whether origin's host is domain or a name inside it, whatever its scheme and port. An IP address
 * has no names inside it: only itself. */
static bool storage_in_domain(const char *origin,const char *domain) {
    const char *host=strstr(origin,"://");if(!host)return false;host+=3;
    size_t n;bool address;
    if(host[0]=='['){const char *end=strchr(host,']');if(!end)return false;n=(size_t)(end-host)+1;address=true;}
    else{
        const char *port=strrchr(host,':');n=port?(size_t)(port-host):strlen(host);
        size_t label=n;while(label && host[label-1]!='.')label--;
        address=label<n;for(size_t i=label;i<n;i++)if(host[i]<'0' || host[i]>'9')address=false;
    }
    size_t dn=strlen(domain);
    if(!n || dn>n || (dn<n && (address || host[n-dn-1]!='.')))return false;
    for(size_t i=0;i<dn;i++){unsigned x=(unsigned char)host[n-dn+i],y=(unsigned char)domain[i];
        if(x-'A'<26u)x+=32;if(y-'A'<26u)y+=32;if(x!=y)return false;}
    return true;
}
/* This store's tabs: the sessionStorage of the origins in domain (NULL: all) becomes empty. Its
 * areas stay for the realms that read their events; events already queued stay queued tasks. */
static void storage_session_clear(webstorage *s,const char *domain) {
    for(struct storage_space *p=s->spaces;p;p=p->next)for(struct storage_area *a=p->areas;a;a=a->next)
        if(!domain || storage_in_domain(a->origin,domain))storage_map_clear(a);
}
/* The clears every window of the user hears of: records of (writer, domain) appended to clears in
 * the shared root, the temporary folder, so they last until the next start, as sessionStorage. */
static bool storage_clears_path(char path[STORAGE_PATH]) {
    const char *root=WEBSTORAGE_SHARED_ROOT;
    return root && snprintf(path,STORAGE_PATH,"%s\\clears",root)<STORAGE_PATH;
}
static int storage_clears_publish(webstorage *s,const char *domain) {
    char path[STORAGE_PATH];size_t dn=domain?strlen(domain):0;
    if(dn>STORAGE_DOMAIN_MAX || !storage_clears_path(path) || !storage_dirs_at(WEBSTORAGE_SHARED_ROOT))return WEB_STORAGE_IO;
    uint8_t record[16+STORAGE_DOMAIN_MAX];
    storage_put32(record,(uint32_t)(16+dn));storage_put64(record+4,s->identity);storage_put32(record+12,(uint32_t)dn);
    if(dn)memcpy(record+16,domain,dn);
    return n_document_append(path,record,16+dn)>=0?WEB_STORAGE_OK:WEB_STORAGE_IO;
}
/* Applies the clears other stores appended since this one last read. A record that does not fit
 * ends the reading at the end: nothing after it can be told apart. */
static void storage_clears_read(webstorage *s) {
    char path[STORAGE_PATH];if(!storage_clears_path(path))return;
    n_handle h=n_document_open(path,N_DOCUMENT_READ);if(h<0)return;
    n_result size=n_document_size(h);
    if(size>=0 && (uint64_t)size<s->clears_seen)s->clears_seen=(uint64_t)size; /* made again: nothing to tell */
    while(size>=0 && s->clears_seen+16<=(uint64_t)size){
        uint8_t head[16];if(!storage_io_at(h,s->clears_seen,head,16,false))break;
        uint32_t n=storage_u32(head),dn=storage_u32(head+12);
        if(dn>STORAGE_DOMAIN_MAX || n!=16+dn || (uint64_t)size-s->clears_seen<n){s->clears_seen=(uint64_t)size;break;}
        char domain[STORAGE_DOMAIN_MAX+1];
        if(dn && !storage_io_at(h,s->clears_seen+16,domain,dn,false))break;
        domain[dn]=0;
        if(storage_u64(head+4)!=s->identity && (!dn || storage_domain(domain)))storage_session_clear(s,dn?domain:NULL);
        s->clears_seen+=n;
    }
    n_handle_close(h);
}
/* Watches clears in queue; what it holds now is older than this store's sessionStorage. */
static n_result storage_clears_watch(webstorage *s,n_handle queue) {
    char path[STORAGE_PATH];
    if(!storage_clears_path(path))return -N_ERR_NOT_FOUND;
    if(!storage_dirs_at(WEBSTORAGE_SHARED_ROOT))return -N_ERR_IO;
    n_handle w=n_entry_watch(path,N_WATCH_NAME);if(w<0)return w;
    n_result r=n_events_watch(queue,w,N_EVENT_RECEIVABLE|N_EVENT_CLOSED,WEBSTORAGE_TOKEN);
    if(r<0){n_handle_close(w);return r;}
    struct n_entry entry;n_result info=n_entry_info(path,&entry);
    s->clears=w;s->clears_queue=queue;s->clears_seen=info>=0?entry.size:0;
    return 0;
}
n_result webstorage_watch(webstorage *s,n_handle queue) {
    if(!s)return -N_ERR_INVALID;
    if(s->clears>=0){n_events_unwatch(s->clears_queue,WEBSTORAGE_TOKEN);n_handle_close(s->clears);s->clears=-1;}
    return storage_clears_watch(s,queue);
}
void webstorage_events(webstorage *s,const struct n_event *events,size_t count) {
    bool mine=false,closed=false;
    for(size_t i=0;s && i<count;i++)if(events[i].token==WEBSTORAGE_TOKEN){mine=true;if(events[i].events&N_EVENT_CLOSED)closed=true;}
    if(!mine || s->clears<0)return;
    n_result taken=n_watch_take(s->clears);
    if(taken==-N_ERR_CLOSED)closed=true;
    if(taken>0 || closed)storage_clears_read(s);
    if(closed){ /* the folder went: watch the clears made after it again */
        n_handle queue=s->clears_queue;n_events_unwatch(queue,WEBSTORAGE_TOKEN);n_handle_close(s->clears);s->clears=-1;
        (void)storage_clears_watch(s,queue);
    }
}
/* The origins with localStorage this store can find: its areas in RAM and the snapshot slots in
 * the profile folder (with their bytes) and in the shared root; with session, its tabs' origins. */
struct storage_known { char *origin; uint64_t disk; };
struct storage_known_list { struct storage_known *at; size_t count, cap; };
static bool storage_known_push(struct storage_known_list *l,const char *origin,uint64_t disk) {
    if(l->count==l->cap){
        size_t cap=l->cap?l->cap*2:32;struct storage_known *at=cap>l->cap?realloc(l->at,cap*sizeof *at):NULL;
        if(!at)return false;l->at=at;l->cap=cap;
    }
    char *copy=storage_copy(origin,strlen(origin));if(!copy)return false;
    l->at[l->count++]=(struct storage_known){copy,disk};return true;
}
static void storage_known_free(struct storage_known_list *l) {
    for(size_t i=0;i<l->count;i++)free(l->at[i].origin);free(l->at);memset(l,0,sizeof *l);
}
/* The origin a snapshot slot holds, when the slot's name is that origin's (storage_path_at). */
static bool storage_slot_origin(const char *root,const struct n_entry *entry,char origin[2048]) {
    size_t len=strlen(entry->name);
    if(entry->kind!=N_KIND_DOCUMENT || len!=66 || entry->name[64]!='.' || (entry->name[65]!='0' && entry->name[65]!='1'))return false;
    char path[STORAGE_PATH],expected[STORAGE_PATH];
    if(snprintf(path,sizeof path,"%s\\%s",root,entry->name)>=STORAGE_PATH)return false;
    n_handle h=n_document_open(path,N_DOCUMENT_READ);if(h<0)return false;
    uint8_t head[56];bool ok=storage_io_at(h,0,head,56,false) && !memcmp(head,"NWEBST1",8);
    uint32_t on=ok?storage_u32(head+48):0;
    ok=ok && on && on<2048 && storage_io_at(h,56,origin,on,false);
    n_handle_close(h);
    if(!ok)return false;
    origin[on]=0;
    if(strlen(origin)!=on || !storage_origin(origin))return false;
    storage_path_at(root,origin,entry->name[65]-'0',expected);
    return !strcmp(expected,path);
}
static int storage_known_folder(struct storage_known_list *l,const char *root,bool disk) {
    if(!root)return 0;
    struct n_entry *entries=NULL;n_result n=n_folder_entries(root,&entries);
    if(n==-N_ERR_NOT_FOUND)return 0;
    if(n<0)return -1;
    int result=0;
    for(n_result i=0;i<n;i++){char origin[2048];
        if(storage_slot_origin(root,&entries[i],origin) && !storage_known_push(l,origin,disk?entries[i].size:0)){result=-1;break;}
    }
    free(entries);return result;
}
static int storage_known_compare(const void *x,const void *y) {
    return strcmp(((const struct storage_known *)x)->origin,((const struct storage_known *)y)->origin);
}
static int storage_known_collect(webstorage *s,struct storage_known_list *l,bool session) {
    int result=0;
    for(struct storage_area *a=s->areas;a && !result;a=a->next)if(!storage_known_push(l,a->origin,0))result=-1;
    for(struct storage_space *p=session?s->spaces:NULL;p && !result;p=p->next)
        for(struct storage_area *a=p->areas;a && !result;a=a->next)if(!storage_known_push(l,a->origin,0))result=-1;
    if(!result)result=storage_known_folder(l,WEBSTORAGE_ROOT,true);
    if(!result)result=storage_known_folder(l,WEBSTORAGE_SHARED_ROOT,false);
    if(l->count)qsort(l->at,l->count,sizeof *l->at,storage_known_compare);
    size_t kept=0;
    for(size_t i=0;i<l->count;i++){
        if(kept && !strcmp(l->at[kept-1].origin,l->at[i].origin)){l->at[kept-1].disk+=l->at[i].disk;free(l->at[i].origin);}
        else l->at[kept++]=l->at[i];
    }
    l->count=kept;return result;
}
/* The UTF-16 units of origin's localStorage as every window sees it: under the lease when this
 * store has it in RAM or a window shares it now (its control exists), else the newer saved slot. */
static int storage_local_units(webstorage *s,const char *origin,uint64_t *units) {
    *units=0;
    struct storage_area *a=storage_area_find(s,NULL,origin);bool loaded=a!=NULL;
    if(!loaded){
        char path[STORAGE_PATH];struct n_entry entry;storage_path_at(WEBSTORAGE_SHARED_ROOT,origin,2,path);
        if(!path[0] || n_entry_info(path,&entry)<0){
            struct storage_area *x=NULL,*y=NULL;int xr=storage_read(origin,0,&x),yr=storage_read(origin,1,&y);
            if(y && (!x || y->generation>x->generation)){storage_area_free(x);x=y;y=NULL;}
            storage_area_free(y);
            int result=x || (xr>=0 && yr>=0)?WEB_STORAGE_OK:WEB_STORAGE_IO;
            if(x)*units=x->units;
            storage_area_free(x);return result;
        }
        int result=storage_get_area(s,NULL,origin,&a);if(result)return result;
    }
    struct storage_control h;int result=storage_shared_lock(s,a,&h);
    if(!result){result=storage_shared_sync(a,&h);n_document_release(a->shared);}
    if(!result)*units=a->units;
    if(!loaded)storage_area_drop(s,a);
    return result;
}
long webstorage_usage(webstorage *s,struct web_storage_usage **out) {
    if(!out)return -1;
    *out=NULL;if(!s)return -1;
    struct storage_known_list l={0};
    if(storage_known_collect(s,&l,true)<0){storage_known_free(&l);return -1;}
    struct web_storage_usage *list=calloc(l.count?l.count:1,sizeof *list);
    if(!list){storage_known_free(&l);return -1;}
    size_t count=0;
    for(size_t i=0;i<l.count;i++){
        uint64_t local=0,session=0;
        (void)storage_local_units(s,l.at[i].origin,&local); /* an unreadable one keeps its disk bytes */
        for(struct storage_space *p=s->spaces;p;p=p->next){struct storage_area *a=storage_area_find(s,p,l.at[i].origin);if(a)session+=a->units;}
        if(!local && !session && !l.at[i].disk)continue;
        list[count++]=(struct web_storage_usage){.origin=l.at[i].origin,.local=local*2,.session=session*2,.disk=l.at[i].disk};
        l.at[i].origin=NULL;
    }
    storage_known_free(&l);*out=list;return (long)count;
}
void webstorage_usage_free(struct web_storage_usage *usage,size_t count) {
    for(size_t i=0;usage && i<count;i++)free(usage[i].origin);free(usage);
}
/* The user agent clears origin's localStorage in its entirety (Storage Standard). Under the lease
 * the empty map becomes the shared area in two checkpoints, so both shared slots hold it and the
 * journal of old changes is cut off, and the saved slots leave the profile folder ("promptly
 * deleted from the underlying storage"); when they cannot be removed, the empty map is saved over
 * both. Every window reads the empty map at its next use (the epoch changed); readers lose the
 * events they had not taken, which name cleared values. */
static int storage_local_clear(webstorage *s,const char *origin) {
    struct storage_area *a=storage_area_find(s,NULL,origin);bool loaded=a!=NULL;
    int result=storage_get_area(s,NULL,origin,&a);if(result)return result;
    struct storage_control h;int present=0;
    result=storage_shared_claim(a,&h,&present);
    if(!result){
        if(!present)(void)storage_seed(a,&h); /* for the generation and slot saved last */
        a->generation=h.generation;a->slot=h.slot;
        storage_map_clear(a);
        result=storage_checkpoint(a,&h);
        if(!result)result=storage_checkpoint(a,&h);
        if(!result){
            bool removed=true;
            for(int slot=0;slot<2;slot++){
                char path[STORAGE_PATH];storage_path_at(WEBSTORAGE_ROOT,origin,slot,path);
                n_result gone=path[0]?n_entry_remove(path):-N_ERR_INVALID;
                if(gone<0 && gone!=-N_ERR_NOT_FOUND)removed=false;
            }
            struct web_storage_result ignored={0};
            if(removed)a->slot=-1;
            else for(int i=0;i<2 && !result;i++)result=storage_save(a,&ignored);
            if(!result){h.persisted=h.revision;h.generation=a->generation;h.slot=a->slot;
                if(!storage_control_commit(a,&h))result=WEB_STORAGE_IO;}
        }
        n_document_release(a->shared);
    }
    if(!result){a->dirty=false;a->unavailable=false;a->retry_at=0;}
    if(!loaded)storage_area_drop(s,a);
    return result;
}
static int storage_clear(webstorage *s,const char *domain) {
    if(!s)return WEB_STORAGE_IO;
    storage_session_clear(s,domain);
    int status=storage_clears_publish(s,domain);
    struct storage_known_list l={0};
    if(storage_known_collect(s,&l,false)<0)status=WEB_STORAGE_IO;
    for(size_t i=0;i<l.count;i++)if(!domain || storage_in_domain(l.at[i].origin,domain)){
        int result=storage_local_clear(s,l.at[i].origin);if(result)status=result;
    }
    storage_known_free(&l);return status;
}
int webstorage_clear_site(webstorage *s,const char *domain) {
    return storage_domain(domain)?storage_clear(s,domain):WEB_STORAGE_SECURITY;
}
int webstorage_clear_all(webstorage *s) { return storage_clear(s,NULL); }
