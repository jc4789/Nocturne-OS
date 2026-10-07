/* runtests: Nocturne's in-OS test suite. scripts/test.py puts tests/ on a fresh data disk; init
   runs /data/tests/autorun.sh at boot, which builds this runner with tcc and runs it.
   Output goes to /dev/kmsg (the serial port), one line per test:
     PASS name
     FAIL name: why
     SKIP name: why
   and finally "TESTS DONE pass=N fail=N skip=N". usage: runtests [-quick] [-nonet] [only-group...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "nocturne.h"

#define OUT "/home/test.out"
#define ANY (-1)     /* any exit status */
#define NONZERO (-2) /* must fail */

struct test {
    const char *group, *name, *cmd;
    int status;          /* expected exit status, or ANY / NONZERO */
    const char *want[4]; /* substrings the output must contain */
    const char *bad;     /* a substring it must not contain */
    int timeout_s;
    bool may_crash; /* child processes crash on purpose */
    bool show;      /* log the output even when the test passes (measurements) */
};

static const struct test tests[] = {
    /* shell */
    {"sh", "pipe", "echo hello world | grep world", 0, {"hello world"}},
    {"sh", "and-or", "false || echo A; true && echo B; false && echo C", NONZERO, {"A\nB\n"}, "C"},
    {"sh", "redirect", "echo abc > /home/r.txt; echo def >> /home/r.txt; cat < /home/r.txt", 0, {"abc\ndef\n"}},
    {"sh", "stderr-redirect", "ls /nonexistent 2> /home/e.txt; cat /home/e.txt", 0, {"nonexistent"}},
    {"sh", "exit-status", "sh -c 'exit 3'; echo status=$?", 0, {"status=3"}},
    {"sh", "quoting", "echo 'a  b' \"c  d\"", 0, {"a  b c  d"}},
    {"sh", "glob", "echo /etc/agent/*.md", 0, {"/etc/agent/system.md"}},
    {"sh", "script-args", "echo 'echo $0 got $# args: $1, $2, all: $@' > /home/s.sh; sh /home/s.sh one 'two too'", 0,
     {"/home/s.sh got 2 args: one, two too, all: one two too"}},
    {"sh", "script-by-name", "echo 'echo run by name' > /home/s2.sh; /home/s2.sh", 0, {"run by name"}},
    {"sh", "escaped-dollar", "echo \\$1 '$2'", 0, {"$1 $2"}},
    {"sh", "background", "sleep 1 & echo started", 0, {"started"}},
    {"sh", "missing-command", "no-such-command", NONZERO, {"not found"}},

    /* command-line tools */
    {"tools", "ls", "ls /bin", 0, {"tcc", "sh", "agent"}},
    {"tools", "ps", "ps", 0, {"init", "runtests"}},
    {"tools", "uname", "uname", 0, {"Nocturne"}},
    {"tools", "wc", "echo one two three | wc", 0, {"3"}},
    {"tools", "head", "head -n 2 /etc/agent/system.md | wc", 0, {"2"}},
    {"tools", "hexdump", "echo AB | hexdump", 0, {"41 42"}},
    {"tools", "dmesg", "dmesg | grep fat:", 0, {"NOCTDATA"}},
    {"tools", "lspci", "lspci", 0, {"8086:7010"}},
    {"tools", "mkdir-touch-rm", "mkdir /home/d && touch /home/d/f && ls /home/d && rm /home/d/f && rmdir /home/d && ls /home",
     0, {"f\n"}},
    {"tools", "cp-mv", "echo data > /home/a; cp /home/a /home/b; mv /home/b /home/c; cat /home/c; ls /home", 0, {"data"}, " b\n"},
    {"tools", "free-uptime-date", "free && uptime && date", 0, {"up"}},

    /* memory and protection */
    {"mem", "malloc-stress", "tcc -o /home/memtest /data/tests/memtest.c && /home/memtest", 0, {"memtest: ok"}, NULL, 120},
    {"mem", "heap-page-reclamation", "tcc -o /home/malloctrimtest /data/tests/malloctrimtest.c && /home/malloctrimtest", 0,
     {"malloctrimtest: ", ", 0 failed"}, "FAIL", 60},
    {"mem", "heap-failure-atomicity", "tcc -o /home/sbrktest /data/tests/sbrktest.c && /home/sbrktest", 0,
     {"sbrktest: ", ", 0 failed"}, "FAIL", 60, false, true},
    {"mem", "w^x", "tcc -o /home/wxtest /data/tests/wxtest.c && /home/wxtest", 0, {"wxtest: 0 failed"}, "FAIL", 60, true},

    /* filesystems */
    {"fs", "fat32", "tcc -o /home/fstest /data/tests/fstest.c && /home/fstest /data/fstest", 0, {"fstest: 0 failed"}, "FAIL", 180},
    {"fs", "ramfs", "/home/fstest /home/fsdir", 0, {"fstest: 0 failed"}, "FAIL", 120},

    /* the C compiler */
    {"tcc", "run", "tcc -run /usr/src/apps/echo.c one two", 0, {"one two"}},
    {"tcc", "errors", "echo 'int main(void) { return x; }' > /home/bad.c; tcc -c /home/bad.c", NONZERO, {"x"}},
    {"tcc", "math-and-printf", "tcc -run /data/tests/ctest.c", 0, {"ctest: ok"}},
    {"tcc", "compile-all-apps", "tcc -o /home/allapps /data/tests/allapps.c && /home/allapps", 0, {", 0 failed"}, NULL, 300},
    {"tcc", "self-host", "cd /data/tests/tcc && tcc -o /home/tcc2 tcc.c && /home/tcc2 -run /usr/src/apps/echo.c self-hosted",
     0, {"self-hosted"}, NULL, 300},

    /* desktop */
    {"gui", "window-and-screenshot", "tcc -o /home/guitest /data/tests/guitest.c && /home/guitest", 0, {"guitest: ok"}, NULL, 60},

    /* sound (scripts/test.py records the AC'97 output and checks the tones in it) */
    {"audio", "card", "dmesg | grep audio:", 0, {"AC'97"}},
    {"audio", "streams-beep-play", "tcc -o /home/audiotest /data/tests/audiotest.c && /home/audiotest", 0,
     {"audiotest: 0 failed"}, "FAIL", 60, false, true},

    /* the web engine, offline: layout, painting, forms, charsets, URLs, hostile input */
    {"web", "fonts", "tcc -o /home/fonttest /data/tests/fonttest.c && /home/fonttest", 0,
     {"fonttest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "engine", "tcc -o /home/webtest /data/tests/webtest.c && /home/webtest", 0, {"webtest: ", ", 0 failed"}, "FAIL",
     120, false, true},
    {"web", "javascript", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "html-tree-builder", "tcc -I/data/tests -o /home/lexbortest /data/tests/lexbortest.c && /home/lexbortest", 0,
     {"lexbortest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "http-transport", "tcc -o /home/httptest /data/tests/httptest.c && /home/httptest", 0,
     {"httptest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "cookies", "tcc -o /home/cookietest /data/tests/cookietest.c && /home/cookietest", 0,
     {"cookietest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "media-policy", "tcc -o /home/mediapolicytest /data/tests/mediapolicytest.c && /home/mediapolicytest", 0,
     {"mediapolicytest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "media-events", "tcc -o /home/mediatest /data/tests/mediatest.c && /home/mediatest", 0,
     {"mediatest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "mouse-boundaries", "tcc -o /home/hovertest /data/tests/hovertest.c && /home/hovertest /data/tests/js_hover_cases.js", 0,
     {"hovertest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "image-elements", "tcc -o /home/imagetest /data/tests/imagetest.c && /home/imagetest", 0,
     {"imagetest: 0 failures, 0 unexpected errors"}, "FAIL", 120, false, true},
    {"web", "image-ownership", "tcc -o /home/imageownershiptest /data/tests/imageownershiptest.c && /home/imageownershiptest", 0,
     {"imageownershiptest: ", ", 0 failures, 0 unexpected errors"}, "FAIL", 120, false, true},
    {"web", "metadata-invalidation", "tcc -I/data/tests -o /home/metadatatest /data/tests/metadatatest.c && /home/metadatatest", 0,
     {"metadatatest: 85 checks, 0 failures"}, "FAIL", 120, false, true},
    {"web", "message-channels", "tcc -I/data/tests -o /home/messagingtest /data/tests/messagingtest.c && /home/messagingtest", 0,
     {"messagingtest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "import-maps", "tcc -I/data/tests -o /home/js_importmaps_native /data/tests/js_importmaps_native.c && /home/js_importmaps_native", 0,
     {"js_importmaps_native: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "css-supports", "tcc -I/data/tests -o /home/csssupportstest /data/tests/csssupportstest.c && /home/csssupportstest", 0,
     {"csssupportstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "storage", "tcc -I/data/tests -o /home/storagetest /data/tests/storagetest.c && /home/storagetest --isolated-scratch", 0,
     {"storagetest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "worker-network", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},

    /* TCP against the host's test server (no internet needed), clean and with simulated loss */
    {"tcp", "bulk-and-loss", "tcc -o /home/tcptest /data/tests/tcptest.c && /home/tcptest", 0, {"tcptest: 0 failed"},
     "FAIL", 300, false, true},

    /* network (QEMU user networking: gateway 10.0.2.2, DNS 10.0.2.3) */
    {"net", "dhcp", "ifconfig", 0, {"10.0.2.15"}},
    {"net", "ping-gateway", "ping -c 2 10.0.2.2", 0, {"2 received"}, NULL, 20},
    {"net", "dns", "host example.com", 0, {"example.com"}, "not found", 20},
    {"net", "http", "fetch http://example.com", 0, {"Example Domain"}, NULL, 30},
    {"net", "https", "fetch https://example.com", 0, {"Example Domain"}, NULL, 30},
    {"net", "https-rejects-bad-cert", "fetch https://expired.badssl.com/", NONZERO, {"fetch:"}, NULL, 30},
    {"net", "webfetch-tls", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest --tls", 0,
     {"webnettest: 0 failed"}, "FAIL", 90, false, true},

    /* the AI agent, offline: no key is ever needed for the tests */
    {"agent", "usage", "agent --help", ANY, {"usage: agent"}},
    {"agent", "unreachable-endpoint",
     "echo 'endpoint=http://10.0.2.2:9/v1' > /home/bad.conf; echo 'api_key=test' >> /home/bad.conf; "
     "agent -c /home/bad.conf say hi",
     ANY, {"network error", "connect to 10.0.2.2:9"}, NULL, 150},
};

static int npass, nfail, nskip;

static void result(const char *verdict, const struct test *t, const char *why) {
    printf("%s %s/%s%s%s\n", verdict, t->group, t->name, why ? ": " : "", why ? why : "");
    fflush(stdout);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return strdup("");
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    size_t r;
    while ((r = fread(b + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) b = realloc(b, cap *= 2);
    }
    b[n] = 0;
    fclose(f);
    return b;
}

/* first line of the output, for failure messages */
static void snippet(const char *out, char *dst, size_t n) {
    size_t i = 0;
    for (; out[i] && i + 1 < n && i < 100; i++) dst[i] = out[i] == '\n' ? '|' : out[i];
    dst[i] = 0;
}

static void run(const struct test *t) {
    int nfail_before = nfail;
    int fd = open(OUT, O_WRONLY | O_CREAT | O_TRUNC);
    int nul = open("/dev/null", O_RDONLY);
    int fdmap[3] = {nul, fd, fd};
    char *argv[] = {"sh", "-c", (char *)t->cmd, NULL};
    uint64_t t0 = uptime_ms();
    int pid = spawn("/bin/sh", argv, fdmap, 0);
    close(fd);
    close(nul);
    if (pid < 0) {
        result("FAIL", t, "cannot spawn sh");
        nfail++;
        return;
    }
    int timeout = (t->timeout_s ? t->timeout_s : 15) * 1000, st = 0, r;
    while ((r = waitpid(pid, &st, WNOHANG)) == 0 && uptime_ms() - t0 < (uint64_t)timeout) msleep(20);
    char why[256];
    if (r == 0) {
        killtree(pid, 1);
        waitpid(pid, &st, 0);
        snprintf(why, sizeof why, "timed out after %d s", timeout / 1000);
        result("FAIL", t, why);
        nfail++;
        return;
    }
    char *out = slurp(OUT), snip[128];
    snippet(out, snip, sizeof snip);
    why[0] = 0;
    if (!t->may_crash && strstr(out, "*** ")) snprintf(why, sizeof why, "crashed: %s", strstr(out, "*** "));
    else if (t->status == NONZERO ? st == 0 : t->status != ANY && st != t->status)
        snprintf(why, sizeof why, "exit status %d: %s", st, snip);
    else {
        for (int i = 0; i < 4 && t->want[i]; i++)
            if (!strstr(out, t->want[i])) {
                snprintf(why, sizeof why, "output lacks \"%s\": %s", t->want[i], snip);
                break;
            }
        if (!why[0] && t->bad && strstr(out, t->bad)) snprintf(why, sizeof why, "output has \"%s\": %s", t->bad, snip);
    }
    for (char *p = why; *p; p++)
        if (*p == '\n') *p = '|';
    if (why[0]) {
        result("FAIL", t, why);
        nfail++;
    } else {
        snprintf(why, sizeof why, "%lu ms", (unsigned long)(uptime_ms() - t0));
        result("PASS", t, why);
        npass++;
    }
    if (nfail != nfail_before || t->show) {
        /* the whole output, indented, for the serial log */
        for (char *line = out, *nl; *line; line = nl ? nl + 1 : line + strlen(line)) {
            nl = strchr(line, '\n');
            printf("  | %.*s\n", nl ? (int)(nl - line) : (int)strlen(line), line);
        }
        fflush(stdout);
    }
    free(out);
}

int main(int argc, char **argv) {
    bool quick = false, nonet = false;
    const char *only[16];
    int nonly = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-quick")) quick = true;
        else if (!strcmp(argv[i], "-nonet")) nonet = true;
        else if (nonly < 16) only[nonly++] = argv[i];
    }
    struct n_stat st;
    bool have_tcc_src = stat("/data/tests/tcc/tcc.c", &st) == 0;
    printf("TESTS BEGIN\n");
    uint64_t t0 = uptime_ms();
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        const struct test *t = &tests[i];
        if (nonly) {
            bool hit = false;
            for (int k = 0; k < nonly; k++) hit |= !strcmp(only[k], t->group);
            if (!hit) continue;
        }
        const char *skip = NULL;
        if (nonet && !strcmp(t->group, "net")) skip = "network tests disabled";
        if (!strcmp(t->name, "self-host") && (quick || !have_tcc_src)) skip = "needs --full (tcc sources)";
        if (quick && !strcmp(t->name, "compile-all-apps")) skip = "quick run";
        if (skip) {
            result("SKIP", t, skip);
            nskip++;
            continue;
        }
        static bool net_waited;
        if (!net_waited && (!strcmp(t->group, "tcp") || !strcmp(t->group, "net") || !strcmp(t->name, "worker-network"))) {
            net_waited = true;
            if (!net_wait_up(20000)) printf("runtests: the network did not come up\n");
        }
        run(t);
    }
    printf("TESTS DONE pass=%d fail=%d skip=%d (%lu s)\n", npass, nfail, nskip, (unsigned long)((uptime_ms() - t0) / 1000));
    return nfail != 0;
}
