/* 試験の媒体のサービス "tests"（tests/testrun.c）の口。試験のプログラムが能力を借りる。 */
#ifndef TESTS_TESTRUN_H
#define TESTS_TESTRUN_H
#include "nocturne.h"
#include "nocturne_services.h"

struct testrun_request { uint32_t cap; }; /* enum n_cap。答えは int64 の結果で、添付が能力。 */

/* 試験の媒体の外では負の結果になる。 */
static inline n_handle test_cap(enum n_cap what) {
    n_handle channel = n_service_connect("tests");
    if (channel < 0) return channel;
    struct testrun_request q = {.cap = what};
    int64_t result = -N_ERR_CLOSED;
    n_handle cap = -1;
    n_result r = n_channel_send(channel, &q, sizeof q, -1);
    if (r >= 0) r = n_channel_receive(channel, &result, sizeof result, N_CHANNEL_WAIT, &cap);
    n_handle_close(channel);
    if (r < 0) return (n_handle)r;
    return result < 0 ? (n_handle)result : cap;
}
/* 試験のプログラムの n_pci_claim。 */
static inline n_handle test_pci_claim(uint8_t bus, uint8_t dev, uint8_t func) {
    static n_handle cap = -1;
    if (cap < 0) cap = test_cap(N_CAP_PCI_CLAIM);
    return n_pci_claim(cap, bus, dev, func);
}
#endif
