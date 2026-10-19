/* NVIDIA RM on Nocturne: the OS side of the NVLink and NVSwitch libraries.
 *
 * RM links NVIDIA's nvlink library (and the NVSwitch library behind INCLUDE_NVSWITCH_LIB), and
 * both ask the host OS for memory, strings, time and a few identity answers. A GeForce RTX 4070
 * has neither NVLink nor an NVSwitch, so these run only for the lock/memory helpers the library
 * uses while it initialises; everything that needs a fabric manager, a client event channel or
 * DMA-mapped switch memory answers "not supported".
 */
#include "nvrm_os.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <os-interface.h>
#include <nvlink_errors.h>
#include <nvlink_os.h>
#include <export_nvswitch.h>

/* ---------------------------------------------------------------- nvlink: memory and strings */
void *nvlink_malloc(NvLength size) { return malloc(size); }
void nvlink_free(void *p) { free(p); }
void *nvlink_memset(void *dest, int value, NvLength size) { return memset(dest, value, size); }
void *nvlink_memcpy(void *dest, const void *src, NvLength size) { return memcpy(dest, src, size); }
char *nvlink_strcpy(char *dest, const char *src) { return strcpy(dest, src); }
NvLength nvlink_strlen(const char *s) { return strlen(s); }
int nvlink_strcmp(const char *a, const char *b) { return strcmp(a, b); }

/* ---------------------------------------------------------------- nvlink: misc */
void nvlink_assert(int expression) {
    if (!expression) nvrm_log("NVRM: nvlink assertion failed");
}
void nvlink_sleep(unsigned int ms) { os_delay((NvU32)ms); }
int nvlink_is_admin(void) { return nvrm_thread_admin; }
NvU64 nvlink_get_platform_time(void) { return nvrm_now_ns(); }
/* A fabric manager is a separate process that holds a capability file: no such thing here. */
NvlStatus nvlink_acquire_fabric_mgmt_cap(void *osPrivate, NvU64 capDescriptor) {
    (void)osPrivate; (void)capDescriptor;
    return NVL_ERR_NOT_SUPPORTED;
}
int nvlink_is_fabric_manager(void *osPrivate) { (void)osPrivate; return 0; }

/* ---------------------------------------------------------------- nvswitch: memory and strings */
void *nvswitch_os_malloc_trace(NvLength size, const char *file, NvU32 line) {
    (void)file; (void)line;
    return malloc(size);
}
void nvswitch_os_free(void *p) { free(p); }
NvLength nvswitch_os_strlen(const char *s) { return strlen(s); }
int nvswitch_os_strncmp(const char *a, const char *b, NvLength n) { return strncmp(a, b, n); }
void *nvswitch_os_memset(void *dest, int value, NvLength size) { return memset(dest, value, size); }
void *nvswitch_os_memcpy(void *dest, const void *src, NvLength size) { return memcpy(dest, src, size); }
NvU32 nvswitch_os_mem_read32(const volatile void *address) { return *(const volatile NvU32 *)address; }
void nvswitch_os_mem_write32(volatile void *address, NvU32 data) { *(volatile NvU32 *)address = data; }
int nvswitch_os_snprintf(char *out, NvLength size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(out, size, format, ap);
    va_end(ap);
    return n;
}
int nvswitch_os_vsnprintf(char *out, NvLength size, const char *format, va_list ap) {
    return vsnprintf(out, size, format, ap);
}

/* ---------------------------------------------------------------- nvswitch: log, time, identity */
void nvswitch_os_print(int log_level, const char *format, ...) {
    (void)log_level;
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    nvrm_log("NVRM: nvswitch: %s", line);
}
void nvswitch_os_report_error(void *os_handle, NvU32 error_code, const char *format, ...) {
    (void)os_handle;
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    nvrm_log("NVRM: nvswitch error 0x%x: %s", (unsigned)error_code, line);
}
void nvswitch_os_assert_log(const char *format, ...) {
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    nvrm_log("NVRM: nvswitch assertion: %s", line);
}
NvU64 nvswitch_os_get_platform_time(void) { return nvrm_now_ns(); }
/* The wall-clock time is not needed by the switch library on a GPU without switches; the
   monotonic time is the best this process has without a clock service. */
NvU64 nvswitch_os_get_platform_time_epoch(void) { return nvrm_now_ns(); }
void nvswitch_os_sleep(unsigned int ms) { os_delay((NvU32)ms); }
int nvswitch_os_is_admin(void) { return nvrm_thread_admin; }
int nvswitch_os_is_fabric_manager(void *osPrivate) { (void)osPrivate; return 0; }
NvlStatus nvswitch_os_acquire_fabric_mgmt_cap(void *osPrivate, NvU64 capDescriptor) {
    (void)osPrivate; (void)capDescriptor;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_get_os_version(NvU32 *major, NvU32 *minor, NvU32 *build) {
    if (major) *major = 0;
    if (minor) *minor = 0;
    if (build) *build = 0;
    return NVL_SUCCESS;
}
NvlStatus nvswitch_os_get_pid(NvU32 *pid) {
    if (pid) *pid = 0;
    return NVL_ERR_NOT_SUPPORTED;
}
NvBool nvswitch_os_is_uuid_in_blacklist(NvUuid *uuid) { (void)uuid; return NV_FALSE; }
void nvswitch_os_override_platform(void *os_handle, NvBool *rtlsim) {
    (void)os_handle;
    if (rtlsim) *rtlsim = NV_FALSE;
}
NvlStatus nvswitch_os_read_registry_dword(void *os_handle, const char *name, NvU32 *data) {
    (void)os_handle; (void)name; (void)data;
    return NVL_ERR_GENERIC; /* not found: the default stays */
}

/* ---------------------------------------------------------------- nvswitch: DMA, events */
NvlStatus nvswitch_os_alloc_contig_memory(void *os_handle, void **virt_addr, NvU32 size, NvBool force_dma32) {
    (void)os_handle; (void)virt_addr; (void)size; (void)force_dma32;
    return NVL_ERR_NOT_SUPPORTED;
}
void nvswitch_os_free_contig_memory(void *os_handle, void *virt_addr, NvU32 size) {
    (void)os_handle; (void)virt_addr; (void)size;
}
NvlStatus nvswitch_os_map_dma_region(void *os_handle, void *cpu_addr, NvU64 *dma_handle, NvU32 size,
                                     NvU32 direction) {
    (void)os_handle; (void)cpu_addr; (void)dma_handle; (void)size; (void)direction;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_unmap_dma_region(void *os_handle, void *cpu_addr, NvU64 dma_handle, NvU32 size,
                                       NvU32 direction) {
    (void)os_handle; (void)cpu_addr; (void)dma_handle; (void)size; (void)direction;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_set_dma_mask(void *os_handle, NvU32 dma_addr_width) {
    (void)os_handle; (void)dma_addr_width;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_sync_dma_region_for_cpu(void *os_handle, NvU64 dma_handle, NvU32 size, NvU32 direction) {
    (void)os_handle; (void)dma_handle; (void)size; (void)direction;
    return NVL_SUCCESS; /* x86 DMA is coherent */
}
NvlStatus nvswitch_os_sync_dma_region_for_device(void *os_handle, NvU64 dma_handle, NvU32 size, NvU32 direction) {
    (void)os_handle; (void)dma_handle; (void)size; (void)direction;
    return NVL_SUCCESS;
}
NvlStatus nvswitch_os_add_client_event(void *osHandle, void *osPrivate, NvU32 eventId) {
    (void)osHandle; (void)osPrivate; (void)eventId;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_remove_client_event(void *osHandle, void *osPrivate) {
    (void)osHandle; (void)osPrivate;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_notify_client_event(void *osHandle, void *osPrivate, NvU32 eventId) {
    (void)osHandle; (void)osPrivate; (void)eventId;
    return NVL_ERR_NOT_SUPPORTED;
}
NvlStatus nvswitch_os_get_supported_register_events_params(NvBool *many, NvBool *user_supplies) {
    if (many) *many = NV_FALSE;
    if (user_supplies) *user_supplies = NV_FALSE;
    return NVL_SUCCESS;
}
