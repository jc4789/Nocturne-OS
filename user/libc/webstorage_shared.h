/* Native shared localStorage. The fixed leased control file is RAM-root only.
 * Two checksummed headers commit an append, never an unvalidated tail. A
 * checkpoint replaces consumed deltas; live document event cursors prohibit
 * compaction of undelivered notifications. Persistent slots are unchanged. */
#define STORAGE_CONTROL_BYTES 256u
#define STORAGE_READER_BYTES 48u
#define STORAGE_EVENT_BYTES (2u * STORAGE_WINDOW_BYTES)
struct storage_control {
    uint64_t serial, epoch, revision, end, checkpoint_revision, persisted;
    uint64_t generation, dirty_since, changed_at;
    int checkpoint_slot, slot;
};
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
static bool storage_io_at(int fd,uint64_t at,void *bytes,size_t n,bool writing) {
    if(at>LONG_MAX || lseek(fd,(long)at,SEEK_SET)<0)return false;
    size_t done=0;
    while(done<n){
        ssize_t size=writing?write(fd,(char *)bytes+done,n-done):read(fd,(char *)bytes+done,n-done);
        if(size<0 && errno==EINTR)continue;
        if(size<=0 || (size_t)size>n-done)return false;
        done+=(size_t)size;
    }
    return true;
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
    struct n_stat st;if(fstat(a->shared_fd,&st)<0)return -1;
    if(!st.size)return 0;
    uint8_t b[128];struct storage_control other;bool found=false;
    for(unsigned i=0;i<2;i++)if(st.size>=(i+1u)*128 && storage_io_at(a->shared_fd,i*128,b,128,false) && storage_control_unpack(b,&other)){
        if(!found || other.serial>h->serial)*h=other;found=true;
    }
    return found?1:-1;
}
static bool storage_control_commit(struct storage_area *a,struct storage_control *h) {
    if(h->serial==UINT64_MAX)return false;
    h->serial++;uint8_t b[128];storage_control_pack(h,b);
    return storage_io_at(a->shared_fd,(h->serial&1u)*128,b,128,true);
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
    for(int i=0;list && i<count;i++)if(list[i].pid==getpid()){start=list[i].start_ms;break;}
    free(list);return start;
}
static bool storage_reader_alive(const uint8_t row[48],struct n_procinfo *list,int count) {
    for(int i=0;i<count;i++)if(list[i].pid==(int)storage_u64(row+8) &&
        list[i].start_ms==storage_u64(row+16) && list[i].state!=3)return true;
    return false;
}
static int storage_readers_open(struct storage_area *a) {
    char path[160];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,3,path);
    return open(path,O_RDWR|O_CREAT);
}
static bool storage_reader_register(struct storage_area *a,uint64_t token,bool holder,
                                    const struct storage_control *h,uint64_t *offset) {
    int fd=storage_readers_open(a);if(fd<0)return false;
    struct n_stat st;bool ok=fstat(fd,&st)==0 && st.size%STORAGE_READER_BYTES==0;
    int count=0;struct n_procinfo *list=ok?storage_processes(&count):NULL;
    if(!list)ok=false;
    uint64_t vacant=UINT64_MAX;uint8_t row[48];bool found=false;
    for(uint64_t at=0;ok && at<st.size;at+=48){
        if(!storage_io_at(fd,at,row,48,false)){ok=false;break;}
        if(storage_u64(row) && !storage_reader_alive(row,list,count)){
            memset(row,0,48);if(!storage_io_at(fd,at,row,48,true)){ok=false;break;}
        }
        if(storage_u64(row)==token && storage_u64(row+40)==(uint64_t)holder){*offset=at;found=true;break;}
        if(!storage_u64(row) && vacant==UINT64_MAX)vacant=at;
    }
    if(ok && !found){
        *offset=vacant==UINT64_MAX?st.size:vacant;memset(row,0,48);
        storage_put64(row,token);storage_put64(row+8,(uint32_t)getpid());storage_put64(row+16,storage_process_start());
        storage_put64(row+24,h->epoch);storage_put64(row+32,h->end);storage_put64(row+40,holder);
        ok=storage_io_at(fd,*offset,row,48,true);
    }
    free(list);if(close(fd)<0)ok=false;return ok;
}
static bool storage_reader_update(struct storage_area *a,uint64_t offset,uint64_t token,
                                  uint64_t epoch,uint64_t cursor,bool remove) {
    int fd=storage_readers_open(a);if(fd<0)return false;uint8_t row[48];
    bool ok=storage_io_at(fd,offset,row,48,false) && storage_u64(row)==token && !storage_u64(row+40);
    if(ok){
        if(remove)memset(row,0,48);else{storage_put64(row+24,epoch);storage_put64(row+32,cursor);}
        ok=storage_io_at(fd,offset,row,48,true);
    }
    if(close(fd)<0)ok=false;return ok;
}
static bool storage_checkpoint_allowed(struct storage_area *a,const struct storage_control *h) {
    int fd=storage_readers_open(a);if(fd<0)return false;struct n_stat st;
    bool ok=fstat(fd,&st)==0 && st.size%48==0;int count=0;
    struct n_procinfo *list=ok?storage_processes(&count):NULL;if(!list)ok=false;
    uint8_t row[48];
    for(uint64_t at=0;ok && at<st.size;at+=48){
        if(!storage_io_at(fd,at,row,48,false)){ok=false;break;}
        if(!storage_u64(row))continue;
        if(!storage_reader_alive(row,list,count)){
            memset(row,0,48);if(!storage_io_at(fd,at,row,48,true))ok=false;continue;
        }
        if(!storage_u64(row+40) && (storage_u64(row+24)!=h->epoch || storage_u64(row+32)<h->end))ok=false;
    }
    free(list);if(close(fd)<0)ok=false;return ok;
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
     * the delta vnode, retaining the lease inode and all existing descriptors.
     * A crash before truncation leaves an ignored tail; after it, the committed
     * checkpoint is complete. Every operation owns the same control lease. */
    char path[160];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,4,path);
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC);if(fd<0)return WEB_STORAGE_IO;
    return close(fd)==0?WEB_STORAGE_OK:WEB_STORAGE_IO;
}
static int storage_shared_lock(webstorage *s,struct storage_area *a,struct storage_control *h) {
    if(a->shared_fd<0){
        if(!storage_dirs_at(WEBSTORAGE_SHARED_ROOT))return WEB_STORAGE_IO;
        char path[160];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,2,path);
        a->shared_fd=open(path,O_RDWR|O_CREAT);if(a->shared_fd<0)return WEB_STORAGE_IO;
    }
    if(a->shared_log_fd<0){
        char path[160];storage_path_at(WEBSTORAGE_SHARED_ROOT,a->origin,4,path);
        a->shared_log_fd=open(path,O_RDWR|O_CREAT);if(a->shared_log_fd<0)return WEB_STORAGE_IO;
    }
    if(fcntl(a->shared_fd,F_NLOCK)<0)return WEB_STORAGE_IO;
    memset(h,0,sizeof *h);int present=storage_control_read(a,h),result=WEB_STORAGE_OK;
    if(present<0)result=WEB_STORAGE_IO;
    else if(!present){
        /* A shared area wins over an older disk snapshot. Only seed from disk
         * when the leased area is first created after boot. */
        struct storage_area *snapshot=NULL,*other=NULL;
        int ar=storage_read(a->origin,0,&snapshot),br=storage_read(a->origin,1,&other);
        if(other && (!snapshot || other->generation>snapshot->generation)){storage_area_free(snapshot);snapshot=other;other=NULL;}
        storage_area_free(other);
        if(!snapshot && (ar<0 || br<0))result=WEB_STORAGE_IO;
        if(snapshot){
            if(snapshot->units>STORAGE_WINDOW_BYTES/2-(s->units-a->units))result=WEB_STORAGE_QUOTA;
            else{s->units=s->units-a->units+snapshot->units;storage_entries_free(a->entries);
                a->entries=snapshot->entries;snapshot->entries=NULL;a->units=snapshot->units;a->count=snapshot->count;
                a->slot=snapshot->slot;a->generation=snapshot->generation;}
            storage_area_free(snapshot);
        }
        h->checkpoint_slot=-1;h->slot=a->slot;h->generation=a->generation;
        if(!result)result=storage_checkpoint(a,h);
    }
    if(!result && !a->shared_holder){uint64_t holder;
        if(!storage_reader_register(a,s->identity,true,h,&holder))result=WEB_STORAGE_IO;
        else a->shared_holder=true;
    }
    if(result)fcntl(a->shared_fd,F_NUNLOCK);
    return result;
}
static int storage_record_read(struct storage_area *a,uint64_t at,uint64_t end,uint8_t **out) {
    uint8_t header[48];if(at<STORAGE_CONTROL_BYTES || end-at<48 || !storage_io_at(a->shared_log_fd,at-STORAGE_CONTROL_BYTES,header,48,false))return WEB_STORAGE_IO;
    uint32_t n=storage_u32(header),op=storage_u32(header+4),kn=storage_u32(header+24),old=storage_u32(header+28),value=storage_u32(header+32),url=storage_u32(header+36);
    if(n<48 || n>end-at || (uint64_t)kn+old+value+url!=n-48 || op<WEB_STORAGE_SET || op>WEB_STORAGE_CLEAR)return WEB_STORAGE_IO;
    uint8_t *bytes=malloc(n);if(!bytes)return WEB_STORAGE_QUOTA;
    memcpy(bytes,header,48);if(!storage_io_at(a->shared_log_fd,at+48-STORAGE_CONTROL_BYTES,bytes+48,n-48,false)){free(bytes);return WEB_STORAGE_IO;}
    *out=bytes;return WEB_STORAGE_OK;
}
static int storage_shared_sync(webstorage *s,struct storage_area *a,const struct storage_control *h) {
    if(a->shared_epoch!=h->epoch){
        struct storage_area *snapshot=NULL;
        if(storage_read_at(WEBSTORAGE_SHARED_ROOT,a->origin,h->checkpoint_slot,&snapshot)!=1)return WEB_STORAGE_IO;
        if(snapshot->generation!=h->checkpoint_revision+1 || snapshot->units>STORAGE_WINDOW_BYTES/2-(s->units-a->units)){storage_area_free(snapshot);return WEB_STORAGE_QUOTA;}
        s->units=s->units-a->units+snapshot->units;storage_entries_free(a->entries);
        a->entries=snapshot->entries;snapshot->entries=NULL;a->count=snapshot->count;a->units=snapshot->units;storage_area_free(snapshot);
        a->shared_epoch=h->epoch;a->shared_cursor=STORAGE_CONTROL_BYTES;
    }
    if(a->shared_cursor>h->end)return WEB_STORAGE_IO;
    while(a->shared_cursor<h->end){
        uint8_t *record=NULL;int result=storage_record_read(a,a->shared_cursor,h->end,&record);if(result)return result;
        uint32_t kn=storage_u32(record+24),old=storage_u32(record+28),vn=storage_u32(record+32);
        struct web_storage_request r={.kind=WEB_STORAGE_LOCAL,.operation=(int)storage_u32(record+4),
            .key=(char *)record+48,.key_len=kn,.value=(char *)record+48+kn+old,.value_len=vn};
        struct web_storage_result ignored;result=storage_access_ram(s,a->origin,&r,&ignored);
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
    p->next=s->sources;s->sources=p;return WEB_STORAGE_OK;
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
    int fd=storage_readers_open(a);uint8_t row[48];
    bool ok=fd>=0 && storage_io_at(fd,p->reader,row,48,false) && storage_u64(row)==p->token;
    if(fd>=0)close(fd);if(!ok)return WEB_STORAGE_IO;
    uint64_t epoch=storage_u64(row+24),cursor=storage_u64(row+32);
    if(epoch!=h->epoch){epoch=h->epoch;cursor=STORAGE_CONTROL_BYTES;}
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
    return storage_reader_update(a,p->reader,p->token,epoch,cursor,false)?WEB_STORAGE_OK:WEB_STORAGE_IO;
}
/* Session storage belongs to one top-level window, but its other live realms
 * still receive queued events. Allocate every recipient before changing RAM. */
static int storage_session_prepare(webstorage *s,struct storage_area *a,const struct web_storage_request *r,
                                    const char *old,size_t old_len,bool existed,struct storage_notification **pending) {
    size_t kn=r->operation==WEB_STORAGE_CLEAR?0:r->key_len,vn=r->operation==WEB_STORAGE_SET?r->value_len:0;
    size_t un=r->url?strlen(r->url):0,bytes=sizeof(struct storage_notification)+kn+old_len+vn+un+4;
    size_t total=s->event_bytes;
    for(struct storage_source *p=s->sources;p;p=p->next)if(p->area==a && p->token!=r->source){
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
        e->data.cursor=++p->cursor;if(p->tail)p->tail->next=e;else p->events=e;p->tail=e;s->event_bytes+=e->bytes;
    }
}
static int storage_session_poll(webstorage *s,struct storage_source *p,const struct web_storage_request *r,
                                struct web_storage_result *out) {
    if(!p || !p->events)return WEB_STORAGE_OK;
    struct storage_notification *e=p->events;
    if(r->operation==WEB_STORAGE_ACK){
        if(r->cursor!=e->data.cursor)return WEB_STORAGE_IO;
        p->events=e->next;if(!p->events)p->tail=NULL;s->event_bytes-=e->bytes;
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
    bool ok=storage_io_at(a->shared_log_fd,h->end-STORAGE_CONTROL_BYTES,record,n,true);free(record);if(!ok)return WEB_STORAGE_IO;
    if(h->revision==h->persisted)h->dirty_since=uptime_ms();h->changed_at=uptime_ms();h->revision++;h->end+=n;
    if(!storage_control_commit(a,h))return WEB_STORAGE_IO;
    a->shared_epoch=h->epoch;a->shared_cursor=h->end;return WEB_STORAGE_OK;
}
static int storage_shared_flush(webstorage *s,struct storage_area *a,struct web_storage_result *out) {
    if(a->kind!=WEB_STORAGE_LOCAL)return WEB_STORAGE_OK;
    struct storage_control h;int result=storage_shared_lock(s,a,&h);if(result)return result;
    result=storage_shared_sync(s,a,&h);
    if(!result && h.revision!=h.persisted){
        result=storage_save(a,out);
        if(!result){h.persisted=h.revision;h.generation=a->generation;h.slot=a->slot;
            if(!storage_control_commit(a,&h))result=WEB_STORAGE_IO;}
    }
    fcntl(a->shared_fd,F_NUNLOCK);return result;
}
static void storage_source_drop(webstorage *s,struct storage_source **link) {
    struct storage_source *p=*link;
    if(p->area->kind==WEB_STORAGE_LOCAL){struct storage_control h;
        if(!storage_shared_lock(s,p->area,&h)){storage_reader_update(p->area,p->reader,p->token,0,0,true);fcntl(p->area->shared_fd,F_NUNLOCK);}}
    while(p->events){struct storage_notification *e=p->events;p->events=e->next;s->event_bytes-=e->bytes;webstorage_result_free(&e->data);free(e);}
    *link=p->next;free(p);
}
static void storage_sources_free(webstorage *s) {
    while(s->sources)storage_source_drop(s,&s->sources);
    /* Holder rows are reclaimed at the next open/checkpoint if a process exits.
       No path is unlinked while another process can still hold its vnode. */
    for(struct storage_area *a=s->areas;a;a=a->next)if(a->shared_fd>=0){
        struct storage_control h;if(storage_shared_lock(s,a,&h))continue;
        int fd=storage_readers_open(a);struct n_stat st;uint8_t row[48];
        if(fd>=0 && fstat(fd,&st)==0)for(uint64_t at=0;at<st.size;at+=48)
            if(storage_io_at(fd,at,row,48,false) && storage_u64(row)==s->identity && storage_u64(row+40)){
                memset(row,0,48);storage_io_at(fd,at,row,48,true);break;
            }
        if(fd>=0)close(fd);fcntl(a->shared_fd,F_NUNLOCK);
    }
}
int webstorage_access(webstorage *s,const char *origin,const struct web_storage_request *r,struct web_storage_result *out) {
    if(!out)return WEB_STORAGE_IO;memset(out,0,sizeof *out);
    if(!s || !r)return WEB_STORAGE_IO;
    if(r->operation==WEB_STORAGE_RELEASE){
        for(struct storage_source **p=&s->sources;*p;)if((*p)->token==r->source)storage_source_drop(s,p);else p=&(*p)->next;
        return WEB_STORAGE_OK;
    }
    if(!storage_origin(origin) || (r->kind!=WEB_STORAGE_LOCAL && r->kind!=WEB_STORAGE_SESSION))return WEB_STORAGE_SECURITY;
    if(r->operation<0 || r->operation>WEB_STORAGE_RELEASE)return WEB_STORAGE_IO;
    size_t ku=0,vu=0;
    if((r->operation==WEB_STORAGE_GET || r->operation==WEB_STORAGE_SET || r->operation==WEB_STORAGE_REMOVE) &&
        (r->key_len>STORAGE_FILE_MAX || !storage_units(r->key,r->key_len,&ku)))return WEB_STORAGE_QUOTA;
    if(r->operation==WEB_STORAGE_SET && (r->value_len>STORAGE_FILE_MAX || !storage_units(r->value,r->value_len,&vu)))return WEB_STORAGE_QUOTA;
    struct storage_area *a=NULL;int result=storage_get_area(s,origin,r->kind,&a);if(result)return result;
    struct storage_control h={0};bool locked=a->kind==WEB_STORAGE_LOCAL;
    if(locked){result=storage_shared_lock(s,a,&h);if(result)return result;result=storage_shared_sync(s,a,&h);if(result)goto done;}
    result=storage_source_add(s,a,r->source,&h);if(result)goto done;
    if(r->operation==WEB_STORAGE_SUBSCRIBE){result=WEB_STORAGE_OK;goto done;}
    struct storage_source *source=storage_source_find(s,a,r->source);
    if(r->operation==WEB_STORAGE_POLL || r->operation==WEB_STORAGE_ACK){
        result=locked?(source?storage_event_poll(a,source,&h,r,out):WEB_STORAGE_IO):storage_session_poll(s,source,r,out);goto done;
    }
    bool mutation=r->operation>=WEB_STORAGE_SET && r->operation<=WEB_STORAGE_CLEAR;
    struct storage_entry *e=mutation && r->operation!=WEB_STORAGE_CLEAR?storage_find(a,r->key,r->key_len):NULL;
    bool changed=mutation && !((r->operation==WEB_STORAGE_REMOVE && !e)||(r->operation==WEB_STORAGE_CLEAR && !a->count)||
        (r->operation==WEB_STORAGE_SET && e && e->vn==r->value_len && (!e->vn || !memcmp(e->value,r->value,e->vn))));
    if(!changed){result=storage_access_ram(s,origin,r,out);goto done;}
    size_t newunits=r->operation==WEB_STORAGE_CLEAR?0:a->units-(e?e->units:0)+(r->operation==WEB_STORAGE_SET?ku+vu:0);
    if(newunits>WEB_STORAGE_QUOTA_BYTES/2 || newunits>STORAGE_WINDOW_BYTES/2-(s->units-a->units) ||
       (r->operation==WEB_STORAGE_SET&&!e&&a->count>=STORAGE_MAX_KEYS)){result=WEB_STORAGE_QUOTA;goto done;}
    struct storage_entry *prepared=NULL;
    if(r->operation==WEB_STORAGE_SET){
        prepared=calloc(1,sizeof *prepared);
        if(prepared){prepared->key=storage_copy(r->key,r->key_len);prepared->value=storage_copy(r->value,r->value_len);
            prepared->kn=r->key_len;prepared->vn=r->value_len;prepared->units=ku+vu;}
        if(!prepared || !prepared->key || !prepared->value){storage_entries_free(prepared);result=WEB_STORAGE_QUOTA;goto done;}
    }
    struct storage_notification *pending=NULL;
    size_t checkpoint_bytes=a->units*3+a->count*8+56;
    if(checkpoint_bytes<65536)checkpoint_bytes=65536;
    if(locked && h.end-STORAGE_CONTROL_BYTES>checkpoint_bytes){
        if(storage_checkpoint_allowed(a,&h)){result=storage_checkpoint(a,&h);if(result)goto mutation_done;}
    }
    if(locked)result=storage_delta_publish(a,&h,r,e?e->value:NULL,e?e->vn:0,e!=NULL);
    else result=storage_session_prepare(s,a,r,e?e->value:NULL,e?e->vn:0,e!=NULL,&pending);
    if(!result){
        struct storage_entry **link=&a->entries;while(*link && *link!=e)link=&(*link)->next;
        if(r->operation==WEB_STORAGE_CLEAR){storage_entries_free(a->entries);a->entries=NULL;a->count=0;}
        else{if(prepared){prepared->next=e?e->next:NULL;*link=prepared;prepared=NULL;if(!e)a->count++;}
            else{*link=e->next;a->count--;}if(e){e->next=NULL;storage_entries_free(e);}}
        s->units=s->units-a->units+newunits;a->units=newunits;
        if(locked){a->dirty=true;a->dirty_since=h.dirty_since;a->changed_at=h.changed_at;}
        else{storage_session_commit(s,a,pending);pending=NULL;}
    }
mutation_done:
    storage_entries_free(prepared);storage_session_discard(pending);
done:
    if(locked)fcntl(a->shared_fd,F_NUNLOCK);
    return result;
}
