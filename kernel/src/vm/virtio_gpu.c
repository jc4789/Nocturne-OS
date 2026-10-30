/* Modern PCI virtio-gpu + a deliberately finite virgl rendering pipeline.
 * No raw command stream or shader supplied by users is submitted to the host.
 * PCI/queue layouts and GPU control packets: OASIS VirtIO 1.2 specification
 * (virtio-gpu device section); virgl command stream: the virgl protocol as documented.
 * The existing Limine display and Hyper-V/RDP remain independent fallbacks.
 * It is the only provider of dev/gpu.h, and only in a VM: on real hardware gpu_init looks for
 * no device and the interface reports the CPU display. */
#include "kernel.h"
#include "dev/gpu.h"
#include "vm/vm.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/sched.h"

#define SIDE target_width
#define QSIZE 8
#define VERSION_1 (1u << 0) /* high feature dword, bit 32 overall */
#define VIRGL_FEATURE 1u
#define CMD_CTX_CREATE 0x200
#define CMD_CTX_ATTACH 0x202
#define CMD_CREATE_3D 0x204
#define CMD_SUBMIT 0x207
#define CMD_ATTACH 0x106
#define CMD_CAPSET_INFO 0x108
#define CMD_CAPSET 0x109
#define OK_NODATA 0x1100
#define FENCE 1u
#define FORMAT_BGRA 1u
#define CTX_ID 1u
#define TARGET_ID target_id
#define VERTEX_ID 2u
#define SOURCE_ID source_id
#define VERTEX_STRIDE (8u*sizeof(uint32_t))
#define VERTEX_TRIANGLES (PAGE_SIZE/(3u*VERTEX_STRIDE))
#define VERTEX_BYTES (VERTEX_TRIANGLES*3u*VERTEX_STRIDE)
#define CMD(c,o,n) ((uint32_t)(c) | ((uint32_t)(o) << 8) | ((uint32_t)(n) << 16))
_Static_assert(VERTEX_BYTES <= PAGE_SIZE, "bounded vertex DMA page");
_Static_assert(sizeof(struct n_gpu_batch) == 1936, "fixed batch ABI");
_Static_assert(sizeof(struct n_gpu_blit) == 32, "fixed texture blit ABI");

/* Device registers, each touched at its own width (arch/cpu.h mmio_*); the 64-bit ones as two
   32-bit halves, low first, which virtio 1.x allows for them. Every field sits at its natural
   alignment. */
struct virtio_common {
    volatile uint32_t device_feature_select, device_feature;
    volatile uint32_t driver_feature_select, driver_feature;
    volatile uint16_t config_msix_vector, num_queues;
    volatile uint8_t device_status, config_generation;
    volatile uint16_t queue_select, queue_size, queue_msix_vector;
    volatile uint16_t queue_enable, queue_notify_off;
    volatile uint32_t queue_desc[2], queue_driver[2], queue_device[2];
};
_Static_assert(sizeof(struct virtio_common) == 56, "virtio common configuration layout");
struct descriptor { uint64_t addr; uint32_t len; uint16_t flags, next; } PACKED;
struct avail { volatile uint16_t flags, idx; uint16_t ring[QSIZE]; } PACKED;
struct used_entry { volatile uint32_t id, len; } PACKED;
struct used { volatile uint16_t flags, idx; struct used_entry ring[QSIZE]; } PACKED;
struct header { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; } PACKED;

static struct virtio_common *common;
static volatile uint16_t *notify;
static struct descriptor *desc;
static struct avail *avail;
static struct used *used;
static uint64_t packets_phys, target_phys, vertex_phys, source_phys;
static uint8_t *packets;
static uint32_t *target, *vertices, *source_pixels;
static uint32_t target_width,target_height,source_width,source_height;
/* Two names per replaceable resource let us create-before-retire and then
   reuse the retired name. A long-lived app never exhausts an ID counter. */
static uint32_t target_id=1,source_id=3;
static uint16_t seen_used;
static uint32_t reply_bytes, capset_count;
static uint64_t next_fence = 1;
static bool attempted, ready, broken, validating_runtime;
static uint32_t busy;
static struct wait_queue completion_wait;
static uint64_t completions; /* moved on by gpu_isr (CPU 0) before it wakes completion_wait */
static bool completion_irq;
static struct n_gpu_info statistics;
static bool gpu_checkpoint(void) {
    if(current_task->kstack && need_resched)schedule();
    return !current_task->kstack || !current_task->killed;
}
static bool gpu_copy(void *destination,const void *source,size_t bytes) {
    for(size_t at=0;at<bytes;){
        size_t piece=MIN(bytes-at,(size_t)PAGE_SIZE);
        memcpy((uint8_t *)destination+at,(const uint8_t *)source+at,piece);at+=piece;
        if(!gpu_checkpoint())return false;
    }
    return true;
}

static void gpu_isr(struct regs *r) {
    (void)r;
    __atomic_fetch_add(&completions, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&completion_wait);
}
/* Normal requests sleep as native continuations and let the GUI run; the one
 * holding busy (any CPU) sleeps until deadline (clock ns) unless an interrupt
 * came since it looked at the used ring. Without MSI-X, and on the boot task
 * (the future idle task, which cannot block on a wait queue), the ring is read
 * again every COMPLETION_POLL_NS instead of monopolising the CPU. */
#define COMPLETION_POLL_NS NS_PER_MS
static void completion_nap(uint64_t deadline) {
    uint64_t seen=__atomic_load_n(&completions,__ATOMIC_SEQ_CST);
    if(used->idx!=seen_used)return;
    if(completion_irq && current_task->kstack)wq_wait_unless(&completion_wait,&completions,seen,deadline);
    else sleep_until(MIN(deadline,clock_after(COMPLETION_POLL_NS)));
}

static inline void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static uint32_t config32(struct pci_device *d, unsigned off) {
    return pci_read32(d->bus, d->dev, d->func, (uint8_t)off);
}
static uint8_t config8(struct pci_device *d, unsigned off) {
    return (uint8_t)(config32(d, off & ~3u) >> ((off & 3u) * 8));
}
static void *cap_map(struct pci_device *d, unsigned cap, uint32_t *length) {
    unsigned bar = config8(d, cap + 4);
    uint64_t base = pci_bar_address(d, bar);
    uint32_t off = config32(d, cap + 8), len = config32(d, cap + 12);
    /* Fixed caps only, not a guest-mappable host-memory aperture. */
    if (!base || !len || len > 65536 || off > (64u << 20) || base + off < base) return NULL;
    *length = len;
    return vmm_map_mmio(base + off, len);
}

static bool transport_init(struct pci_device *d) {
    uint8_t cap = config8(d, 0x34) & ~3u;
    volatile uint8_t *notify_base = NULL;
    uint32_t notify_length = 0, notify_multiplier = 0;
    uint32_t visited[8] = {0};
    for (unsigned count = 0; cap && count < 48; count++) {
        if (cap < 0x40 || cap > 0xFC || (visited[cap >> 5] & (1u << (cap & 31)))) return false;
        visited[cap >> 5] |= 1u << (cap & 31);
        unsigned next = config8(d, cap + 1) & ~3u;
        if (config8(d, cap) == 9) {
            unsigned len = config8(d, cap + 2), type = config8(d, cap + 3);
            if (len < 16 || (unsigned)cap + len > 256) return false;
            uint32_t length = 0;
            if (type == 1 && !common) {
                common = cap_map(d, cap, &length);
                if (!common || length < sizeof *common) return false;
            } else if (type == 2 && !notify_base) {
                if (len < 20) return false;
                notify_base = cap_map(d, cap, &notify_length);
                notify_multiplier = config32(d, cap + 16);
            } else if (type == 4) {
                volatile uint32_t *config=cap_map(d,cap,&length);
                if (config && length>=16) capset_count=mmio_read32(config + 3);
            }
        }
        cap = (uint8_t)next;
    }
    if (!common || !notify_base) return false;
    pci_enable_busmaster(d);
    mmio_write8(&common->device_status, 0);
    uint64_t deadline = clock_after(100 * NS_PER_MS); /* the reset reads 0 once it is done */
    while (mmio_read8(&common->device_status)) {
        if (clock_now_ns() >= deadline) return false;
        pause();
    }
    mmio_write8(&common->device_status, 1 | 2); /* ACKNOWLEDGE | DRIVER */
    mmio_write32(&common->device_feature_select, 0);
    if (!(mmio_read32(&common->device_feature) & VIRGL_FEATURE)) return false;
    mmio_write32(&common->device_feature_select, 1);
    if (!(mmio_read32(&common->device_feature) & VERSION_1)) return false;
    mmio_write32(&common->driver_feature_select, 0);
    mmio_write32(&common->driver_feature, VIRGL_FEATURE);
    mmio_write32(&common->driver_feature_select, 1);
    mmio_write32(&common->driver_feature, VERSION_1);
    mmio_write8(&common->device_status, 1 | 2 | 8);
    if (!(mmio_read8(&common->device_status) & 8)) return false;

    uint64_t ring_phys = pmm_alloc_zeroed();
    packets_phys = pmm_alloc_contig(2, 64);
    if (!ring_phys || !packets_phys) return false;
    packets = phys_to_virt(packets_phys); memset(packets, 0, PAGE_SIZE * 2);
    desc = phys_to_virt(ring_phys);
    avail = (struct avail *)((uint8_t *)desc + 256);
    used = (struct used *)((uint8_t *)desc + 512);
    avail->flags = 1;
    mmio_write16(&common->queue_select, 0);
    mmio_write16(&common->config_msix_vector, 0xFFFF);
    if (mmio_read16(&common->queue_size) < QSIZE || mmio_read16(&common->queue_enable)) return false;
    mmio_write16(&common->queue_size, QSIZE);
    mmio_write16(&common->queue_msix_vector, 0xFFFF);
    const uint64_t rings[3] = {ring_phys, ring_phys + 256, ring_phys + 512};
    volatile uint32_t *ring_regs[3] = {common->queue_desc, common->queue_driver, common->queue_device};
    for (unsigned i = 0; i < 3; i++) {
        mmio_write32(ring_regs[i], (uint32_t)rings[i]);
        mmio_write32(ring_regs[i] + 1, (uint32_t)(rings[i] >> 32));
    }
    uint64_t notify_off = (uint64_t)mmio_read16(&common->queue_notify_off) * notify_multiplier;
    if (notify_length < 2 || notify_off > notify_length - 2) return false;
    notify = (volatile uint16_t *)(notify_base + notify_off);
    int vector=irq_vector_alloc(1,1);
    if(vector>=0){
        vector_register(vector,gpu_isr);
        if(pci_msix_init(d) && pci_msix_route(d,0,(uint8_t)vector,pci_interrupt_dest())){
            mmio_write16(&common->queue_msix_vector, 0);
            if(mmio_read16(&common->queue_msix_vector)==0){
                pci_msix_mask(d,0,false);pci_msix_function_mask(d,false);
                completion_irq=true;avail->flags=0;
            }
        }
        if(!completion_irq && pci_msi_disable(d))irq_vector_free((unsigned)vector,1); /* else it stays taken */
    }
    mmio_write16(&common->queue_enable, 1);
    mmio_write8(&common->device_status, 1 | 2 | 8 | 4);
    unsigned status = mmio_read8(&common->device_status);
    kprintf("gpu: queue desc=%lx avail=%lx used=%lx notify=%p off=%lu mult=%u size=%u status=%x\n",
        ring_phys,ring_phys+256,ring_phys+512,notify,notify_off,notify_multiplier,QSIZE,status);
    return true;
}

/* All requests are driver-authored and physically contiguous; a timed-out DMA
 * buffer is deliberately retained forever and the device is no longer used. */
static bool exchange(const void *request, size_t len, uint32_t response_type) {
    if (broken || !packets || len > PAGE_SIZE) return false;
    memcpy(packets, request, len);
    struct header *h = (struct header *)packets;
    h->flags = FENCE; h->fence = next_fence++;
    memset(packets + PAGE_SIZE, 0, PAGE_SIZE);
    desc[0] = (struct descriptor){packets_phys, (uint32_t)len, 1, 1};
    desc[1] = (struct descriptor){packets_phys + PAGE_SIZE, PAGE_SIZE, 2, 0};
    unsigned at = avail->idx % QSIZE;
    avail->ring[at] = 0;
    barrier();
    avail->idx++;
    barrier();
    mmio_write16(notify, 0);
    /* The descriptor, packet and resource backing stay driver-owned while
     * this continuation sleeps. Timeout retires the device without freeing
     * any possibly in-flight DMA or reusing the serialized packet buffers. */
    uint64_t started=clock_now_ns();
    uint64_t budget=(validating_runtime || (ready&&!cmdline_has("gpu-slow-test"))?2:10)*NS_PER_S,deadline=started+budget;
    unsigned spins = 0;
    while (used->idx == seen_used) {
        if (clock_now_ns()>=deadline || (current_task->kstack && current_task->killed)) {
                unsigned status=mmio_read8(&common->device_status);
                kprintf("gpu: timeout request=%x ctx=%u fence=%lu len=%lu used=%u seen=%u avail=%u elapsed_us=%lu spins=%u status=%x desc0=%lx/%u desc1=%lx/%u notify=%p\n",
                    h->type,h->ctx,h->fence,len,used->idx,seen_used,avail->idx,(clock_now_ns()-started)/NS_PER_US,
                    spins,status,desc[0].addr,desc[0].len,desc[1].addr,desc[1].len,notify);
                goto failed;
        }
        spins++;
        completion_nap(deadline);
    }
    barrier();
    struct used_entry *u = &used->ring[seen_used % QSIZE];
    seen_used++;
    struct header *reply = (struct header *)(packets + PAGE_SIZE);
    if (u->id != 0 || u->len < sizeof *reply || u->len > PAGE_SIZE ||
        reply->type != response_type || !(reply->flags & FENCE) || reply->fence != h->fence) {
        kprintf("gpu: reply request=%x expect=%x got=%x flags=%x fence=%lu/%lu used=%u len=%u idx=%u/%u\n",
            h->type,response_type,reply->type,reply->flags,reply->fence,h->fence,u->id,u->len,seen_used,used->idx);
        goto failed;
    }
    statistics.submissions++;
    reply_bytes=u->len;
    if (cmdline_has("gpu-timing") || cmdline_has("gpu-slow-test")) {
        kprintf("gpu: complete request=%x ctx=%u fence=%lu stage=%s elapsed_us=%lu spins=%u\n",
            h->type,h->ctx,h->fence,validating_runtime?"validation":ready?"runtime":"init",
            (clock_now_ns()-started)/NS_PER_US,spins);
    }
    return true;
failed:
    broken = true; ready = false; statistics.failures++;
    statistics.backend=N_GPU_BACKEND_NONE; statistics.capabilities = 0;
    strlcpy(statistics.name,"virgl failed; CPU display retained",sizeof statistics.name);
    kprintf("gpu: control request %x failed; retaining DMA buffers, CPU fallback\n", h->type);
    return false;
}

static struct header hdr(uint32_t type, bool context) {
    return (struct header){.type = type, .ctx = context ? CTX_ID : 0};
}
static bool resource(uint32_t id, uint32_t target_kind, uint32_t format, uint32_t bind,
                     uint32_t w, uint32_t h, uint64_t phys, uint32_t bytes) {
    struct { struct header h; uint32_t id, target, format, bind, w, height, depth,
        arrays, level, samples, flags, pad; } PACKED create = {
        .h = hdr(CMD_CREATE_3D, false), .id=id, .target=target_kind, .format=format, .bind=bind,
        .w=w, .height=h, .depth=1, .arrays=1, .flags=0};
    /* A single wire origin for uploads, copy fast paths, scaled blits and
       readback: native packed row zero is GPU row zero. Y_0_TOP would ask
       the renderer to invert only some of those operations. The native
       top-origin drawing convention is expressed by the viewport instead. */
    kprintf("gpu: resource id=%u target=%u format=%u bind=%x flags=%x size=%ux%u backing=%lx bytes=%u\n",
        id,target_kind,format,bind,create.flags,w,h,phys,bytes);
    if (!exchange(&create, sizeof create, OK_NODATA)) return false;
    struct { struct header h; uint32_t id, n; uint64_t phys; uint32_t len, pad; } PACKED attach = {
        .h=hdr(CMD_ATTACH,false), .id=id, .n=1, .phys=phys, .len=bytes};
    if (!exchange(&attach, sizeof attach, OK_NODATA)) return false;
    struct { struct header h; uint32_t id, pad; } PACKED ctx = {.h=hdr(CMD_CTX_ATTACH,true), .id=id};
    return exchange(&ctx, sizeof ctx, OK_NODATA);
}
struct stream { uint32_t data[768]; unsigned n; bool overflow; };
static void emit(struct stream *s, uint32_t word) {
    if (s->n < ARRAY_SIZE(s->data)) s->data[s->n++] = word;
    else s->overflow=true;
}
static void command(struct stream *s, unsigned op, unsigned object, const uint32_t *p, unsigned n) {
    emit(s,CMD(op,object,n)); for (unsigned i=0;i<n;i++) emit(s,p[i]);
}
static void stream_transfer(struct stream *s,uint32_t id,uint32_t w,uint32_t h,uint32_t stride,uint32_t direction) {
    /* VIRGL_CCMD_TRANSFER3D=43, 13 words: resource, level, usage, stride,
     * layer stride, xyz, whd, backing offset, direction. Keep transfers in
     * the drawing context and retire them with the same fenced SUBMIT.
     * Fencing separate virtio control transfers on this host took ~4s each;
     * neither an unfenced DMA shortcut nor a larger production timeout is
     * a valid remedy. This command is used only after CAP_TRANSFER proof. */
    command(s,43,0,(uint32_t[]){id,0,0,stride,stride*h,0,0,0,w,h,1,0,direction},13);
}
static void shader(struct stream *s, uint32_t handle, uint32_t type, const char *text) {
    size_t bytes=strlen(text)+1; unsigned words=(unsigned)((bytes+3)/4);
    emit(s,CMD(1,4,words+5)); emit(s,handle); emit(s,type); emit(s,(uint32_t)bytes);
    emit(s,128); /* bounded token allocation, more than these fixed TGSI texts need */
    emit(s,0); /* stream output */
    for (unsigned i=0;i<words;i++) {
        uint32_t v=0;
        for (unsigned j=0;j<4 && i*4+j<bytes;j++) v|=(uint32_t)(uint8_t)text[i*4+j]<<(j*8);
        emit(s,v);
    }
}
static bool submit(struct stream *s) {
    if (s->overflow || s->n > ARRAY_SIZE(s->data)) return false;
    struct { struct header h; uint32_t size,pad; uint32_t data[768]; } PACKED p;
    p.h=hdr(CMD_SUBMIT,true); p.size=s->n*4; p.pad=0;
    memcpy(p.data,s->data,p.size);
    return exchange(&p,32+p.size,OK_NODATA);
}
static void free_backing(uint64_t phys,uint32_t w,uint32_t h) {
    size_t pages=ALIGN_UP(n_gpu_frame_bytes(w,h),PAGE_SIZE)/PAGE_SIZE;
    pmm_free_contig(phys,pages);
}
static bool retire_resource(uint32_t id) {
    struct {struct header h;uint32_t id,pad;} PACKED p={.h=hdr(0x203,true),.id=id};
    if(!exchange(&p,sizeof p,OK_NODATA))return false;
    p.h=hdr(0x107,false);
    if(!exchange(&p,sizeof p,OK_NODATA))return false;
    p.h=hdr(0x102,false);
    return exchange(&p,sizeof p,OK_NODATA);
}
/* Allocate only the requested image. The protocol's backing entry length is
 * uint32_t; actual contiguous RAM and host allocation may fail below it.
 * Old backing is freed only after detach/unref fences complete. Any timeout
 * keeps both old and new backing out of the allocator. */
static bool image_resource(bool input,uint32_t w,uint32_t h) {
    uint32_t *cw=input?&source_width:&target_width,*ch=input?&source_height:&target_height;
    uint64_t *phys=input?&source_phys:&target_phys;
    uint32_t *id=input?&source_id:&target_id;
    if(*phys && *cw>=w && *ch>=h)return true;
    uint32_t requested_w=w,requested_h=h;
    if(w>statistics.max_width || h>statistics.max_height)return false;
    w=MAX(w,*cw);h=MAX(h,*ch); /* keep capacity for smaller subsequent frames */
    size_t bytes=n_gpu_frame_bytes(w,h);
    size_t pages=bytes && bytes<=UINT32_MAX?ALIGN_UP(bytes,PAGE_SIZE)/PAGE_SIZE:0;
    uint64_t fresh=pages && pages<=pmm_free_pages()?pmm_alloc_contig(pages,64):0;
    if(!fresh && (w!=requested_w || h!=requested_h)){
        /* Orthogonal image shapes must not inherit a huge cross-product
           allocation from unrelated earlier requests. Retry the exact shape
           when keeping both dimensions would exceed real available memory. */
        w=requested_w;h=requested_h;bytes=n_gpu_frame_bytes(w,h);
        pages=bytes && bytes<=UINT32_MAX?ALIGN_UP(bytes,PAGE_SIZE)/PAGE_SIZE:0;
        fresh=pages && pages<=pmm_free_pages()?pmm_alloc_contig(pages,64):0;
    }
    if(!fresh)return false;
    uint32_t fresh_id=*phys?(input?(*id==3?5:3):(*id==1?4:1)):*id;
    for(size_t at=0;at<bytes;at+=PAGE_SIZE){
        memset((uint8_t *)phys_to_virt(fresh)+at,0,MIN(bytes-at,PAGE_SIZE));
        if(!gpu_checkpoint()){free_backing(fresh,w,h);return false;}
    }
    if(!resource(fresh_id,2,FORMAT_BGRA,2|8,w,h,fresh,(uint32_t)bytes))return false;
    if(!input){
        struct stream s={0};
        if(*phys)command(&s,3,8,(uint32_t[]){1},1); /* DESTROY_OBJECT, not BIND_OBJECT */
        command(&s,1,8,(uint32_t[]){1,fresh_id,FORMAT_BGRA,0,0},5);
        command(&s,5,0,(uint32_t[]){1,0,1},3);
        if(!submit(&s))return false;
    }
    if(*phys){
        if(!retire_resource(*id))return false;
        free_backing(*phys,*cw,*ch);
    }
    *phys=fresh;*id=fresh_id;*cw=w;*ch=h;
    if(input)source_pixels=phys_to_virt(fresh);else target=phys_to_virt(fresh);
    return true;
}
static bool pipeline_init(void) {
    struct { struct header h; uint32_t len,init; char name[64]; } PACKED ctx = {
        .h=hdr(CMD_CTX_CREATE,true), .len=8, .name="Nocturne"};
    if (!exchange(&ctx,sizeof ctx,OK_NODATA)) return false;
    vertex_phys=pmm_alloc_zeroed();
    if (!vertex_phys) return false;
    vertices=phys_to_virt(vertex_phys);
    /* PIPE_TEXTURE_2D=2, RENDER_TARGET|SAMPLER_VIEW; PIPE_BUFFER=0. */
    if (!image_resource(false,32,32) ||
        !resource(VERTEX_ID,0,64,16,VERTEX_BYTES,1,vertex_phys,PAGE_SIZE)) return false;
    struct stream s={0};
    shader(&s,2,0,"VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], COLOR\nMOV OUT[0], IN[0]\nMOV OUT[1], IN[1]\nEND\n");
    shader(&s,3,1,"FRAG\nDCL IN[0], COLOR, PERSPECTIVE\nDCL OUT[0], COLOR\nMOV OUT[0], IN[0]\nEND\n");
    command(&s,31,0,(uint32_t[]){2,0},2); /* BIND_SHADER */
    command(&s,31,0,(uint32_t[]){3,1},2);
    command(&s,1,5,(uint32_t[]){4,0,0,0,31,16,0,0,31},9);
    command(&s,2,5,(uint32_t[]){4},1);
    command(&s,1,2,(uint32_t[]){5,(1u<<1)|(1u<<15)|(1u<<29),0x3f800000,0,0,0x3f800000,0,0,0},9);
    command(&s,2,2,(uint32_t[]){5},1);
    command(&s,1,1,(uint32_t[]){6,0,0,15u<<27,0,0,0,0,0,0,0},11);
    command(&s,2,1,(uint32_t[]){6},1);
    command(&s,1,3,(uint32_t[]){7,0,0,0,0},5);
    command(&s,2,3,(uint32_t[]){7},1);
    command(&s,6,0,(uint32_t[]){32,0,VERTEX_ID},3);
    return submit(&s);
}
static bool blit_pipeline_init(void) {
    /* Separate from the target even for source/output aliases. This fixed
     * private texture/backing persists; timeout never frees in-flight DMA. */
    return image_resource(true,4,3);
}
static bool valid_blit(const struct n_gpu_blit *r) {
    return r && r->source_width && r->source_height && r->width && r->height &&
        n_gpu_frame_bytes(r->source_width,r->source_height) && n_gpu_frame_bytes(r->width,r->height) &&
        r->source_w && r->source_h && r->source_x<=r->source_width && r->source_y<=r->source_height &&
        r->source_w<=r->source_width-r->source_x && r->source_h<=r->source_height-r->source_y;
}
static bool blit_internal(const struct n_gpu_blit *r,const uint32_t *pixels) {
    if(!image_resource(true,r->source_width,r->source_height) || !image_resource(false,r->width,r->height))return false;
    /* Caller holds busy. Copy ALL packed input before touching the host or
     * the output, so an aliased copyout cannot destroy subsequent input rows. */
    for (unsigned y=0;y<r->source_height;y++){
        if(!gpu_copy(source_pixels+(size_t)y*source_width,pixels+(size_t)y*r->source_width,(size_t)r->source_width*4))return false;
    }
    struct stream s={0};
    stream_transfer(&s,SOURCE_ID,r->source_width,r->source_height,source_width*4,1);
    /* VIRGL_CCMD_BLIT=16: RGBA/nearest, no blend/scissor, mip0/depth1.
       Same wire row origin for scaled drawing and equal-size copy paths. */
    command(&s,16,0,(uint32_t[]){15,0,0,TARGET_ID,0,FORMAT_BGRA,0,0,0,r->width,r->height,1,
        SOURCE_ID,0,FORMAT_BGRA,r->source_x,r->source_y,0,r->source_w,r->source_h,1},21);
    stream_transfer(&s,TARGET_ID,r->width,r->height,SIDE*4,2);
    return submit(&s); /* One fence covers input upload, GPU blit and readback. */
}
static bool blit_pixel_test(void) {
    /* Asymmetric ARGB rows/crop, then scale. No CPU-filled fake output. */
    const uint32_t input[12]={0xff010203,0xff102030,0xff405060,0xff708090,
        0xffa1b2c3,0x7f112233,0x80224466,0xffabcdef,
        0xff998877,0x20345678,0x4056789a,0xff987654};
    struct n_gpu_blit r={.source_width=4,.source_height=3,.source_x=1,.source_y=1,
        .source_w=2,.source_h=2,.width=4,.height=4};
    if (!blit_internal(&r,input)) return false;
    for (unsigned y=0;y<r.height;y++) for (unsigned x=0;x<r.width;x++) {
        uint32_t expected=input[(1+y/2)*4+1+x/2];
        if (target[y*SIDE+x]!=expected) {
            kprintf("gpu: blit pixel mismatch x=%u y=%u got=%x expected=%x\n",x,y,target[y*SIDE+x],expected);
            return false;
        }
    }
    return true;
}

/* Integer-only conversion, including round-to-nearest-even. The kernel never
 * borrows a user's FPU context or emits SIMD loads against MMIO. */
static uint32_t fixed_float(int64_t q) {
    if (!q) return 0;
    uint32_t sign=q<0?0x80000000u:0;
    uint64_t mag=q<0?(uint64_t)-q:(uint64_t)q;
    unsigned top=63u-(unsigned)__builtin_clzll(mag);
    uint64_t sig;
    if (top<=23) sig=mag<<(23-top);
    else {
        unsigned shift=top-23; sig=mag>>shift;
        uint64_t remain=mag&((1ull<<shift)-1), halfway=1ull<<(shift-1);
        if (remain>halfway || (remain==halfway && (sig&1))) sig++;
        if (sig==0x1000000) { sig>>=1; top++; }
    }
    return sign|((top+127-16)<<23)|(sig&0x7fffff);
}
static uint32_t color_float(unsigned c) { return fixed_float((int32_t)((c*65536u+127)/255)); }
static bool render_frame(unsigned width, unsigned height, uint32_t color,
                         const struct n_gpu_vertex *v, unsigned triangle_count) {
    if(!image_resource(false,width,height))return false;
    struct stream s={0};
    command(&s,4,0,(uint32_t[]){0,fixed_float((int64_t)width*32768),
        fixed_float(-(int64_t)height*32768),0x3f000000,
        fixed_float((int64_t)width*32768),fixed_float((int64_t)height*32768),0x3f000000},7);
    /* Native row zero is the top, and NDC +Y points up. A negative Y scale
       converts this drawing convention into our unified row-zero storage.
       The fixed rasterizer has cull_face=0 and two-sided fill, so reversing
       winding cannot suppress an existing triangle. */
    command(&s,7,0,(uint32_t[]){4,color_float((color>>16)&255),color_float((color>>8)&255),
        color_float(color&255),color_float(color>>24),0,0x3ff00000,0},8);
    for(unsigned done=0;done<triangle_count;){
        if(!gpu_checkpoint())return false;
        unsigned triangles=MIN(triangle_count-done,VERTEX_TRIANGLES),count=triangles*3;
        for (unsigned i=0;i<count;i++) {
            const struct n_gpu_vertex *a=&v[(size_t)done*3+i];
            vertices[i*8]=fixed_float(a->x); vertices[i*8+1]=fixed_float(a->y);
            vertices[i*8+2]=fixed_float(a->z); vertices[i*8+3]=fixed_float(a->w);
            vertices[i*8+4]=color_float((a->argb>>16)&255); vertices[i*8+5]=color_float((a->argb>>8)&255);
            vertices[i*8+6]=color_float(a->argb&255); vertices[i*8+7]=color_float(a->argb>>24);
        }
        /* One contiguous upload, one triangle-list draw, no per-primitive
         * fence or CPU readback. The pipeline objects persist across frames. */
        stream_transfer(&s,VERTEX_ID,count*32,1,0,1);
        command(&s,8,0,(uint32_t[]){0,count,4,0,1,0,0,0,0,0,count-1,0},12);
        done+=triangles;
        if(done<triangle_count){
            if(!submit(&s))return false;
            s=(struct stream){0};
        }
    }
    stream_transfer(&s,TARGET_ID,width,height,SIDE*4,2);
    /* The fence covers upload, draw, and readback before CPU consumption. */
    return submit(&s);
}
static bool render_internal(const struct n_gpu_render *r) {
    return render_frame(r->width,r->height,r->clear_argb,r->vertex,r->operation==N_GPU_TRIANGLE?1:0);
}
static bool valid_vertices(const struct n_gpu_vertex *v, size_t count) {
    for (size_t i=0;i<count;i++) {
        if(i%VERTEX_TRIANGLES==0 && !gpu_checkpoint())return false;
        if (!v[i].w) return false; /* perspective division has no value at w=0 */
    }
    return true;
}
static bool valid(const struct n_gpu_render *r) {
    if (!r || !n_gpu_frame_bytes(r->width,r->height) ||
        (r->operation!=N_GPU_CLEAR && r->operation!=N_GPU_TRIANGLE)) return false;
    return r->operation!=N_GPU_TRIANGLE || valid_vertices(r->vertex,3);
}
static bool batch_pixel_test(void) {
    /* Exercise the last vertex of the largest upload as well as its first.
     * Intermediate primitives lie outside the homogeneous clipping volume. */
    struct n_gpu_batch r={.width=32,.height=32,.clear_argb=0xff284c78,.triangle_count=N_GPU_MAX_TRIANGLES};
    for (unsigned i=0;i<N_GPU_MAX_TRIANGLES*3;i++)
        r.vertex[i]=(struct n_gpu_vertex){131072,0,0,65536,0xff00ff00};
    r.vertex[0]=(struct n_gpu_vertex){-58982,-49152,0,65536,0xffff0000};
    r.vertex[1]=(struct n_gpu_vertex){-6554,-49152,0,65536,0xffff0000};
    r.vertex[2]=(struct n_gpu_vertex){-32768,49152,0,65536,0xffff0000};
    unsigned last=(N_GPU_MAX_TRIANGLES-1)*3;
    r.vertex[last]=(struct n_gpu_vertex){6554,-49152,0,65536,0xff0000ff};
    r.vertex[last+1]=(struct n_gpu_vertex){58982,-49152,0,65536,0xff0000ff};
    r.vertex[last+2]=(struct n_gpu_vertex){32768,49152,0,65536,0xff0000ff};
    if (!render_frame(r.width,r.height,r.clear_argb,r.vertex,r.triangle_count)) return false;
    bool ok=target[0]==r.clear_argb && target[16*SIDE+8]==0xffff0000 && target[16*SIDE+24]==0xff0000ff;
    if (!ok) kprintf("gpu: batch pixels corner=%x first=%x last=%x\n",target[0],target[16*SIDE+8],target[16*SIDE+24]);
    return ok;
}
static bool pixel_test(void) {
    struct n_gpu_render r={.width=32,.height=32,.operation=N_GPU_CLEAR,.clear_argb=0xff284c78};
    if (!render_internal(&r)) return false;
    for (unsigned y=0;y<r.height;y++) for (unsigned x=0;x<r.width;x++)
        if (target[y*SIDE+x]!=r.clear_argb) {
            kprintf("gpu: clear pixel mismatch x=%u y=%u got=%x expected=%x\n",x,y,target[y*SIDE+x],r.clear_argb);
            return false;
        }
    r.operation=N_GPU_TRIANGLE;
    r.vertex[0]=(struct n_gpu_vertex){-49152,-49152,0,65536,0xffff0000};
    r.vertex[1]=(struct n_gpu_vertex){49152,-49152,0,65536,0xff00ff00};
    r.vertex[2]=(struct n_gpu_vertex){0,49152,0,65536,0xff0000ff};
    if (!render_internal(&r)) return false;
    uint32_t center=target[16*SIDE+16];
    bool ok=target[0]==r.clear_argb && center!=r.clear_argb && (center>>24)==255 &&
           ((center>>16)&255)>10 && ((center>>8)&255)>10 && (center&255)>10;
    if (!ok) kprintf("gpu: triangle pixels corner=%x center=%x top=%x bottom=%x expected_background=%x\n",
        target[0],center,target[4*SIDE+16],target[27*SIDE+16],r.clear_argb);
    return ok;
}

void gpu_init(void) {
    if (attempted) return;
    attempted=true;
    strlcpy(statistics.name,"CPU display; no verified virgl pipeline",sizeof statistics.name);
    if (!vm_hosted()) return;
    struct pci_device *d=pci_find(0x1af4,0x1050);
    if (!d || cmdline_has("nogpu")) return;
    if (!cmdline_has("gpu-enable") && !cmdline_has("gpu-slow-test")) {
        kprintf("gpu: optional virgl device present; gpu-enable required (cold host fences can be slow)\n");
        return;
    }
    if (!transport_init(d)) {
        if (common) mmio_write8(&common->device_status, mmio_read8(&common->device_status) | 128); /* FAILED */
        kprintf("gpu: virgl transport unavailable; boot framebuffer unchanged\n");
        return;
    }
    /* its queues and resources would be gone after a sleep: no sleep while it is in use */
    static struct pm_device gpu_pm = {.name = "virtio-gpu", .stage = PM_STAGE_IO};
    pm_register(&gpu_pm);
    /* Read only the fixed prefix of growable capset2, not host pointers or
     * arbitrary resources. virgl_caps_v1 is 77 dwords, then capability_bits
     * is v2 word 98 (byte392). All formats/limits needed by this finite
     * pipeline are still validated by the actual pixel self-test. */
    bool transfers=false;
    for (uint32_t i=0;i<capset_count;i++) {
        struct { struct header h; uint32_t index,pad; } PACKED cap={.h=hdr(CMD_CAPSET_INFO,false),.index=i};
        if (!exchange(&cap,sizeof cap,0x1102) || reply_bytes<40) return;
        uint32_t *info=(uint32_t *)(packets+PAGE_SIZE+24);
        if (info[0]!=2 || info[1]<1 || info[2]<488 || info[2]>PAGE_SIZE-24) continue;
        struct { struct header h; uint32_t id,version; } PACKED get={.h=hdr(CMD_CAPSET,false),.id=2,.version=1};
        if (!exchange(&get,sizeof get,0x1103) || reply_bytes<24+488) return;
        uint32_t bits=*(uint32_t *)(packets+PAGE_SIZE+24+392);
        transfers=(bits&(1u<<17))!=0;
        /* virgl_caps_v2: max_texture_2d_size follows the compute block/grid
           dimensions at dword 121. This is a host device capability, not a
           private software ceiling. The backing byte width is checked too. */
        statistics.max_width=statistics.max_height=*(uint32_t *)(packets+PAGE_SIZE+24+121*4);
        kprintf("gpu: capset2 bits=%x stream_transfer=%u\n",bits,transfers);
        if (reply_bytes>=24+760) {
            char renderer[65];
            memcpy(renderer,packets+PAGE_SIZE+24+696,64);renderer[64]=0;
            for (unsigned j=0;renderer[j];j++) if ((uint8_t)renderer[j]<32 || (uint8_t)renderer[j]>126) renderer[j]='?';
            kprintf("gpu: host GL_RENDERER=%s (identity only, not a performance guarantee)\n",renderer);
        }
        break;
    }
    if (!transfers) {
        kprintf("gpu: fenced stream transfer unavailable; CPU display retained\n");
        return;
    }
    if (!pipeline_init() || !pixel_test()) {
        ready=false; statistics.failures++;
        kprintf("gpu: virgl pixel self-test failed; no 3D capabilities advertised\n");
        return;
    }
    bool cold_batch_ok=batch_pixel_test();
    if (broken) return; /* Failed DMA/fence retires the entire device. */
    bool cold_blit_ok=blit_pipeline_init() && blit_pixel_test();
    if (broken) return;
    /* A cold ten-second pixel pass is not proof that the production two-second
     * deadline is usable. Verify clear AND triangle under the actual runtime
     * budget before publishing any capability, even in a slow diagnostic boot.
     * The device can be real yet too slow; fallback is the correct outcome. */
    kprintf("gpu: cold pixel self-test passed; validating two-second runtime budget\n");
    validating_runtime=true;
    /* A batch pixel mismatch is not evidence that legacy rendering failed.
     * Validate legacy AFTER a cold mismatch. If only the runtime batch has
     * wrong pixels, revalidate legacy after it before retaining those bits. */
    bool runtime_ok=pixel_test(), batch_ok=false, blit_ok=false;
    if (runtime_ok && cold_batch_ok) {
        batch_ok=batch_pixel_test();
        if (!batch_ok && !broken) runtime_ok=pixel_test();
    }
    if (runtime_ok && cold_blit_ok) {
        blit_ok=blit_pixel_test();
        /* BLIT changes host FBO state. Check legacy AFTER it even on success;
         * a new pixel mismatch alone drops only the new capability. */
        runtime_ok=!broken && pixel_test();
        if (runtime_ok && batch_ok) {
            batch_ok=batch_pixel_test();
            if (!batch_ok && !broken) runtime_ok=pixel_test();
        }
    }
    runtime_ok=runtime_ok && !broken;
    validating_runtime=false;
    if (!runtime_ok) {
        ready=false;statistics.backend=N_GPU_BACKEND_NONE;statistics.capabilities=0;
        if (!broken) statistics.failures++;
        kprintf("gpu: runtime budget validation failed; no 3D capabilities advertised\n");
        return;
    }
    ready=true;
    statistics.backend=N_GPU_BACKEND_VIRGL;
    statistics.capabilities=N_GPU_CAP_CLEAR|N_GPU_CAP_TRIANGLE|(batch_ok?N_GPU_CAP_BATCH:0)|(blit_ok?N_GPU_CAP_BLIT:0);
    strlcpy(statistics.name,"virtio-gpu virgl: verified clear/triangle",sizeof statistics.name);
    kprintf("gpu: virgl clear/triangle/readback pixel self-test passed\n");
    kprintf("gpu: batch capability=%u (cold=%u runtime=%u; legacy independently revalidated)\n",
        batch_ok,cold_batch_ok,batch_ok);
    kprintf("gpu: blit capability=%u (cold=%u runtime=%u; nearest texture pixels)\n",blit_ok,cold_blit_ok,blit_ok);
    if (cmdline_has("gpu-slow-test")) {
        /* Same public kernel entry, but before a user task/CR3 is involved.
           This distinguishes post-initialization/repeated fences from the
           ring0-to-user transition. The ten-second budget is diagnostic only. */
        static uint32_t test_pixels[32*32];
        struct n_gpu_render r={.width=32,.height=32,.operation=N_GPU_CLEAR};
        unsigned passed=0;
        for (unsigned i=0;i<3;i++) {
            r.clear_argb=0xff123456u+i*0x10203u;
            int result=gpu_render(&r,test_pixels,sizeof test_pixels);
            bool same=result==0;
            if (same) for (unsigned j=0;j<ARRAY_SIZE(test_pixels);j++) if (test_pixels[j]!=r.clear_argb) same=false;
            kprintf("gpu: kernel replay=%u result=%d pixels=%s\n",i,result,same?"PASS":"FAIL");
            if (!same) break;
            passed++;
        }
        kprintf("gpu: kernel replay passed=%u/3 (diagnostic slow budget)\n",passed);
    }
}
void gpu_get_info(struct n_gpu_info *info) { *info=statistics; }
int gpu_render(const struct n_gpu_render *r, uint32_t *out, size_t bytes) {
    if (!valid(r)) return -N_ERR_INVALID;
    size_t need=(size_t)r->width*r->height*4;
    if (!out || bytes<need) return -N_ERR_INVALID;
    if (!ready || broken) return -N_ERR_UNSUPPORTED;
    if (__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return -N_ERR_BUSY;
    int result=0;
    if (!render_internal(r)) result=-N_ERR_IO;
    else for (unsigned y=0;y<r->height;y++) if(!gpu_copy(out+(size_t)y*r->width,target+(size_t)y*SIDE,(size_t)r->width*4)){result=-N_ERR_STOPPED;break;}
    __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
    return result;
}
int gpu_render_batch(const struct n_gpu_batch *r, uint32_t *out, size_t bytes) {
    if (!r || !n_gpu_frame_bytes(r->width,r->height) ||
        r->triangle_count>N_GPU_MAX_TRIANGLES || !valid_vertices(r->vertex,r->triangle_count*3)) return -N_ERR_INVALID;
    size_t need=(size_t)r->width*r->height*4;
    if (!out || bytes<need) return -N_ERR_INVALID;
    if (!ready || broken || !(statistics.capabilities&N_GPU_CAP_BATCH)) return -N_ERR_UNSUPPORTED;
    if (__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return -N_ERR_BUSY;
    int result=0;
    if (!render_frame(r->width,r->height,r->clear_argb,r->vertex,r->triangle_count)) result=-N_ERR_IO;
    else for (unsigned y=0;y<r->height;y++) if(!gpu_copy(out+(size_t)y*r->width,target+(size_t)y*SIDE,(size_t)r->width*4)){result=-N_ERR_STOPPED;break;}
    __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
    return result;
}
int gpu_render_list(const struct n_gpu_scene *r,const struct n_gpu_vertex *v,uint32_t *out,size_t bytes) {
    if(!r || !n_gpu_frame_bytes(r->width,r->height) ||
       (r->triangle_count && !v) ||
       !valid_vertices(v,(size_t)r->triangle_count*3) || !out || bytes<n_gpu_frame_bytes(r->width,r->height))return -N_ERR_INVALID;
    if(!ready || broken || !(statistics.capabilities&N_GPU_CAP_BATCH))return -N_ERR_UNSUPPORTED;
    if(__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE))return -N_ERR_BUSY;
    int result=render_frame(r->width,r->height,r->clear_argb,v,r->triangle_count)?0:-N_ERR_IO;
    if(!result)for(unsigned y=0;y<r->height;y++)if(!gpu_copy(out+(size_t)y*r->width,target+(size_t)y*SIDE,(size_t)r->width*4)){result=-N_ERR_STOPPED;break;}
    __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
    return result;
}
int gpu_blit(const struct n_gpu_blit *r,const uint32_t *pixels,size_t source_bytes,uint32_t *out,size_t bytes) {
    if (!valid_blit(r) || !pixels || !out || source_bytes<(size_t)r->source_width*r->source_height*4 ||
        bytes<(size_t)r->width*r->height*4) return -N_ERR_INVALID;
    if (!ready || broken || !(statistics.capabilities&N_GPU_CAP_BLIT)) return -N_ERR_UNSUPPORTED;
    if (__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return -N_ERR_BUSY;
    int result=0;
    if (!blit_internal(r,pixels)) result=-N_ERR_IO;
    else for (unsigned y=0;y<r->height;y++) if(!gpu_copy(out+(size_t)y*r->width,target+(size_t)y*SIDE,(size_t)r->width*4)){result=-N_ERR_STOPPED;break;}
    __atomic_store_n(&busy,0,__ATOMIC_RELEASE);
    return result;
}
