/* Direct product backpressure predicates only; not kernel/decoder execution. */
#include <stddef.h>
size_t strlcpy(char *,const char *,size_t); /* native declaration; unreachable here */
#ifdef _WIN32
#include <corecrt.h>
#define ssize_t nocturne_host_ssize_t
#endif
static int nmsw_native_child_main(void); /* unused native entry is not exported */
#define main nmsw_native_child_main
#include "../user/apps/browsermseworker.c"
#undef main
#ifdef _WIN32
#undef ssize_t
#endif
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("共有窓失敗 行 %d\n",__LINE__);}}while(0)
int main(void){
    struct nmsw_shared_video a={0},b={0},c={0};
    struct video_storage current={.shared=&a},old={.shared=&b},older={.shared=&c};
    struct video_tx tx={.storage=&current,.retired=&old};old.next=&older;
    CHECK(!video_full(&tx));CHECK(!video_storage_busy(&current));
    a.occupied[0]=1;CHECK(!video_full(&tx));CHECK(video_storage_busy(&current));
    b.occupied[0]=1;CHECK(video_full(&tx)); /* resized frame plus old loan */
    a.occupied[0]=0;CHECK(!video_full(&tx)); /* receiver returns current */
    c.occupied[1]=1;CHECK(video_full(&tx)); /* no current but two old loans */
    b.occupied[0]=0;CHECK(!video_full(&tx));
    a.occupied[1]=1;CHECK(video_full(&tx));
    c.occupied[1]=0;CHECK(!video_full(&tx));
    a.occupied[0]=1;CHECK(video_full(&tx)); /* ordinary double buffer */
    a.occupied[0]=a.occupied[1]=0;tx.pending=true;CHECK(video_full(&tx));
    tx.pending=false;CHECK(!video_full(&tx));
    printf("共有窓 %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
}
