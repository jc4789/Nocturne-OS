/* NVKMDの実RM境界を検査する。RPCとnativeサービスだけを置換し、GPUを模擬しない。
   偽のRMは、GSPファームウェアつきの実機のRM（CPU側はGSPクライアント）が引数に課す規則を持つ（rm_rules）。 */
#include <stdio.h>
#include "../third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c"
#include "class/cl902d.h"
#include "class/cla140.h"
#include "class/clc639.h"
#include "class/clc640.h"
#include "class/clc7b5.h"
#include "class/clc997.h"
#include "class/clc9c0.h"

/* 物体が持つGPUの番地の範囲 [va_lo, va_hi)：NVKの固定の仮想メモリー、またはRMが自分で選んだ写像 */
struct object { uint32_t id, parent, cls; void *map; uint64_t va_lo, va_hi; struct object *next; };
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

/* libnvrmの引数の領域：RMが引数の中のポインターを通して書く一覧はここに置かれなければならない */
static uint8_t arena[4096];
static size_t arena_used;
static unsigned arena_depth;
void nvrm_lock(struct nvrm *rm) { (void)rm; arena_depth++; }
void nvrm_unlock(struct nvrm *rm) { (void)rm; if (!--arena_depth) arena_used = 0; }
void *nvrm_scratch(struct nvrm *rm, size_t bytes) {
   (void)rm; CHECK(arena_depth > 0);
   size_t at = (arena_used + 15) & ~(size_t)15;
   if (at + bytes > sizeof(arena)) return NULL;
   arena_used = at + bytes; return arena + at;
}
static bool in_arena(const void *p, size_t bytes) {
   return arena_depth > 0 && (const uint8_t *)p >= arena && (const uint8_t *)p + bytes <= arena + arena_used;
}

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
/* ---- GSPクライアントのRMのVA空間（装置の既定のもの） ----
   [4 GiB, 4.5 GiB)はGSP-RMの範囲で、CPU側のRMが予約する（gpu_vaspace.cのgvaspaceReserveSplitVaSpace、
   SPLIT_VAS_SERVER_RM_MANAGED_VA_*）。チャネルの文脈バッファーはCPU側のRMが自分で写し、4 GiBから上の最初の空きを取る
   （dmaAllocMapping_GM107、bDmaEnforce32BitPointer）。固定の番地の確保が重なればNV_ERR_NO_MEMORY、
   空間の下端（1 MiB）より下はNV_ERR_INVALID_ARGUMENT。 */
#define GSP_VA_LO (1ull<<32)
#define GSP_VA_HI (GSP_VA_LO+(512ull<<20))
#define RM_CTX_BUFFER (2ull<<20)
static uint64_t rm_ctx_lo=~0ull,rm_ctx_hi;
static bool va_taken(struct nvrm *rm,uint64_t lo,uint64_t hi) {
   if(lo<GSP_VA_HI && hi>GSP_VA_LO)return true;
   for(struct object *o=rm->objects;o;o=o->next)
      if(o->va_hi && lo<o->va_hi && hi>o->va_lo)return true;
   return false;
}
/* エンジンごとのクラス（GET_ENGINE_CLASSLIST）。装置の一覧（CLASSLIST_V2）はこれに加えて、エンジンを持たない
   MIGのクラスも含み、AMPERE_SMC_MONITOR_SESSION（0xc640）の下位バイトはM2MFの0x40と重なる */
static const uint32_t gr_classes[]={ADA_A,ADA_COMPUTE_A,FERMI_TWOD_A,KEPLER_INLINE_TO_MEMORY_B};
static const uint32_t ce_classes[]={AMPERE_DMA_COPY_B};
static bool listed(const uint32_t *list,unsigned count,uint32_t cls) {
   for(unsigned i=0;i<count;i++)if(list[i]==cls)return true;
   return false;
}
static uint32_t rm_rules(struct nvrm *rm,struct object *o,uint32_t parent,uint32_t cls,const void *params) {
   struct object *owner=object(rm,parent);
   if(owner && owner->cls==VOLTA_CHANNEL_GPFIFO_A){
      /* チャネルの下に作れるのはエンジンのクラスだけ。コピーの物体はコピーエンジンに置く */
      if(listed(ce_classes,1,cls)){
         const NVB0B5_ALLOCATION_PARAMETERS *p=params;
         if(!p || !NV2080_ENGINE_TYPE_IS_COPY(p->engineType))return NV_ERR_INVALID_ARGUMENT;
      }else if(!listed(gr_classes,sizeof(gr_classes)/sizeof(gr_classes[0]),cls))return NV_ERR_INVALID_CLASS;
   }
   if(cls==NV01_DEVICE_0 && params){
      /* GSPクライアントは装置をGSPにも作らせ、旗は送るが内部のVA範囲は送らない（rpc.hの
         NV_RM_RPC_ALLOC_SHARE_DEVICE_FWCLIENT）。GSPのdeviceConstruct_IMPLは旗があって範囲が0なら断る（実機9回目） */
      const NV0080_ALLOC_PARAMETERS *p=params;
      if(p->flags & NV_DEVICE_ALLOCATION_FLAGS_RESTRICT_RESERVED_VALIMITS)return NV_ERR_INVALID_ARGUMENT;
   }else if(cls==NV50_MEMORY_VIRTUAL){
      const NV_MEMORY_ALLOCATION_PARAMS *p=params;
      CHECK(p && (p->flags & NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE) && p->hVASpace==0);
      if(p->offset<(1ull<<20))return NV_ERR_INVALID_ARGUMENT;
      if(va_taken(rm,p->offset,p->offset+p->size))return NV_ERR_NO_MEMORY;
      o->va_lo=p->offset;o->va_hi=p->offset+p->size;
   }else if(cls==ADA_A || cls==ADA_COMPUTE_A){
      /* CPU側のRMが文脈バッファーを写す */
      uint64_t at=GSP_VA_LO;
      while(va_taken(rm,at,at+RM_CTX_BUFFER))at+=RM_CTX_BUFFER;
      o->va_lo=at;o->va_hi=at+RM_CTX_BUFFER;
      if(at<rm_ctx_lo)rm_ctx_lo=at;
      if(at+RM_CTX_BUFFER>rm_ctx_hi)rm_ctx_hi=at+RM_CTX_BUFFER;
   }
   return NV_OK;
}
uint32_t nvrm_alloc(struct nvrm *rm,uint32_t root,uint32_t parent,uint32_t *id,
                    uint32_t cls,void *params,uint32_t size) {
   (void)root;(void)size;
   bool fail=++allocs==fail_alloc;
   if(fail && !partial_alloc)return NV_ERR_NO_MEMORY;
   struct object *o=calloc(1,sizeof(*o));
   uint32_t st=rm_rules(rm,o,parent,cls,params);
   if(st!=NV_OK){free(o);return st;}
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
   }else if(cmd==NV2080_CTRL_CMD_MC_GET_ARCH_INFO){
      NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS *p=params;
      p->architecture=NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_AD100;
      p->implementation=NV2080_CTRL_MC_ARCH_INFO_IMPLEMENTATION_AD104;
   }else if(cmd==NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2){
      static const uint32_t ada[]={ADA_A,ADA_COMPUTE_A,AMPERE_DMA_COPY_B,FERMI_TWOD_A,
                                   KEPLER_INLINE_TO_MEMORY_B,VOLTA_CHANNEL_GPFIFO_A,VOLTA_USERMODE_A,
                                   AMPERE_SMC_CONFIG_SESSION,AMPERE_SMC_MONITOR_SESSION};
      NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS *p=params;
      p->numClasses=sizeof(ada)/sizeof(ada[0]);memcpy(p->classList,ada,sizeof(ada));
   }else if(cmd==NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST){
      /* gpuGetClassListの約束：一覧が足りなければNV_ERR_INVALID_PARAM_STRUCT。一覧は呼ぶ側の引数の領域にある */
      NV2080_CTRL_GPU_GET_ENGINE_CLASSLIST_PARAMS *p=params;
      const uint32_t *list;unsigned count;
      if(p->engineType==NV2080_ENGINE_TYPE_GRAPHICS){list=gr_classes;count=sizeof(gr_classes)/sizeof(gr_classes[0]);}
      else if(NV2080_ENGINE_TYPE_IS_COPY(p->engineType)){list=ce_classes;count=1;}
      else return NV_ERR_INVALID_ARGUMENT;
      uint32_t *out=NvP64_VALUE(p->classList);
      CHECK(out==NULL || in_arena(out,p->numClasses*sizeof(uint32_t)));
      if(out){
         if(p->numClasses<count)return NV_ERR_INVALID_PARAM_STRUCT;
         memcpy(out,list,count*sizeof(uint32_t));
      }
      p->numClasses=count;
   }else if(cmd==NV2080_CTRL_CMD_GPU_GET_ENGINES_V2){
      NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS *p=params;
      p->engineCount=4;p->engineList[0]=NV2080_ENGINE_TYPE_GRAPHICS;
      for(unsigned i=0;i<3;i++)p->engineList[1+i]=NV2080_ENGINE_TYPE_COPY(i);
   }else if(cmd==NV2080_CTRL_CMD_CE_GET_ALL_CAPS){
      /* CE0がグラフィックスの相方（GRCE）、CE1とCE2が非同期 */
      NV2080_CTRL_CE_GET_ALL_CAPS_PARAMS *p=params;
      p->present=0x7;p->capsTbl[0][(1?NV2080_CTRL_CE_CAPS_CE_GRCE)]|=(0?NV2080_CTRL_CE_CAPS_CE_GRCE);
   }else if(cmd==NV2080_CTRL_CMD_GPU_GET_ENGINE_PARTNERLIST){
      NV2080_CTRL_GPU_GET_ENGINE_PARTNERLIST_PARAMS *p=params;
      CHECK(p->engineType==NV2080_ENGINE_TYPE_GRAPHICS);
      p->numPartners=1;p->partnerList[0]=NV2080_ENGINE_TYPE_COPY(0);
   }else if(cmd==NV2080_CTRL_CMD_GR_GET_GPC_MASK){
      ((NV2080_CTRL_GR_GET_GPC_MASK_PARAMS *)params)->gpcMask=0x1f;
   }else if(cmd==NV2080_CTRL_CMD_GR_GET_TPC_MASK){
      ((NV2080_CTRL_GR_GET_TPC_MASK_PARAMS *)params)->tpcMask=0xf;
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
   CHECK(nnrm_chan_create(n,0,&a,NULL)==NV_OK);unsigned successful_allocs=allocs-before,successful_maps=map_calls;
   CHECK(nnrm_chan_create(n,0,&b,NULL)==NV_OK && local_allocs==1 && n->refs==3);
   void *map=n->posted_map;CHECK(map!=NULL);
   nnrm_close(n);CHECK(connections==1 && n->refs==2);
   nnrm_chan_destroy(a);CHECK(connections==1 && n->refs==1 && n->posted_map==map && posted_unmap==0);
   struct nnrm_gp gp={.addr=NNRM_VA_START,.size_B=sizeof(uint32_t)};
   CHECK(nnrm_chan_submit(b,&gp,1)==NV_OK);
   nnrm_chan_destroy(b);CHECK(connections==0 && posted_unmap>channel_free);
   for(unsigned failure=1;failure<=successful_allocs;failure++){
      reset();n=space();fail_alloc=allocs+failure;const char *step=NULL;
      CHECK(nnrm_chan_create(n,0,&a,&step)!=NV_OK && step && n->refs==1);
      nnrm_close(n);CHECK(connections==0);
   }
   for(unsigned failure=1;failure<=successful_maps;failure++){
      reset();n=space();fail_map=failure;const char *step=NULL;
      CHECK(nnrm_chan_create(n,0,&a,&step)!=NV_OK && step && n->refs==1);
      nnrm_close(n);CHECK(connections==0);
   }
   /* 共有対象が既に生きている場合も、別channelの途中失敗で解放しない。 */
   reset();n=space();CHECK(nnrm_chan_create(n,0,&a,NULL)==NV_OK);
   before=allocs;unsigned before_maps=map_calls;
   CHECK(nnrm_chan_create(n,0,&b,NULL)==NV_OK);
   successful_allocs=allocs-before;successful_maps=map_calls-before_maps;
   nnrm_chan_destroy(b);nnrm_chan_destroy(a);nnrm_close(n);
   for(unsigned partial=0;partial<=1;partial++){
      for(unsigned failure=1;failure<=successful_allocs+successful_maps;failure++){
         reset();n=space();CHECK(nnrm_chan_create(n,0,&a,NULL)==NV_OK);
         unsigned live=object_count(n->rm);map=n->posted_map;partial_alloc=partial;
         if(failure<=successful_allocs)fail_alloc=allocs+failure;
         else fail_map=map_calls+failure-successful_allocs;
         const char *step=NULL;
         CHECK(nnrm_chan_create(n,0,&b,&step)!=NV_OK && step && n->refs==2);
         CHECK(object_count(n->rm)==live && n->posted_map==map && posted_unmap==0 && local_allocs==1);
         CHECK(nnrm_chan_submit(a,&gp,1)==NV_OK);
         nnrm_close(n);nnrm_chan_destroy(a);
         CHECK(connections==0 && posted_unmap>channel_free);
      }
   }
   /* 失敗した段階の名前：RMの制御はその名前、作れない種類はその理由。 */
   reset();n=space();failed_control=NVA06F_CTRL_CMD_GPFIFO_SCHEDULE;const char *step=NULL;
   CHECK(nnrm_chan_create(n,0,&a,&step)==NV_ERR_INVALID_STATE && step && !strcmp(step,"NVA06F_CTRL_CMD_GPFIFO_SCHEDULE") && n->refs==1);
   failed_control=0;step=NULL;
   CHECK(nnrm_chan_create(n,NNRM_CHAN_VDEC,&a,&step)==NV_ERR_NOT_SUPPORTED && step && strstr(step,"video decode"));
   step=NULL;n->copy_engine_type=NV2080_ENGINE_TYPE_NULL;
   CHECK(nnrm_chan_create(n,NNRM_CHAN_COPY,&a,&step)==NV_ERR_NOT_SUPPORTED && step && strstr(step,"copy engine") && n->refs==1);
   nnrm_close(n);CHECK(connections==0);
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
/* 装置を開く全段階をGSPクライアントのRMの規則で通す（実機9回目はNV01_DEVICE_0で断られた）。
   NVKの番地はGSP-RMの範囲にも、CPU側のRMが自分で写す文脈バッファーにも出会わない。 */
static void address_space(void) {
   reset();struct nnrm *n=NULL;struct nnrm_gpu_info info;const char *step=NULL;
   CHECK(nnrm_open_device(N_PCI_TARGET(1,1,0),&n,&info,&step)==NV_OK && n && step==NULL);
   if(!n){printf("装置を開けない：%s\n",step?step:"?");return;}
   CHECK(info.chipset==0x194 && info.cls_eng3d==ADA_A && info.gpc_count==5 && info.tpc_count==20);
   /* 各エンジンのクラスはそのエンジンの一覧から（装置の一覧ではM2MFに0xc640を拾っていた） */
   CHECK(info.cls_m2mf==KEPLER_INLINE_TO_MEMORY_B && info.cls_eng2d==FERMI_TWOD_A &&
         info.cls_compute==ADA_COMPUTE_A && info.cls_copy==AMPERE_DMA_COPY_B &&
         info.cls_gpfifo==VOLTA_CHANNEL_GPFIFO_A);
   CHECK(n->copy_engine_type==NV2080_ENGINE_TYPE_COPY(1) && n->grce_engine_type==NV2080_ENGINE_TYPE_COPY(0));
   CHECK(arena_depth==0);
   struct nnrm_chan *c=NULL,*copy=NULL;
   CHECK(nnrm_chan_create(n,NNRM_CHAN_3D|NNRM_CHAN_COMPUTE|NNRM_CHAN_COPY|NNRM_CHAN_M2MF|NNRM_CHAN_2D,
                          &c,&step)==NV_OK);
   CHECK(nnrm_chan_create(n,NNRM_CHAN_COPY,&copy,&step)==NV_OK);
   nnrm_chan_destroy(copy);
   CHECK(rm_ctx_lo>=GSP_VA_HI && rm_ctx_hi<=NNRM_VA_START);
   struct nnrm_virt *lo=NULL,*hi=NULL,*gsp=NULL;
   CHECK(nnrm_virt_reserve(n,NNRM_VA_START,1<<16,false,&lo)==NV_OK);
   CHECK(nnrm_virt_reserve(n,NNRM_VA_END-(1<<16),1<<16,true,&hi)==NV_OK);
   /* 偽物の規則が効いていること：GSP-RMの範囲は断られる */
   CHECK(nnrm_virt_reserve(n,GSP_VA_LO,1<<16,false,&gsp)==NV_ERR_NO_MEMORY && gsp==NULL);
   nnrm_virt_release(lo);nnrm_virt_release(hi);
   nnrm_chan_destroy(c);nnrm_close(n);CHECK(connections==0);
}
int main(void) {
   enumeration();lifetime();scanout_import();address_space();
   printf("NVK RM境界ホスト試験: %u件失敗（実GPU・実並列threadは未検証）\n",failures);
   return failures!=0;
}
