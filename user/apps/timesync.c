/* サービス timesync：時計を網で合わせる方針の持ち主。カーネルは UTC の時計と RTC を持ち、変える口を
   N_CAP_CLOCK で絞る。ここがいつ、どこに聞き、どう合わせるかを持つ。時間帯はカーネルに無く、表示する側が
   設定 N_TIME_ZONE_MINUTES を足す。

   SNTP（RFC 4330）の依頼側。聞く先は DHCP の言う NTP のサーバー、無ければ設定 N_TIME_SERVER の名前。
   次の期限まで事象の行列で眠るので、待機中は回らない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"

#define NS_PER_S 1000000000ll
#define NTP_PORT 123
#define NTP_BYTES 48
#define NTP_UNIX_OFFSET 2208988800ll      /* 1900 年から 1970 年までの秒（RFC 5905 6） */
#define NTP_ANSWER_WAIT (2 * NS_PER_S)    /* 答えを待つ間。過ぎたら次の住所へ進む */
#define POLL_MIN 64u                      /* RFC 5905 7.3：MINPOLL は 2^6 秒 */
#define POLL_MAX (1u << 17)               /* RFC 5905 7.3：MAXPOLL は 2^17 秒 */
#define STEP_THRESHOLD (128 * 1000000ll)  /* RFC 5905 11.3：STEPT は 128 ms。これ未満は緩やかに合わせる */
#define SLEW_PPM 500u                     /* RFC 5905 11.3：周波数の許容 MAXFREQ */

static n_handle clock_cap;

/* NTP の時刻（big endian の 64 位）と UNIX の ns。 */
static void wall_to_ntp(int64_t ns, uint8_t *out) {
    uint64_t seconds = (uint64_t)(ns / NS_PER_S + NTP_UNIX_OFFSET);
    uint32_t fraction = (uint32_t)(((uint64_t)(ns % NS_PER_S) << 32) / (uint64_t)NS_PER_S);
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(seconds >> (24 - 8 * i));
        out[4 + i] = (uint8_t)(fraction >> (24 - 8 * i));
    }
}
static int64_t ntp_to_wall(const uint8_t *p) {
    uint32_t seconds = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    uint32_t fraction = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    /* RFC 4330 3：上の位が 0 の値は 2036 年からの時代のもの */
    int64_t since_1900 = (int64_t)seconds + (seconds >> 31 ? 0 : 1ll << 32);
    return (since_1900 - NTP_UNIX_OFFSET) * NS_PER_S + (int64_t)(((uint64_t)fraction * (uint64_t)NS_PER_S) >> 32);
}

/* 一つのサーバーに聞く。時計の差（ns）を *offset へ。 */
static bool ask_server(uint32_t server, int64_t *offset) {
    n_handle socket = udp_socket(0);
    if (socket < 0) return false;
    uint8_t request[NTP_BYTES] = {0}, reply[NTP_BYTES];
    request[0] = 4 << 3 | 3; /* LI 0、版 4、mode 3（依頼側） */
    int64_t sent_at = n_clock_wall();
    wall_to_ntp(sent_at, request + 40);
    bool done = false;
    if (udp_sendto(socket, server, NTP_PORT, request, sizeof request) == sizeof request) {
        uint64_t until = n_clock_after(NTP_ANSWER_WAIT);
        for (;;) {
            struct n_sockaddr from;
            n_result n = udp_recvfrom(socket, reply, sizeof reply, until, &from);
            if (n < 0) break;
            if (n != NTP_BYTES || from.ip != server || from.port != NTP_PORT) continue;
            if (memcmp(reply + 24, request + 40, 8)) continue; /* origin が違う：別の問いの答え */
            int64_t received_at = n_clock_wall();
            /* RFC 4330 5：LI 3 は合っていない、mode 4 はサーバー、stratum 0 は断り、16 以上は同期していない */
            if (reply[0] >> 6 == 3 || (reply[0] & 7) != 4 || !reply[1] || reply[1] > 15) break;
            int64_t t2 = ntp_to_wall(reply + 32), t3 = ntp_to_wall(reply + 40);
            *offset = ((t2 - sent_at) + (t3 - received_at)) / 2;
            done = true;
            break;
        }
    }
    udp_close(socket);
    return done;
}

/* 聞く先の列（free する）。 */
static uint32_t *servers_list(size_t *count) {
    *count = 0;
    struct n_network_link *links = NULL;
    n_result link_count = n_network_links(&links);
    size_t total = 0;
    struct n_network_link *link = links;
    for (n_result i = 0; i < link_count; i++, link = n_network_link_next(link)) total += link->ntp_count;
    uint32_t *list = total ? malloc(total * sizeof *list) : NULL;
    if (list) {
        link = links;
        for (n_result i = 0; i < link_count; i++, link = n_network_link_next(link)) {
            const uint32_t *addresses = n_network_link_addresses(link);
            for (uint32_t a = 0; a < link->ntp_count; a++) list[(*count)++] = addresses[link->dns_count + a];
        }
    }
    if (link_count >= 0) free(links);
    if (*count) return list;
    free(list);
    char name[DNS_NAME_TEXT_BYTES + 1];
    n_result length = n_setting_get(n_setting_at(N_DOMAIN_TIME, N_TIME_SERVER), name, sizeof name - 1);
    if (length <= 0) return NULL;
    name[length] = 0;
    struct n_dns_result answer;
    uint32_t first;
    if (net_resolve_all(name, &first, 1, &answer) < 0 || !answer.count) return NULL;
    list = malloc(answer.count * sizeof *list);
    if (!list) return NULL;
    if (net_resolve_all(name, list, answer.count, &answer) < 0) {
        free(list);
        return NULL;
    }
    *count = answer.count;
    return list;
}

static bool rtc_written;

/* 差を時計へ。step した後と、起動して最初の同期の後に、次の秒の境目で RTC へ書く。 */
static void clock_apply(int64_t offset) {
    bool step = offset >= STEP_THRESHOLD || offset <= -STEP_THRESHOLD;
    n_result r = step ? n_clock_wall_step(clock_cap, n_clock_wall() + offset) : n_clock_wall_slew(clock_cap, offset, SLEW_PPM);
    if (r < 0) {
        printf("timesync: the clock refused the time (%s)\n", n_result_text(r));
        return;
    }
    if (step) printf("timesync: the clock moved %lld ms\n", (long long)(offset / 1000000));
    if (step || !rtc_written) {
        int64_t now = n_clock_wall();
        n_sleep_until(n_clock_after((uint64_t)(NS_PER_S - now % NS_PER_S)));
        r = n_rtc_write(clock_cap, n_clock_wall() / NS_PER_S);
        if (r < 0) printf("timesync: the RTC was not written (%s)\n", n_result_text(r));
        rtc_written = true;
    }
}

static bool synchronize(void) {
    size_t count;
    uint32_t *servers = servers_list(&count);
    bool done = false;
    for (size_t i = 0; i < count && !done; i++) {
        int64_t offset;
        if (ask_server(servers[i], &offset)) {
            clock_apply(offset);
            done = true;
        }
    }
    free(servers);
    return done;
}

static bool has_address(void) {
    n_result count = n_net_adapters(NULL, 0);
    if (count <= 0) return false;
    struct n_net_adapter *list = malloc((size_t)count * sizeof *list);
    if (!list) return false;
    count = n_net_adapters(list, (size_t)count);
    bool up = false;
    for (n_result i = 0; i < count; i++) up = up || list[i].ip;
    free(list);
    return up;
}

static bool sync_wanted(void) {
    int32_t on = 1;
    n_setting_get_int(n_setting_at(N_DOMAIN_TIME, N_TIME_SYNC), &on);
    return on != 0;
}

enum { TOKEN_CONTROL = 1, TOKEN_WATCH, TOKEN_SETTINGS, TOKEN_TIMER };

int main(void) {
    clock_cap = n_cap_find(N_CAP_CLOCK);
    n_handle queue = n_events_create(), net = n_net_watch();
    if (clock_cap < 0 || queue < 0 || net < 0) return 1;
    n_handle settings = n_setting_watch(N_DOMAIN_TIME, N_SETTING_ANY);
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_CONTROL);
    n_events_watch(queue, net, N_EVENT_RECEIVABLE, TOKEN_WATCH);
    if (settings >= 0) n_events_watch(queue, settings, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_SETTINGS);
    n_service_ready();
    bool wanted = sync_wanted(), up = has_address(), timer_set = false, again = wanted && up;
    uint32_t poll = POLL_MIN; /* 次に聞くまでの秒。同じ結果が続く間は倍々に、結果が変われば初めへ */
    bool last_done = true;
    for (;;) {
        if (again) {
            again = false;
            if (timer_set) n_events_unwatch(queue, TOKEN_TIMER);
            timer_set = false;
            if (wanted && up) {
                bool done = synchronize();
                if (done != last_done) poll = POLL_MIN;
                last_done = done;
                uint32_t wait = poll;
                if (poll < POLL_MAX) poll *= 2;
                n_events_timer(queue, n_clock_after((uint64_t)wait * (uint64_t)NS_PER_S), 0, TOKEN_TIMER);
                timer_set = true;
            }
        }
        struct n_event event;
        n_result got = n_events_wait(queue, &event, 1, N_CLOCK_NEVER);
        if (got < 0) return 1;
        if (got == 0) continue;
        switch (event.token) {
        case TOKEN_CONTROL: {
            n_handle stranger = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, 0);
            if (stranger == -N_ERR_CLOSED) return 0;
            if (stranger >= 0) n_handle_close(stranger);
            break;
        }
        case TOKEN_WATCH: {
            n_watch_take(net);
            bool now_up = has_address();
            if (now_up && !up) {
                poll = POLL_MIN;
                again = true;
            }
            up = now_up;
            break;
        }
        case TOKEN_SETTINGS: {
            if (event.events & N_EVENT_CLOSED) {
                n_handle_close(settings);
                settings = n_setting_watch(N_DOMAIN_TIME, N_SETTING_ANY);
                if (settings >= 0) n_events_watch(queue, settings, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_SETTINGS);
            } else {
                struct n_settings_changed changed;
                while (n_setting_watch_take(settings, &changed) >= 0) {}
            }
            wanted = sync_wanted();
            poll = POLL_MIN;
            again = true; /* 切るなら次の期限を消す。入れるか先を変えたなら今聞く */
            break;
        }
        case TOKEN_TIMER:
            timer_set = false;
            again = true;
            break;
        }
    }
}
