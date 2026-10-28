/* runtests: Nocturne's in-OS test suite. scripts/test.py puts tests/ in C:\Tests on its own copy
   of the built disk; init runs C:\Tests\autorun.sh at boot, which builds this runner with tcc and
   runs it. Each test is "sh -c" in the folder <TEMP>\runtests (the TEMP setting, emptied at every
   start), so the documents a test makes by a relative name go there.
   Output goes to the system log (the serial port), one line per test:
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
    {"slop8", "script-lifecycle", "tcc -o .\\scriptlifecycletest C:\\Tests\\scriptlifecycletest.c && .\\scriptlifecycletest", 0,
     {"scriptlifecycletest: 22 checks, 0 failed"}, "FAIL", 60, false, true},
    {"slop8", "storage-windows", "tcc -o .\\storagewindowtest C:\\Tests\\storagewindowtest.c && .\\storagewindowtest", 0,
     {"STORAGE-WINDOW-DONE", "failures=0"}, "FAIL", 120, false, true},
    {"wasm", "native-webassembly-js-and-streaming", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest wasm", 0,
     {"WebAssembly checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasm", "native-webassembly-spec-boundaries", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest wasm-boundary", 0,
     {"WebAssembly boundary checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmcli", "wasm2c-tcc-native-executable", "tcc -o .\\wasm2ctest C:\\Tests\\wasm2ctest.c && .\\wasm2ctest", 0,
     {"WASM2C_NATIVE_RESULT 42", "PASS wasm2ctest:"}, "FAIL", 180, false, true},
    {"wasmjspi", "native-webassembly-promise-integration", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest wasm-jspi", 0,
     {"WebAssembly JSPI checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmworker", "native-webassembly-dedicated-worker", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest wasm-worker", 0,
     {"WebAssembly worker checks ", "jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmpolicy", "native-webassembly-csp-window", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest wasm-policy", 0,
     {"WebAssembly policy checks ", ", 0 failed"}, "FAIL", 180, false, true},
    {"wasmpolicy", "native-webassembly-csp-http-worker", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest wasm-worker-policy", 0,
     {"WebAssembly worker policy checks ", ", 0 failed"}, "FAIL", 180, false, true},
    {"slop8", "dom-events-observers", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest dom-api", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"slop8", "network-stream", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest network-stream", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"slop8", "fetch-regression", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest fetch-api", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"slop8", "xhr-regression", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest xhr", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"slop8", "worker-events", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest html-worker", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    /* shell */
    {"sh", "pipe", "echo hello world | grep world", 0, {"hello world"}},
    {"sh", "and-or", "false || echo A; true && echo B; false && echo C", NONZERO, {"A\nB\n"}, "C"},
    {"sh", "redirect", "echo abc > .\\r.txt; echo def >> .\\r.txt; cat < .\\r.txt", 0, {"abc\ndef\n"}},
    {"sh", "stderr-redirect", "ls C:\\nonexistent 2> .\\e.txt; cat .\\e.txt", 0, {"nonexistent"}},
    {"sh", "exit-status", "sh -c 'exit 3'; echo status=$?", 0, {"status=3"}},
    {"sh", "quoting", "echo 'a  b' \"c  d\"", 0, {"a  b c  d"}},
    {"sh", "glob", "echo C:\\Nocturne\\Agent\\*.md", 0, {"C:\\Nocturne\\Agent\\system.md"}},
    {"sh", "script-args", "echo 'echo $0 got $# args: $1, $2, all: $@' > .\\s.sh; sh .\\s.sh one 'two too'", 0,
     {".\\s.sh got 2 args: one, two too, all: one two too"}},
    {"sh", "script-by-name", "echo 'echo run by name' > .\\s2.sh; .\\s2.sh", 0, {"run by name"}},
    /* '\' is the folder separator, never an escape: single quotes keep a '$' */
    {"sh", "backslash-and-quoted-dollar", "echo C:\\Users\\a '$1'", 0, {"C:\\Users\\a $1"}},
    {"sh", "background", "sleep 1 & echo started", 0, {"started"}},
    {"sh", "missing-command", "no-such-command", NONZERO, {"not found"}},

    /* command-line tools */
    {"tools", "ls", "ls C:\\Nocturne\\System", 0, {"tcc", "sh", "fetch"}},
    {"tools", "ps", "ps", 0, {"init", "runtests"}},
    {"tools", "uname", "uname", 0, {"Nocturne"}},
    {"tools", "wc", "echo one two three | wc", 0, {"3"}},
    {"tools", "head", "head -n 2 C:\\Nocturne\\Agent\\system.md | wc", 0, {"2"}},
    {"tools", "hexdump", "echo AB | hexdump", 0, {"41 42"}},
    {"tools", "dmesg", "dmesg | grep store:", 0, {"C: \"Nocturne\""}},
    {"tools", "lspci", "lspci", 0, {"8086:7010"}},
    {"tools", "mkdir-touch-rm", "mkdir d && touch d\\f && ls d && rm d\\f && rmdir d && ls",
     0, {"f\n"}},
    {"tools", "cp-mv", "echo data > a; cp a b; mv b c; cat c; ls", 0, {"data"}, " b\n"},
    {"tools", "free-uptime-date", "free && uptime && date", 0, {"up"}},

    /* memory and protection */
    {"mem", "malloc-stress", "tcc -o .\\memtest C:\\Tests\\memtest.c && .\\memtest", 0, {"memtest: ok"}, NULL, 120},
    {"mem", "cpu-capabilities", "tcc -o .\\cpuinfotest C:\\Tests\\cpuinfotest.c && .\\cpuinfotest", 0,
     {"cpuinfotest: ", "; ok"}, "invalid", 60},
    {"mem", "heap-page-reclamation", "tcc -o .\\malloctrimtest C:\\Tests\\malloctrimtest.c && .\\malloctrimtest", 0,
     {"malloctrimtest: ", ", 0 failed"}, "FAIL", 60},
    {"mem", "heap-failure-atomicity", "tcc -o .\\memreservetest C:\\Tests\\memreservetest.c && .\\memreservetest", 0,
     {"memreservetest: ", ", 0 failed"}, "FAIL", 60, false, true},
    {"mem", "w^x", "tcc -o .\\wxtest C:\\Tests\\wxtest.c && .\\wxtest", 0, {"wxtest: 0 failed"}, "FAIL", 60, true},

    /* filesystems */
    {"fs", "store", "tcc -o .\\fstest C:\\Tests\\fstest.c && .\\fstest C:\\Tests\\fstest", 0, {"fstest: 0 failed"}, "FAIL", 180},

    /* Host cross-built C++ application, using the native C startup and APIs. */
    {"cxx", "native-runtime", "cppdemo --self-test", 0,
     {"CPP_SELFTEST_PASS", "CPP_EXIT_PASS"}, "CPP_FAIL", 120, false, true},

    /* the C compiler */
    {"tcc", "run", "tcc -run C:\\Nocturne\\SDK\\Source\\Apps\\echo.c one two", 0, {"one two"}},
    {"tcc", "errors", "echo 'int main(void) { return x; }' > .\\bad.c; tcc -c .\\bad.c", NONZERO, {"x"}},
    {"tcc", "math-and-printf", "tcc -run C:\\Tests\\ctest.c", 0, {"ctest: ok"}},
    {"tcc", "math-and-printf-executable", "tcc -o .\\ctest C:\\Tests\\ctest.c && .\\ctest", 0, {"ctest: ok"}},
    {"tcc", "compile-all-apps", "tcc -o .\\allapps C:\\Tests\\allapps.c && .\\allapps", 0, {", 0 failed"}, NULL, 300},
    {"tcc", "self-host", "tcc -o .\\tcc2 C:\\Tests\\tcc\\tcc.c && .\\tcc2 -run C:\\Nocturne\\SDK\\Source\\Apps\\echo.c self-hosted",
     0, {"self-hosted"}, NULL, 300},

    /* the LLVM toolchain (ports/llvm): clang, clang++ and ld.lld with the SDK's libc++ */
    {"clang", "c-executable", "clang -O2 -o .\\ctest C:\\Tests\\ctest.c && .\\ctest", 0, {"ctest: ok"}, NULL, 300},
    {"clang", "errors", "echo 'int main(void) { return x; }' > .\\bad.c; clang -c .\\bad.c", NONZERO,
     {"undeclared identifier 'x'"}, NULL, 120},
    {"clang", "cxx-executable", "clang++ -std=c++23 -O2 -o .\\cxxtest C:\\Tests\\cxxtest.cpp && .\\cxxtest", 0,
     {"cxxtest: ok"}, "FAIL", 600},
    {"clang", "threads-executable", "clang -O2 -o .\\threadtest C:\\Tests\\threadtest.c && .\\threadtest", 0,
     {"threadtest: ", "PASS", "active="}, "FAIL", 300, false, true},
    {"clang", "compile-all-apps", "tcc -o .\\allapps C:\\Tests\\allapps.c && .\\allapps clang", 0, {", 0 failed"},
     NULL, 1200},

    /* desktop */
    {"gui", "window-and-screenshot", "tcc -o .\\guitest C:\\Tests\\guitest.c && .\\guitest", 0, {"guitest: ok"}, NULL, 60},
    {"gui", "gpu-or-explicit-fallback", "tcc -run C:\\Tests\\gputest.c", 0,
     {"gputest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "canvas-native-pixels", "tcc -run C:\\Tests\\canvastest.c", 0,
     {"canvastest: ", ", 0 failed", "painted=1"}, "FAIL", 120, false, true},

    /* 実圧縮入力からNocturne PCM/画素、媒体要素の寿命までを検査。実サイト試験とは別。 */
    {"media", "codec-pcm-frame-seek", "tcc -run C:\\Tests\\media_codectest.c", 0,
     {"media_codectest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"media", "browser-native-lifetime", "tcc -run C:\\Tests\\avmediatest.c", 0,
     {"avmediatest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"media", "webm-ogg-pcm-frame-seek", "tcc -run C:\\Tests\\media_webmtest.c", 0,
     {"media_webmtest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"media", "browser-webm-ogg-lifetime", "tcc -run C:\\Tests\\avmediawebmtest.c", 0,
     {"avmediawebmtest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"media", "native-playback-rate", "tcc -run C:\\Tests\\mediaratetest.c", 0,
     {"mediaratetest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"media", "bmff-codecs", "tcc -run C:\\Tests\\media_bmfftest.c", 0,
     {"media_bmfftest: ", ", 0 failures"}, "FAIL", 180, false, true},
    {"cenc", "native-packet-owner", "C:\\Tests\\media_cenc_test", 0,
     {"media_cenc_test: 11 checks, 0 failures"}, "CENC_FAIL", 120, false, true},

    /* sound (scripts/test.py records the AC'97 output and checks the tones in it) */
    {"audio", "card", "dmesg | grep audio:", 0, {"AC'97"}},
    {"audio", "streams-beep-play", "tcc -o .\\audiotest C:\\Tests\\audiotest.c && .\\audiotest", 0,
     {"audiotest: 0 failed"}, "FAIL", 60, false, true},

    /* the web engine, offline: layout, painting, forms, charsets, URLs, hostile input */
    {"web", "fonts", "tcc -o .\\fonttest C:\\Tests\\fonttest.c && .\\fonttest", 0,
     {"fonttest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"browserfreeze", "slot-cssom-native-stop", "tcc -IC:\\Tests -o .\\browserfreezetest C:\\Tests\\browserfreezetest.c && .\\browserfreezetest", 0,
     {"browserfreezetest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"browserlayoutstop", "flex-native-stop", "tcc -IC:\\Tests -o .\\browserfreezetest C:\\Tests\\browserfreezetest.c && .\\browserfreezetest --layout-only", 0,
     {"browserfreezetest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"browsergridstop", "grid-native-stop", "tcc -IC:\\Tests -o .\\browserfreezetest C:\\Tests\\browserfreezetest.c && .\\browserfreezetest --grid-only", 0,
     {"browserfreezetest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"threads", "native-shared-threads", "tcc -o .\\threadtest C:\\Tests\\threadtest.c && .\\threadtest", 0,
     {"threadtest: ", "PASS", "active="}, "FAIL", 120, false, true},
    {"parallel", "native-browser-parallel", "tcc -IC:\\Tests -o .\\paralleltest C:\\Tests\\paralleltest.c && .\\paralleltest", 0,
     {"paralleltest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"styleincremental", "native-scoped-incremental-style", "tcc -IC:\\Tests -o .\\styleincrementaltest C:\\Tests\\styleincrementaltest.c && .\\styleincrementaltest", 0,
     {"styleincremental: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"paintperformance", "damage-and-spatial-hit-index", "tcc -IC:\\Tests -o .\\paintperformancetest C:\\Tests\\paintperformancetest.c && .\\paintperformancetest", 0,
     {"paintperformance: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"placeholder", "authored-native-placeholder-paint", "tcc -IC:\\Tests -o .\\paintperformancetest C:\\Tests\\paintperformancetest.c && .\\paintperformancetest --placeholder", 0,
     {"paintperformance placeholder: 12 checks, 0 failed"}, "FAIL", 120, false, true},
    {"placeholder", "computed-pseudo-style-live-declaration", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest computed-style-declaration", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"placeholder", "computed-body-without-generated-box", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --pseudo-bodies", 0,
     {"csscache-pseudo-bodies: 17 checks, 0 failures"}, "FAIL", 120, false, true},
    {"xmlserializer", "native-namespace-serialization-and-existing-documents", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest xml-serialization && .\\jstest documents", 0,
     {"XMLSerializer checks 53", "jstest: 9 checks, 0 failed", "Document checks 153"}, "FAIL", 120, false, true},
    {"paintboundaries", "new-marker-sort-cancellation", "tcc -IC:\\Tests -o .\\paintperformancetest C:\\Tests\\paintperformancetest.c && .\\paintperformancetest --boundaries", 0,
     {"paintperformance boundaries: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"scrollinput", "native-scrollport-chain-and-extent", "tcc -IC:\\Tests -o .\\paintperformancetest C:\\Tests\\paintperformancetest.c && .\\paintperformancetest --scroll-input", 0,
     {"paintperformance scroll-input: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"scrollextent", "clipped-absolute-vs-flow-control", "tcc -IC:\\Tests -o .\\paintperformancetest C:\\Tests\\paintperformancetest.c && .\\paintperformancetest --scroll-extent", 0,
     {"paintperformance scroll-extent: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptperformance", "original-hit-and-scroll-subset", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest http://10.0.2.2:8766/", 0,
     {"限定WPT 3 ページ 6 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 150, false, true},
    {"wptcompat", "original-conditional-relational-module-subset", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --compat http://10.0.2.2:8766/", 0,
     {"限定WPT 4 ページ 48 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 180, false, true},
    {"wptmodule", "original-modulepreload-sri-transport-repair", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --modulepreload http://10.0.2.2:8766/", 0,
     {"限定WPT 1 ページ 1 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 90, false, true},
    {"networkboundaries", "adaptive-admission-and-callback-cancel", "tcc -o .\\webnettest C:\\Tests\\webnettest.c && .\\webnettest --performance-boundaries", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"udpcapacity", "dynamic-live-queues-and-dns-capacity", "tcc -o .\\webnettest C:\\Tests\\webnettest.c && .\\webnettest --udp-capacity", 0,
     {"webnet-udp-capacity: 0 failed"}, "FAIL", 120, false, true},
    {"tcpcapacity", "live-and-closing-native-tuple-capacity", "tcc -o .\\webnettest C:\\Tests\\webnettest.c && .\\webnettest --tcp-capacity", 0,
     {"webnet-tcp-capacity: 0 failed"}, "FAIL", 120, false, true},
    {"nativeabi", "documents-channels-apps-events", "tcc -o .\\nativeabitest C:\\Tests\\nativeabitest.c && .\\nativeabitest", 0,
     {"nativeabitest: ", ", 0 failed"}, "nativeabitest FAIL", 120, false, true},
    {"nativeabi", "user-settings", "tcc -o .\\usersettingstest C:\\Tests\\usersettingstest.c && .\\usersettingstest", 0,
     {"usersettingstest: ", ", 0 failed"}, "usersettingstest FAIL", 60, false, true},
    {"nativeabi", "iso-c-stdio", "tcc -o .\\stdiotest C:\\Tests\\stdiotest.c && .\\stdiotest", 0,
     {"stdiotest: ", ", 0 failed"}, "stdiotest FAIL", 120, false, true},
    {"nativeabi", "entry-watch", "tcc -o .\\entrywatchtest C:\\Tests\\entrywatchtest.c && .\\entrywatchtest", 0,
     {"entrywatchtest: ", ", 0 failed"}, "entrywatchtest FAIL", 180, false, true},
    {"sharedpages", "native-video-shared-pages", "tcc -o .\\sharedpagetest C:\\Tests\\sharedpagetest.c && .\\sharedpagetest", 0,
     {"共有ページ ", "検査 0 失敗"}, "共有ページ失敗", 120, true, true}, /* writer-ro faults on purpose */
    {"pager", "native-pager-shared-memory", "tcc -o .\\pagertest C:\\Tests\\pagertest.c && .\\pagertest", 0,
     {"ページャー ", "検査 0 失敗"}, "ページャー失敗", 120, true, true}, /* two touchers fault on purpose */
    {"indexeddb", "async-native-storage", "tcc -IC:\\Tests -o .\\indexeddbguesttest C:\\Tests\\indexeddbguesttest.c && .\\indexeddbguesttest", 0,
     {"indexeddbguesttest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"csscache", "completed-external-ast", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest", 0,
     {"csscache: 27 checks, 0 failures"}, "FAIL", 120, false, true},
    {"paralleljs", "native-dom-hooks", "tcc -IC:\\Tests -o .\\paralleljstest C:\\Tests\\paralleljstest.c && .\\paralleljstest", 0,
     {"paralleljstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"currentscript", "classic-checkpoint", "tcc -IC:\\Tests -o .\\currentscripttest C:\\Tests\\currentscripttest.c && .\\currentscripttest", 0,
     {"currentscripttest: 21 checks, 0 failed"}, "FAIL", 120, false, true},
    {"waitingstream", "inputless-native-parser", "tcc -IC:\\Tests -o .\\waitingstreamtest C:\\Tests\\waitingstreamtest.c && .\\waitingstreamtest", 0,
     {"waitingstreamtest: 29 checks, 0 failed"}, "FAIL", 120, false, true},
    {"svggeometry", "native-viewbox-path-ctm", "tcc -IC:\\Tests -o .\\svggeometrytest C:\\Tests\\svggeometrytest.c && .\\svggeometrytest", 0,
     {"svggeometrytest: 109 checks, 0 failed"}, "FAIL", 120, false, true},
    {"parserdedup", "native-transaction-delta", "tcc -IC:\\Tests -o .\\parserdeduptest C:\\Tests\\parserdeduptest.c && .\\parserdeduptest", 0,
     {"parserdedup: 78 checks, 0 failures"}, "FAIL", 120, false, true},
    {"parserdedup", "render-resume", "tcc -IC:\\Tests -o .\\parserdeduptest C:\\Tests\\parserdeduptest.c && .\\parserdeduptest --render-resume", 0,
     {"parser-render-resume: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"translate", "native-translation-geometry-hit-overflow", "tcc -IC:\\Tests -o .\\translatetest C:\\Tests\\translatetest.c && .\\translatetest", 0,
     {"translatetest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"htmlparser", "modern-native-tree", "tcc -IC:\\Tests -o .\\html_modern_tree_test C:\\Tests\\html_modern_tree_test.c && .\\html_modern_tree_test", 0,
     {"html_modern_tree_test: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"htmlparser", "native-input", "tcc -IC:\\Tests -o .\\html_input_test C:\\Tests\\html_input_test.c && .\\html_input_test", 0,
     {"html_input_test: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"htmlparser", "encoding-restart", "tcc -IC:\\Tests -o .\\html_encoding_restarttest C:\\Tests\\html_encoding_restarttest.c && .\\html_encoding_restarttest", 0,
     {"html_encoding_restarttest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"htmlparser", "wpt-product-tree", "tcc -IC:\\Tests -o .\\html5lib_tree_test C:\\Tests\\html5lib_tree_test.c && .\\html5lib_tree_test", 0,
     {"html5lib: 610 selected, 610 passed, 0 failed, 0 explicit skipped"}, "FAIL", 120, false, true},
    {"htmlparser", "javascript-api", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest html-parser", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"htmlworker", "policy-inheritance", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest html-worker", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"htmlcsp", "native-policy-and-dynamic-code", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest html-csp", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"observerfairness", "continuous-rendering-feedback", "tcc -IC:\\Tests -o .\\observerfairnesstest C:\\Tests\\observerfairnesstest.c && .\\observerfairnesstest", 0,
     {"observerfairnesstest: 11 checks, 0 failed"}, "FAIL", 120, false, true},
    {"objectfallback", "native-unsupported-flow", "tcc -IC:\\Tests -o .\\objectfallbacktest C:\\Tests\\objectfallbacktest.c && .\\objectfallbacktest", 0,
     {"objectfallback: 37 checks, 0 failures"}, "FAIL", 120, false, true},
    {"fixedvisual", "native-viewport-contract", "tcc -IC:\\Tests -o .\\fixedvisualtest C:\\Tests\\fixedvisualtest.c && .\\fixedvisualtest", 0,
     {"fixedvisual: 57 checks, 0 failures"}, "FAIL", 120, false, true},
    {"canvascontextprobe", "debug-only-native-context", "tcc -IC:\\Tests -o .\\canvascontextprobetest C:\\Tests\\canvascontextprobetest.c && .\\canvascontextprobetest", 0,
     {"canvascontextprobe: 22 checks, 0 failed"}, "FAIL", 120, false, true},
    {"sandboxprofile", "generation-bound-native-policy", "tcc -IC:\\Tests -o .\\sandboxprofiletest C:\\Tests\\sandboxprofiletest.c && .\\sandboxprofiletest", 0,
     {"sandboxprofiletest: 94 checks, 0 failed"}, "FAIL", 120, false, true},
    {"navigator", "window-worker-identity", "tcc -IC:\\Tests -o .\\navigatortest C:\\Tests\\navigatortest.c && .\\navigatortest", 0,
     {"Navigator languages=", "navigatortest: 264 checks, 0 failed"}, "FAIL", 120, false, true},
    {"browserdeferred", "hover-fifo-boundaries", "tcc -IC:\\Tests -o .\\browserdeferredtest C:\\Tests\\browserdeferredtest.c && .\\browserdeferredtest", 0,
     {"browserdeferred: 27 checks, 0 failed"}, "FAIL", 120, false, true},
    {"canvasgradient", "native-linear-radial-rgba", "tcc -IC:\\Tests -o .\\canvasgradienttest C:\\Tests\\canvasgradienttest.c && .\\canvasgradienttest", 0,
     {"canvasgradient: 89 checks, 0 failed"}, "FAIL", 120, false, true},
    {"lineclamp", "legacy-native-fragments", "tcc -IC:\\Tests -o .\\lineclamptest C:\\Tests\\lineclamptest.c && .\\lineclamptest", 0,
     {"lineclamp: 83 checks, 0 failures"}, "FAIL", 120, false, true},
    {"lineclampflow", "anonymous-inline-list-height", "tcc -IC:\\Tests -o .\\lineclampflowtest C:\\Tests\\lineclampflowtest.c && .\\lineclampflowtest", 0,
     {"lineclampflow: 35 checks, 0 failures"}, "FAIL", 120, false, true},
    {"svgcsssize", "viewbox-css-auto-axis", "tcc -IC:\\Tests -o .\\svgcsssizetest C:\\Tests\\svgcsssizetest.c && .\\svgcsssizetest", 0,
     {"svgcsssize: 37 checks, 0 failures"}, "FAIL", 120, false, true},
    {"cssanimation", "native-scoped-opacity-clock", "tcc -IC:\\Tests -o .\\cssanimationtest C:\\Tests\\cssanimationtest.c && .\\cssanimationtest", 0,
     {"cssanimation: 94 checks, 0 failures (JS 10/0)"}, "FAIL", 120, false, true},
    {"lineclamppredicate", "debug-eligible-candidate-publication", "tcc -IC:\\Tests -o .\\lineclamppredicatetest C:\\Tests\\lineclamppredicatetest.c && .\\lineclamppredicatetest", 0,
     {"lineclamppredicate: 25 checks, 0 failures"}, "FAIL", 120, false, true},
    {"cssanimationregression", "live-resource-publication-failed-boundary", "tcc -IC:\\Tests -o .\\cssanimationregressiontest C:\\Tests\\cssanimationregressiontest.c && .\\cssanimationregressiontest", 0,
     {"cssanimationregression: 34 checks, 0 failures (JS 1/0)"}, "FAIL", 120, false, true},
    {"lineclampreentry", "same-pass-measurement-stale-visibility", "tcc -IC:\\Tests -o .\\lineclampreentrytest C:\\Tests\\lineclampreentrytest.c && .\\lineclampreentrytest", 0,
     {"lineclampreentry: 60 checks, 0 failures"}, "FAIL", 120, false, true},
    {"cssanimationpixel", "opacity-invariant-blend-failed-boundary", "tcc -IC:\\Tests -o .\\cssanimationpixeltest C:\\Tests\\cssanimationpixeltest.c && .\\cssanimationpixeltest", 0,
     {"cssanimationpixel: 4 checks, 0 failures"}, "FAIL", 120, false, true},
    {"imagebitmap", "native-owned-decode-raster", "tcc -IC:\\Tests -o .\\imagebitmaptest C:\\Tests\\imagebitmaptest.c && .\\imagebitmaptest", 0,
     {"imagebitmap: 109 checks, 0 failed (JS 84)"}, "FAIL", 120, false, true},
    {"imagebitmapheap", "sized-allocator-lifetime", "tcc -IC:\\Tests -o .\\imagebitmaptest C:\\Tests\\imagebitmaptest.c && .\\imagebitmaptest --heap", 0,
     {"imagebitmapheap: 9 checks, 0 failed"}, "FAIL", 120, false, true},
    {"imagebitmapoom", "observed-allocation-request", "tcc -IC:\\Tests -o .\\imagebitmaptest C:\\Tests\\imagebitmaptest.c && .\\imagebitmaptest --heap-oom", 0,
     {"imagebitmapheapoom: 3 checks, 0 failed"}, "FAIL", 120, false, true},
    {"imagebitmappromise", "early-rejection-microtasks", "tcc -IC:\\Tests -o .\\imagebitmappromisetest C:\\Tests\\imagebitmappromisetest.c && .\\imagebitmappromisetest", 0,
     {"imagebitmappromise: 14 checks, 0 failed (JS 10)"}, "FAIL", 120, false, true},
    {"web", "engine", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest", 0, {"webtest: ", ", 0 failed"}, "FAIL",
     120, false, true},
    {"web", "javascript", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest", 0,
     /* Whole-suite QEMU wall time includes repeated fresh browser contexts and
        7,000+ collation comparisons; the page's 5s JS watchdog is unchanged. */
     {"jstest: ", ", 0 failed"}, "FAIL", 900, false, true},
    {"web", "html-interfaces", "tcc -o .\\jstest C:\\Tests\\jstest.c && .\\jstest html-interfaces", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "html-tree-builder", "tcc -IC:\\Tests -o .\\lexbortest C:\\Tests\\lexbortest.c && .\\lexbortest", 0,
     {"lexbortest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "attribute-nodes", "tcc -IC:\\Tests -o .\\js_attributes_native C:\\Tests\\js_attributes_native.c && .\\js_attributes_native", 0,
     {"js_attributes_native: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "shadow-dom-native", "tcc -IC:\\Tests -o .\\shadownativetest C:\\Tests\\shadownativetest.c && .\\shadownativetest", 0,
     {"shadownativetest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "semantic-elements", "tcc -IC:\\Tests -o .\\semanticstest C:\\Tests\\semanticstest.c && .\\semanticstest", 0,
     {"semanticstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "form-control-values", "tcc -IC:\\Tests -o .\\js_form_controls_native C:\\Tests\\js_form_controls_native.c && .\\js_form_controls_native", 0,
     {"js_form_controls_native: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"pointerfocus", "native-area-and-textarea-scroll", "tcc -IC:\\Tests -o .\\js_form_controls_native C:\\Tests\\js_form_controls_native.c && .\\js_form_controls_native pointer-focus", 0,
     {"Textarea native scroll API checks 27", "js_form_controls_native/pointer-focus: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "formdata-entries", "tcc -IC:\\Tests -o .\\js_form_controls_native C:\\Tests\\js_form_controls_native.c && .\\js_form_controls_native formdata-entries", 0,
     {"js_form_controls_native/formdata-entries: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "shadow-dom-render", "tcc -IC:\\Tests -o .\\shadowtest C:\\Tests\\shadowtest.c && .\\shadowtest", 0,
     {"shadowtest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "collation-decoder", "tcc -IC:\\Tests -o .\\js_collator_native C:\\Tests\\js_collator_native.c && .\\js_collator_native", 0,
     {"js_collator_native: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "http-transport", "tcc -o .\\httptest C:\\Tests\\httptest.c && .\\httptest", 0,
     {"httptest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "cookies", "tcc -o .\\cookietest C:\\Tests\\cookietest.c && .\\cookietest", 0,
     {"cookietest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "media-policy", "tcc -o .\\mediapolicytest C:\\Tests\\mediapolicytest.c && .\\mediapolicytest", 0,
     {"mediapolicytest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "media-events", "tcc -o .\\mediatest C:\\Tests\\mediatest.c && .\\mediatest", 0,
     {"mediatest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "mouse-boundaries", "tcc -o .\\hovertest C:\\Tests\\hovertest.c && .\\hovertest C:\\Tests\\js_hover_cases.js", 0,
     {"hovertest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "image-elements", "tcc -o .\\imagetest C:\\Tests\\imagetest.c && .\\imagetest", 0,
     {"imagetest: 0 failures, 0 unexpected errors"}, "FAIL", 120, false, true},
    {"web", "image-ownership", "tcc -o .\\imageownershiptest C:\\Tests\\imageownershiptest.c && .\\imageownershiptest", 0,
     {"imageownershiptest: ", ", 0 failures, 0 unexpected errors"}, "FAIL", 120, false, true},
    {"web", "metadata-invalidation", "tcc -IC:\\Tests -o .\\metadatatest C:\\Tests\\metadatatest.c && .\\metadatatest", 0,
     {"metadatatest: 85 checks, 0 failures"}, "FAIL", 120, false, true},
    {"web", "message-channels", "tcc -IC:\\Tests -o .\\messagingtest C:\\Tests\\messagingtest.c && .\\messagingtest", 0,
     {"messagingtest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"web", "import-maps", "tcc -IC:\\Tests -o .\\js_importmaps_native C:\\Tests\\js_importmaps_native.c && .\\js_importmaps_native", 0,
     {"js_importmaps_native: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "relational-selectors", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest", 0,
     {"cssrelational: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "relational-shadow-boundaries", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --boundaries", 0,
     {"cssrelational-boundaries: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "media-range-calc", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --media-calc", 0,
     {"cssrelational-media-calc: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "font-src-fallback", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --font-sources", 0,
     {"csscache-font-sources: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compat", "parallel-css-order", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --parallel-css", 0,
     {"csscache-parallel: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compat", "initial-render-blockers", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --render", 0,
     {"csscache-render: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compat", "modulepreload", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest modulepreload", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"flexzero", "definite-zero-basis-wrap", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest --flex-zero", 0,
        {"webtest-flex-zero: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptgeometry", "original-inline-and-slotted", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --observer-geometry http://10.0.2.2:8766/", 0,
        {"限定WPT 2 ページ 4 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"wptflex", "original-invalid-flex", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --flex-invalid http://10.0.2.2:8766/", 0,
        {"限定WPT 1 ページ 6 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"fixedtable", "declared-and-first-row-tracks", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest --fixed-table", 0,
        {"webtest-fixed-table: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"boxshadow", "native-paint-and-inline-lifetime", "tcc -IC:\\Tests -o .\\boxshadowtest C:\\Tests\\boxshadowtest.c && .\\boxshadowtest", 0,
        {"boxshadowtest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"shadowdamage", "atomic-ink-and-block-fallback", "tcc -IC:\\Tests -o .\\boxshadowtest C:\\Tests\\boxshadowtest.c && .\\boxshadowtest --damage-only", 0,
        {"boxshadow-damage: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"radius", "percentage-replaced-overflow-hit-and-retained-mask", "tcc -IC:\\Tests -o .\\boxshadowtest C:\\Tests\\boxshadowtest.c && .\\boxshadowtest --radius", 0,
        {"radius-native: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"imageassociation", "partial-hover-background-mask-and-scroll", "tcc -IC:\\Tests -o .\\boxshadowtest C:\\Tests\\boxshadowtest.c && .\\boxshadowtest --image-association", 0,
        {"image-association-native: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"childlistlocality", "candidate-work-bound", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --child-list-locality", 0,
        {"cssrelational-child-locality: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"layers", "normal-important-inline-revert-and-scope", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --layers", 0,
        {"cssrelational-layers: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"layerimports", "reversed-completion-scope", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --layers-import", 0,
        {"csscache-layer-imports: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"layernames", "named-font-and-keyframes-metadata", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --layers-names", 0,
        {"csscache-layer-names: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"textoverflow", "logical-text-retained-paint-ellipsis", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest --text-overflow", 0,
        {"webtest-text-overflow: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"buttonformat", "author-flex-grid-and-native-activation", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest --button-formatting", 0,
        {"webtest-button-formatting: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"flexfreeze", "redistribute-min-max-clamped-flex-items", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest --flex-freeze", 0,
        {"webtest-flex-freeze: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"autotablepercent", "intrinsic-percentage-and-specified-width-distribution", "tcc -IC:\\Tests -o .\\webtest C:\\Tests\\webtest.c && .\\webtest --auto-table-percent", 0,
        {"webtest-auto-table-percent: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptradius", "original-parsing-and-computed", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --radius http://10.0.2.2:8766/", 0,
        {"限定WPT 3 ページ 48 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"wptradiuscomputed", "original-computed-fourteen-only", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --radius-computed http://10.0.2.2:8766/", 0,
        {"限定WPT 1 ページ 14 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"computedstyle", "native-property-publication-readonly-live-and-indices", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest computed-style-declaration", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"inputsnapshot", "coherent-passive-wheel-cancellation-and-generation", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest input-snapshot", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"windownamed", "native-window-named-property-lifetime", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest window-named", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"domsubtree", "native-subtree-publication-and-existing-dom-behavior", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest detached && .\\jstest dom && .\\jstest documents && .\\jstest collections && .\\jstest lifetime", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"cssownerevents", "owner-critical-imports-without-global-join", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --owner-events", 0,
        {"csscache-owner-events: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"frameviewport", "parent-layout-to-child-cssom-without-paint", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --frame-viewport", 0,
        {"csscache-frame-viewport: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"colorformat", "computed-opaque-and-alpha-roundtrip", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --color-format", 0,
        {"csscache-color-format: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"layerdynamic", "cssom-order-rollback-and-scope-mutation", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --layers-dynamic", 0,
        {"cssrelational-layer-dynamic: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptlayers", "original-normal-important-inline-and-media", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --layers http://10.0.2.2:8766/", 0,
        {"限定WPT 4 ページ 55 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 180, false, true},
    {"wptlayersfailed", "original-inline-named-target-and-child-viewport", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --layers-failed http://10.0.2.2:8766/", 0,
        {"限定WPT 2 ページ 12 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 180, false, true},
    {"wptellipsis", "original-fullpixel-flex-container-reference", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --text-overflow-ref http://10.0.2.2:8766/", 0,
        {"限定WPT画像 1 組 0 不一致画素 0 実行基盤失敗"}, "FAIL", 120, false, true},
    {"entrycontract", "branded-v1-entry-snapshot", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest intersection-entry", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"childlistdirty", "local-parent-and-relational-witness", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --child-list", 0,
        {"cssrelational-child-list: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"wptinline", "original-inline-fragment-offset-repair", "tcc -o .\\wptperformancetest C:\\Tests\\wptperformancetest.c && .\\wptperformancetest --observer-inline http://10.0.2.2:8766/", 0,
        {"限定WPT 1 ページ 3 原ケース 0 失敗 0 ページ失敗"}, "FAIL", 120, false, true},
    {"moduleapirepair", "sri-query-asset", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest modulepreload-api", 0,
        {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"observergeometry", "inline-and-slotted-intersection", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest intersection-inline", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compat", "css-supports", "tcc -IC:\\Tests -o .\\csssupportstest C:\\Tests\\csssupportstest.c && .\\csssupportstest", 0,
     {"csssupportstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compatrepair", "relational-selectors-initialized", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest", 0,
     {"cssrelational: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "relational-shadow-initialized", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --boundaries", 0,
     {"cssrelational-boundaries: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "media-calc-initialized", "tcc -IC:\\Tests -o .\\cssrelationaltest C:\\Tests\\cssrelationaltest.c && .\\cssrelationaltest --media-calc", 0,
     {"cssrelational-media-calc: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "font-failure-quiet-snapshot", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --font-failure-noop", 0,
     {"csscache-font-noop: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compatrepair", "render-fallback-one-shot", "tcc -IC:\\Tests -o .\\csscachetest C:\\Tests\\csscachetest.c && .\\csscachetest --render", 0,
     {"csscache-render: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"compatrepair", "modulepreload-native-executable", "tcc -IC:\\Tests -o .\\jstest C:\\Tests\\jstest.c && .\\jstest modulepreload", 0,
     {"jstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"compatrepair", "native-filter-cancel-commit", "tcc -IC:\\Tests -o .\\csssupportstest C:\\Tests\\csssupportstest.c && .\\csssupportstest --filter-cancel", 0,
     {"csssupportstest-filter-cancel: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "css-supports", "tcc -IC:\\Tests -o .\\csssupportstest C:\\Tests\\csssupportstest.c && .\\csssupportstest", 0,
     {"csssupportstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "storage", "tcc -IC:\\Tests -o .\\storagetest C:\\Tests\\storagetest.c && .\\storagetest --isolated-scratch", 0,
     {"storagetest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "worker-network", "tcc -o .\\webnettest C:\\Tests\\webnettest.c && .\\webnettest", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"web", "tcp-capacity-recovery", "tcc -o .\\tcpcapacitytest C:\\Tests\\tcpcapacitytest.c && .\\tcpcapacitytest", 0,
     {"tcpcapacitytest: ", ", 0 failed"}, "FAIL", 180, false, true},

    {"nettransport", "dns-deadline-smp", "tcc -o .\\nettransporttest C:\\Tests\\nettransporttest.c && .\\nettransporttest", 0,
     {"nettransporttest: 0 failed"}, "FAIL", 120, false, true},
    {"nettransport", "default-dns-stream", "tcc -o .\\nettransporttest C:\\Tests\\nettransporttest.c && .\\nettransporttest --stream-candidates", 0,
     {"nettransport stream: 0 failed", "STREAM-CANDIDATES plain candidates=2 attempts=2"}, "FAIL", 30, false, true},
    {"nettransport", "tls-dns-candidates", "tcc -o .\\nettransporttest C:\\Tests\\nettransporttest.c && .\\nettransporttest --tls-candidates", 0,
     {"nettransport TLS candidates: 0 failed", "STREAM-CANDIDATES tls=1"}, "FAIL", 90, false, true},
    {"networkpacing", "pause-fairness-batch", "tcc -o .\\webnettest C:\\Tests\\webnettest.c && .\\webnettest --network-pacing", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"transport8", "http-pool-h2", "tcc -o .\\transport8 C:\\Tests\\transport8.c && .\\transport8", 0,
     {"transport8:" , "0 failed"}, "FAIL", 120, false, true},
    {"transport8", "h2-late-cancel", "tcc -o .\\transport8 C:\\Tests\\transport8.c && .\\transport8 --late-cancel", 0,
     {"transport8 late-cancel: 5 checks, 0 failed"}, "FAIL", 30, false, true},
    {"transport8", "h2-header-budget", "tcc -o .\\transport8 C:\\Tests\\transport8.c && .\\transport8 --header-budget", 0,
     {"transport8 header-budget: 12 checks, 0 failed"}, "FAIL", 60, false, true},
    {"transport8", "tls-handshake-cancel", "tcc -o .\\transport8 C:\\Tests\\transport8.c && .\\transport8 --tls-cancel", 0,
     {"transport8 TLS-cancel:" , "0 failed"}, "FAIL", 120, false, true},
    {"net", "tls-alpn-session-resume", "tcc -o .\\transport8 C:\\Tests\\transport8.c && .\\transport8 --tls", 0,
     {"transport8 TLS:" , "0 failed"}, "FAIL", 120, false, true},

    /* TCP against the host's test server (no internet needed), clean and with simulated loss */
    {"tcpperf", "clean-repeat", "tcc -o .\\tcptest C:\\Tests\\tcptest.c && .\\tcptest --clean-repeat 5", 0,
     {"TCPPERF sample=5", "tcptest: 0 failed"}, "FAIL", 120, false, true},
    {"tcp", "bulk-and-loss", "tcc -o .\\tcptest C:\\Tests\\tcptest.c && .\\tcptest", 0, {"tcptest: 0 failed"},
     "FAIL", 300, false, true},

    /* network (QEMU user networking: gateway 10.0.2.2, DNS 10.0.2.3) */
    {"net", "dhcp", "ifconfig", 0, {"10.0.2.15"}},
    {"net", "ping-gateway", "ping -c 2 10.0.2.2", 0, {"2 received"}, NULL, 20},
    {"net", "dns", "host example.com", 0, {"example.com"}, "not found", 20},
    {"net", "http", "fetch http://example.com", 0, {"Example Domain"}, NULL, 30},
    {"net", "https", "fetch https://example.com", 0, {"Example Domain"}, NULL, 30},
    {"net", "https-rejects-bad-cert", "fetch https://expired.badssl.com/", NONZERO, {"fetch:"}, NULL, 30},
    {"net", "webfetch-tls", "tcc -o .\\webnettest C:\\Tests\\webnettest.c && .\\webnettest --tls", 0,
     {"webnettest: 0 failed"}, "FAIL", 90, false, true},

    /* the AI agent, offline: no key is ever needed for the tests */
    {"agent", "usage", "agent --help", ANY, {"usage: agent"}},
    {"agent", "unreachable-endpoint",
     "echo 'endpoint=http://10.0.2.2:9/v1' > .\\bad.conf; echo 'api_key=test' >> .\\bad.conf; "
     "agent -c .\\bad.conf say hi",
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

/* <TEMP>\runtests, made by main, and the current folder of the test that runs: a new one in it,
   removed after the test. A test's compiled program is tens of MiB, and hundreds of them left
   behind fill C:. */
static char runs_folder[4096], work_folder[4096];

/* folder and everything in it */
static void remove_tree(const char *path) {
    struct n_entry *e = NULL;
    n_result count = n_folder_entries(path, &e);
    for (n_result i = 0; i < count; i++) {
        char child[4096];
        if (n_path_join(path, e[i].name, child, sizeof child) < 0) continue;
        if (e[i].kind == N_KIND_FOLDER) remove_tree(child);
        else n_entry_remove(child);
    }
    free(e);
    n_entry_remove(path);
}

/* Runs "sh -c cmd" in work_folder with a closed stdin and stdout/stderr on one channel that is drained while it runs.
   Returns 1 when it ended (*status set), 0 when it overran timeout_ms and was stopped, -1 when it could not start. */
static int run_shell(const char *cmd, int timeout_ms, struct capture *c, int *status) {
    n_handle out[2], in[2];
    if (n_channel_pair(out) < 0) return -1;
    if (n_channel_pair(in) < 0) { n_handle_close(out[0]); n_handle_close(out[1]); return -1; }
    n_handle_close(in[0]); /* reading stdin reports the end at once, like an empty device */
    const char *args[] = {"sh", "-c", cmd};
    char folder[sizeof work_folder + 8];
    snprintf(folder, sizeof folder, "FOLDER=%s", work_folder);
    const char *settings[] = {folder};
    int32_t handles[] = {in[1], out[1], out[1]};
    struct n_launch launch = {.arguments = args, .settings = settings, .handles = handles,
                              .argument_count = 3, .setting_count = 1, .handle_count = 3};
    n_handle app = n_app_launch(N_FOLDER_SYSTEM "\\sh", &launch);
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
    remove_tree(work_folder); /* left by a run that was stopped */
    if (n_folder_create(work_folder) < 0) {
        result("FAIL", t, "cannot make its folder");
        nfail++;
        return;
    }
    int r = run_shell(t->cmd, timeout, &cap, &st);
    remove_tree(work_folder);
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
    n_result made = n_path_join(n_temp_folder(), "runtests", runs_folder, sizeof runs_folder);
    if (made >= 0 && (made = n_folder_create(runs_folder)) == -N_ERR_EXISTS) made = 0;
    if (made >= 0) made = n_path_join(runs_folder, "test", work_folder, sizeof work_folder);
    if (made < 0) {
        printf("TESTS ERROR no folder to run the tests in: %s\n", n_result_text(made));
        free(only);
        return 1;
    }
    struct n_entry st;
    bool have_tcc_src = n_entry_info("C:\\Tests\\tcc\\tcc.c", &st) == 0;
    /* the disk has LLVM when its Nocturne build was made (ports/llvm/README.md) */
    bool have_llvm = n_entry_info("C:\\Nocturne\\System\\llvm", &st) == 0;
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
        if (!strcmp(t->group, "clang") && !have_llvm) skip = "no LLVM on the disk (ports/llvm)";
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
