/* Production child body collector and negotiated policy, no guest/site claim. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define ssize_t nocturne_test_ssize_t
#define main static webfetch_child_entry
#define uptime_ms body_test_uptime
#include "../user/apps/webfetch.c"
#undef main
uint64_t body_test_uptime(void){return 100;}
static unsigned checks,failed;
#define CHECK(c) do {checks++;if(!(c)){failed++;printf("FAIL child body:%d %s\n",__LINE__,#c);}}while(0)
static void collector(unsigned kind,unsigned flags,size_t limit) {
    struct job *j=calloc(1,sizeof *j);char *block=malloc(1024*1024);CHECK(j&&block);if(!j||!block){free(j);free(block);return;}
    memset(block,'x',1024*1024);j->wire.kind=kind;j->wire.user_navigation=flags;j->wire.deadline=1000;
    for(size_t at=0;at<limit;at+=1024*1024)CHECK(body_cb(j,block,1024*1024)==0);
    CHECK(j->body_len==limit&&j->body_cap==limit+1&&j->body[limit]==0);
    CHECK(body_cb(j,"x",1)<0&&j->body_len==limit&&j->error[0]);free(j->body);free(j);free(block);
}
int main(void) {
    collector(WEBNET_CLASSIC,WEBNET_WIRE_LARGE_SCRIPT,WEBNET_SCRIPT_BODY_LIMIT);
    collector(WEBNET_MODULE,WEBNET_WIRE_LARGE_SCRIPT,WEBNET_SCRIPT_BODY_LIMIT);
    collector(WEBNET_MODULE,0,WEBNET_BODY_LIMIT); /* persisted old client */
    collector(WEBNET_RESOURCE,0,WEBNET_BODY_LIMIT);collector(WEBNET_FETCH,0,WEBNET_BODY_LIMIT);
    CHECK(!webnet_wire_script_flag_valid(WEBNET_NAVIGATION,WEBNET_WIRE_LARGE_SCRIPT));
    CHECK(!webnet_wire_script_flag_valid(WEBNET_FETCH,WEBNET_WIRE_LARGE_SCRIPT));
    CHECK(!webnet_wire_script_flag_valid(WEBNET_RESOURCE,WEBNET_WIRE_LARGE_SCRIPT));
    printf("webfetch_body_limit_hosttest: %u checks, %u failed\n",checks,failed);return failed!=0;
}
