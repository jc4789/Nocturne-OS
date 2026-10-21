/* NVIDIA RM on Nocturne: the OS callbacks Nocturne has no equivalent for, or that this build of
 * RM never needs.
 *
 * Each one answers the way RM expects from a platform without the feature: NV_ERR_NOT_SUPPORTED
 * for operations, NV_FALSE for "is X present", nothing for notifications. The list is the
 * "unsupported" table of ports/nvidia/README.md and is grouped the same way:
 *   - Tegra / SoC display (clocks, I2C, DCE, IMP, BPMP, brightness)
 *   - ACPI methods (Nocturne has no AML interpreter reachable from an app)
 *   - NUMA, struct page, user-page pinning, IOMMU/SVA, dma-buf, scatter-gather import, P2P
 *   - virtualisation (vGPU, GRID, VFIO), cgroups, IMEX channels, capability files
 *   - power management hooks (runtime suspend, S0ix, dynamic power)
 *   - file descriptors (clients are channels; RM's second-file lookups find nothing)
 */
#include "nvrm_os.h"

#include <stdlib.h>

#include <os-interface.h>

#define NOT_SUPPORTED NV_ERR_NOT_SUPPORTED

/* ---------------------------------------------------------------- events and file privates */
/* nv_post_event and nv_get_event (client events) are in rm_api.c, with the clients' queues. */
/* A client names a second open file by descriptor (export/import of object handles): there are
   no descriptors. */
nv_file_private_t *NV_API_CALL nv_get_file_private(NvS32 fd, NvBool ctl, void **priv) {
    (void)fd; (void)ctl; (void)priv;
    return NULL;
}
void NV_API_CALL nv_put_file_private(void *priv) { (void)priv; }

/* ---------------------------------------------------------------- mmap bookkeeping */
/* Live mapping publication/revocation and mmap locking belong to rm_api.c. */
/* A client reaches the GPU only once the GPU started (rm_init_adapter succeeded), as opening a
   GPU that failed to start fails. RM checks this when a client allocates the device
   (deviceConstruct: NV_ERR_INSUFFICIENT_PERMISSIONS); the control device's requests (the
   adapter status, system queries) stay available for diagnosis. */
NvBool NV_API_CALL nv_is_gpu_accessible(nv_state_t *nv) {
    return nv == &nvrm_dev.nv && nvrm_dev.adapter_ready;
}
NvBool NV_API_CALL nv_match_gpu_os_info(nv_state_t *nv, void *os_info) { (void)nv; (void)os_info; return NV_FALSE; }
void NV_API_CALL nv_flush_coherent_cpu_cache_range(nv_state_t *nv, NvU64 address, NvU64 size) {
    (void)nv; (void)address; (void)size; /* x86 is coherent */
}
void NV_API_CALL nv_get_updated_emu_seg(NvU32 *start, NvU32 *end) { (void)start; (void)end; }

/* ---------------------------------------------------------------- DMA mapping (no IOMMU) */
NV_STATUS NV_API_CALL nv_dma_map_alloc(nv_dma_device_t *d, NvU64 pages, NvU64 *pte, NvBool contig, NvBool cached,
                                       void **priv) {
    (void)d; (void)pages; (void)pte; (void)contig; (void)cached; (void)priv;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_dma_unmap_alloc(nv_dma_device_t *d, NvU64 pages, NvU64 *pte, void **priv) {
    (void)d; (void)pages; (void)pte; (void)priv;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_dma_map_peer(nv_dma_device_t *a, nv_dma_device_t *b, NvU8 bar, NvU64 size, NvU64 *address) {
    (void)a; (void)b; (void)bar; (void)size; (void)address;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_dma_unmap_peer(nv_dma_device_t *d, NvU64 address, NvU64 size) { (void)d; (void)address; (void)size; }
NV_STATUS NV_API_CALL nv_dma_map_mmio(nv_dma_device_t *d, NvU64 size, NvU64 *address) {
    (void)d; (void)size; (void)address;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_dma_unmap_mmio(nv_dma_device_t *d, NvU64 address, NvU64 size) { (void)d; (void)address; (void)size; }
void NV_API_CALL nv_dma_cache_invalidate(nv_dma_device_t *d, void *priv) { (void)d; (void)priv; }
void *NV_API_CALL nv_dma_get_dev_pagemap(NvU64 address) { (void)address; return NULL; }
void NV_API_CALL nv_dma_put_dev_pagemap(void *pagemap) { (void)pagemap; }
NV_STATUS NV_API_CALL nv_dma_init_sysmem_window_for_fabric_access(nv_state_t *nv, NvU64 a, NvU64 *b, NvU64 *c,
                                                                  NvBool *d) {
    (void)nv; (void)a; (void)b; (void)c; (void)d;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_dma_destroy_sysmem_window_for_fabric_access(nv_state_t *nv, NvU64 a, NvU64 b) {
    (void)nv; (void)a; (void)b;
}
NvBool NV_API_CALL nv_grdma_pci_topology_supported(nv_state_t *nv, nv_dma_device_t *d) { (void)nv; (void)d; return NV_FALSE; }

/* ---------------------------------------------------------------- importing foreign memory */
NV_STATUS NV_API_CALL nv_dma_import_sgt(nv_dma_device_t *d, struct sg_table *sgt, struct drm_gem_object *gem) {
    (void)d; (void)sgt; (void)gem;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_dma_release_sgt(struct sg_table *sgt, struct drm_gem_object *gem) { (void)sgt; (void)gem; }
NV_STATUS NV_API_CALL nv_dma_import_dma_buf(nv_dma_device_t *d, struct dma_buf *buf, NvBool ro, NvU32 *size,
                                            struct sg_table **sgt, nv_dma_buf_t **out) {
    (void)d; (void)buf; (void)ro; (void)size; (void)sgt; (void)out;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_dma_import_from_fd(nv_dma_device_t *d, NvS32 fd, NvBool ro, NvU32 *size,
                                            struct sg_table **sgt, nv_dma_buf_t **out) {
    (void)d; (void)fd; (void)ro; (void)size; (void)sgt; (void)out;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_dma_release_dma_buf(nv_dma_buf_t *buf) { (void)buf; }

NV_STATUS NV_API_CALL nv_register_user_pages(nv_state_t *nv, NvU64 pages, NvU64 *pte, void *user, void **priv,
                                             NvBool write) {
    (void)nv; (void)pages; (void)pte; (void)user; (void)priv; (void)write;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_unregister_user_pages(nv_state_t *nv, NvU64 pages, void **priv, void **pagelist) {
    (void)nv; (void)pages; (void)priv; (void)pagelist;
}
NV_STATUS NV_API_CALL nv_register_peer_io_mem(nv_state_t *nv, NvU64 *pte, NvU64 pages, void **priv) {
    (void)nv; (void)pte; (void)pages; (void)priv;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_unregister_peer_io_mem(nv_state_t *nv, void *priv) { (void)nv; (void)priv; }
NV_STATUS NV_API_CALL nv_register_sgt(nv_state_t *nv, NvU64 *pte, NvU64 pages, NvU32 cache, void **priv,
                                      struct sg_table *sgt, void *import, NvBool ro) {
    (void)nv; (void)pte; (void)pages; (void)cache; (void)priv; (void)sgt; (void)import; (void)ro;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_unregister_sgt(nv_state_t *nv, struct sg_table **sgt, void **import, void *priv) {
    (void)nv; (void)sgt; (void)import; (void)priv;
}
NV_STATUS NV_API_CALL nv_register_phys_pages(nv_state_t *nv, NvU64 *pte, NvU64 pages, NvU32 cache, void **priv) {
    (void)nv; (void)pte; (void)pages; (void)cache; (void)priv;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_unregister_phys_pages(nv_state_t *nv, void *priv) { (void)nv; (void)priv; }
NV_STATUS NV_API_CALL nv_schedule_uvm_drain_p2p(NvU8 *uuid) { (void)uuid; return NOT_SUPPORTED; }
void NV_API_CALL nv_schedule_uvm_resume_p2p(NvU8 *uuid) { (void)uuid; }

NV_STATUS NV_API_CALL os_lock_user_pages(void *address, NvU64 pages, void **pagelist, NvU32 flags) {
    (void)address; (void)pages; (void)pagelist; (void)flags;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_lookup_user_io_memory(void *address, NvU64 pages, NvU64 **pte) {
    (void)address; (void)pages; (void)pte;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_unlock_user_pages(NvU64 pages, void *pagelist, NvU32 flags) {
    (void)pages; (void)pagelist; (void)flags;
    return NOT_SUPPORTED;
}

/* ---------------------------------------------------------------- host pages, NUMA, GPU memory onlining */
NV_STATUS NV_API_CALL os_alloc_pages_node(NvS32 node, NvU32 size, NvU32 flags, NvU64 *address) {
    (void)node; (void)size; (void)flags; (void)address;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_get_page(NvU64 address) { (void)address; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL os_put_page(NvU64 address) { (void)address; return NOT_SUPPORTED; }
NvU32 NV_API_CALL os_get_page_refcount(NvU64 address) { (void)address; return 0; }
NvU32 NV_API_CALL os_count_tail_pages(NvU64 address) { (void)address; return 0; }
NV_STATUS NV_API_CALL os_offline_page_at_address(NvU64 address) { (void)address; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL os_numa_memblock_size(NvU64 *size) { (void)size; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL os_numa_add_gpu_memory(void *h, NvU64 start, NvU64 size, NvU32 *node) {
    (void)h; (void)start; (void)size; (void)node;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_numa_remove_gpu_memory(void *h, NvU64 start, NvU64 size, NvU32 node) {
    (void)h; (void)start; (void)size; (void)node;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_get_numa_node_memory_usage(NvS32 node, NvU64 *free_bytes, NvU64 *total_bytes) {
    (void)node; (void)free_bytes; (void)total_bytes;
    return NOT_SUPPORTED;
}
NvU64 NV_API_CALL os_get_reclaimable_memory_usage(void) { return 0; }
NV_STATUS NV_API_CALL nv_get_device_memory_config(nv_state_t *nv, NvU64 *a, NvU64 *b, NvU64 *c, NvU64 *d, NvU32 *e,
                                                  NvS32 *f) {
    (void)nv; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_get_egm_info(nv_state_t *nv, NvU64 *a, NvU64 *b, NvS32 *c) {
    (void)nv; (void)a; (void)b; (void)c;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_iommu_sva_bind(void *arg, void **handle, NvU32 *pasid) {
    (void)arg; (void)handle; (void)pasid;
    return NOT_SUPPORTED;
}
void NV_API_CALL os_iommu_sva_unbind(void *handle) { (void)handle; }

/* ---------------------------------------------------------------- files */
NV_STATUS NV_API_CALL os_allocate_temporary_file(void **file, NvU64 size) { (void)file; (void)size; return NOT_SUPPORTED; }
void NV_API_CALL os_close_file(void *file) { (void)file; }
NV_STATUS NV_API_CALL os_write_file(void *file, NvU8 *buffer, NvU64 size, NvU64 offset) {
    (void)file; (void)buffer; (void)size; (void)offset;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL os_read_file(void *file, NvU8 *buffer, NvU64 size, NvU64 offset) {
    (void)file; (void)buffer; (void)size; (void)offset;
    return NOT_SUPPORTED;
}

/* ---------------------------------------------------------------- platform queries */
/* RM asks for the boot method to decide about VBIOS shadow copies and the console. Nocturne
   boots from UEFI through Limine, and there is no BIOS to call into. */
NvBool NV_API_CALL os_is_efi_enabled(void) { return NV_TRUE; }
NV_STATUS NV_API_CALL os_get_smbios_header(NvU64 *address) { (void)address; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL os_get_acpi_rsdp_from_uefi(NvU32 *address) { (void)address; return NOT_SUPPORTED; }
void NV_API_CALL os_add_record_for_crashLog(void *record, NvU32 size) { (void)record; (void)size; }
void NV_API_CALL os_delete_record_for_crashLog(void *record) { (void)record; }
NvBool NV_API_CALL nv_is_chassis_notebook(void) { return NV_FALSE; }
NvBool NV_API_CALL os_supports_kernel_suspend_notifiers(void) { return NV_FALSE; }

/* ---------------------------------------------------------------- virtualisation, cgroups, caps */
NvBool NV_API_CALL os_is_vgx_hyper(void) { return NV_FALSE; }
NV_STATUS NV_API_CALL os_inject_vgx_msi(NvU16 guest, NvU64 address, NvU32 data) {
    (void)guest; (void)address; (void)data;
    return NOT_SUPPORTED;
}
NvBool NV_API_CALL os_is_grid_supported(void) { return NV_FALSE; }
NvU32 NV_API_CALL os_get_grid_csp_support(void) { return 0; }
NV_STATUS NV_API_CALL os_call_vgpu_vfio(void *a, NvU32 b) { (void)a; (void)b; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL os_device_vm_present(void) { return NOT_SUPPORTED; }
NvBool NV_API_CALL os_is_nvswitch_present(void) { return NV_FALSE; }
void *NV_API_CALL os_cgroup_for_pid(int pid, void *pid_info, int impl) { (void)pid; (void)pid_info; (void)impl; return NULL; }
void *NV_API_CALL os_get_pid_info(void) { return NULL; }
void NV_API_CALL os_put_pid_info(void *pid_info) { (void)pid_info; }
NV_STATUS NV_API_CALL os_find_ns_pid(void *pid_info, NvU32 *ns_pid) { (void)pid_info; (void)ns_pid; return NOT_SUPPORTED; }
NvBool NV_API_CALL os_is_init_ns(void) { return NV_TRUE; }
NvS32 NV_API_CALL os_imex_channel_get(NvU64 descriptor) { (void)descriptor; return -1; }
NvS32 NV_API_CALL os_imex_channel_count(void) { return 0; }
NV_STATUS NV_API_CALL nv_acquire_fabric_mgmt_cap(int fd, int *duped) { (void)fd; (void)duped; return NOT_SUPPORTED; }

/* RM's capability files (/proc/driver/nvidia/capabilities in the original): nvidia_caps_root is
   NULL, so RM skips creating them and none of these is reached. */
nv_cap_t *NV_API_CALL os_nv_cap_create_dir_entry(nv_cap_t *parent, const char *name, int mode) {
    (void)parent; (void)name; (void)mode;
    return NULL;
}
nv_cap_t *NV_API_CALL os_nv_cap_create_file_entry(nv_cap_t *parent, const char *name, int mode) {
    (void)parent; (void)name; (void)mode;
    return NULL;
}
void NV_API_CALL os_nv_cap_destroy_entry(nv_cap_t *cap) { (void)cap; }
int NV_API_CALL os_nv_cap_validate_and_dup_fd(const nv_cap_t *cap, int fd) { (void)cap; (void)fd; return -1; }
void NV_API_CALL os_nv_cap_close_fd(int fd) { (void)fd; }

/* ---------------------------------------------------------------- power management */
NV_STATUS NV_API_CALL nv_indicate_idle(nv_state_t *nv) { (void)nv; return NV_OK; }
NV_STATUS NV_API_CALL nv_indicate_not_idle(nv_state_t *nv) { (void)nv; return NV_OK; }
void NV_API_CALL nv_idle_holdoff(nv_state_t *nv) { (void)nv; }
NvBool NV_API_CALL nv_dynamic_power_available(nv_state_t *nv) { (void)nv; return NV_FALSE; }
void NV_API_CALL nv_audio_dynamic_power(nv_state_t *nv) { (void)nv; }
void NV_API_CALL nv_allow_runtime_suspend(nv_state_t *nv) { (void)nv; }
void NV_API_CALL nv_disallow_runtime_suspend(nv_state_t *nv) { (void)nv; }
NvBool NV_API_CALL nv_platform_supports_s0ix(void) { return NV_FALSE; }
NvBool NV_API_CALL nv_s2idle_pm_configured(void) { return NV_FALSE; }
void NV_API_CALL nv_pci_cxl_set_caching(nv_state_t *nv, NvBool enable) { (void)nv; (void)enable; }
NV_STATUS NV_API_CALL nv_pci_read_gpu_pdi_from_dvsec(nv_state_t *nv, NvU64 *pdi) { (void)nv; (void)pdi; return NOT_SUPPORTED; }

/* ---------------------------------------------------------------- ACPI */
void NV_API_CALL nv_acpi_methods_init(NvU32 *handlesPresent) { if (handlesPresent) *handlesPresent = 0; }
void NV_API_CALL nv_acpi_methods_uninit(void) {}
NV_STATUS NV_API_CALL nv_acpi_d3cold_dsm_for_upstream_port(nv_state_t *nv, NvU8 *guid, NvU32 rev, NvU32 func,
                                                          NvU32 *status) {
    (void)nv; (void)guid; (void)rev; (void)func; (void)status;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_acpi_dsm_method(nv_state_t *nv, NvU8 *guid, NvU32 rev, NvBool a, NvU32 func, void *in,
                                         NvU16 in_size, NvU32 *status, void *out, NvU16 *out_size) {
    (void)nv; (void)guid; (void)rev; (void)a; (void)func; (void)in; (void)in_size; (void)status; (void)out;
    (void)out_size;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_acpi_ddc_method(nv_state_t *nv, void *edid, NvU32 *size, NvBool read_short) {
    (void)nv; (void)edid; (void)size; (void)read_short;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_acpi_dod_method(nv_state_t *nv, NvU32 *devices, NvU32 *count) {
    (void)nv; (void)devices; (void)count;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_acpi_rom_method(nv_state_t *nv, NvU32 *offset, NvU32 *size) {
    (void)nv; (void)offset; (void)size;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_acpi_get_powersource(NvU32 *source) { (void)source; return NOT_SUPPORTED; }
NvBool NV_API_CALL nv_acpi_is_battery_present(void) { return NV_FALSE; }
NV_STATUS NV_API_CALL nv_acpi_mux_method(nv_state_t *nv, NvU32 *mode, NvU32 mux, const char *method) {
    (void)nv; (void)mode; (void)mux; (void)method;
    return NOT_SUPPORTED;
}

/* ---------------------------------------------------------------- I2C */
void *NV_API_CALL nv_i2c_add_adapter(nv_state_t *nv, NvU32 port) { (void)nv; (void)port; return NULL; }
void NV_API_CALL nv_i2c_del_adapter(nv_state_t *nv, void *adapter) { (void)nv; (void)adapter; }
NV_STATUS NV_API_CALL nv_i2c_transfer(nv_state_t *nv, NvU32 port, NvU8 address, nv_i2c_msg_t *msgs, int count) {
    (void)nv; (void)port; (void)address; (void)msgs; (void)count;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_i2c_unregister_clients(nv_state_t *nv) { (void)nv; }
NV_STATUS NV_API_CALL nv_i2c_bus_status(nv_state_t *nv, NvU32 port, NvS32 *scl, NvS32 *sda) {
    (void)nv; (void)port; (void)scl; (void)sda;
    return NOT_SUPPORTED;
}

/* ---------------------------------------------------------------- Tegra / SoC (not a PCI GPU) */
NV_STATUS NV_API_CALL nv_enable_clk(nv_state_t *nv, TEGRASOC_WHICH_CLK clk) { (void)nv; (void)clk; return NOT_SUPPORTED; }
void NV_API_CALL nv_disable_clk(nv_state_t *nv, TEGRASOC_WHICH_CLK clk) { (void)nv; (void)clk; }
NV_STATUS NV_API_CALL nv_get_max_freq(nv_state_t *nv, TEGRASOC_WHICH_CLK clk, NvU32 *khz) {
    (void)nv; (void)clk; (void)khz;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_set_freq(nv_state_t *nv, TEGRASOC_WHICH_CLK clk, NvU32 khz) {
    (void)nv; (void)clk; (void)khz;
    return NOT_SUPPORTED;
}
void NV_API_CALL nv_control_soc_irqs(nv_state_t *nv, NvBool enable) { (void)nv; (void)enable; }
void NV_API_CALL nv_get_disp_smmu_stream_ids(nv_state_t *nv, NvU32 *iso, NvU32 *niso) {
    (void)nv;
    if (iso) *iso = 0;
    if (niso) *niso = 0;
}
NV_STATUS NV_API_CALL nv_get_num_dpaux_instances(nv_state_t *nv, NvU32 *count) {
    (void)nv; (void)count;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_get_syncpoint_aperture(NvU32 id, NvU64 *base, NvU64 *size, NvU32 *offset) {
    (void)id; (void)base; (void)size; (void)offset;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_get_tegra_brightness_level(nv_state_t *nv, NvU32 *level) { (void)nv; (void)level; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL nv_set_tegra_brightness_level(nv_state_t *nv, NvU32 level) { (void)nv; (void)level; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL nv_bpmp_send_mrq(nv_state_t *nv, NvU32 mrq, const void *in, NvU32 in_size, void *out,
                                       NvU32 out_size, NvS32 *ret, NvS32 *api_ret) {
    (void)nv; (void)mrq; (void)in; (void)in_size; (void)out; (void)out_size; (void)ret; (void)api_ret;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_imp_get_import_data(TEGRA_IMP_IMPORT_DATA *data) { (void)data; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL nv_imp_get_uefi_data(nv_state_t *nv, NvU32 *iso, NvU32 *floor) {
    (void)nv; (void)iso; (void)floor;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_imp_enable_disable_rfl(nv_state_t *nv, NvBool enable) { (void)nv; (void)enable; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL nv_imp_icc_set_bw(nv_state_t *nv, NvU32 avg, NvU32 floor) {
    (void)nv; (void)avg; (void)floor;
    return NOT_SUPPORTED;
}
NvU32 NV_API_CALL nv_tegra_get_rm_interface_type(NvU32 clientId) { (void)clientId; return 0; }
NV_STATUS NV_API_CALL nv_tegra_dce_register_ipc_client(NvU32 interface, void *context, nvTegraDceClientIpcCallback cb,
                                                       NvU32 *id) {
    (void)interface; (void)context; (void)cb; (void)id;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_tegra_dce_client_ipc_send_recv(NvU32 id, void *message, NvU32 size) {
    (void)id; (void)message; (void)size;
    return NOT_SUPPORTED;
}
NV_STATUS NV_API_CALL nv_tegra_dce_unregister_ipc_client(NvU32 id) { (void)id; return NOT_SUPPORTED; }
NvBool NV_API_CALL nv_pci_tegra_pm_init(nv_state_t *nv) { (void)nv; return NV_FALSE; }
void NV_API_CALL nv_pci_tegra_pm_deinit(nv_state_t *nv) { (void)nv; }
NV_STATUS NV_API_CALL os_get_tegra_platform(NvU32 *platform) { (void)platform; return NOT_SUPPORTED; }
NV_STATUS NV_API_CALL os_tegra_igpu_perf_boost(void *handle, NvBool boost, NvU32 duration, int type) {
    (void)handle; (void)boost; (void)duration; (void)type;
    return NOT_SUPPORTED;
}
