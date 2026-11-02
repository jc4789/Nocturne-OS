/* The settings' gate (sys/settings.h). */
#include "sys/settings.h"
#include "abi.h"
#include "authority_abi.h"
#include "fs/bootrecord.h"
#include "mm/heap.h"
#include "sys/authority.h"
#include "sys/syscall.h"
#include "sys/watch.h"

struct setting {
    uint32_t domain, key, type, flags;
    int64_t low, high;
    setting_put put;
    setting_get get;
};

#define SIZE_OF(type) ((type) == N_SETTING_ENDPOINT ? sizeof(struct n_sockaddr) \
                       : (type) == N_SETTING_MONITOR_MODE ? sizeof(struct n_setting_monitor_mode) : sizeof(int32_t))

static const struct setting settings[] = {
    {N_DOMAIN_BOOT, N_BOOT_IOMMU, N_SETTING_BOOL, N_SETTING_AT_START, 0, 1, boot_setting_put, boot_setting_get},
    {N_DOMAIN_BOOT, N_BOOT_ACPI_AML, N_SETTING_BOOL, N_SETTING_AT_START, 0, 1, boot_setting_put, boot_setting_get},
    {N_DOMAIN_BOOT, N_BOOT_SMP, N_SETTING_BOOL, N_SETTING_AT_START, 0, 1, boot_setting_put, boot_setting_get},
    {N_DOMAIN_BOOT, N_BOOT_RESUME, N_SETTING_BOOL, N_SETTING_AT_START, 0, 1, boot_setting_put, boot_setting_get},
    {N_DOMAIN_BOOT, N_BOOT_UNISOLATED_DRIVERS, N_SETTING_BOOL, N_SETTING_AT_START, 0, 1, boot_setting_put, boot_setting_get},
    {N_DOMAIN_DISPLAY, N_DISPLAY_MODE, N_SETTING_MONITOR_MODE, N_SETTING_LIST, 0, 0, screen_modes_setting_put, screen_modes_setting_get},
    {N_DOMAIN_DISPLAY, N_DISPLAY_HOLD_SECONDS, N_SETTING_INT, 0, 0, INT32_MAX, screen_hold_setting_put, screen_hold_setting_get},
    {N_DOMAIN_WINDOW, N_WINDOW_SMOOTH_GRADIENTS, N_SETTING_BOOL, 0, 0, 1, smooth_gradients_setting_put, smooth_gradients_setting_get},
    {N_DOMAIN_INPUT, N_INPUT_POINTER_SPEED, N_SETTING_INT, 0, -2, 2, pointer_speed_setting_put, pointer_speed_setting_get},
    {N_DOMAIN_POWER, N_POWER_PERFORMANCE_PREFERENCE, N_SETTING_INT, 0, 0, 255, perf_preference_setting_put, perf_preference_setting_get},
    {N_DOMAIN_NETWORK, N_NETWORK_DNS_SERVER, N_SETTING_ENDPOINT, 0, 0, 0, dns_setting_put, dns_setting_get},
    {N_DOMAIN_NETWORK, N_NETWORK_TCP_RX_BYTES, N_SETTING_INT, 0, 1, INT32_MAX, tcp_buffer_setting_put, tcp_buffer_setting_get},
    {N_DOMAIN_NETWORK, N_NETWORK_TCP_TX_BYTES, N_SETTING_INT, 0, 1, INT32_MAX, tcp_buffer_setting_put, tcp_buffer_setting_get},
};

static struct watched watches[N_DOMAIN_COUNT];

static const struct setting *setting_find(uint32_t domain, uint32_t key) {
    for (size_t i = 0; i < ARRAY_SIZE(settings); i++)
        if (settings[i].domain == domain && settings[i].key == key) return &settings[i];
    return NULL;
}

void settings_init(void) {
    for (unsigned d = 0; d < N_DOMAIN_COUNT; d++) watched_init(&watches[d], NULL);
}

void settings_changed(enum n_setting_domain domain) {
    watched_changed(&watches[domain]);
}

/* value in the kernel, of s's type: within its range. */
static int value_check(const struct setting *s, const void *value) {
    if (s->type == N_SETTING_BOOL || s->type == N_SETTING_INT) {
        int32_t v;
        memcpy(&v, value, sizeof v);
        return v < s->low || v > s->high ? -N_ERR_INVALID : 0;
    }
    if (s->type == N_SETTING_MONITOR_MODE) {
        const struct n_setting_monitor_mode *m = value;
        if (!m->monitor[0] || m->monitor[sizeof m->monitor - 1] || !m->width || !m->height || !m->refresh_mhz)
            return -N_ERR_INVALID;
    }
    return 0;
}

static int apply(const struct setting *s, const void *value) {
    int r = value_check(s, value);
    return r < 0 ? r : s->put(s->key, value);
}

int64_t settings_syscall_apply(int cap, uint32_t domain, uint32_t key, const void *uvalue, size_t len) {
    int r = cap_check(cap, N_CAP_SETTINGS_APPLY);
    if (r < 0) return r;
    const struct setting *s = setting_find(domain, key);
    if (!s) return -N_ERR_NOT_FOUND;
    if (len != SIZE_OF(s->type)) return -N_ERR_INVALID;
    union {
        int32_t i;
        struct n_sockaddr endpoint;
        struct n_setting_monitor_mode mode;
    } value;
    if (copy_from_user(&value, uvalue, len)) return -N_ERR_BAD_ADDRESS;
    return apply(s, &value);
}

/* The bytes of the value now, however many: what fits cap is copied. */
int64_t settings_syscall_effective(uint32_t domain, uint32_t key, void *uout, size_t cap) {
    const struct setting *s = setting_find(domain, key);
    if (!s) return -N_ERR_NOT_FOUND;
    int64_t n = s->get(s->key, NULL, 0);
    if (n <= 0 || !cap) return n;
    size_t room = (size_t)n;
    uint8_t *value = kmalloc(room);
    if (!value) return -N_ERR_NO_MEMORY;
    n = s->get(s->key, value, room);
    /* a list that grew meanwhile copies what it had */
    if (n > 0 && copy_to_user(uout, value, MIN(MIN((size_t)n, room), cap))) n = -N_ERR_BAD_ADDRESS;
    kfree(value);
    return n;
}

int64_t settings_syscall_describe(uint32_t domain, uint32_t key, void *uout) {
    const struct setting *s = setting_find(domain, key);
    if (!s) return -N_ERR_NOT_FOUND;
    struct n_setting_desc d = {.type = s->type, .size = SIZE_OF(s->type), .flags = s->flags, .low = s->low, .high = s->high};
    return copy_to_user(uout, &d, sizeof d) ? -N_ERR_BAD_ADDRESS : 0;
}

int64_t settings_watch(uint32_t domain) {
    if (domain >= N_DOMAIN_COUNT) return -N_ERR_INVALID;
    return watched_open(&watches[domain]);
}

#ifdef NOCTURNE_TEST_FLAGS
/* dns-server=<a.b.c.d>:<port>, which a test's own resolver is at. */
static void test_dns(void) {
    const char *at = test_text("dns-server");
    if (!at) return;
    uint32_t ip = 0, part = 0;
    for (unsigned i = 0; i < 4; i++) {
        for (part = 0; *at >= '0' && *at <= '9'; at++) part = part * 10 + (uint32_t)(*at - '0');
        ip = ip << 8 | part;
        if (*at++ != (i == 3 ? ':' : '.')) return;
    }
    for (part = 0; *at >= '0' && *at <= '9'; at++) part = part * 10 + (uint32_t)(*at - '0');
    struct n_sockaddr endpoint = {.ip = ip, .port = (uint16_t)part};
    apply(setting_find(N_DOMAIN_NETWORK, N_NETWORK_DNS_SERVER), &endpoint);
}

void settings_test_flags(void) {
    static const struct {
        const char *word;
        uint32_t domain, key;
    } numbers[] = {{"tcp-rx-bytes", N_DOMAIN_NETWORK, N_NETWORK_TCP_RX_BYTES},
                   {"tcp-tx-bytes", N_DOMAIN_NETWORK, N_NETWORK_TCP_TX_BYTES},
                   {"fbhold", N_DOMAIN_DISPLAY, N_DISPLAY_HOLD_SECONDS}};
    for (size_t i = 0; i < ARRAY_SIZE(numbers); i++) {
        uint64_t v;
        if (test_number(numbers[i].word, &v) && v <= INT32_MAX) {
            int32_t value = (int32_t)v;
            apply(setting_find(numbers[i].domain, numbers[i].key), &value);
        }
    }
    if (test_flag("fbsim10")) {
        int32_t on = 1;
        apply(setting_find(N_DOMAIN_WINDOW, N_WINDOW_SMOOTH_GRADIENTS), &on);
    }
    test_dns();
}
#else
void settings_test_flags(void) {}
#endif
