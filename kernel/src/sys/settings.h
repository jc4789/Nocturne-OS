/* What the kernel is told to go by (docs/nocturne-native-abi.md §12, common/settings_abi.h). The
   owner of each value is the part that uses it: it holds the value, its default until told, and
   applies it at once. This gate knows which owner has which key and each value's type and range;
   nothing here is kept on C:, and no text is read. */
#pragma once
#include "kernel.h"
#include "settings_abi.h"

/* An owner's side of a key. value: a copy in the kernel, of the key's type, its range checked.
   get: the bytes of the value now, into out when they fit cap (out NULL, cap 0: only their number).
   A put may sleep, and is not called with a lock held. */
typedef int (*setting_put)(uint32_t key, const void *value);
typedef int64_t (*setting_get)(uint32_t key, void *out, size_t cap);

static inline int64_t setting_int(void *out, size_t cap, int32_t v) {
    if (cap >= sizeof v) memcpy(out, &v, sizeof v);
    return sizeof v;
}

/* Before the owners may say anything changed (the start of kmain_late). */
void settings_init(void);
/* The test build's flags for keys (the kernel-test build only): the owners are all up. */
void settings_test_flags(void);
/* What SYS_SETTINGS_EFFECTIVE reports of the domain changed, whoever changed it. Any CPU, any
   context: as watched_changed (sys/watch.h). */
void settings_changed(enum n_setting_domain domain);

/* The system calls. */
int64_t settings_syscall_apply(int cap, uint32_t domain, uint32_t key, const void *value, size_t len);
int64_t settings_syscall_effective(uint32_t domain, uint32_t key, void *out, size_t cap);
int64_t settings_syscall_describe(uint32_t domain, uint32_t key, void *out);
int64_t settings_watch(uint32_t domain);

/* The owners. */
int screen_hold_setting_put(uint32_t key, const void *value);
int64_t screen_hold_setting_get(uint32_t key, void *out, size_t cap);
int smooth_gradients_setting_put(uint32_t key, const void *value);
int64_t smooth_gradients_setting_get(uint32_t key, void *out, size_t cap);
int pointer_speed_setting_put(uint32_t key, const void *value);
int64_t pointer_speed_setting_get(uint32_t key, void *out, size_t cap);
int perf_preference_setting_put(uint32_t key, const void *value);
int64_t perf_preference_setting_get(uint32_t key, void *out, size_t cap);
int dns_setting_put(uint32_t key, const void *value);
int64_t dns_setting_get(uint32_t key, void *out, size_t cap);
int tcp_buffer_setting_put(uint32_t key, const void *value);
int64_t tcp_buffer_setting_get(uint32_t key, void *out, size_t cap);
