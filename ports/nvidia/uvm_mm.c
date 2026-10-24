/* UVM's OS layer in nvrm, the memory part (uvm_os.h): pages, DMA, the GPU's PCI description,
 * and the address spaces of nvrm's clients.
 *
 * Pages. alloc_pages is RM's DMA memory for the GPU (nvrm_uvm_memory_alloc): one contiguous run
 * per allocation, mapped in nvrm for its whole life, with a view that a pager can bind client
 * pages to. The struct pages of an allocation are one array after its group header; the group's
 * reference count (on its first page) frees the run. Groups are found by nvrm address
 * (virt_to_page) and by frame (pfn_to_page). Pinned client pages (get_user_pages_remote) are
 * groups too, over a mapping in nvrm of the client's argument arena.
 *
 * Address spaces. A connection of a client is an mm_struct (uvm_os_mm). A mapping of a UVM file
 * is paged shared memory (docs/nocturne-native-abi.md §7) that nvrm is the pager of: the client
 * maps it, and the first touch of a page without the access it needs waits until the
 * connection's fault thread ran the mapping's fault handler, as a CPU page fault runs it on
 * Linux. vm_insert_page binds a page (n_pager_supply), unmap_mapping_range takes pages away
 * (n_pager_revoke). Which pages are bound, and how, is kept per mapping ("shadow"), because UVM
 * relies on vm_insert_page refusing a page that is already there.
 */
#include "uvm_os.h"
#include "nvrm_serve.h"

/* ==========================================================================================
 * Page groups
 * ======================================================================================== */

struct uvm_os_pages {
    struct nvrm_uvm_memory memory;   /* pinned: memory.cpu is nvrm's mapping, memory.view the view */
    bool pinned;                     /* client pages (get_user_pages_remote), not alloc_pages' */
    size_t count;
    struct rb_node by_address;       /* every group */
    struct rb_node by_frame;         /* alloc_pages' groups */
    struct page page[];
};

static mtx_t groups_lock;
static struct rb_root groups_by_address, groups_by_frame;

static uintptr_t group_start(const struct uvm_os_pages *g)
{
    return (uintptr_t)g->memory.cpu;
}

static void group_insert(struct uvm_os_pages *g)
{
    mtx_lock(&groups_lock);
    struct rb_node **link = &groups_by_address.rb_node, *parent = NULL;
    while (*link) {
        parent = *link;
        link = group_start(g) < group_start(rb_entry(parent, struct uvm_os_pages, by_address)) ? &parent->rb_left
                                                                                                : &parent->rb_right;
    }
    rb_link_node(&g->by_address, parent, link);
    rb_insert_color(&g->by_address, &groups_by_address);
    if (!g->pinned) {
        link = &groups_by_frame.rb_node;
        parent = NULL;
        while (*link) {
            parent = *link;
            link = g->memory.bus < rb_entry(parent, struct uvm_os_pages, by_frame)->memory.bus ? &parent->rb_left
                                                                                                : &parent->rb_right;
        }
        rb_link_node(&g->by_frame, parent, link);
        rb_insert_color(&g->by_frame, &groups_by_frame);
    }
    mtx_unlock(&groups_lock);
}

/* The group whose [start, start + count pages) holds `at` in the tree; groups_lock held. */
static struct uvm_os_pages *group_find(const struct rb_root *root, bool by_frame, uint64_t at)
{
    struct rb_node *node = root->rb_node;
    struct uvm_os_pages *below = NULL;
    while (node) {
        struct uvm_os_pages *g = by_frame ? rb_entry(node, struct uvm_os_pages, by_frame)
                                          : rb_entry(node, struct uvm_os_pages, by_address);
        uint64_t start = by_frame ? g->memory.bus : group_start(g);
        if (at < start) {
            node = node->rb_left;
        }
        else {
            below = g;
            node = node->rb_right;
        }
    }
    if (!below)
        return NULL;
    uint64_t start = by_frame ? below->memory.bus : group_start(below);
    return at - start < below->count * PAGE_SIZE ? below : NULL;
}

static void group_free(struct uvm_os_pages *g)
{
    mtx_lock(&groups_lock);
    rb_erase(&g->by_address, &groups_by_address);
    if (!g->pinned)
        rb_erase(&g->by_frame, &groups_by_frame);
    mtx_unlock(&groups_lock);
    if (g->pinned) {
        n_memory_release(g->memory.cpu, g->count * PAGE_SIZE);
        n_handle_close(g->memory.view);
    }
    else {
        nvrm_uvm_memory_free(&g->memory);
    }
    free(g);
}

static struct uvm_os_pages *group_new(size_t count)
{
    struct uvm_os_pages *g = calloc(1, sizeof *g + count * sizeof(struct page));
    if (g)
        g->count = count;
    return g;
}

/* Fills the descriptors: page i at nvrm's cpu + i pages, bus address bus + i pages. */
static void group_describe(struct uvm_os_pages *g)
{
    for (size_t i = 0; i < g->count; i++) {
        struct page *p = &g->page[i];
        p->pages = g;
        p->address = (char *)g->memory.cpu + i * PAGE_SIZE;
        p->dma = g->pinned ? 0 : g->memory.bus + i * PAGE_SIZE;
    }
    atomic_set(&g->page[0].refcount, 1);
}

struct page *alloc_pages(gfp_t flags, unsigned order)
{
    if (order > MAX_ORDER + 9)
        return NULL;
    size_t count = (size_t)1 << order;
    struct uvm_os_pages *g = group_new(count);
    if (!g)
        return NULL;
    /* RM's memory is zeroed when it is allocated (nvrm_uvm_memory_alloc), so __GFP_ZERO holds. */
    if (!nvrm_uvm_memory_alloc(count, (flags & (__GFP_DMA32 | __GFP_DMA)) != 0, &g->memory)) {
        if (!(flags & __GFP_NOWARN))
            printk("nvidia-uvm: no memory for %zu pages\n", count);
        free(g);
        return NULL;
    }
    group_describe(g);
    group_insert(g);
    return &g->page[0];
}

void get_page(struct page *page)
{
    atomic_inc(&page->pages->page[0].refcount);
}

void put_page(struct page *page)
{
    struct uvm_os_pages *g = page->pages;
    if (atomic_dec_and_test(&g->page[0].refcount))
        group_free(g);
}

void __free_pages(struct page *page, unsigned order)
{
    (void)order;
    put_page(page);
}

int page_count(struct page *page)
{
    return atomic_read(&page->pages->page[0].refcount);
}

void set_page_count(struct page *page, int count)
{
    atomic_set(&page->pages->page[0].refcount, count);
}

struct page *pfn_to_page(unsigned long pfn)
{
    uint64_t bus = (uint64_t)pfn << PAGE_SHIFT;
    mtx_lock(&groups_lock);
    struct uvm_os_pages *g = group_find(&groups_by_frame, true, bus);
    mtx_unlock(&groups_lock);
    return g ? &g->page[(bus - g->memory.bus) >> PAGE_SHIFT] : NULL;
}

struct page *virt_to_page(const void *p)
{
    uintptr_t at = (uintptr_t)p;
    mtx_lock(&groups_lock);
    struct uvm_os_pages *g = group_find(&groups_by_address, false, at);
    mtx_unlock(&groups_lock);
    return g ? &g->page[(at - group_start(g)) >> PAGE_SHIFT] : NULL;
}

/* ==========================================================================================
 * DMA
 * ======================================================================================== */

dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t offset, size_t size,
                        enum dma_data_direction dir)
{
    (void)dev;
    (void)dir;
    struct uvm_os_pages *g = page ? page->pages : NULL;
    if (!g || g->pinned)
        return DMA_MAPPING_ERROR;
    size_t from = (size_t)(page - g->page) * PAGE_SIZE + offset;
    if (from > g->count * PAGE_SIZE || size > g->count * PAGE_SIZE - from)
        return DMA_MAPPING_ERROR;
    return page->dma + offset;
}

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma, gfp_t flags)
{
    (void)dev;
    struct page *page = alloc_pages(flags | __GFP_ZERO, (unsigned)get_order(size));
    if (!page)
        return NULL;
    *dma = page->dma;
    return page->address;
}

void dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t dma)
{
    (void)dev;
    (void)size;
    (void)dma;
    struct page *page = virt_to_page(cpu);
    if (!page || page != &page->pages->page[0]) {
        printk("nvidia-uvm: dma_free_coherent of %p, which dma_alloc_coherent did not return\n", cpu);
        return;
    }
    put_page(page);
}

/* ==========================================================================================
 * vmap: one mapping in nvrm of a list of pages
 * ======================================================================================== */

/* The mappings vmap made (vunmap releases only those). */
struct vmapping {
    struct list_head node;
    void *address;
    size_t bytes;
};
static mtx_t vmappings_lock;
static LIST_HEAD(vmappings);

void *vmap(struct page **pages, unsigned count, unsigned long flags, pgprot_t prot)
{
    (void)flags;
    (void)prot;
    if (!count)
        return NULL;
    /* The pages of one group in order are mapped in nvrm already, in that order. */
    struct uvm_os_pages *g = pages[0]->pages;
    bool in_order = g != NULL;
    for (unsigned i = 1; in_order && i < count; i++)
        in_order = pages[i] == pages[0] + i && pages[0] + i < g->page + g->count;
    if (in_order)
        return pages[0]->address;

    /* Otherwise a new shared object of the groups' pages, in runs. */
    struct n_shared_range *ranges = malloc(count * sizeof *ranges);
    struct vmapping *m = malloc(sizeof *m);
    if (!ranges || !m) {
        free(ranges);
        free(m);
        return NULL;
    }
    size_t runs = 0;
    for (unsigned i = 0; i < count; i++) {
        struct uvm_os_pages *pg = pages[i]->pages;
        if (!pg) {
            free(ranges);
            free(m);
            return NULL;
        }
        uint64_t offset = (uint64_t)(pages[i] - pg->page) * PAGE_SIZE;
        struct n_shared_range *last = runs ? &ranges[runs - 1] : NULL;
        if (last && last->shared == pg->memory.view && last->offset + last->bytes == offset) {
            last->bytes += PAGE_SIZE;
            continue;
        }
        ranges[runs++] = (struct n_shared_range){ .shared = pg->memory.view, .offset = offset, .bytes = PAGE_SIZE };
    }
    n_handle composed = n_shared_compose(ranges, runs, N_MEMORY_READ_WRITE);
    free(ranges);
    void *address = NULL;
    if (composed >= 0) {
        n_result mapped = n_shared_map(composed, (size_t)count * PAGE_SIZE, N_MEMORY_READ_WRITE, &address);
        n_handle_close(composed);
        if (mapped < 0)
            address = NULL;
    }
    if (!address) {
        printk("nvidia-uvm: vmap of %u pages failed (%s)\n", count,
               composed < 0 ? n_result_text(composed) : "no address space");
        free(m);
        return NULL;
    }
    m->address = address;
    m->bytes = (size_t)count * PAGE_SIZE;
    mtx_lock(&vmappings_lock);
    list_add(&m->node, &vmappings);
    mtx_unlock(&vmappings_lock);
    return address;
}

void vunmap(const void *addr)
{
    struct vmapping *m, *found = NULL;
    mtx_lock(&vmappings_lock);
    list_for_each_entry(m, &vmappings, node) {
        if (m->address == addr) {
            found = m;
            list_del(&m->node);
            break;
        }
    }
    mtx_unlock(&vmappings_lock);
    if (found) {
        n_memory_release(found->address, found->bytes);
        free(found);
    }
}

/* ==========================================================================================
 * The GPU's PCI description
 * ======================================================================================== */

static struct pci_dev gpu_pci;
static once_flag gpu_pci_once = ONCE_FLAG_INIT;

static void gpu_pci_fill(void)
{
    struct nvrm_uvm_pci info;
    nvrm_uvm_pci_info(&info);
    gpu_pci.vendor = info.vendor;
    gpu_pci.device = info.device;
    gpu_pci.subsystem_vendor = info.subsystem_vendor;
    gpu_pci.subsystem_device = info.subsystem_device;
    gpu_pci.domain = 0;
    gpu_pci.bus_number = info.bus;
    gpu_pci.devfn = (unsigned char)((info.dev << 3) | (info.func & 7));
    gpu_pci.dev.dma_mask = gpu_pci.dev.coherent_dma_mask = info.dma_mask;
    for (unsigned i = 0; i < 6; i++) {
        if (!info.bar[i].size)
            continue;
        gpu_pci.resource[i].start = info.bar[i].address;
        gpu_pci.resource[i].end = info.bar[i].address + info.bar[i].size - 1;
        gpu_pci.resource[i].flags = info.bar[i].is_64bit ? IORESOURCE_MEM_64 : 0;
    }
}

struct pci_dev *uvm_os_pci_dev(void)
{
    call_once(&gpu_pci_once, gpu_pci_fill);
    return &gpu_pci;
}

/* ==========================================================================================
 * Address spaces: a connection and its mappings
 * ======================================================================================== */

/* Which pages of a mapping are bound and how: per 2 MiB of the mapping, an access byte a page
   (0, N_MEMORY_READ or N_MEMORY_READ_WRITE). */
#define SHADOW_PAGES 512u
struct shadow_block {
    struct rb_node node;
    uint64_t index;           /* page / SHADOW_PAGES */
    unsigned bound;           /* pages with access */
    uint8_t access[SHADOW_PAGES];
};

struct uvm_os_vma {
    struct vm_area_struct area;   /* once committed */
    struct uvm_os_mm *owner;
    n_handle pager;
    n_handle memory;              /* kept until the client mapped it (commit) */
    uint64_t token, bytes;
    struct file *file;            /* a reference (vm_file once committed) */
    struct list_head pending;     /* in owner->pending until committed */
    bool committed, watched;
    mtx_t lock;                   /* the pager's bindings and the shadow */
    struct rb_root shadow;
};

struct uvm_os_mm {
    struct mm_struct mm;
    uint32_t principal;
    struct list_head pending;     /* uncommitted maps (mmap_lock) */
    uint64_t next_token;
    struct vm_area_struct arena;  /* the client's argument arena (no ops, no file) */
    bool has_arena;
    /* the fault thread */
    n_handle events;
    thrd_t fault_thread;
    bool fault_running;
    volatile uint32_t stop;
    struct task_struct fault_task;
};

static struct uvm_os_mm *mm_conn(struct mm_struct *mm)
{
    return container_of(mm, struct uvm_os_mm, mm);
}

/* ---- the shadow (os->lock held) ---- */

static struct shadow_block *shadow_block(struct uvm_os_vma *os, uint64_t index, bool create)
{
    struct rb_node **link = &os->shadow.rb_node, *parent = NULL;
    while (*link) {
        struct shadow_block *b = rb_entry(*link, struct shadow_block, node);
        parent = *link;
        if (index < b->index)
            link = &parent->rb_left;
        else if (index > b->index)
            link = &parent->rb_right;
        else
            return b;
    }
    if (!create)
        return NULL;
    struct shadow_block *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->index = index;
    rb_link_node(&b->node, parent, link);
    rb_insert_color(&b->node, &os->shadow);
    return b;
}

static uint8_t shadow_get(struct uvm_os_vma *os, uint64_t page)
{
    struct shadow_block *b = shadow_block(os, page / SHADOW_PAGES, false);
    return b ? b->access[page % SHADOW_PAGES] : 0;
}

static void shadow_set(struct shadow_block *b, uint64_t page, uint8_t access)
{
    uint8_t *slot = &b->access[page % SHADOW_PAGES];
    if (!*slot && access)
        b->bound++;
    else if (*slot && !access)
        b->bound--;
    *slot = access;
}

/* The first block at or after index, or NULL. */
static struct shadow_block *shadow_from(struct uvm_os_vma *os, uint64_t index)
{
    struct rb_node *node = os->shadow.rb_node;
    struct shadow_block *found = NULL;
    while (node) {
        struct shadow_block *b = rb_entry(node, struct shadow_block, node);
        if (b->index >= index) {
            found = b;
            node = node->rb_left;
        }
        else {
            node = node->rb_right;
        }
    }
    return found;
}

/* Pages [first, first + count) are unbound: write_only keeps them read-only. */
static void shadow_clear(struct uvm_os_vma *os, uint64_t first, uint64_t count, bool write_only)
{
    uint64_t end = first + count;
    struct shadow_block *b = shadow_from(os, first / SHADOW_PAGES);
    while (b && b->index * SHADOW_PAGES < end) {
        struct rb_node *next = rb_next(&b->node);
        uint64_t lo = max(first, b->index * SHADOW_PAGES), hi = min(end, (b->index + 1) * SHADOW_PAGES);
        for (uint64_t page = lo; page < hi; page++) {
            if (write_only && b->access[page % SHADOW_PAGES])
                shadow_set(b, page, N_MEMORY_READ);
            else if (!write_only)
                shadow_set(b, page, 0);
        }
        if (!b->bound) {
            rb_erase(&b->node, &os->shadow);
            free(b);
        }
        b = next ? rb_entry(next, struct shadow_block, node) : NULL;
    }
}

static void shadow_free(struct uvm_os_vma *os)
{
    struct rb_node *node;
    while ((node = rb_first(&os->shadow))) {
        rb_erase(node, &os->shadow);
        free(rb_entry(node, struct shadow_block, node));
    }
}

/* ---- binding pages ---- */

static int errno_of(n_result r)
{
    switch (-r) {
    case N_ERR_NO_MEMORY: return -ENOMEM;
    case N_ERR_CLOSED: return -EFAULT;
    case N_ERR_DENIED: return -EACCES;
    default: return -EINVAL;
    }
}

/* Binds `count` pages of group g from page `from` at byte `offset` of the mapping with access.
   refuse_bound: -EBUSY when one of them is bound already (vm_insert_page's rule). */
static int bind(struct uvm_os_vma *os, uint64_t offset, struct uvm_os_pages *g, size_t from, size_t count,
                uint32_t access, bool refuse_bound)
{
    if (!g || g->pinned || from > g->count || count > g->count - from)
        return -EINVAL;
    uint64_t first = offset >> PAGE_SHIFT;
    int result = 0;
    mtx_lock(&os->lock);
    /* Every block exists before the pages are bound, so the shadow cannot miss a binding. */
    for (uint64_t page = first; page < first + count; page++) {
        struct shadow_block *b = shadow_block(os, page / SHADOW_PAGES, true);
        if (!b) {
            result = -ENOMEM;
            break;
        }
        if (refuse_bound && b->access[page % SHADOW_PAGES]) {
            result = -EBUSY;
            break;
        }
    }
    if (!result) {
        struct n_pager_supply run = {
            .offset = offset,
            .bytes = count * PAGE_SIZE,
            .source_offset = from * PAGE_SIZE,
            .source = g->memory.view,
            .access = access,
        };
        n_result r = n_pager_supply(os->pager, &run, 1);
        if (r < 0) {
            result = errno_of(r);
            printk("nvidia-uvm: binding %zu pages at 0x%llx failed (%s)\n", count,
                   (unsigned long long)(os->area.vm_start + offset), n_result_text(r));
        }
        else {
            for (uint64_t page = first; page < first + count; page++)
                shadow_set(shadow_block(os, page / SHADOW_PAGES, false), page, (uint8_t)access);
        }
    }
    if (result) {
        /* blocks made for nothing go again */
        shadow_clear(os, first, 0, false);
        struct shadow_block *b = shadow_from(os, first / SHADOW_PAGES);
        while (b && b->index * SHADOW_PAGES < first + count) {
            struct rb_node *next = rb_next(&b->node);
            if (!b->bound) {
                rb_erase(&b->node, &os->shadow);
                free(b);
            }
            b = next ? rb_entry(next, struct shadow_block, node) : NULL;
        }
    }
    mtx_unlock(&os->lock);
    return result;
}

static uint32_t prot_access(pgprot_t prot)
{
    return (pgprot_val(prot) & _PAGE_RW) ? N_MEMORY_READ_WRITE : N_MEMORY_READ;
}

int vm_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{
    struct uvm_os_vma *os = vma->os;
    if (!os || addr < vma->vm_start || addr >= vma->vm_end || (addr & ~PAGE_MASK))
        return -EFAULT;
    struct uvm_os_pages *g = page ? page->pages : NULL;
    if (!g)
        return -EINVAL;
    return bind(os, addr - vma->vm_start, g, (size_t)(page - g->page), 1, prot_access(vma->vm_page_prot), true);
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot)
{
    struct uvm_os_vma *os = vma->os;
    size = PAGE_ALIGN(size);
    if (!os || (addr & ~PAGE_MASK) || addr < vma->vm_start || addr > vma->vm_end || size > vma->vm_end - addr)
        return -EINVAL;
    for (unsigned long done = 0; done < size;) {
        struct page *page = pfn_to_page(pfn + (done >> PAGE_SHIFT));
        if (!page)
            return -EINVAL;
        struct uvm_os_pages *g = page->pages;
        /* as many pages of this group as follow in order */
        size_t from = (size_t)(page - g->page);
        size_t count = min((size_t)((size - done) >> PAGE_SHIFT), g->count - from);
        int r = bind(os, addr + done - vma->vm_start, g, from, count, prot_access(prot), true);
        if (r)
            return r;
        done += count * PAGE_SIZE;
    }
    return 0;
}

/* Bytes [offset, offset + bytes) of the mapping lose their pages, everywhere. */
static void vma_unbind(struct uvm_os_vma *os, uint64_t offset, uint64_t bytes)
{
    mtx_lock(&os->lock);
    n_result r = n_pager_revoke(os->pager, offset, bytes, 0);
    if (r < 0 && r != -N_ERR_CLOSED)
        printk("nvidia-uvm: taking pages back at 0x%llx failed (%s)\n",
               (unsigned long long)(os->area.vm_start + offset), n_result_text(r));
    shadow_clear(os, offset >> PAGE_SHIFT, bytes >> PAGE_SHIFT, false);
    mtx_unlock(&os->lock);
}

void unmap_mapping_range(struct address_space *mapping, loff_t offset, loff_t len, int even_cows)
{
    (void)even_cows;
    if (!mapping || offset < 0)
        return;
    uint64_t start = (uint64_t)offset & PAGE_MASK;
    uint64_t end = len <= 0 || (uint64_t)len > UINT64_MAX - (uint64_t)offset - PAGE_SIZE
                       ? UINT64_MAX & PAGE_MASK
                       : PAGE_ALIGN((uint64_t)offset + (uint64_t)len);
    mtx_lock(&mapping->lock);
    struct vm_area_struct *vma;
    list_for_each_entry(vma, &mapping->mappings, mapping_node) {
        uint64_t file_start = (uint64_t)vma->vm_pgoff << PAGE_SHIFT;
        uint64_t file_end = file_start + (vma->vm_end - vma->vm_start);
        uint64_t lo = max(start, file_start), hi = min(end, file_end);
        if (lo < hi)
            vma_unbind(vma->os, lo - file_start, hi - lo);
    }
    mtx_unlock(&mapping->lock);
}

pgprot_t vm_get_page_prot(unsigned long vm_flags)
{
    if (vm_flags & VM_WRITE)
        return PAGE_SHARED;
    if (vm_flags & VM_READ)
        return PAGE_READONLY;
    return PAGE_NONE;
}

/* ---- the mappings of an address space (mmap_lock held) ---- */

struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
    struct rb_node *node = mm->vmas.rb_node;
    struct vm_area_struct *best = NULL;
    while (node) {
        struct vm_area_struct *v = rb_entry(node, struct vm_area_struct, mm_node);
        if (v->vm_end > addr) {
            best = v;
            if (v->vm_start <= addr)
                break;
            node = node->rb_left;
        }
        else {
            node = node->rb_right;
        }
    }
    return best;
}

struct vm_area_struct *find_vma_intersection(struct mm_struct *mm, unsigned long start, unsigned long end)
{
    struct vm_area_struct *v = find_vma(mm, start);
    return v && v->vm_start < end ? v : NULL;
}

static void vma_link(struct mm_struct *mm, struct vm_area_struct *vma)
{
    struct rb_node **link = &mm->vmas.rb_node, *parent = NULL;
    while (*link) {
        parent = *link;
        link = vma->vm_start < rb_entry(parent, struct vm_area_struct, mm_node)->vm_start ? &parent->rb_left
                                                                                          : &parent->rb_right;
    }
    rb_link_node(&vma->mm_node, parent, link);
    rb_insert_color(&vma->mm_node, &mm->vmas);
}

/* The committed mapping that starts at `start`. */
static struct vm_area_struct *vma_at(struct mm_struct *mm, unsigned long start)
{
    struct vm_area_struct *v = find_vma(mm, start);
    return v && v->vm_start == start && v->os ? v : NULL;
}

/* An uncommitted map, or a committed one whose commit failed: its memory and pager go. Any
   touch of a page not bound fails from then on, and a client that still maps it keeps nothing
   of the GPU's. */
static void os_free(struct uvm_os_vma *os)
{
    if (os->bytes)
        n_pager_revoke(os->pager, 0, os->bytes, 0);
    if (os->memory >= 0)
        n_handle_close(os->memory);
    n_handle_close(os->pager);
    if (os->file)
        fput(os->file);
    shadow_free(os);
    free(os);
}

/* A committed mapping ends (mmap_lock held for writing): UVM's close, then it leaves the
   address space and the file's mappings, and its memory loses every page, so nothing UVM frees
   later stays reachable by the client. */
static void vma_close(struct uvm_os_mm *conn, struct vm_area_struct *vma)
{
    struct uvm_os_vma *os = vma->os;
    if (vma->vm_ops && vma->vm_ops->close)
        vma->vm_ops->close(vma);
    rb_erase(&vma->mm_node, &conn->mm.vmas);
    struct address_space *mapping = vma->vm_file ? vma->vm_file->f_mapping : NULL;
    if (mapping) {
        mtx_lock(&mapping->lock);
        list_del(&vma->mapping_node);
        mtx_unlock(&mapping->lock);
    }
    else {
        list_del(&vma->mapping_node);
    }
    if (os->watched && conn->events >= 0)
        n_events_unwatch(conn->events, vma->vm_start);
    os_free(os);
}

/* ---- the fault thread ---- */

#define FAULTS_AT_ONCE 16
#define FAULT_TRIES 4

/* A touch of the client, as Linux would take it to the mapping's fault handler. */
static vm_fault_t run_fault(struct vm_area_struct *vma, unsigned long address, unsigned flags)
{
    if (!vma->vm_ops)
        return 0;   /* the arena: always mapped in the client */
    vm_fault_t (*handler)(struct vm_fault *) = (flags & FAULT_FLAG_WRITE) && vma->vm_ops->page_mkwrite &&
                                                       shadow_get(vma->os, (address - vma->vm_start) >> PAGE_SHIFT)
                                                   ? vma->vm_ops->page_mkwrite
                                                   : vma->vm_ops->fault;
    if (!handler)
        return VM_FAULT_SIGBUS;
    struct vm_fault vmf = {
        .vma = vma,
        .flags = flags,
        .address = address & PAGE_MASK,
        .real_address = address,
        .pgoff = vma->vm_pgoff + ((address - vma->vm_start) >> PAGE_SHIFT),
    };
    vm_fault_t result = handler(&vmf);
    if (vmf.page && !(result & VM_FAULT_ERROR)) {
        int r = vm_insert_page(vma, vmf.address, vmf.page);
        put_page(vmf.page);
        if (r && r != -EBUSY)
            result |= r == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS;
    }
    return result;
}

vm_fault_t handle_mm_fault(struct vm_area_struct *vma, unsigned long address, unsigned flags, void *regs)
{
    (void)regs;
    return run_fault(vma, address, flags);
}

/* Answers one fault the pager took: the handler binds the page, or the touch fails. */
static void fault_answer(struct vm_area_struct *vma, const struct n_pager_fault *f)
{
    struct uvm_os_vma *os = vma->os;
    uint64_t page = f->offset >> PAGE_SHIFT;
    bool write = f->access == N_MEMORY_READ_WRITE;
    unsigned flags = FAULT_FLAG_USER | FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_KILLABLE | (write ? FAULT_FLAG_WRITE : 0);
    vm_fault_t result = 0;
    for (int tries = 0; tries < FAULT_TRIES; tries++) {
        mtx_lock(&os->lock);
        uint8_t have = shadow_get(os, page);
        mtx_unlock(&os->lock);
        if (have >= f->access)
            return;   /* bound (that settled the touch) */
        if (tries)
            flags |= FAULT_FLAG_TRIED;
        result = run_fault(vma, vma->vm_start + f->offset, flags);
        if (result & VM_FAULT_ERROR)
            break;
    }
    mtx_lock(&os->lock);
    bool answered = shadow_get(os, page) >= f->access;
    mtx_unlock(&os->lock);
    if (answered)
        return;
    printk_ratelimited("nvidia-uvm: app %u's %s of 0x%llx fails (fault result 0x%x)\n", f->app,
                       write ? "write" : "read", (unsigned long long)(vma->vm_start + f->offset), result);
    n_pager_fail(os->pager, f->offset, PAGE_SIZE);
}

static void faults_serve(struct uvm_os_mm *conn, uint64_t start)
{
    mmap_read_lock(&conn->mm);
    struct vm_area_struct *vma = vma_at(&conn->mm, start);
    if (vma) {
        struct n_pager_fault faults[FAULTS_AT_ONCE];
        for (;;) {
            n_result got = n_pager_take(vma->os->pager, faults, FAULTS_AT_ONCE, 0);
            if (got <= 0)
                break;
            for (n_result i = 0; i < got; i++)
                fault_answer(vma, &faults[i]);
        }
    }
    mmap_read_unlock(&conn->mm);
}

/* The client mapped nothing of it any more and nvrm holds no handle: the mapping ends as at an
   unmap. */
static void mapping_gone(struct uvm_os_mm *conn, uint64_t start)
{
    mmap_write_lock(&conn->mm);
    struct vm_area_struct *vma = vma_at(&conn->mm, start);
    if (vma)
        vma_close(conn, vma);
    mmap_write_unlock(&conn->mm);
}

#define STOP_TOKEN 0   /* mappings' tokens are their start addresses, never 0 */

static int fault_main(void *arg)
{
    struct uvm_os_mm *conn = arg;
    struct task_struct *previous = uvm_os_task_enter(&conn->fault_task);
    nvrm_thread_process = conn->principal;
    n_result watching = n_events_watch_address(conn->events, &conn->stop, 0, STOP_TOKEN);
    while (watching >= 0 && !__atomic_load_n(&conn->stop, __ATOMIC_ACQUIRE)) {
        struct n_event events[16];
        n_result n = n_events_wait(conn->events, events, ARRAY_SIZE(events), -1);
        if (n < 0) {
            printk("nvidia-uvm: app %u's fault thread cannot wait (%s); its touches of UVM memory fail\n",
                   conn->principal, n_result_text(n));
            break;
        }
        for (n_result i = 0; i < n; i++) {
            if (events[i].token == STOP_TOKEN)
                continue;
            if (events[i].events & N_EVENT_RECEIVABLE)
                faults_serve(conn, events[i].token);
            if (events[i].events & N_EVENT_CLOSED)
                mapping_gone(conn, events[i].token);
        }
    }
    uvm_os_task_leave(previous);
    return 0;
}

static bool fault_thread_start(struct uvm_os_mm *conn)
{
    if (conn->fault_running)
        return true;
    if (conn->events < 0) {
        conn->events = n_events_create();
        if (conn->events < 0)
            return false;
    }
    if (thrd_create(&conn->fault_thread, fault_main, conn) != thrd_success)
        return false;
    conn->fault_running = true;
    return true;
}

void uvm_os_mm_stop_faults(struct mm_struct *mm)
{
    struct uvm_os_mm *conn = mm_conn(mm);
    if (!conn->fault_running || current == &conn->fault_task)
        return;
    __atomic_store_n(&conn->stop, 1, __ATOMIC_RELEASE);
    wake_address(&conn->stop, 0);
    thrd_join(conn->fault_thread, NULL);
    conn->fault_running = false;
}

/* ---- the connection's address space ---- */

struct mm_struct *uvm_os_mm_create(uint32_t principal)
{
    struct uvm_os_mm *conn = calloc(1, sizeof *conn);
    if (!conn)
        return NULL;
    struct mm_struct *mm = &conn->mm;
    init_rwsem(&mm->mmap_lock);
    atomic_set(&mm->mm_users, 1);
    atomic_set(&mm->mm_count, 1);
    mm->app = principal;
    mm->vmas = RB_ROOT;
    conn->principal = principal;
    INIT_LIST_HEAD(&conn->pending);
    conn->events = -1;
    conn->fault_task.pid = conn->fault_task.tgid = (pid_t)principal;
    conn->fault_task.mm = mm;
    snprintf(conn->fault_task.comm, sizeof conn->fault_task.comm, "uvm-fault-%u", principal);
    uint64_t address, bytes;
    if (nvrm_user_arena(&address, &bytes)) {
        struct vm_area_struct *arena = &conn->arena;
        arena->vm_start = address;
        arena->vm_end = address + bytes;
        arena->vm_mm = mm;
        arena->vm_flags = VM_READ | VM_WRITE | VM_SHARED | VM_MAYREAD | VM_MAYWRITE | VM_MAYSHARE;
        arena->vm_page_prot = vm_get_page_prot(arena->vm_flags);
        arena->vm_pgoff = address >> PAGE_SHIFT;
        INIT_LIST_HEAD(&arena->mapping_node);
        vma_link(mm, arena);
        conn->has_arena = true;
    }
    return mm;
}

int uvm_os_map(struct mm_struct *mm, struct file *file, uint64_t bytes, uint64_t *token, n_handle *memory)
{
    struct uvm_os_mm *conn = mm_conn(mm);
    if (!bytes || (bytes & ~PAGE_MASK) || bytes > TASK_SIZE)
        return -EINVAL;
    if (!file->f_op->mmap)
        return -ENODEV;
    struct uvm_os_vma *os = calloc(1, sizeof *os);
    if (!os)
        return -ENOMEM;
    os->pager = n_pager_create((size_t)bytes, &os->memory);
    if (os->pager < 0) {
        int r = errno_of(os->pager);
        free(os);
        return r;
    }
    os->owner = conn;
    os->bytes = bytes;
    os->file = get_file(file);
    mmap_write_lock(mm);
    os->token = ++conn->next_token;
    list_add_tail(&os->pending, &conn->pending);
    mmap_write_unlock(mm);
    *token = os->token;
    *memory = os->memory;
    return 0;
}

static struct uvm_os_vma *pending_take(struct uvm_os_mm *conn, uint64_t token)
{
    struct uvm_os_vma *os;
    list_for_each_entry(os, &conn->pending, pending) {
        if (os->token == token) {
            list_del_init(&os->pending);
            return os;
        }
    }
    return NULL;
}

int uvm_os_map_commit(struct mm_struct *mm, uint64_t token, uint64_t address)
{
    struct uvm_os_mm *conn = mm_conn(mm);
    mmap_write_lock(mm);
    struct uvm_os_vma *os = pending_take(conn, token);
    if (!os) {
        mmap_write_unlock(mm);
        return -ENOENT;
    }
    int r = 0;
    if (!address || (address & ~PAGE_MASK) || address > TASK_SIZE || os->bytes > TASK_SIZE - address)
        r = -EINVAL;
    else if (find_vma_intersection(mm, address, address + os->bytes))
        r = -EEXIST;
    if (r) {
        mmap_write_unlock(mm);
        os_free(os);
        return r;
    }
    struct vm_area_struct *vma = &os->area;
    vma->vm_start = address;
    vma->vm_end = address + os->bytes;
    vma->vm_mm = mm;
    vma->vm_flags = VM_READ | VM_WRITE | VM_SHARED | VM_MAYREAD | VM_MAYWRITE | VM_MAYSHARE;
    vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
    vma->vm_pgoff = address >> PAGE_SHIFT;   /* UVM requires a mapping's file offset to be its address */
    vma->vm_file = os->file;
    vma->os = os;
    INIT_LIST_HEAD(&vma->mapping_node);
    r = os->file->f_op->mmap(os->file, vma);
    if (r) {
        mmap_write_unlock(mm);
        os_free(os);   /* the file's mmap took no reference on success only */
        return r;
    }
    os->file = NULL;   /* vm_file's now */
    vma_link(mm, vma);
    struct address_space *mapping = vma->vm_file->f_mapping;
    if (mapping) {
        mtx_lock(&mapping->lock);
        list_add_tail(&vma->mapping_node, &mapping->mappings);
        mtx_unlock(&mapping->lock);
    }
    /* The client maps it now: from here on its mapping holds the memory. */
    n_handle_close(os->memory);
    os->memory = -1;
    os->committed = true;
    if (!fault_thread_start(conn)) {
        printk("nvidia-uvm: app %u: no fault thread; its mapping at 0x%llx is taken back\n", conn->principal,
               (unsigned long long)address);
        os->file = NULL;
        vma_close(conn, vma);
        mmap_write_unlock(mm);
        return -ENOMEM;
    }
    n_result watched = n_events_watch(conn->events, os->pager, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, vma->vm_start);
    os->watched = watched >= 0;
    mmap_write_unlock(mm);
    if (watched < 0) {
        printk("nvidia-uvm: app %u: the mapping at 0x%llx cannot be watched (%s)\n", conn->principal,
               (unsigned long long)address, n_result_text(watched));
        uvm_os_unmap(mm, address, 0);
        return -ENOMEM;
    }
    return 0;
}

int uvm_os_unmap(struct mm_struct *mm, uint64_t address, uint64_t token)
{
    struct uvm_os_mm *conn = mm_conn(mm);
    mmap_write_lock(mm);
    int r = 0;
    if (address) {
        struct vm_area_struct *vma = vma_at(mm, address);
        if (vma)
            vma_close(conn, vma);
        else
            r = -ENOENT;
        mmap_write_unlock(mm);
        return r;
    }
    struct uvm_os_vma *os = pending_take(conn, token);
    mmap_write_unlock(mm);
    if (!os)
        return -ENOENT;
    os_free(os);
    return 0;
}

/* The address space's last user went (Linux's exit_mmap): every mapping ends. Runs on a thread
   whose task has no mm, so UVM's close takes it as the process's end. */
static void exit_mmap(struct uvm_os_mm *conn)
{
    struct mm_struct *mm = &conn->mm;
    uvm_os_mm_stop_faults(mm);
    mmap_write_lock(mm);
    if (conn->has_arena) {
        rb_erase(&conn->arena.mm_node, &mm->vmas);
        conn->has_arena = false;
    }
    struct rb_node *node;
    while ((node = rb_first(&mm->vmas)))
        vma_close(conn, rb_entry(node, struct vm_area_struct, mm_node));
    struct uvm_os_vma *os;
    while ((os = list_first_entry_or_null(&conn->pending, struct uvm_os_vma, pending))) {
        list_del_init(&os->pending);
        os_free(os);
    }
    mmap_write_unlock(mm);
}

void mmput(struct mm_struct *mm)
{
    if (atomic_dec_and_test(&mm->mm_users)) {
        exit_mmap(mm_conn(mm));
        mmdrop(mm);
    }
}

void mmdrop(struct mm_struct *mm)
{
    if (!atomic_dec_and_test(&mm->mm_count))
        return;
    struct uvm_os_mm *conn = mm_conn(mm);
    if (conn->events >= 0)
        n_handle_close(conn->events);
    free(conn);
}

/* ==========================================================================================
 * Client memory: the request's arena
 * ======================================================================================== */

long get_user_pages_remote(struct mm_struct *mm, unsigned long start, unsigned long nr_pages,
                           unsigned gup_flags, struct page **pages, int *locked)
{
    (void)gup_flags;
    (void)locked;
    start &= PAGE_MASK;
    if (!mm || mm != current->mm || !nr_pages || nr_pages > (SIZE_MAX >> PAGE_SHIFT))
        return -EFAULT;
    struct uvm_os_mm *conn = mm_conn(mm);
    uint64_t bytes = (uint64_t)nr_pages << PAGE_SHIFT;
    if (!conn->has_arena || start < conn->arena.vm_start || start > conn->arena.vm_end ||
        bytes > conn->arena.vm_end - start)
        return -EFAULT;
    size_t offset;
    n_handle view = nvrm_user_view((const void *)start, (size_t)bytes, &offset);
    if (view < 0)
        return -EFAULT;
    struct uvm_os_pages *g = group_new(nr_pages);
    void *address = NULL;
    if (!g || n_shared_map(view, (size_t)bytes, N_MEMORY_READ_WRITE, &address) < 0) {
        free(g);
        n_handle_close(view);
        return -ENOMEM;
    }
    g->pinned = true;
    g->memory.cpu = address;
    g->memory.view = view;
    group_describe(g);
    atomic_set(&g->page[0].refcount, (int)min(nr_pages, (unsigned long)INT_MAX));
    group_insert(g);
    for (unsigned long i = 0; i < nr_pages; i++)
        pages[i] = &g->page[i];
    return (long)nr_pages;
}

unsigned long copy_from_user(void *to, const void __user *from, unsigned long n)
{
    if (!n)
        return 0;
    const void *at = nvrm_user_translate(from, n);
    if (!at)
        return n;
    memcpy(to, at, n);
    return 0;
}

unsigned long copy_to_user(void __user *to, const void *from, unsigned long n)
{
    if (!n)
        return 0;
    void *at = nvrm_user_translate(to, n);
    if (!at)
        return n;
    memcpy(at, from, n);
    return 0;
}
