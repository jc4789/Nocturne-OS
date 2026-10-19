/* NVIDIA RM on Nocturne: the entry points the request server uses (nvrm_serve.h).
 *
 * A client of nvrm is what RM calls an open file of its control device: it gets an
 * nv_file_private_t, every escape goes to rm_ioctl on the control device's state, and closing
 * the client runs rm_cleanup_file_private, which frees every RM object the client still holds.
 */
#include "nvrm_os.h"

#include <stdlib.h>
#include <string.h>

#include <nv-ioctl.h>
#include <nv_escape.h>
#include <nv-unix-nvos-params-wrappers.h>
#include <nvos.h>

#define PAGE 4096ull

/* pLinearAddress of the last NV_ESC_RM_MAP_MEMORY this thread ran: its low 12 bits are the
   position of the mapping inside its first page (RM does not keep that in the parked context). */
static thread_local uint64_t last_linear;

void *nvrm_rm_file_open(void) {
    nv_file_private_t *nvfp = calloc(1, sizeof *nvfp);
    if (!nvfp) return NULL;
    nvfp->deviceInstance = NV_MAX_DEVICES;
    nvfp->gpuInstanceId = NV_U32_MAX;
    return nvfp;
}

void nvrm_rm_file_close(void *file) {
    nv_file_private_t *nvfp = file;
    if (!nvfp) return;
    rm_cleanup_file_private(nvrm_dev.sp, nv_get_ctl_state(), nvfp);
    free(nvfp);
}

uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    nvrm_drop_mapping();
    last_linear = 0;
    NV_STATUS status = rm_ioctl(nvrm_dev.sp, nv_get_ctl_state(), file, escape, args, size);
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd))
        last_linear = (uint64_t)((nv_ioctl_nvos33_parameters_with_fd *)args)->params.pLinearAddress;
    return status;
}

n_handle nvrm_rm_mapping_object(uint64_t *offset, uint64_t *bytes) {
    struct nvrm_mapping m;
    if (!nvrm_take_mapping(&m)) return -1;
    uint64_t in_page = last_linear & (PAGE - 1);
    n_handle h;
    uint64_t length;
    if (m.is_alloc) {
        uint64_t whole = nvrm_alloc_size(m.alloc);
        uint64_t start = m.page_index * PAGE;
        length = (in_page + m.size + PAGE - 1) & ~(PAGE - 1);
        if (start >= whole) return -1;
        if (length > whole - start) length = whole - start;
        h = nvrm_alloc_view(m.alloc, m.page_index, length, m.caching);
    } else {
        /* A BAR mapping is one run of pages; the ranges are already page aligned. */
        if (m.range_count != 1) {
            nvrm_log("NVRM: a mapping of %u separate ranges is not supported", (unsigned)m.range_count);
            return -1;
        }
        uint64_t view_offset = 0;
        length = m.ranges[0].size;
        h = nvrm_bar_view(m.ranges[0].start, length, m.caching, &view_offset);
        in_page += view_offset;
    }
    if (h < 0) {
        nvrm_log("NVRM: no shared object for the mapping: %s", n_result_text(h));
        return -1;
    }
    *offset = in_page;
    *bytes = length;
    return h;
}
