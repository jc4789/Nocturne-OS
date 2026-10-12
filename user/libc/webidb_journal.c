/* Checked delta journal plus dual small commit heads. Unchanged metadata and
 * reference spans are COPY operations, not whole-manifest FAT rewrites.
 * The two heads keep the current/previous committed generations and their
 * immutable journal ancestry. Compaction only truncates an unreferenced lane. */
#include "webidb_journal.h"
#include <nocturne.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <bearssl.h>

#define COMMIT_HEAD 136u
#define FRAME_HEAD 112u
#define MATCH_BYTES 32u
struct byte_vector {uint8_t *bytes;size_t length,capacity;};
struct snapshot {
    uint8_t *bytes;size_t length;
    uint8_t signature[32],frame_hash[32];
    uint64_t offset,frame_length,chain_length;
    unsigned journal;
    bool valid,journalled;
};
/* One database working set per broker. Switching evicts, never changes
 * database semantics or imposes a feature/record quota. No borrowed pointers. */
static char cache_base[192];
static struct snapshot cache[2];

static uint64_t get64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n|=(uint64_t)p[i]<<(i*8);return n;}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(i*8));}
static void hash(const uint8_t *p,size_t n,uint8_t out[32]){br_sha256_context h;br_sha256_init(&h);br_sha256_update(&h,p,n);br_sha256_out(&h,out);}
static bool exact(int fd,uint8_t *bytes,size_t length,bool writing){
    size_t at=0;
    while(at<length){size_t step=length-at;if(step>INT_MAX)step=INT_MAX;ssize_t n=writing?write(fd,bytes+at,step):read(fd,bytes+at,step);if(n<=0)return false;at+=(size_t)n;}
    return true;
}
static void clear_snapshot(struct snapshot *s){free(s->bytes);memset(s,0,sizeof *s);}
void webidb_journal_forget(void){clear_snapshot(&cache[0]);clear_snapshot(&cache[1]);cache_base[0]=0;}
static void select_base(const char *base){
    if(!strcmp(cache_base,base))return;
    webidb_journal_forget();snprintf(cache_base,sizeof cache_base,"%s",base);
}
static bool append(struct byte_vector *v,const void *bytes,size_t length){
    if(length>SIZE_MAX-v->length)return false;
    size_t needed=v->length+length;
    if(needed>v->capacity){
        size_t capacity=v->capacity?v->capacity:256;
        while(capacity<needed){
            if(capacity>SIZE_MAX/2){capacity=needed;break;}
            capacity*=2;
        }
        uint8_t *more=realloc(v->bytes,capacity);
        if(!more)return false;
        v->bytes=more;v->capacity=capacity;
    }
    if(length)memcpy(v->bytes+v->length,bytes,length);v->length=needed;return true;
}
static bool operation(struct byte_vector *v,unsigned kind,size_t offset,const uint8_t *bytes,size_t length){
    if(!length)return true;
    uint8_t header[17];header[0]=(uint8_t)kind;put64(header+1,offset);put64(header+9,length);
    return append(v,header,sizeof header)&&(kind!=1||append(v,bytes,length));
}
struct match {uint32_t weak;size_t offset;bool used;};
static uint32_t weak_hash(const uint8_t *bytes){
    uint32_t sum=0,weighted=0;
    for(unsigned i=0;i<MATCH_BYTES;i++){sum+=bytes[i];weighted+=(MATCH_BYTES-i)*bytes[i];}
    return sum|(weighted<<16);
}
static size_t bucket(uint32_t weak,size_t capacity){return (size_t)((weak^(weak>>16))*2654435761u)&(capacity-1);}
static bool delta(const uint8_t *old,size_t old_length,const uint8_t *bytes,size_t length,struct byte_vector *out){
    size_t blocks=old_length/MATCH_BYTES,capacity=16;
    if(blocks>SIZE_MAX/2)return false;
    while(capacity<blocks*2){if(capacity>SIZE_MAX/2)return false;capacity*=2;}
    struct match *table=capacity<=SIZE_MAX/sizeof *table?calloc(capacity,sizeof *table):NULL;
    if(!table)return operation(out,1,0,bytes,length); /* optional compression */
    for(size_t i=0;i<blocks;i++){
        size_t offset=i*MATCH_BYTES;uint32_t weak=weak_hash(old+offset);size_t slot=bucket(weak,capacity);
        while(table[slot].used)slot=(slot+1)&(capacity-1);
        table[slot]=(struct match){weak,offset,true};
    }
    size_t at=0,literal=0;uint32_t weak=length>=MATCH_BYTES?weak_hash(bytes):0;
    bool ok=true;
    while(at+MATCH_BYTES<=length){
        size_t slot=bucket(weak,capacity),match=SIZE_MAX;
        while(table[slot].used){
            if(table[slot].weak==weak&&!memcmp(old+table[slot].offset,bytes+at,MATCH_BYTES)){match=table[slot].offset;break;}
            slot=(slot+1)&(capacity-1);
        }
        if(match!=SIZE_MAX){
            size_t count=MATCH_BYTES;
            while(count<length-at&&count<old_length-match&&bytes[at+count]==old[match+count])count++;
            if(!operation(out,1,0,bytes+literal,at-literal)||!operation(out,2,match,NULL,count)){ok=false;break;}
            at+=count;literal=at;
            if(length-at>=MATCH_BYTES)weak=weak_hash(bytes+at);
        }else{
            if(length-at>MATCH_BYTES){
                uint32_t sum=weak&65535u,weighted=weak>>16;
                sum=sum-bytes[at]+bytes[at+MATCH_BYTES];
                weighted=(weighted-MATCH_BYTES*bytes[at]+sum)&65535u;
                weak=sum|(weighted<<16);
            }
            at++;
        }
    }
    if(ok)ok=operation(out,1,0,bytes+literal,length-literal);
    free(table);return ok;
}
static int apply(const uint8_t *old,size_t old_length,const uint8_t *patch,size_t patch_length,size_t length,uint8_t **result){
    uint8_t *bytes=malloc(length?length:1);if(!bytes)return -2;
    size_t at=0,written=0;
    while(at<patch_length){
        if(patch_length-at<17)goto malformed;
        unsigned kind=patch[at];uint64_t offset=get64(patch+at+1),count=get64(patch+at+9);at+=17;
        if(!count||count>length-written)goto malformed;
        if(kind==1){if(count>patch_length-at)goto malformed;memcpy(bytes+written,patch+at,(size_t)count);at+=(size_t)count;}
        else if(kind==2){if(offset>old_length||count>old_length-offset)goto malformed;memcpy(bytes+written,old+(size_t)offset,(size_t)count);}
        else goto malformed;
        written+=(size_t)count;
    }
    if(written!=length)goto malformed;
    *result=bytes;return 1;
malformed:
    free(bytes);return -1;
}
struct frame {uint8_t *bytes;size_t length;};
static int reconstruct(const char *base,const uint8_t *head,uint8_t **result,size_t *length){
    unsigned journal=(unsigned)get64(head+72);char path[224];snprintf(path,sizeof path,"%s.journal%u",base,journal);
    int fd=open(path,O_RDONLY);if(fd<0)return -1;
    struct n_stat st;if(fstat(fd,&st)<0){close(fd);return -1;}
    uint64_t offset=get64(head+80),n=get64(head+88);uint8_t expected[32];memcpy(expected,head+104,32);
    struct frame *frames=NULL;size_t count=0,capacity=0;uint64_t generation=get64(head+40);int status=1;
    for(;;){
        if(n<FRAME_HEAD||n>SIZE_MAX||n>LONG_MAX||offset>st.size||n>st.size-offset){status=-1;break;}
        uint8_t *bytes=malloc((size_t)n);if(!bytes){status=-2;break;}
        if(offset>LONG_MAX||lseek(fd,(long)offset,SEEK_SET)<0||!exact(fd,bytes,(size_t)n,false)){free(bytes);status=-1;break;}
        uint8_t digest[32];hash(bytes+40,(size_t)n-40,digest);
        if(memcmp(bytes,"NIDJ0001",8)||memcmp(digest,bytes+8,32)||memcmp(digest,expected,32)||
           get64(bytes+40)!=generation||get64(bytes+96)>SIZE_MAX||get64(bytes+104)!=(size_t)n-FRAME_HEAD){free(bytes);status=-1;break;}
        if(count==capacity){
            size_t next=capacity?capacity*2:8;
            if(next<=capacity||next>SIZE_MAX/sizeof *frames){
                free(bytes);status=-2;break;
            }
            struct frame *more=realloc(frames,next*sizeof *frames);
            if(!more){free(bytes);status=-2;break;}
            frames=more;capacity=next;
        }
        frames[count++]=(struct frame){bytes,(size_t)n};
        uint64_t previous_length=get64(bytes+56),previous_offset=get64(bytes+48);
        if(!previous_length)break;
        if(!generation||previous_offset>=offset||previous_length>offset-previous_offset){status=-1;break;}
        generation--;offset=previous_offset;n=previous_length;memcpy(expected,bytes+64,32);
    }
    close(fd);
    uint8_t *current=NULL;size_t current_length=0;
    for(size_t i=count;status==1&&i;i--){
        const struct frame *f=&frames[i-1];uint8_t *next=NULL;size_t next_length=(size_t)get64(f->bytes+96);
        status=apply(current,current_length,f->bytes+FRAME_HEAD,f->length-FRAME_HEAD,next_length,&next);
        if(status==1){free(current);current=next;current_length=next_length;}
    }
    for(size_t i=0;i<count;i++)free(frames[i].bytes);free(frames);
    if(status!=1){free(current);return status;}
    *result=current;*length=current_length;return 1;
}
int webidb_journal_read(const char *base,int slot,uint8_t **bytes,size_t *length){
    *bytes=NULL;*length=0;if(slot<0||slot>1)return -1;select_base(base);
    char path[208];snprintf(path,sizeof path,"%s.%d",base,slot);struct n_stat st;
    if(stat(path,&st)<0){clear_snapshot(&cache[slot]);return errno==ENOENT?0:-1;}
    if(st.size<64||st.size>SIZE_MAX||st.size>LONG_MAX)return -1;
    int fd=open(path,O_RDONLY);if(fd<0)return -1;
    uint8_t prefix[COMMIT_HEAD];size_t prefix_length=st.size<sizeof prefix?(size_t)st.size:sizeof prefix;
    if(!exact(fd,prefix,prefix_length,false)){close(fd);return -1;}
    bool journalled=!memcmp(prefix,"NIDB0004",8);
    uint8_t signature[32];
    if(journalled){
        if(st.size<COMMIT_HEAD||get64(prefix+72)>1||get64(prefix+56)>st.size-COMMIT_HEAD||
           get64(prefix+64)!=st.size-COMMIT_HEAD-get64(prefix+56)){close(fd);return -1;}
        memcpy(signature,prefix+8,32);
    }else{
        /* Legacy heads already carry a full-manifest content digest. */
        if(memcmp(prefix,"NIDB0003",8)&&memcmp(prefix,"NIDB0002",8)){close(fd);return -1;}
        memcpy(signature,prefix+8,32);
    }
    struct snapshot *cached=&cache[slot];
    if(cached->valid&&cached->journalled==journalled&&!memcmp(cached->signature,signature,32)){
        close(fd);uint8_t *copy=malloc(cached->length?cached->length:1);if(!copy)return -2;
        memcpy(copy,cached->bytes,cached->length);*bytes=copy;*length=cached->length;return 1;
    }
    uint8_t *head=malloc((size_t)st.size);if(!head){close(fd);return -2;}
    memcpy(head,prefix,prefix_length);
    if(!exact(fd,head+prefix_length,(size_t)st.size-prefix_length,false)){close(fd);free(head);return -1;}
    close(fd);
    uint8_t *raw=NULL;size_t raw_length=0;
    if(journalled){
        uint8_t actual[32];hash(head+40,(size_t)st.size-40,actual);
        if(memcmp(actual,head+8,32)){free(head);return -1;}
        int status=reconstruct(base,head,&raw,&raw_length);if(status!=1){free(head);return status;}
    }else {raw=head;head=NULL;raw_length=(size_t)st.size;}
    clear_snapshot(cached);
    cached->bytes=malloc(raw_length?raw_length:1);
    if(cached->bytes){
        memcpy(cached->bytes,raw,raw_length);cached->length=raw_length;cached->valid=true;cached->journalled=journalled;
        memcpy(cached->signature,signature,32);
        if(journalled){
            cached->journal=(unsigned)get64(head+72);
            cached->offset=get64(head+80);
            cached->frame_length=get64(head+88);
            cached->chain_length=get64(head+96);
            memcpy(cached->frame_hash,head+104,32);
        }
    }
    free(head);*bytes=raw;*length=raw_length;return 1;
}
int webidb_journal_write(const char *base,int slot,const uint8_t *bytes,size_t length){
    if(slot<0||slot>1||length<120||memcmp(bytes,"NIDB0003",8))return -1;
    select_base(base);
    struct snapshot *previous=&cache[1-slot];
    bool chained=previous->valid&&previous->journalled;
    uint64_t generation=get64(bytes+72);
    if(chained&&(previous->length<120||get64(previous->bytes+72)!=generation-1))chained=false;
    struct byte_vector patch={0};
    bool full=!chained;
    if(!full&&(!delta(previous->bytes,previous->length,bytes,length,&patch)||patch.length>=length))full=true;
    uint64_t threshold=length>(UINT64_MAX-65536)/2?UINT64_MAX:(uint64_t)length*2+65536;
    if(chained&&previous->chain_length>threshold)full=true;
    unsigned journal=chained?previous->journal:0;
    bool truncate=false;
    if(full){
        unsigned candidate=1-journal;
        const struct snapshot *other=&cache[slot];
        if(!other->valid||!other->journalled||other->journal!=candidate){journal=candidate;truncate=true;}
        free(patch.bytes);memset(&patch,0,sizeof patch);
        if(!operation(&patch,1,0,bytes,length)){free(patch.bytes);return -2;}
    }
    if(patch.length>SIZE_MAX-FRAME_HEAD||patch.length>(size_t)LONG_MAX-FRAME_HEAD){free(patch.bytes);return -2;}
    char journal_path[224];snprintf(journal_path,sizeof journal_path,"%s.journal%u",base,journal);
    int fd=open(journal_path,O_RDWR|O_CREAT|(truncate?O_TRUNC:O_APPEND));
    if(fd<0){free(patch.bytes);return -1;}
    struct n_stat st;if(fstat(fd,&st)<0||st.size>LONG_MAX){close(fd);free(patch.bytes);return -1;}
    uint64_t offset=st.size;
    uint8_t frame[FRAME_HEAD]={0};memcpy(frame,"NIDJ0001",8);put64(frame+40,generation);
    if(!full){put64(frame+48,previous->offset);put64(frame+56,previous->frame_length);memcpy(frame+64,previous->frame_hash,32);}
    put64(frame+96,length);put64(frame+104,patch.length);
    br_sha256_context digest;
    br_sha256_init(&digest);
    br_sha256_update(&digest,frame+40,sizeof frame-40);
    br_sha256_update(&digest,patch.bytes,patch.length);
    br_sha256_out(&digest,frame+8);
    bool ok=exact(fd,frame,sizeof frame,true)&&exact(fd,patch.bytes,patch.length,true);
    if(close(fd)<0)ok=false;
    free(patch.bytes);
    if(!ok)return -1;
    uint64_t on=get64(bytes+96),nn=get64(bytes+104);
    if(on>length-120||nn>length-120-on||on>SIZE_MAX-COMMIT_HEAD||nn>SIZE_MAX-COMMIT_HEAD-on)return -1;
    size_t head_length=COMMIT_HEAD+(size_t)on+(size_t)nn;
    uint8_t *head=calloc(1,head_length);if(!head)return -2;
    memcpy(head,"NIDB0004",8);
    put64(head+40,generation);
    put64(head+48,get64(bytes+112));
    put64(head+56,on);put64(head+64,nn);
    put64(head+72,journal);put64(head+80,offset);put64(head+88,FRAME_HEAD+get64(frame+104));
    uint64_t chain=FRAME_HEAD+get64(frame+104);
    if(!full){
        if(previous->chain_length>UINT64_MAX-chain){free(head);return -1;}
        chain+=previous->chain_length;
    }
    put64(head+96,chain);
    memcpy(head+104,frame+8,32);
    memcpy(head+COMMIT_HEAD,bytes+120,(size_t)on+(size_t)nn);
    hash(head+40,head_length-40,head+8);
    char head_path[208];snprintf(head_path,sizeof head_path,"%s.%d",base,slot);
    fd=open(head_path,O_WRONLY|O_CREAT|O_TRUNC);
    ok=fd>=0&&exact(fd,head,head_length,true);
    if(fd>=0&&close(fd)<0)ok=false;
    if(!ok){unlink(head_path);free(head);return -1;}
    clear_snapshot(&cache[slot]);
    struct snapshot *published=&cache[slot];
    published->bytes=malloc(length);
    if(published->bytes){
        memcpy(published->bytes,bytes,length);
        published->length=length;
        published->valid=published->journalled=true;
        published->journal=journal;published->offset=offset;
        published->frame_length=get64(head+88);published->chain_length=chain;
        memcpy(published->signature,head+8,32);memcpy(published->frame_hash,frame+8,32);
    }
    free(head);return 1; /* Commit is durable even if optional cache allocation failed. */
}
