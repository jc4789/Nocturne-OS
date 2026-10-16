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
#include "nocturne.h"

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
    /* Focused product routes for the eight WHATWG repairs. */
    {"slop8", "script-lifecycle", "tcc -o /home/scriptlifecycletest /data/tests/scriptlifecycletest.c && /home/scriptlifecycletest", 0,
     {"scriptlifecycletest: 22 checks, 0 failed"}, "FAIL", 60, false, true},
    {"slop8", "storage-windows", "tcc -o /home/storagewindowtest /data/tests/storagewindowtest.c && /home/storagewindowtest", 0,
     {"STORAGE-WINDOW-DONE", "failures=0"}, "FAIL", 120, false, true},
    {"wasm", "native-webassembly-js-and-streaming", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest wasm", 0,
     {"WebAssembly checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasm", "native-webassembly-spec-boundaries", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest wasm-boundary", 0,
     {"WebAssembly boundary checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmcli", "wasm2c-tcc-native-executable", "tcc -o /home/wasm2ctest /data/tests/wasm2ctest.c && /home/wasm2ctest", 0,
     {"WASM2C_NATIVE_RESULT 42", "PASS wasm2ctest:"}, "FAIL", 180, false, true},
    {"wasmjspi", "native-webassembly-promise-integration", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest wasm-jspi", 0,
     {"WebAssembly JSPI checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmworker", "native-webassembly-dedicated-worker", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest wasm-worker", 0,
     {"WebAssembly worker checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmpolicy", "native-webassembly-csp-window", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest wasm-policy", 0,
     {"WebAssembly policy checks ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmpolicy", "native-webassembly-csp-http-worker", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest wasm-worker-policy", 0,
     {"WebAssembly worker policy checks ", ", 0 failed"}, "FAIL", 180, false, true},
    {"slop8", "dom-events-observers", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest dom-api", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"slop8", "network-stream", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest network-stream", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"slop8", "fetch-regression", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest fetch-api", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"slop8", "xhr-regression", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest xhr", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"slop8", "worker-events", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest html-worker", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
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
    {"mem", "cpu-capabilities", "tcc -o /home/cpuinfotest /data/tests/cpuinfotest.c && /home/cpuinfotest", 0,
     {"cpuinfotest: ", "; ok"}, "invalid", 60},
    {"mem", "heap-page-reclamation", "tcc -o /home/malloctrimtest /data/tests/malloctrimtest.c && /home/malloctrimtest", 0,
     {"malloctrimtest: ", ", 0 failed"}, "FAIL", 60},
    {"mem", "heap-failure-atomicity", "tcc -o /home/memreservetest /data/tests/memreservetest.c && /home/memreservetest", 0,
     {"memreservetest: ", ", 0 failed"}, "FAIL", 60, false, true},
    {"mem", "w^x", "tcc -o /home/wxtest /data/tests/wxtest.c && /home/wxtest", 0, {"wxtest: 0 failed"}, "FAIL", 60, true},

    /* filesystems */
    {"fs", "fat32", "tcc -o /home/fstest /data/tests/fstest.c && /home/fstest /data/fstest", 0, {"fstest: 0 failed"}, "FAIL", 180},
    {"fs", "ramfs", "/home/fstest /home/fsdir", 0, {"fstest: 0 failed"}, "FAIL", 120},

    /* Host cross-built C++ application, using the native C startup and APIs. */
    {"cxx", "native-runtime", "cppdemo --self-test", 0,
     {"CPP_SELFTEST_PASS", "CPP_EXIT_PASS"}, "CPP_FAIL", 120, false, true},

    /* the C compiler */
    {"tcc", "run", "tcc -run /usr/src/apps/echo.c one two", 0, {"one two"}},
    {"tcc", "errors", "echo 'int main(void) { return x; }' > /home/bad.c; tcc -c /home/bad.c", NONZERO, {"x"}},
    {"tcc", "math-and-printf", "tcc -run /data/tests/ctest.c", 0, {"ctest: ok"}},
    {"tcc", "compile-all-apps", "tcc -o /home/allapps /data/tests/allapps.c && /home/allapps", 0, {", 0 failed"}, NULL, 300},
    {"tcc", "self-host", "cd /data/tests/tcc && tcc -o /home/tcc2 tcc.c && /home/tcc2 -run /usr/src/apps/echo.c self-hosted",
     0, {"self-hosted"}, NULL, 300},

    /* desktop */
    {"gui", "window-and-screenshot", "tcc -o /home/guitest /data/tests/guitest.c && /home/guitest", 0, {"guitest: ok"}, NULL, 60},
    {"gui", "gpu-or-explicit-fallback", "tcc -run /data/tests/gputest.c", 0,
     {"gputest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "canvas-native-pixels", "tcc -run /data/tests/canvastest.c", 0,
     {"canvastest: ", ", 0 failed", "painted=1"}, "FAIL", 120, false, true},

    /* 実圧縮入力からNocturne PCM/画素、媒体要素の寿命までを検査。実サイト試験とは別。 */
    {"media", "codec-pcm-frame-seek", "tcc -run /data/tests/media_codectest.c", 0,
     {"media_codectest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"media", "browser-native-lifetime", "tcc -run /data/tests/avmediatest.c", 0,
     {"avmediatest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"media", "webm-ogg-pcm-frame-seek", "tcc -run /data/tests/media_webmtest.c", 0,
     {"media_webmtest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"media", "browser-webm-ogg-lifetime", "tcc -run /data/tests/avmediawebmtest.c", 0,
     {"avmediawebmtest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"media", "native-playback-rate", "tcc -run /data/tests/mediaratetest.c", 0,
     {"mediaratetest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"media", "bmff-codecs", "tcc -run /data/tests/media_bmfftest.c", 0,
     {"media_bmfftest: ", ", 0 failures"}, "FAIL", 180, false, true},
    {"cenc", "native-packet-owner", "/data/tests/media_cenc_test", 0,
     {"media_cenc_test: 11 checks, 0 failures"}, "CENC_FAIL", 120, false, true},

    /* sound (scripts/test.py records the AC'97 output and checks the tones in it) */
    {"audio", "card", "dmesg | grep audio:", 0, {"AC'97"}},
    {"audio", "streams-beep-play", "tcc -o /home/audiotest /data/tests/audiotest.c && /home/audiotest", 0,
     {"audiotest: 0 failed"}, "FAIL", 60, false, true},

    /* the web engine, offline: layout, painting, forms, charsets, URLs, hostile input */
    {"web", "fonts", "tcc -o /home/fonttest /data/tests/fonttest.c && /home/fonttest", 0,
     {"fonttest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"browserfreeze", "slot-cssom-native-stop", "tcc -I/data/tests -o /home/browserfreezetest /data/tests/browserfreezetest.c && /home/browserfreezetest", 0,
     {"browserfreezetest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"browserlayoutstop", "flex-native-stop", "tcc -I/data/tests -o /home/browserfreezetest /data/tests/browserfreezetest.c && /home/browserfreezetest --layout-only", 0,
     {"browserfreezetest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"browsergridstop", "grid-native-stop", "tcc -I/data/tests -o /home/browserfreezetest /data/tests/browserfreezetest.c && /home/browserfreezetest --grid-only", 0,
     {"browserfreezetest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"threads", "native-shared-threads", "tcc -o /home/threadtest /data/tests/threadtest.c && /home/threadtest", 0,
     {"threadtest: ", "PASS", "active="}, "FAIL", 120, false, true},
    {"parallel", "native-browser-parallel", "tcc -I/data/tests -o /home/paralleltest /data/tests/paralleltest.c && /home/paralleltest", 0,
     {"paralleltest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"styleincremental", "native-scoped-incremental-style", "tcc -I/data/tests -o /home/styleincrementaltest /data/tests/styleincrementaltest.c && /home/styleincrementaltest", 0,
     {"styleincremental: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"paintperformance", "damage-and-spatial-hit-index", "tcc -I/data/tests -o /home/paintperformancetest /data/tests/paintperformancetest.c && /home/paintperformancetest", 0,
     {"paintperformance: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"placeholder", "authored-native-placeholder-paint", "tcc -I/data/tests -o /home/paintperformancetest /data/tests/paintperformancetest.c && /home/paintperformancetest --placeholder", 0,
     {"paintperformance placeholder: 12 checks, 0 failed"}, "FAIL", 120, false, true},
    {"placeholder", "computed-pseudo-style-live-declaration", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest computed-style-declaration", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"placeholder", "computed-body-without-generated-box", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --pseudo-bodies", 0,
     {"csscache-pseudo-bodies: 17 checks, 0 failures"}, "FAIL", 120, false, true},
    {"xmlserializer", "native-namespace-serialization-and-existing-documents", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest xml-serialization && /home/jstest documents", 0,
     {"XMLSerializer checks 53", "jstest: 9 checks, 0 failed", "Document checks 153"}, "FAIL", 120, false, true},
    {"paintboundaries", "new-marker-sort-cancellation", "tcc -I/data/tests -o /home/paintperformancetest /data/tests/paintperformancetest.c && /home/paintperformancetest --boundaries", 0,
     {"paintperformance boundaries: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"scrollinput", "native-scrollport-chain-and-extent", "tcc -I/data/tests -o /home/paintperformancetest /data/tests/paintperformancetest.c && /home/paintperformancetest --scroll-input", 0,
     {"paintperformance scroll-input: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"scrollextent", "clipped-absolute-vs-flow-control", "tcc -I/data/tests -o /home/paintperformancetest /data/tests/paintperformancetest.c && /home/paintperformancetest --scroll-extent", 0,
     {"paintperformance scroll-extent: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptperformance", "original-hit-and-scroll-subset", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest http://10.0.2.2:8766/", 0,
     {"限定WPT 3 ページ 6 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 150, false, true},
    {"wptcompat", "original-conditional-relational-module-subset", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --compat http://10.0.2.2:8766/", 0,
     {"限定WPT 4 ページ 48 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 180, false, true},
    {"wptmodule", "original-modulepreload-sri-transport-repair", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --modulepreload http://10.0.2.2:8766/", 0,
     {"限定WPT 1 ページ 1 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 90, false, true},
    {"networkboundaries", "adaptive-admission-and-callback-cancel", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest --performance-boundaries", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"udpcapacity", "dynamic-live-queues-and-dns-capacity", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest --udp-capacity", 0,
     {"webnet-udp-capacity: 0 failed"}, "FAIL", 120, false, true},
    {"tcpcapacity", "live-and-closing-native-tuple-capacity", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest --tcp-capacity", 0,
     {"webnet-tcp-capacity: 0 failed"}, "FAIL", 120, false, true},
    {"nativeabi", "documents-channels-apps-events", "tcc -o /home/nativeabitest /data/tests/nativeabitest.c && /home/nativeabitest", 0,
     {"nativeabitest: ", ", 0 failed"}, "nativeabitest FAIL", 120, false, true},
    {"nativeabi", "iso-c-stdio", "tcc -o /home/stdiotest /data/tests/stdiotest.c && /home/stdiotest", 0,
     {"stdiotest: ", ", 0 failed"}, "stdiotest FAIL", 120, false, true},
    {"sharedpages", "native-video-shared-pages", "tcc -o /home/sharedpagetest /data/tests/sharedpagetest.c && /home/sharedpagetest", 0,
     {"共有ページ ", "検査 0 失敗"}, "共有ページ失敗", 120, false, true},
    {"indexeddb", "async-native-storage", "tcc -I/data/tests -o /home/indexeddbguesttest /data/tests/indexeddbguesttest.c && /home/indexeddbguesttest", 0,
     {"indexeddbguesttest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"csscache", "completed-external-ast", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest", 0,
     {"csscache: 27 checks, 0 failures"}, "FAIL", 120, false, true},
    {"paralleljs", "native-dom-hooks", "tcc -I/data/tests -o /home/paralleljstest /data/tests/paralleljstest.c && /home/paralleljstest", 0,
     {"paralleljstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"currentscript", "classic-checkpoint", "tcc -I/data/tests -o /home/currentscripttest /data/tests/currentscripttest.c && /home/currentscripttest", 0,
     {"currentscripttest: 21 checks, 0 failed"}, "FAIL", 120, false, true},
    {"waitingstream", "inputless-native-parser", "tcc -I/data/tests -o /home/waitingstreamtest /data/tests/waitingstreamtest.c && /home/waitingstreamtest", 0,
     {"waitingstreamtest: 29 checks, 0 failed"}, "FAIL", 120, false, true},
    {"svggeometry", "native-viewbox-path-ctm", "tcc -I/data/tests -o /home/svggeometrytest /data/tests/svggeometrytest.c && /home/svggeometrytest", 0,
     {"svggeometrytest: 109 checks, 0 failed"}, "FAIL", 120, false, true},
    {"parserdedup", "native-transaction-delta", "tcc -I/data/tests -o /home/parserdeduptest /data/tests/parserdeduptest.c && /home/parserdeduptest", 0,
     {"parserdedup: 78 checks, 0 failures"}, "FAIL", 120, false, true},
    {"parserdedup", "render-resume", "tcc -I/data/tests -o /home/parserdeduptest /data/tests/parserdeduptest.c && /home/parserdeduptest --render-resume", 0,
     {"parser-render-resume: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"translate", "native-translation-geometry-hit-overflow", "tcc -I/data/tests -o /home/translatetest /data/tests/translatetest.c && /home/translatetest", 0,
     {"translatetest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"htmlparser", "modern-native-tree", "tcc -I/data/tests -o /home/html_modern_tree_test /data/tests/html_modern_tree_test.c && /home/html_modern_tree_test", 0,
     {"html_modern_tree_test: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"htmlparser", "native-input", "tcc -I/data/tests -o /home/html_input_test /data/tests/html_input_test.c && /home/html_input_test", 0,
     {"html_input_test: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"htmlparser", "encoding-restart", "tcc -I/data/tests -o /home/html_encoding_restarttest /data/tests/html_encoding_restarttest.c && /home/html_encoding_restarttest", 0,
     {"html_encoding_restarttest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"htmlparser", "wpt-product-tree", "tcc -I/data/tests -o /home/html5lib_tree_test /data/tests/html5lib_tree_test.c && /home/html5lib_tree_test", 0,
     {"html5lib: 610 selected, 610 passed, 0 failed, 0 explicit skipped"}, "FAIL", 120, false, true},
    {"htmlparser", "javascript-api", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest html-parser", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"htmlworker", "policy-inheritance", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest html-worker", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"htmlcsp", "native-policy-and-dynamic-code", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest html-csp", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"observerfairness", "continuous-rendering-feedback", "tcc -I/data/tests -o /home/observerfairnesstest /data/tests/observerfairnesstest.c && /home/observerfairnesstest", 0,
     {"observerfairnesstest: 11 checks, 0 failed"}, "FAIL", 120, false, true},
    {"objectfallback", "native-unsupported-flow", "tcc -I/data/tests -o /home/objectfallbacktest /data/tests/objectfallbacktest.c && /home/objectfallbacktest", 0,
     {"objectfallback: 37 checks, 0 failures"}, "FAIL", 120, false, true},
    {"fixedvisual", "native-viewport-contract", "tcc -I/data/tests -o /home/fixedvisualtest /data/tests/fixedvisualtest.c && /home/fixedvisualtest", 0,
     {"fixedvisual: 57 checks, 0 failures"}, "FAIL", 120, false, true},
    {"canvascontextprobe", "debug-only-native-context", "tcc -I/data/tests -o /home/canvascontextprobetest /data/tests/canvascontextprobetest.c && /home/canvascontextprobetest", 0,
     {"canvascontextprobe: 22 checks, 0 failed"}, "FAIL", 120, false, true},
    {"sandboxprofile", "generation-bound-native-policy", "tcc -I/data/tests -o /home/sandboxprofiletest /data/tests/sandboxprofiletest.c && /home/sandboxprofiletest", 0,
     {"sandboxprofiletest: 92 checks, 0 failed"}, "FAIL", 120, false, true},
    {"browserdeferred", "hover-fifo-boundaries", "tcc -I/data/tests -o /home/browserdeferredtest /data/tests/browserdeferredtest.c && /home/browserdeferredtest", 0,
     {"browserdeferred: 27 checks, 0 failed"}, "FAIL", 120, false, true},
    {"canvasgradient", "native-linear-radial-rgba", "tcc -I/data/tests -o /home/canvasgradienttest /data/tests/canvasgradienttest.c && /home/canvasgradienttest", 0,
     {"canvasgradient: 89 checks, 0 failed"}, "FAIL", 120, false, true},
    {"lineclamp", "legacy-native-fragments", "tcc -I/data/tests -o /home/lineclamptest /data/tests/lineclamptest.c && /home/lineclamptest", 0,
     {"lineclamp: 83 checks, 0 failures"}, "FAIL", 120, false, true},
    {"lineclampflow", "anonymous-inline-list-height", "tcc -I/data/tests -o /home/lineclampflowtest /data/tests/lineclampflowtest.c && /home/lineclampflowtest", 0,
     {"lineclampflow: 35 checks, 0 failures"}, "FAIL", 120, false, true},
    {"svgcsssize", "viewbox-css-auto-axis", "tcc -I/data/tests -o /home/svgcsssizetest /data/tests/svgcsssizetest.c && /home/svgcsssizetest", 0,
     {"svgcsssize: 37 checks, 0 failures"}, "FAIL", 120, false, true},
    {"cssanimation", "native-scoped-opacity-clock", "tcc -I/data/tests -o /home/cssanimationtest /data/tests/cssanimationtest.c && /home/cssanimationtest", 0,
     {"cssanimation: 94 checks, 0 failures (JS 10/0)"}, "FAIL", 120, false, true},
    {"lineclamppredicate", "debug-eligible-candidate-publication", "tcc -I/data/tests -o /home/lineclamppredicatetest /data/tests/lineclamppredicatetest.c && /home/lineclamppredicatetest", 0,
     {"lineclamppredicate: 25 checks, 0 failures"}, "FAIL", 120, false, true},
    {"cssanimationregression", "live-resource-publication-failed-boundary", "tcc -I/data/tests -o /home/cssanimationregressiontest /data/tests/cssanimationregressiontest.c && /home/cssanimationregressiontest", 0,
     {"cssanimationregression: 34 checks, 0 failures (JS 1/0)"}, "FAIL", 120, false, true},
    {"lineclampreentry", "same-pass-measurement-stale-visibility", "tcc -I/data/tests -o /home/lineclampreentrytest /data/tests/lineclampreentrytest.c && /home/lineclampreentrytest", 0,
     {"lineclampreentry: 60 checks, 0 failures"}, "FAIL", 120, false, true},
    {"cssanimationpixel", "opacity-invariant-blend-failed-boundary", "tcc -I/data/tests -o /home/cssanimationpixeltest /data/tests/cssanimationpixeltest.c && /home/cssanimationpixeltest", 0,
     {"cssanimationpixel: 4 checks, 0 failures"}, "FAIL", 120, false, true},
    {"imagebitmap", "native-owned-decode-raster", "tcc -I/data/tests -o /home/imagebitmaptest /data/tests/imagebitmaptest.c && /home/imagebitmaptest", 0,
     {"imagebitmap: 109 checks, 0 failed (JS 84)"}, "FAIL", 120, false, true},
    {"imagebitmapheap", "sized-allocator-lifetime", "tcc -I/data/tests -o /home/imagebitmaptest /data/tests/imagebitmaptest.c && /home/imagebitmaptest --heap", 0,
     {"imagebitmapheap: 9 checks, 0 failed"}, "FAIL", 120, false, true},
    {"imagebitmapoom", "observed-allocation-request", "tcc -I/data/tests -o /home/imagebitmaptest /data/tests/imagebitmaptest.c && /home/imagebitmaptest --heap-oom", 0,
     {"imagebitmapheapoom: 3 checks, 0 failed"}, "FAIL", 120, false, true},
    {"imagebitmappromise", "early-rejection-microtasks", "tcc -I/data/tests -o /home/imagebitmappromisetest /data/tests/imagebitmappromisetest.c && /home/imagebitmappromisetest", 0,
     {"imagebitmappromise: 14 checks, 0 failed (JS 10)"}, "FAIL", 120, false, true},
    {"web", "engine", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest", 0, {"webtest: ", ", 0 failed"}, "FAIL",
     120, false, true},
    {"web", "javascript", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest", 0,
     /* Whole-suite QEMU wall time includes repeated fresh browser contexts and
        7,000+ collation comparisons; the page's 5s JS watchdog is unchanged. */
     {"jstest: ", ", 0 failed"}, "FAIL", 900, false, true},
    {"web", "html-interfaces", "tcc -o /home/jstest /data/tests/jstest.c && /home/jstest html-interfaces", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "html-tree-builder", "tcc -I/data/tests -o /home/lexbortest /data/tests/lexbortest.c && /home/lexbortest", 0,
     {"lexbortest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "attribute-nodes", "tcc -I/data/tests -o /home/js_attributes_native /data/tests/js_attributes_native.c && /home/js_attributes_native", 0,
     {"js_attributes_native: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "shadow-dom-native", "tcc -I/data/tests -o /home/shadownativetest /data/tests/shadownativetest.c && /home/shadownativetest", 0,
     {"shadownativetest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "semantic-elements", "tcc -I/data/tests -o /home/semanticstest /data/tests/semanticstest.c && /home/semanticstest", 0,
     {"semanticstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "form-control-values", "tcc -I/data/tests -o /home/js_form_controls_native /data/tests/js_form_controls_native.c && /home/js_form_controls_native", 0,
     {"js_form_controls_native: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"pointerfocus", "native-area-and-textarea-scroll", "tcc -I/data/tests -o /home/js_form_controls_native /data/tests/js_form_controls_native.c && /home/js_form_controls_native pointer-focus", 0,
     {"Textarea native scroll API checks 27", "js_form_controls_native/pointer-focus: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "formdata-entries", "tcc -I/data/tests -o /home/js_form_controls_native /data/tests/js_form_controls_native.c && /home/js_form_controls_native formdata-entries", 0,
     {"js_form_controls_native/formdata-entries: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "shadow-dom-render", "tcc -I/data/tests -o /home/shadowtest /data/tests/shadowtest.c && /home/shadowtest", 0,
     {"shadowtest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "collation-decoder", "tcc -I/data/tests -o /home/js_collator_native /data/tests/js_collator_native.c && /home/js_collator_native", 0,
     {"js_collator_native: ", ", 0 failed"}, "FAIL", 120, false, true},
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
    {"compat", "relational-selectors", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest", 0,
     {"cssrelational: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "relational-shadow-boundaries", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --boundaries", 0,
     {"cssrelational-boundaries: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "media-range-calc", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --media-calc", 0,
     {"cssrelational-media-calc: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "font-src-fallback", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --font-sources", 0,
     {"csscache-font-sources: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compat", "parallel-css-order", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --parallel-css", 0,
     {"csscache-parallel: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compat", "initial-render-blockers", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --render", 0,
     {"csscache-render: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compat", "modulepreload", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest modulepreload", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"flexzero", "definite-zero-basis-wrap", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest --flex-zero", 0,
        {"webtest-flex-zero: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptgeometry", "original-inline-and-slotted", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --observer-geometry http://10.0.2.2:8766/", 0,
        {"限定WPT 2 ページ 4 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"wptflex", "original-invalid-flex", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --flex-invalid http://10.0.2.2:8766/", 0,
        {"限定WPT 1 ページ 6 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"fixedtable", "declared-and-first-row-tracks", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest --fixed-table", 0,
        {"webtest-fixed-table: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"boxshadow", "native-paint-and-inline-lifetime", "tcc -I/data/tests -o /home/boxshadowtest /data/tests/boxshadowtest.c && /home/boxshadowtest", 0,
        {"boxshadowtest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"shadowdamage", "atomic-ink-and-block-fallback", "tcc -I/data/tests -o /home/boxshadowtest /data/tests/boxshadowtest.c && /home/boxshadowtest --damage-only", 0,
        {"boxshadow-damage: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"radius", "percentage-replaced-overflow-hit-and-retained-mask", "tcc -I/data/tests -o /home/boxshadowtest /data/tests/boxshadowtest.c && /home/boxshadowtest --radius", 0,
        {"radius-native: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"imageassociation", "partial-hover-background-mask-and-scroll", "tcc -I/data/tests -o /home/boxshadowtest /data/tests/boxshadowtest.c && /home/boxshadowtest --image-association", 0,
        {"image-association-native: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"childlistlocality", "candidate-work-bound", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --child-list-locality", 0,
        {"cssrelational-child-locality: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"layers", "normal-important-inline-revert-and-scope", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --layers", 0,
        {"cssrelational-layers: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"layerimports", "reversed-completion-scope", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --layers-import", 0,
        {"csscache-layer-imports: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"layernames", "named-font-and-keyframes-metadata", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --layers-names", 0,
        {"csscache-layer-names: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"textoverflow", "logical-text-retained-paint-ellipsis", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest --text-overflow", 0,
        {"webtest-text-overflow: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"buttonformat", "author-flex-grid-and-native-activation", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest --button-formatting", 0,
        {"webtest-button-formatting: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"flexfreeze", "redistribute-min-max-clamped-flex-items", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest --flex-freeze", 0,
        {"webtest-flex-freeze: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"autotablepercent", "intrinsic-percentage-and-specified-width-distribution", "tcc -I/data/tests -o /home/webtest /data/tests/webtest.c && /home/webtest --auto-table-percent", 0,
        {"webtest-auto-table-percent: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptradius", "original-parsing-and-computed", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --radius http://10.0.2.2:8766/", 0,
        {"限定WPT 3 ページ 48 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"wptradiuscomputed", "original-computed-fourteen-only", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --radius-computed http://10.0.2.2:8766/", 0,
        {"限定WPT 1 ページ 14 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"computedstyle", "native-property-publication-readonly-live-and-indices", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest computed-style-declaration", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"inputsnapshot", "coherent-passive-wheel-cancellation-and-generation", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest input-snapshot", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"windownamed", "native-window-named-property-lifetime", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest window-named", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"domsubtree", "native-subtree-publication-and-existing-dom-behavior", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest detached && /home/jstest dom && /home/jstest documents && /home/jstest collections && /home/jstest lifetime", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"cssownerevents", "owner-critical-imports-without-global-join", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --owner-events", 0,
        {"csscache-owner-events: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"frameviewport", "parent-layout-to-child-cssom-without-paint", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --frame-viewport", 0,
        {"csscache-frame-viewport: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"colorformat", "computed-opaque-and-alpha-roundtrip", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --color-format", 0,
        {"csscache-color-format: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"layerdynamic", "cssom-order-rollback-and-scope-mutation", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --layers-dynamic", 0,
        {"cssrelational-layer-dynamic: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptlayers", "original-normal-important-inline-and-media", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --layers http://10.0.2.2:8766/", 0,
        {"限定WPT 4 ページ 55 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 180, false, true},
    {"wptlayersfailed", "original-inline-named-target-and-child-viewport", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --layers-failed http://10.0.2.2:8766/", 0,
        {"限定WPT 2 ページ 12 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 180, false, true},
    {"wptellipsis", "original-fullpixel-flex-container-reference", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --text-overflow-ref http://10.0.2.2:8766/", 0,
        {"限定WPT画像 1 組 0 不一致画素 0 実行基盤失敗"}, "FAIL", 120, false, true},
    {"entrycontract", "branded-v1-entry-snapshot", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest intersection-entry", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"childlistdirty", "local-parent-and-relational-witness", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --child-list", 0,
        {"cssrelational-child-list: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptinline", "original-inline-fragment-offset-repair", "tcc -o /home/wptperformancetest /data/tests/wptperformancetest.c && /home/wptperformancetest --observer-inline http://10.0.2.2:8766/", 0,
        {"限定WPT 1 ページ 3 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"moduleapirepair", "sri-query-asset", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest modulepreload-api", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"observergeometry", "inline-and-slotted-intersection", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest intersection-inline", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "css-supports", "tcc -I/data/tests -o /home/csssupportstest /data/tests/csssupportstest.c && /home/csssupportstest", 0,
     {"csssupportstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compatrepair", "relational-selectors-initialized", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest", 0,
     {"cssrelational: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "relational-shadow-initialized", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --boundaries", 0,
     {"cssrelational-boundaries: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "media-calc-initialized", "tcc -I/data/tests -o /home/cssrelationaltest /data/tests/cssrelationaltest.c && /home/cssrelationaltest --media-calc", 0,
     {"cssrelational-media-calc: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "font-failure-quiet-snapshot", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --font-failure-noop", 0,
     {"csscache-font-noop: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compatrepair", "render-fallback-one-shot", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest --render", 0,
     {"csscache-render: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compatrepair", "modulepreload-native-executable", "tcc -I/data/tests -o /home/jstest /data/tests/jstest.c && /home/jstest modulepreload", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "native-filter-cancel-commit", "tcc -I/data/tests -o /home/csssupportstest /data/tests/csssupportstest.c && /home/csssupportstest --filter-cancel", 0,
     {"csssupportstest-filter-cancel: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "css-supports", "tcc -I/data/tests -o /home/csssupportstest /data/tests/csssupportstest.c && /home/csssupportstest", 0,
     {"csssupportstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "storage", "tcc -I/data/tests -o /home/storagetest /data/tests/storagetest.c && /home/storagetest --isolated-scratch", 0,
     {"storagetest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "worker-network", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"web", "tcp-capacity-recovery", "tcc -o /home/tcpcapacitytest /data/tests/tcpcapacitytest.c && /home/tcpcapacitytest", 0,
     {"tcpcapacitytest: ", ", 0 failed"}, "FAIL", 180, false, true},

    {"nettransport", "dns-deadline-smp", "tcc -o /home/nettransporttest /data/tests/nettransporttest.c && /home/nettransporttest", 0,
     {"nettransporttest: 0 failed"}, "FAIL", 120, false, true},
    {"nettransport", "default-dns-stream", "tcc -o /home/nettransporttest /data/tests/nettransporttest.c && /home/nettransporttest --stream-candidates", 0,
     {"nettransport stream: 0 failed", "STREAM-CANDIDATES plain candidates=2 attempts=2"}, "FAIL", 30, false, true},
    {"nettransport", "tls-dns-candidates", "tcc -o /home/nettransporttest /data/tests/nettransporttest.c && /home/nettransporttest --tls-candidates", 0,
     {"nettransport TLS candidates: 0 failed", "STREAM-CANDIDATES tls=1"}, "FAIL", 90, false, true},
    {"networkpacing", "pause-fairness-batch", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest --network-pacing", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"transport8", "http-pool-h2", "tcc -o /home/transport8 /data/tests/transport8.c && /home/transport8", 0,
     {"transport8:" , "0 failed"}, "FAIL", 120, false, true},
    {"transport8", "h2-late-cancel", "tcc -o /home/transport8 /data/tests/transport8.c && /home/transport8 --late-cancel", 0,
     {"transport8 late-cancel: 5 checks, 0 failed"}, "FAIL", 30, false, true},
    {"transport8", "h2-header-budget", "tcc -o /home/transport8 /data/tests/transport8.c && /home/transport8 --header-budget", 0,
     {"transport8 header-budget: 12 checks, 0 failed"}, "FAIL", 60, false, true},
    {"transport8", "tls-handshake-cancel", "tcc -o /home/transport8 /data/tests/transport8.c && /home/transport8 --tls-cancel", 0,
     {"transport8 TLS-cancel:" , "0 failed"}, "FAIL", 120, false, true},
    {"net", "tls-alpn-session-resume", "tcc -o /home/transport8 /data/tests/transport8.c && /home/transport8 --tls", 0,
     {"transport8 TLS:" , "0 failed"}, "FAIL", 120, false, true},

    /* TCP against the host's test server (no internet needed), clean and with simulated loss */
    {"tcpperf", "clean-repeat", "tcc -o /home/tcptest /data/tests/tcptest.c && /home/tcptest --clean-repeat 5", 0,
     {"TCPPERF sample=5", "tcptest: 0 failed"}, "FAIL", 120, false, true},
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

/* The output of one test: every message its shell sent on stdout/stderr, as one NUL-terminated string. */
struct capture {
    char *text;
    size_t length, capacity;
};

/* Takes the messages that are already waiting on the output channel. */
static void drain(n_handle channel, struct capture *c) {
    for (;;) {
        n_result size = n_channel_receive(channel, NULL, 0, N_CHANNEL_PEEK, NULL);
        if (size < 0) return;
        if (size == 0) { /* an empty message carries nothing; drop it */
            char drop;
            if (n_channel_receive(channel, &drop, 1, N_CHANNEL_DISCARD, NULL) < 0) return;
            continue;
        }
        if (c->length + (size_t)size + 1 > c->capacity) {
            size_t want = c->capacity ? c->capacity : 4096;
            while (want < c->length + (size_t)size + 1) want *= 2;
            char *grown = realloc(c->text, want);
            if (!grown) return;
            c->text = grown;
            c->capacity = want;
        }
        n_result got = n_channel_receive(channel, c->text + c->length, (size_t)size, 0, NULL);
        if (got <= 0) return;
        c->length += (size_t)got;
    }
}

/* Runs "sh -c cmd" with a closed stdin and stdout/stderr on one channel that is drained while it runs.
   Returns 1 when it ended (*status set), 0 when it overran timeout_ms and was stopped, -1 when it could not start. */
static int run_shell(const char *cmd, int timeout_ms, struct capture *c, int *status) {
    n_handle out[2], in[2];
    if (n_channel_pair(out) < 0) return -1;
    if (n_channel_pair(in) < 0) { n_handle_close(out[0]); n_handle_close(out[1]); return -1; }
    n_handle_close(in[0]); /* reading stdin reports the end at once, like an empty device */
    const char *args[] = {"sh", "-c", cmd};
    int32_t handles[] = {in[1], out[1], out[1]};
    struct n_launch launch = {.arguments = args, .argument_count = 3, .handles = handles, .handle_count = 3};
    n_handle app = n_app_launch("/bin/sh", &launch);
    n_handle_close(in[1]);
    n_handle_close(out[1]);
    n_handle q = n_events_create();
    if (app < 0 || q < 0) {
        if (app >= 0) { n_app_stop(app, N_APP_STOP_TREE); n_handle_close(app); }
        if (q >= 0) n_handle_close(q);
        n_handle_close(out[0]);
        return -1;
    }
    n_events_watch(q, app, N_EVENT_APP_ENDED, 1);
    n_events_watch(q, out[0], N_EVENT_RECEIVABLE, 2);
    uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
    int result = 0;
    for (;;) {
        drain(out[0], c);
        if (n_app_result(app, 0, status) == 0) { drain(out[0], c); result = 1; break; }
        uint64_t now = uptime_ms();
        if (now >= deadline) break;
        struct n_event e;
        n_events_wait(q, &e, 1, (int64_t)(deadline - now));
    }
    if (!result) {
        n_app_stop(app, N_APP_STOP_TREE);
        n_app_result(app, -1, status);
        drain(out[0], c);
    }
    if (!c->text) c->text = strdup("");
    else c->text[c->length] = 0;
    n_handle_close(q);
    n_handle_close(app);
    n_handle_close(out[0]);
    return result;
}

/* first line of the output, for failure messages */
static void snippet(const char *out, char *dst, size_t n) {
    size_t i = 0;
    for (; out[i] && i + 1 < n && i < 100; i++) dst[i] = out[i] == '\n' ? '|' : out[i];
    dst[i] = 0;
}

static void show_output(const char *out) {
    for (const char *line = out, *nl; *line; line = nl ? nl + 1 : line + strlen(line)) {
        nl = strchr(line, '\n');
        printf("  | %.*s\n", nl ? (int)(nl - line) : (int)strlen(line), line);
    }
    fflush(stdout);
}

static void run(const struct test *t) {
    int nfail_before = nfail;
    uint64_t t0 = uptime_ms();
    int timeout = (t->timeout_s ? t->timeout_s : 15) * 1000, st = 0;
    struct capture cap = {0};
    int r = run_shell(t->cmd, timeout, &cap, &st);
    char why[256];
    if (r < 0) {
        result("FAIL", t, "cannot start sh");
        nfail++;
        free(cap.text);
        return;
    }
    if (r == 0) {
        snprintf(why, sizeof why, "timed out after %d s", timeout / 1000);
        result("FAIL", t, why);
        nfail++;
        show_output(cap.text);
        free(cap.text);
        return;
    }
    char *out = cap.text, snip[128];
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
        show_output(out);
    }
    free(out);
}

int main(int argc, char **argv) {
    bool quick = false, nonet = false;
    const char **only = malloc((size_t)argc * sizeof *only);
    if (!only) { printf("TESTS ERROR selector allocation failed\n"); return 1; }
    int nonly = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-quick")) quick = true;
        else if (!strcmp(argv[i], "-nonet")) nonet = true;
        else {
            bool known = false;
            for (size_t j = 0; j < sizeof tests / sizeof *tests; j++) {
                char qualified[128];
                snprintf(qualified, sizeof qualified, "%s/%s", tests[j].group, tests[j].name);
                if (!strcmp(argv[i], tests[j].group) || !strcmp(argv[i], qualified)) { known = true; break; }
            }
            if (!known) { printf("TESTS ERROR unknown selector: %s\n", argv[i]); free(only); return 1; }
            only[nonly++] = argv[i];
        }
    }
    struct n_entry st;
    bool have_tcc_src = n_entry_info("/data/tests/tcc/tcc.c", &st) == 0;
    printf("TESTS BEGIN\n");
    uint64_t t0 = uptime_ms();
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        const struct test *t = &tests[i];
        if (nonly) {
            bool hit = false;
            char qualified[128];snprintf(qualified,sizeof qualified,"%s/%s",t->group,t->name);
            for (int k = 0; k < nonly; k++)
                hit |= !strcmp(only[k], t->group) || !strcmp(only[k],qualified);
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
        if (!net_waited && (!strcmp(t->group, "tcp") || !strcmp(t->group, "tcpperf") || !strcmp(t->group, "net") || !strcmp(t->name, "worker-network"))) {
            net_waited = true;
            if (!net_wait_up(20000)) printf("runtests: the network did not come up\n");
        }
        run(t);
    }
    free(only);
    printf("TESTS DONE pass=%d fail=%d skip=%d (%lu s)\n", npass, nfail, nskip, (unsigned long)((uptime_ms() - t0) / 1000));
    return nfail != 0;
}
