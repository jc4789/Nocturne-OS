/* Execute the production network checksum and DNS parser/cache with host allocation.
   Hardware, scheduling and live UDP/TCP are covered separately in QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef SYS_OPEN /* mingw stdio's unrelated stream-count macro */
#include "../kernel/src/sys/sched.h"
static struct task host_task;
#undef current_task
#define current_task (&host_task)
#define task_process() (&host_task)
#include "../kernel/src/net/net.c"

volatile uint64_t timer_ticks=1000;
void *kmalloc(size_t bytes){return malloc(bytes);}
void kfree(void *p){free(p);}
void panic(const char *format,...){(void)format;abort();}
/* Fail closed if a parser/cache test accidentally crosses a hardware/kernel
   boundary. These substitutes never manufacture network success. */
const char *kernel_cmdline="";
int kprintf(const char *format,...){(void)format;abort();}
void *krealloc(void *p,size_t n){return realloc(p,n);}
unsigned cpu_current_index(void){abort();}
void cpu_bsp_wake(void){abort();}
uint64_t vmm_get_pte(uint64_t pml4,uint64_t va){(void)pml4;(void)va;abort();}
void wq_wake_all(struct wait_queue *q){(void)q;abort();}
bool wq_wait_timeout(struct wait_queue *q,uint64_t ms){(void)q;(void)ms;abort();}
void sched_yield(void){abort();}
struct task *kthread_create(const char *name,void (*fn)(void *),void *arg){(void)name;(void)fn;(void)arg;abort();}
bool netvsc_probe(void){abort();}
bool e1000_probe(void){abort();}
bool tulip_probe(void){abort();}
void entropy_get(void *p,size_t n){(void)p;(void)n;abort();}
bool user_ok(const void *p,size_t n){(void)p;(void)n;abort();}
bool user_ok_w(void *p,size_t n){(void)p;(void)n;abort();}
int user_str(char *p,const char *s,size_t n){(void)p;(void)s;(void)n;abort();}
size_t strlcpy(char *out,const char *in,size_t n){size_t len=strlen(in);if(n){size_t copy=MIN(n-1,len);memcpy(out,in,copy);out[copy]=0;}return len;}
void vfs_close(struct file *file){(void)file;abort();}
struct file *vfs_dup(struct file *file){(void)file;abort();}
static unsigned failed,checks;
#define CHECK(condition) do{checks++;if(!(condition)){failed++;printf("FAIL line=%u %s\n",__LINE__,#condition);}}while(0)
/* Independent byte-pair golden: no native-width loads, byte swaps or memcpy.
   Compare the unfolded carry result as well as the final wire checksum. */
static uint32_t checksum_golden(uint32_t seed,const uint8_t *p,size_t length) {
    uint64_t sum=seed;
    while(length>=2) {sum+=(uint32_t)p[0]*256+p[1];p+=2;length-=2;}
    if(length)sum+=(uint32_t)*p*256;
    while(sum>UINT32_MAX)sum=(uint32_t)sum+(sum>>32);
    return (uint32_t)sum;
}
static void checksum_case(const uint8_t *p,size_t length,uint32_t seed) {
    uint32_t expected=checksum_golden(seed,p,length);
    CHECK(csum_add(seed,p,length)==expected);
    while(expected>UINT16_MAX)expected=(expected&UINT16_MAX)+(expected>>16);
    CHECK(csum_fold(csum_add(seed,p,length))==(uint16_t)~expected);
}
static void test_checksum(void) {
    static uint8_t bytes[65535+64];
    static const uint32_t seeds[]={0,1,UINT16_MAX,0x10000,0x80000000,UINT32_MAX};
    static const size_t lengths[]={1460,1500,2047,2048,4095,4096,4097,16383,16384,65534,65535};
    static const uint8_t rfc1071[]={0,1,0xf2,3,0xf4,0xf5,0xf6,0xf7};
    CHECK(csum_fold(csum_add(0,rfc1071,sizeof rfc1071))==0x220d);
    uint32_t pattern=0x12345678;
    for(size_t i=0;i<sizeof bytes;i++) {pattern=pattern*1664525u+1013904223u;bytes[i]=(uint8_t)(pattern>>24);}
    for(size_t alignment=0;alignment<64;alignment++) {
        for(size_t length=0;length<=1024;length++)
            for(size_t seed=0;seed<ARRAY_SIZE(seeds);seed++)checksum_case(bytes+alignment,length,seeds[seed]);
        for(size_t length=0;length<ARRAY_SIZE(lengths);length++)
            for(size_t seed=0;seed<ARRAY_SIZE(seeds);seed++)checksum_case(bytes+alignment,lengths[length],seeds[seed]);
    }
    for(unsigned fill=0;fill<2;fill++) {
        memset(bytes,fill?0xff:0,sizeof bytes);
        for(size_t alignment=0;alignment<64;alignment++)
            for(size_t seed=0;seed<ARRAY_SIZE(seeds);seed++)checksum_case(bytes+alignment,65535,seeds[seed]);
    }
}
static void test_dns_boot_resolver(void) {
    static const struct {const char *command;int status;uint32_t ip;uint16_t port;} cases[]={
        {"",0,0,0}, {" console=serial\troot=ram\r\n",0,0,0},
        {"x-dns-server=bad dns-server-other=bad",0,0,0},
        {"dns-server=10.0.2.2:53",1,0x0a000202,53},
        {"console=serial\tdns-server=1.2.3.4:1\r\nroot=ram",1,0x01020304,1},
        {"dns-server=255.255.255.255:65535",1,UINT32_MAX,UINT16_MAX},
        {"dns-server=010.000.002.002:00053",1,0x0a000202,53},
        {"dns-server",-EINVAL,0,0}, {"dns-server=",-EINVAL,0,0},
        {"dns-server=1.2.3.4",-EINVAL,0,0}, {"dns-server=1.2.3.4:",-EINVAL,0,0},
        {"dns-server=1.2.3.4:0",-EINVAL,0,0}, {"dns-server=1.2.3.4:65536",-EINVAL,0,0},
        {"dns-server=1.2.3.4:99999999999999999999999999",-EINVAL,0,0},
        {"dns-server=256.2.3.4:53",-EINVAL,0,0}, {"dns-server=1.2.3.999999999999999999999:53",-EINVAL,0,0},
        {"dns-server=1.2.3:53",-EINVAL,0,0}, {"dns-server=1.2.3.4.5:53",-EINVAL,0,0},
        {"dns-server=1..3.4:53",-EINVAL,0,0}, {"dns-server=.2.3.4:53",-EINVAL,0,0},
        {"dns-server=+1.2.3.4:53",-EINVAL,0,0}, {"dns-server=1.2.3.4:-1",-EINVAL,0,0},
        {"dns-server=1.2.3.4:+53",-EINVAL,0,0}, {"dns-server=1.2.3.4:53suffix",-EINVAL,0,0},
        {"dns-server=host.example:53",-EINVAL,0,0}, {"dns-server=https://1.2.3.4:53",-EINVAL,0,0},
        {"dns-server=1.2.3.4: 53",-EINVAL,0,0},
        {"dns-server=1.2.3.4:53 dns-server=1.2.3.4:53",-EINVAL,0,0},
        {"dns-server=1.2.3.4:53 dns-server=5.6.7.8:54",-EINVAL,0,0},
        {"dns-server=1.2.3.4:53 dns-server=bad",-EINVAL,0,0},
        {"dns-server=bad dns-server=1.2.3.4:53",-EINVAL,0,0},
    };
    for(size_t i=0;i<ARRAY_SIZE(cases);i++) {
        struct n_sockaddr result={.ip=UINT32_MAX,.port=UINT16_MAX};
        CHECK(dns_parse_boot_resolver(cases[i].command,&result)==cases[i].status);
        CHECK(result.ip==cases[i].ip && result.port==cases[i].port);
    }
    uint32_t saved_dns=my_dns;int saved_setting=dns_boot_setting;
    struct n_sockaddr saved_boot=dns_boot_resolver;
    my_dns=0x01010101;dns_boot_setting=0;
    struct n_sockaddr selected=dns_default_resolver();CHECK(selected.ip==my_dns && selected.port==53);
    dns_boot_setting=dns_parse_boot_resolver("dns-server=10.0.2.2:49152",&dns_boot_resolver);
    selected=dns_default_resolver();CHECK(selected.ip==0x0a000202 && selected.port==49152);
    my_dns=0x08080808; /* DHCP renewal must not replace the explicit endpoint. */
    selected=dns_default_resolver();CHECK(selected.ip==0x0a000202 && selected.port==49152);
    dns_boot_setting=dns_parse_boot_resolver("dns-server=invalid",&dns_boot_resolver);
    selected=dns_default_resolver();CHECK(!selected.ip && !selected.port);
    dns_boot_setting=dns_parse_boot_resolver("",&dns_boot_resolver);
    selected=dns_default_resolver();CHECK(selected.ip==my_dns && selected.port==53);
    my_dns=saved_dns;dns_boot_setting=saved_setting;dns_boot_resolver=saved_boot;
}
static size_t name_at(uint8_t *p,size_t at,const char *name) {
    size_t bytes=0;CHECK(dns_name_encode(name,strlen(name),p+at,65535-at,&bytes));return at+bytes;
}
static size_t begin(uint8_t *p,const char *name,unsigned count,unsigned rcode) {
    memset(p,0,65535);p[2]=0x81;p[3]=(uint8_t)(0x80|rcode);p[5]=1;p[6]=(uint8_t)(count>>8);p[7]=(uint8_t)count;
    size_t at=name_at(p,12,name);p[at+1]=1;p[at+3]=1;return at+4;
}
static size_t record(uint8_t *p,size_t at,const char *owner,unsigned type,const void *bytes,size_t length) {
    at=name_at(p,at,owner);p[at++]=(uint8_t)(type>>8);p[at++]=(uint8_t)type;p[at++]=0;p[at++]=1;
    p[at++]=0;p[at++]=0;p[at++]=0;p[at++]=60;p[at++]=(uint8_t)(length>>8);p[at++]=(uint8_t)length;
    memcpy(p+at,bytes,length);return at+length;
}
int main(void) {
    test_checksum();
    test_dns_boot_resolver();
    uint8_t *message=malloc(65535);char decoded[DNS_NAME_TEXT_BYTES+1];struct dns_answer answer;
    CHECK(message!=NULL);if(!message)return 1;
    uint8_t bad[]={0xc0,0};CHECK(!dns_name_decode(bad,sizeof bad,0,decoded));
    uint8_t short_pointer[]={0xc0};CHECK(!dns_name_decode(short_pointer,sizeof short_pointer,0,decoded));
    uint8_t compressed[]={3,'W','W','W',0,0xc0,0};CHECK(dns_name_decode(compressed,sizeof compressed,5,decoded)==7&&!strcmp(decoded,"www"));
    size_t at=begin(message,"multi.fixture.test",32,0);
    for(unsigned i=0;i<32;i++){uint8_t ip[]={10,0,2,(uint8_t)(i+1)};at=record(message,at,"multi.fixture.test",1,ip,4);}
    memset(&answer,0,sizeof answer);CHECK(!dns_parse_reply(message,at,"multi.fixture.test",&answer)&&answer.count==32&&answer.ttl==60);
    dns_cache_store("multi.fixture.test",0x0a000202,53,&answer);kfree(answer.ips);
    memset(&answer,0,sizeof answer);CHECK(!dns_query("MULTI.FIXTURE.TEST.",0x0a000202,53,&answer)&&answer.count==32&&answer.status==N_DNS_OK);kfree(answer.ips);
    memset(&answer,0,sizeof answer);CHECK(dns_query("multi.fixture.test",0x0a000203,53,&answer)<0&&answer.status==N_DNS_UNREACHABLE);
    uint8_t target[255],address[]={10,0,2,2},unrelated[]={222,173,190,239};size_t target_bytes=0;
    CHECK(dns_name_encode("target.fixture.test",19,target,sizeof target,&target_bytes));
    at=begin(message,"alias.fixture.test",3,0);
    at=record(message,at,"alias.fixture.test",5,target,target_bytes);
    at=record(message,at,"evil.fixture.test",1,unrelated,4);
    at=record(message,at,"target.fixture.test",1,address,4);
    memset(&answer,0,sizeof answer);CHECK(!dns_parse_reply(message,at,"alias.fixture.test",&answer)&&answer.count==1&&answer.ips[0]==0x0a000202);kfree(answer.ips);
    CHECK(dns_name_encode("loop.fixture.test",17,target,sizeof target,&target_bytes));
    at=begin(message,"loop.fixture.test",1,0);at=record(message,at,"loop.fixture.test",5,target,target_bytes);
    memset(&answer,0,sizeof answer);CHECK(dns_parse_reply(message,at,"loop.fixture.test",&answer)<0&&answer.status==N_DNS_FORMAT);
    static const struct{unsigned rcode,status;} errors[]={{3,N_DNS_NXDOMAIN},{2,N_DNS_SERVFAIL},{5,N_DNS_REFUSED},{1,N_DNS_FORMAT},{0,N_DNS_NODATA}};
    for(unsigned i=0;i<ARRAY_SIZE(errors);i++) {
        at=begin(message,"error.fixture.test",0,errors[i].rcode);memset(&answer,0,sizeof answer);
        CHECK(dns_parse_reply(message,at,"error.fixture.test",&answer)<0&&answer.status==errors[i].status);
    }
    at=begin(message,"different.fixture.test",0,3);memset(&answer,0,sizeof answer);
    CHECK(dns_parse_reply(message,at,"error.fixture.test",&answer)<0&&answer.status==N_DNS_FORMAT);
    CHECK(dns_error_status(-ENOMEM)==N_DNS_RESOURCE&&dns_error_status(-EPROTO)==N_DNS_FORMAT);
    memset(&answer,0,sizeof answer);CHECK(dns_query("multi.fixture.test..",0x0a000202,53,&answer)<0&&answer.status==N_DNS_FORMAT);
    char longname[200];
    for(unsigned i=0;i<24;i++) {
        snprintf(longname,sizeof longname,"cache-%u.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.fixture.test",i);
        at=begin(message,longname,1,0);at=record(message,at,longname,1,address,4);memset(&answer,0,sizeof answer);
        CHECK(!dns_parse_reply(message,at,longname,&answer));dns_cache_store(longname,0x0a000202,53,&answer);kfree(answer.ips);
    }
    for(unsigned i=0;i<24;i++) {
        snprintf(longname,sizeof longname,"cache-%u.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.fixture.test",i);
        memset(&answer,0,sizeof answer);CHECK(!dns_query(longname,0x0a000202,53,&answer)&&answer.count==1&&answer.ips[0]==0x0a000202);kfree(answer.ips);
    }
    timer_ticks+=60001;memset(&answer,0,sizeof answer);
    CHECK(dns_query(longname,0x0a000202,53,&answer)<0&&answer.status==N_DNS_UNREACHABLE);
    free(message);printf("dns-transport-host: %u checks, %u failed\n",checks,failed);return failed!=0;
}
