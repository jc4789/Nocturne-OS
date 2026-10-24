/* UVM's OS layer in nvrm, the client part (uvm_os.h, uvm_nvrm.h): UVM's devices and files, and
 * the NVRM_UVM requests of nvrm's clients (include/nvrm_protocol.h).
 *
 * UVM registers its two devices (nvidia-uvm, nvidia-uvm-tools) with cdev_add as on Linux; here
 * that is a list a client's NVRM_UVM_OPEN looks the minor up in. A UVM file belongs to one
 * client connection, numbered there (what UVM's parameter blocks call an fd: fget looks it up in
 * the connection whose request this thread serves). Each connection is one UVM process: a task
 * (pid = the client app) and an address space (uvm_mm.c) in which its mappings of UVM memory are
 * made. A request runs as that task; when the connection ends, its address space goes first, as
 * at a process's exit, and then its files close.
 */
#include "uvm_os.h"
#include "nvrm_protocol.h"
#include "uvm_common.h"

/* ==========================================================================================
 * Devices
 * ======================================================================================== */

/* One cdev_add: the inode of each of its minors. */
struct registration {
    struct list_head node;
    struct cdev *cdev;
    unsigned count;
    struct inode inode[];
};

static mtx_t devices_lock;   /* zeroed: a plain mutex */
static LIST_HEAD(devices);
static atomic_t next_major = ATOMIC_INIT(0);

void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
    memset(cdev, 0, sizeof *cdev);
    cdev->ops = fops;
}

int cdev_add(struct cdev *cdev, dev_t dev, unsigned count)
{
    if (!count)
        return -EINVAL;
    struct registration *r = calloc(1, sizeof *r + count * sizeof(struct inode));
    if (!r)
        return -ENOMEM;
    cdev->dev = dev;
    cdev->count = count;
    r->cdev = cdev;
    r->count = count;
    for (unsigned i = 0; i < count; i++) {
        struct inode *inode = &r->inode[i];
        inode->i_rdev = dev + i;
        inode->i_cdev = cdev;
        address_space_init_once(&inode->i_data);
        inode->i_data.host = inode;
        inode->i_mapping = &inode->i_data;
    }
    mtx_lock(&devices_lock);
    list_add_tail(&r->node, &devices);
    mtx_unlock(&devices_lock);
    return 0;
}

/* UVM removes its devices at its stop, after every client's files closed (nvrm_uvm_stop runs
   once the request server ended), so no file refers to an inode freed here. */
void cdev_del(struct cdev *cdev)
{
    struct registration *r, *found = NULL;
    mtx_lock(&devices_lock);
    list_for_each_entry(r, &devices, node) {
        if (r->cdev == cdev) {
            found = r;
            list_del(&r->node);
            break;
        }
    }
    mtx_unlock(&devices_lock);
    free(found);
}

/* Device numbers only tell UVM's devices apart from each other here: a region is a new major. */
int alloc_chrdev_region(dev_t *dev, unsigned first_minor, unsigned count, const char *name)
{
    (void)count;
    (void)name;
    *dev = MKDEV((unsigned)atomic_inc_return(&next_major), first_minor);
    return 0;
}

void unregister_chrdev_region(dev_t first, unsigned count)
{
    (void)first;
    (void)count;
}

/* The inode of UVM's device with this minor (UVM numbers its devices by minor: 0 nvidia-uvm,
   1 nvidia-uvm-tools). */
static struct inode *device_inode(unsigned minor)
{
    struct registration *r;
    struct inode *found = NULL;
    mtx_lock(&devices_lock);
    list_for_each_entry(r, &devices, node) {
        unsigned first = MINOR(r->cdev->dev);
        if (minor >= first && minor - first < r->count) {
            found = &r->inode[minor - first];
            break;
        }
    }
    mtx_unlock(&devices_lock);
    return found;
}

/* ==========================================================================================
 * Connections and their files
 * ======================================================================================== */

struct nvrm_uvm_client {
    uint32_t principal;
    struct mm_struct *mm;
    struct task_struct task;      /* the connection's requests run as this task */
    struct list_head files;       /* struct file (files_node); only the connection's thread */
    int next_id;
};

/* The connection whose request this thread serves (fget). */
static _Thread_local struct nvrm_uvm_client *serving;

static struct file *file_of(struct nvrm_uvm_client *c, int id)
{
    struct file *f;
    list_for_each_entry(f, &c->files, files_node) {
        if (f->id == id)
            return f;
    }
    return NULL;
}

struct file *fget(unsigned fd)
{
    if (!serving || fd > INT_MAX)
        return NULL;
    struct file *f = file_of(serving, (int)fd);
    return f ? get_file(f) : NULL;
}

/* The last reference: UVM's release, then the file is gone. A file still open in its connection
   holds a reference, so this runs only after it left the connection. */
void fput(struct file *filp)
{
    if (!atomic_long_dec_and_test(&filp->f_count))
        return;
    if (filp->f_op->release)
        filp->f_op->release(filp->f_inode, filp);
    free(filp);
}

static NV_STATUS status_of(long rc)
{
    return rc ? errno_to_nv_status((int)rc) : NV_OK;
}

static NV_STATUS file_open(struct nvrm_uvm_client *c, unsigned minor, int32_t *id)
{
    struct inode *inode = device_inode(minor);
    if (!inode || !inode->i_cdev->ops)
        return NV_ERR_INVALID_ARGUMENT;
    if (c->next_id == INT32_MAX)
        return NV_ERR_INSUFFICIENT_RESOURCES;
    struct file *f = calloc(1, sizeof *f);
    if (!f)
        return NV_ERR_NO_MEMORY;
    f->f_op = inode->i_cdev->ops;
    f->f_inode = inode;
    f->f_mapping = inode->i_mapping;
    f->f_flags = O_RDWR;
    atomic_long_set(&f->f_count, 1);
    f->app = c->principal;
    f->id = ++c->next_id;
    int rc = f->f_op->open ? f->f_op->open(inode, f) : 0;
    if (rc) {
        free(f);
        return status_of(rc);
    }
    f->owner = c;
    list_add_tail(&f->files_node, &c->files);
    *id = f->id;
    return NV_OK;
}

/* The file leaves the connection; mappings of it keep it until they end. */
static void file_close(struct file *f)
{
    list_del(&f->files_node);
    f->owner = NULL;
    fput(f);
}

static struct nvrm_uvm_client *client_new(uint32_t principal)
{
    struct nvrm_uvm_client *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->principal = principal;
    INIT_LIST_HEAD(&c->files);
    c->task.pid = c->task.tgid = (pid_t)principal;
    snprintf(c->task.comm, sizeof c->task.comm, "app-%u", principal);
    c->mm = uvm_os_mm_create(principal);
    if (!c->mm) {
        free(c);
        return NULL;
    }
    c->task.mm = c->mm;
    return c;
}

/* ==========================================================================================
 * Requests
 * ======================================================================================== */

#define POLL_SLICE_MS 1000   /* the longest one NVRM_UVM_POLL waits (nvrm_protocol.h) */

static NV_STATUS poll_file(struct file *f, int64_t timeout_ms, uint32_t *events)
{
    if (!f->f_op->poll)
        return NV_ERR_NOT_SUPPORTED;
    uint64_t wait = timeout_ms < 0 || timeout_ms > POLL_SLICE_MS ? POLL_SLICE_MS : (uint64_t)timeout_ms;
    uint64_t deadline = n_clock_uptime_ms() + wait;
    for (;;) {
        poll_table pt = { 0 };
        unsigned mask = f->f_op->poll(f, &pt);
        uint64_t now = n_clock_uptime_ms();
        if (mask || !pt.queue || now >= deadline) {
            *events = mask;
            return NV_OK;
        }
        uvm_os_wait_queue_sleep(pt.queue, pt.sequence, (unsigned)(deadline - now));
    }
}

static NV_STATUS serve(struct nvrm_uvm_client *c, struct nvrm_uvm_request *r, n_handle *attach)
{
    struct file *f = NULL;
    switch (r->op) {
    case NVRM_UVM_CLOSE:
    case NVRM_UVM_IOCTL:
    case NVRM_UVM_MAP:
    case NVRM_UVM_POLL:
        f = file_of(c, r->file);
        if (!f)
            return NV_ERR_INVALID_ARGUMENT;
        break;
    default:
        break;
    }
    int rc;
    switch (r->op) {
    case NVRM_UVM_OPEN:
        return file_open(c, r->minor, &r->file);
    case NVRM_UVM_CLOSE:
        file_close(f);
        return NV_OK;
    case NVRM_UVM_IOCTL:
        if (!f->f_op->unlocked_ioctl)
            return NV_ERR_NOT_SUPPORTED;
        return status_of(f->f_op->unlocked_ioctl(f, r->cmd, (unsigned long)r->arg));
    case NVRM_UVM_MAP:
        rc = uvm_os_map(c->mm, f, r->bytes, &r->token, attach);
        if (rc)
            *attach = -1;
        return status_of(rc);
    case NVRM_UVM_MAP_COMMIT:
        return status_of(uvm_os_map_commit(c->mm, r->token, r->address));
    case NVRM_UVM_UNMAP:
        return status_of(uvm_os_unmap(c->mm, r->address, r->token));
    case NVRM_UVM_POLL:
        return poll_file(f, r->timeout_ms, &r->events);
    default:
        return NV_ERR_INVALID_ARGUMENT;
    }
}

n_result nvrm_uvm_request(struct nvrm_uvm_client **client, uint32_t principal, void *args, uint32_t size,
                          n_handle *attach)
{
    *attach = -1;
    if (size != sizeof(struct nvrm_uvm_request))
        return -N_ERR_INVALID;
    struct nvrm_uvm_request *r = args;
    r->events = 0;
    if (!nvrm_uvm_running()) {
        r->status = NV_ERR_NOT_SUPPORTED;
        return 0;
    }
    if (!*client) {
        *client = client_new(principal);
        if (!*client) {
            r->status = NV_ERR_NO_MEMORY;
            return 0;
        }
    }
    struct nvrm_uvm_client *c = *client;
    struct task_struct *previous = uvm_os_task_enter(&c->task);
    struct nvrm_uvm_client *previous_serving = serving;
    uint32_t previous_process = nvrm_thread_process;
    serving = c;
    nvrm_thread_process = principal;
    r->status = serve(c, r, attach);
    nvrm_thread_process = previous_process;
    serving = previous_serving;
    uvm_os_task_leave(previous);
    return 0;
}

/* As at a process's exit: the address space goes (its mappings end, UVM's close seeing no mm,
   as on a process's teardown), then the files close. */
void nvrm_uvm_client_end(struct nvrm_uvm_client *c)
{
    if (!c)
        return;
    c->task.mm = NULL;
    WRITE_ONCE(c->task.fatal_pending, true);
    struct task_struct *previous = uvm_os_task_enter(&c->task);
    struct nvrm_uvm_client *previous_serving = serving;
    uint32_t previous_process = nvrm_thread_process;
    serving = c;
    nvrm_thread_process = c->principal;
    mmput(c->mm);
    struct file *f;
    while ((f = list_first_entry_or_null(&c->files, struct file, files_node)))
        file_close(f);
    nvrm_thread_process = previous_process;
    serving = previous_serving;
    uvm_os_task_leave(previous);
    free(c);
}
