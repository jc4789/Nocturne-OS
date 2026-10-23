/* Scheduler, VM and driver stubs for the net.c harness. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "sys/sched.h"
#include "sys/object.h"
#include "sys/channel.h"
#include "net/net.h"

/* The handle table: the harnesses give net.c no app's handles. */
struct handle *handle_peek(int index) { (void)index; return NULL; }
struct handle *handle_ref(struct handle *h) { return h; }
void handle_close(struct handle *h) { (void)h; }
uint32_t handle_ready(struct handle *h) { (void)h; return 0; }
bool message_channel_is(struct handle *h) { (void)h; return false; }

void cpu_bsp_wake(void) {}
unsigned cpu_current_index(void) { return 0; }
bool e1000_probe(void) { return false; }
bool rtl8125_probe(void) { return false; }
bool tulip_probe(void) { return false; }
bool netvsc_probe(void) { return false; }
void entropy_get(void *buf, size_t n) {
    static uint64_t s = 0x9e3779b97f4a7c15ull;
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) { s = s * 6364136223846793005ull + 1442695040888963407ull; p[i] = (uint8_t)(s >> 56); }
}
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg) { (void)name; (void)fn; (void)arg; return NULL; }
void sched_yield(void) {}
bool user_ok(const void *p, size_t n) { return p || !n; }
bool user_ok_w(void *p, size_t n) { return p || !n; }
int user_str(char *dst, const char *src, size_t max) { strlcpy(dst, src, max); return 0; }
bool vmm_ap_uaccess_begin(void) { return true; }
void vmm_ap_uaccess_end(void) {}
uint64_t vmm_get_pte(uint64_t pml4, uint64_t va) { (void)pml4; (void)va; return 0; }
void wq_wake_all(struct wait_queue *wq) { (void)wq; }
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms) { (void)wq; timer_ticks += ms; return false; }
