/* NVKMDの実RM境界を検査する。RPCとnativeサービスだけを置換し、GPUを模擬しない。 */
#include <stdio.h>
#include "../third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c"

struct object { uint32_t id, parent, cls; void *map; struct object *next; };
struct nvrm { uint32_t target; struct object *objects; };
static unsigned failures, connections, connects, allocs, map_calls, unmaps, local_allocs;
static unsigned fail_alloc, fail_map, event, channel_free, posted_unmap;
static uint32_t next_handle, failed_control, control_status, absent_target, mismatch_target, unready_target;
static int64_t connect_error;
static bool pci_failure, partial_alloc;
static struct n_pciinfo pci_list[] = {
   {.vendor=0x10de,.cls=3,.subcls=0,.progif=0,.bus=1,.dev=1},
   {.vendor=0x10de,.cls=3,.subcls=2,.progif=0,.bus=1,.dev=2},
   {.vendor=0x10de,.cls=3,.subcls=1,.progif=0,.bus=1,.dev=3},
   {.vendor=0x10de,.cls=3,.subcls=0,.progif=1,.bus=1,.dev=4},
   {.vendor=0x1234,.cls=3,.subcls=0,.progif=0,.bus=1,.dev=5},
};
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void mtx_destroy(mtx_t *m) { (void)m; }
void thrd_yield(void) {}

n_result pcilist(struct n_pciinfo *out, int capacity) {
   if (pci_failure) return -N_ERR_NO_MEMORY;
   unsigned count=sizeof(pci_list)/sizeof(pci_list[0]);
   if ((unsigned)capacity<count) count=capacity;
   memcpy(out,pci_list,count*sizeof(*out));
   return count;
}
int64_t nvrm_connect_device_status(uint32_t target,struct nvrm **out) {
   *out=NULL; connects++;
   if(connect_error)return connect_error;
   if(target==absent_target)return -N_ERR_NOT_FOUND;
   *out=calloc(1,sizeof(**out));
   if(!*out)return -N_ERR_NO_MEMORY;
   (*out)->target=target;connections++;return 0;
}
bool nvrm_device_ready(const struct nvrm *rm) { return rm->target != unready_target; }
static struct object *object(struct nvrm *rm,uint32_t id) {
   for(struct object *o=rm->objects;o;o=o->next)if(o->id==id)return o;
   return NULL;
}
static unsigned object_count(struct nvrm *rm) {
   unsigned count=0;for(struct object *o=rm->objects;o;o=o->next)count++;return count;
}
uint32_t nvrm_alloc(struct nvrm *rm,uint32_t root,uint32_t parent,uint32_t *id,
                    uint32_t cls,void *params,uint32_t size) {
   (void)root;(void)params;(void)size;
   bool fail=++allocs==fail_alloc;
   if(fail && !partial_alloc)return NV_ERR_NO_MEMORY;
   struct object *o=calloc(1,sizeof(*o));
   o->id=++next_handle;o->parent=parent;o->cls=cls;o->next=rm->objects;rm->objects=o;*id=o->id;
   if(cls==NV01_MEMORY_LOCAL_USER)local_allocs++;
   return fail?NV_ERR_NO_MEMORY:NV_OK;
}
/* 直接表示の画像：nvrmがクライアントの装置の下にメモリー物体として複製する。 */
static uint32_t import_status;
uint32_t nvrm_scanout_import(struct nvrm *rm,uint32_t image,uint32_t client,uint32_t parent,uint32_t *object) {
   (void)client;
   if(import_status)return import_status;
   CHECK(image==0x100);
   return nvrm_alloc(rm,client,parent,object,NV01_MEMORY_LOCAL_USER,NULL,0);
}
uint32_t nvrm_free(struct nvrm *rm,uint32_t root,uint32_t parent,uint32_t id) {
   (void)root;(void)parent;
   struct object *o=object(rm,id);
   if(!o)return NV_ERR_INVALID_OBJECT_HANDLE;
   if(o->cls==VOLTA_CHANNEL_GPFIFO_A)channel_free=++event;
   for(;;){
      struct object *child=rm->objects;
      while(child && child->parent!=id)child=child->next;
      if(!child)break;
      nvrm_free(rm,root,id,child->id);
   }
   struct object **p=&rm->objects;while(*p!=o)p=&(*p)->next;
   *p=o->next;free(o->map);free(o);return NV_OK;
}
void nvrm_disconnect(struct nvrm *rm) {
   while(rm->objects)nvrm_free(rm,0,0,rm->objects->id);
   connections--;free(rm);
}
uint32_t nvrm_control(struct nvrm *rm,uint32_t client,uint32_t id,uint32_t cmd,void *params,uint32_t bytes) {
   (void)client;(void)id;(void)bytes;
   if(cmd==failed_control)return control_status;
   if(cmd==NV0000_CTRL_CMD_GPU_GET_PROBED_IDS){
      NV0000_CTRL_GPU_GET_PROBED_IDS_PARAMS *p=params;
      for(unsigned i=0;i<NV0000_CTRL_GPU_MAX_PROBED_GPUS;i++)
         CHECK(p->excludedGpuIds[i]==0 && p->gpuFlags[i]==0);
      p->gpuIds[0]=rm->target+2; /* 先頭IDは別BDF。index=0固定では誤選択する。 */
      p->gpuIds[1]=rm->target+1;
      p->gpuIds[2]=NV0000_CTRL_GPU_INVALID_ID;
   }else if(cmd==NV0000_CTRL_CMD_GPU_GET_PCI_INFO){
      NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS *p=params;
      p->domain=0;p->bus=rm->target>>8;
      p->slot=((rm->target>>3)&0x1f)+((p->gpuId!=rm->target+1 || rm->target==mismatch_target)?1:0);
   }
   return NV_OK;
}
uint32_t nvrm_map(struct nvrm *rm,uint32_t client,uint32_t dev,uint32_t id,uint64_t offset,
                  uint64_t length,uint32_t flags,void **out) {
   (void)client;(void)dev;(void)offset;(void)flags;
   if(++map_calls==fail_map)return NV_ERR_NO_MEMORY;
   struct object *o=object(rm,id);CHECK(o!=NULL);
   o->map=calloc(1,length);*out=o->map;return *out?NV_OK:NV_ERR_NO_MEMORY;
}
uint32_t nvrm_unmap(struct nvrm *rm,uint32_t client,uint32_t dev,uint32_t id,void *map,uint32_t flags) {
   (void)client;(void)dev;(void)flags;
   struct object *o=object(rm,id);CHECK(o && o->map==map);
   if(o->cls==NV01_MEMORY_LOCAL_USER)posted_unmap=++event;
   free(map);o->map=NULL;unmaps++;return NV_OK;
}
uint32_t nvrm_escape(struct nvrm *rm,uint32_t escape,void *p,uint32_t bytes) {
   (void)rm;(void)bytes;
   if(escape==NV_ESC_RM_MAP_MEMORY_DMA)((NVOS46_PARAMETERS *)p)->status=NV_OK;
   if(escape==NV_ESC_RM_UNMAP_MEMORY_DMA)((NVOS47_PARAMETERS *)p)->status=NV_OK;
   return NV_OK;
}
bool nvrm_alive(struct nvrm *rm) { (void)rm;return true; }

static void reset(void) {
   CHECK(connections==0);
   connects=allocs=map_calls=unmaps=local_allocs=0;
   fail_alloc=fail_map=event=channel_free=posted_unmap=0;
   failed_control=0;control_status=NV_ERR_INVALID_STATE;
   absent_target=mismatch_target=unready_target=N_PCI_NONE;connect_error=0;pci_failure=partial_alloc=false;
}
static void enumeration(void) {
   uint32_t *ids=NULL,count=0;
   reset();
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_OK && count==2 && connects==2 && connections==0);
   CHECK(ids[0]==N_PCI_TARGET(1,1,0) && ids[1]==N_PCI_TARGET(1,2,0));
   uint32_t preserved=ids[1];
   absent_target=ids[0];
   struct nnrm *missing=NULL;
   CHECK(nnrm_open_device(ids[0],&missing,NULL,NULL)==NV_ERR_OBJECT_NOT_FOUND && missing==NULL && connections==0);
   uint32_t *later=NULL,later_count=0;
   CHECK(nnrm_enumerate_gpus(&later,&later_count)==NV_OK && later_count==1 && later[0]==preserved);
   CHECK(ids[1]==preserved);free(later);free(ids);
   mismatch_target=preserved;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_OK && count==0);free(ids);
   reset();unready_target=N_PCI_TARGET(1,1,0);
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_OK && count==1 && ids[0]==preserved && allocs==1 && connections==0);
   free(ids);
   struct nnrm *unready=NULL;const char *step=NULL;
   CHECK(nnrm_open_device(unready_target,&unready,NULL,&step)==NV_ERR_OBJECT_NOT_FOUND && step && !strcmp(step,"the connection to nvrm") && unready==NULL && connections==0);
   reset();failed_control=NV0000_CTRL_CMD_GPU_GET_PCI_INFO;control_status=NV_ERR_NO_MEMORY;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_NO_MEMORY && ids==NULL && count==0 && connections==0);
   reset();failed_control=NV0000_CTRL_CMD_GPU_GET_PROBED_IDS;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_INVALID_STATE && connections==0);
   reset();connect_error=-N_ERR_PROTOCOL;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_INVALID_STATE && connections==0);
   reset();connect_error=-N_ERR_NO_MEMORY;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_NO_MEMORY && !nnrm_gpu_unavailable(NV_ERR_NO_MEMORY) && ids==NULL && count==0);
   reset();connect_error=-N_ERR_CLOSED;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_GPU_IS_LOST && !nnrm_gpu_unavailable(NV_ERR_GPU_IS_LOST));
   reset();connect_error=-N_ERR_NOT_READY;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_OK && count==0 && connections==0);free(ids);
   reset();fail_alloc=1;partial_alloc=true;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_NO_MEMORY && connections==0 && ids==NULL && count==0);
   reset();pci_failure=true;
   CHECK(nnrm_enumerate_gpus(&ids,&count)==NV_ERR_NO_MEMORY && connects==0);
}
static struct nnrm *space(void) {
   struct nnrm *n=calloc(1,sizeof(*n));
   CHECK(nvrm_connect_device_status(N_PCI_TARGET(1,1,0),&n->rm)==0);
   n->refs=1;n->info.cls_gpfifo=VOLTA_CHANNEL_GPFIFO_A;
   mtx_init(&n->usermode_lock,mtx_plain);
   atomic_init(&n->internal_va,NNRM_INTERNAL_VA);atomic_init(&n->daemon_lost,false);
   CHECK(nvrm_alloc(n->rm,0,0,&n->client,NV01_ROOT_CLIENT,NULL,0)==NV_OK);
   CHECK(nvrm_alloc(n->rm,n->client,n->client,&n->device,NV01_DEVICE_0,NULL,0)==NV_OK);
   return n;
}
static void lifetime(void) {
   reset();struct nnrm *n=space();struct nnrm_chan *a=NULL,*b=NULL;
   unsigned before=allocs;
   CHECK(nnrm_chan_create(n,0,&a)==NV_OK);unsigned successful_allocs=allocs-before,successful_maps=map_calls;
   CHECK(nnrm_chan_create(n,0,&b)==NV_OK && local_allocs==1 && n->refs==3);
   void *map=n->posted_map;CHECK(map!=NULL);
   nnrm_close(n);CHECK(connections==1 && n->refs==2);
   nnrm_chan_destroy(a);CHECK(connections==1 && n->refs==1 && n->posted_map==map && posted_unmap==0);
   struct nnrm_gp gp={.addr=NNRM_VA_START,.size_B=sizeof(uint32_t)};
   CHECK(nnrm_chan_submit(b,&gp,1)==NV_OK);
   nnrm_chan_destroy(b);CHECK(connections==0 && posted_unmap>channel_free);
   for(unsigned failure=1;failure<=successful_allocs;failure++){
      reset();n=space();fail_alloc=allocs+failure;
      CHECK(nnrm_chan_create(n,0,&a)!=NV_OK && n->refs==1);
      nnrm_close(n);CHECK(connections==0);
   }
   for(unsigned failure=1;failure<=successful_maps;failure++){
      reset();n=space();fail_map=failure;
      CHECK(nnrm_chan_create(n,0,&a)!=NV_OK && n->refs==1);
      nnrm_close(n);CHECK(connections==0);
   }
   /* 共有対象が既に生きている場合も、別channelの途中失敗で解放しない。 */
   reset();n=space();CHECK(nnrm_chan_create(n,0,&a)==NV_OK);
   before=allocs;unsigned before_maps=map_calls;
   CHECK(nnrm_chan_create(n,0,&b)==NV_OK);
   successful_allocs=allocs-before;successful_maps=map_calls-before_maps;
   nnrm_chan_destroy(b);nnrm_chan_destroy(a);nnrm_close(n);
   for(unsigned partial=0;partial<=1;partial++){
      for(unsigned failure=1;failure<=successful_allocs+successful_maps;failure++){
         reset();n=space();CHECK(nnrm_chan_create(n,0,&a)==NV_OK);
         unsigned live=object_count(n->rm);map=n->posted_map;partial_alloc=partial;
         if(failure<=successful_allocs)fail_alloc=allocs+failure;
         else fail_map=map_calls+failure-successful_allocs;
         CHECK(nnrm_chan_create(n,0,&b)!=NV_OK && n->refs==2);
         CHECK(object_count(n->rm)==live && n->posted_map==map && posted_unmap==0 && local_allocs==1);
         CHECK(nnrm_chan_submit(a,&gp,1)==NV_OK);
         nnrm_close(n);nnrm_chan_destroy(a);
         CHECK(connections==0 && posted_unmap>channel_free);
      }
   }
   reset();n=space();unsigned live=object_count(n->rm);
   fail_alloc=allocs+1;partial_alloc=true;
   CHECK(posted_map(n)==NV_ERR_NO_MEMORY && object_count(n->rm)==live && n->posted_map==NULL);
   nnrm_close(n);CHECK(connections==0);
}
static void scanout_import(void) {
   reset();struct nnrm *n=space();unsigned live=object_count(n->rm);struct nnrm_mem *mem=NULL;
   CHECK(nnrm_mem_import_scanout(n,0x100,1<<20,&mem)==NV_OK && mem && mem->kind==NNRM_MEM_VRAM && mem->size_B==1<<20);
   CHECK(mem && object(n->rm,mem->handle) && object(n->rm,mem->handle)->parent==n->device && object_count(n->rm)==live+1);
   nnrm_mem_free(mem);CHECK(object_count(n->rm)==live);
   /* 系が取込みを拒んだ：状態をそのまま返し、何も残さない。 */
   import_status=NV_ERR_INVALID_OBJECT_HANDLE;mem=NULL;
   CHECK(nnrm_mem_import_scanout(n,0x100,1<<20,&mem)==NV_ERR_INVALID_OBJECT_HANDLE && mem==NULL && object_count(n->rm)==live);
   import_status=NV_OK;
   nnrm_close(n);CHECK(connections==0);
}
int main(void) {
   enumeration();lifetime();scanout_import();
   printf("NVK RM境界ホスト試験: %u件失敗（実GPU・実並列threadは未検証）\n",failures);
   return failures!=0;
}
