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
    /* Focused product routes for the eight WHATWG repairs. */
    {"slop8", "script-lifecycle", "tcc -o /home/scriptlifecycletest /data/tests/scriptlifecycletest.c && /home/scriptlifecycletest", 0,
     {"scriptlifecycletest: 15 checks, 0 failed"}, "FAIL", 60, false, true},
    {"slop8", "storage-windows", "tcc -o /home/storagewindowtest /data/tests/storagewindowtest.c && /home/storagewindowtest", 0,
     {"STORAGE-WINDOW-DONE", "failures=0"}, "FAIL", 120, false, true},
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
    {"csscache", "completed-external-ast", "tcc -I/data/tests -o /home/csscachetest /data/tests/csscachetest.c && /home/csscachetest", 0,
     {"csscache: 23 checks, 0 failures"}, "FAIL", 120, false, true},
    {"paralleljs", "native-dom-hooks", "tcc -I/data/tests -o /home/paralleljstest /data/tests/paralleljstest.c && /home/paralleljstest", 0,
     {"paralleljstest: ", ", 0 failed"}, "FAIL", 120, false, true},
    {"currentscript", "classic-checkpoint", "tcc -I/data/tests -o /home/currentscripttest /data/tests/currentscripttest.c && /home/currentscripttest", 0,
     {"currentscripttest: 21 checks, 0 failed"}, "FAIL", 120, false, true},
    {"waitingstream", "inputless-native-parser", "tcc -I/data/tests -o /home/waitingstreamtest /data/tests/waitingstreamtest.c && /home/waitingstreamtest", 0,
     {"waitingstreamtest: 29 checks, 0 failed"}, "FAIL", 120, false, true},
    {"svggeometry", "native-viewbox-path-ctm", "tcc -I/data/tests -o /home/svggeometrytest /data/tests/svggeometrytest.c && /home/svggeometrytest", 0,
     {"svggeometrytest: 109 checks, 0 failed"}, "FAIL", 120, false, true},
    {"parserdedup", "native-transaction-delta", "tcc -I/data/tests -o /home/parserdeduptest /data/tests/parserdeduptest.c && /home/parserdeduptest", 0,
     {"parserdedup: 37 checks, 0 failures"}, "FAIL", 120, false, true},
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
    {"web", "css-supports", "tcc -I/data/tests -o /home/csssupportstest /data/tests/csssupportstest.c && /home/csssupportstest", 0,
     {"csssupportstest: ", ", 0 failures"}, "FAIL", 120, false, true},
    {"web", "storage", "tcc -I/data/tests -o /home/storagetest /data/tests/storagetest.c && /home/storagetest --isolated-scratch", 0,
     {"storagetest: ", ", 0 failed"}, "FAIL", 180, false, true},
    {"web", "worker-network", "tcc -o /home/webnettest /data/tests/webnettest.c && /home/webnettest", 0,
     {"webnettest: 0 failed"}, "FAIL", 120, false, true},
    {"web", "tcp-capacity-recovery", "tcc -o /home/tcpcapacitytest /data/tests/tcpcapacitytest.c && /home/tcpcapacitytest", 0,
     {"tcpcapacitytest: ", ", 0 failed"}, "FAIL", 180, false, true},

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

static void show_output(const char *out) {
    for (const char *line = out, *nl; *line; line = nl ? nl + 1 : line + strlen(line)) {
        nl = strchr(line, '\n');
        printf("  | %.*s\n", nl ? (int)(nl - line) : (int)strlen(line), line);
    }
    fflush(stdout);
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
        char *out = slurp(OUT);
        show_output(out);
        free(out);
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
        show_output(out);
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
        if (!net_waited && (!strcmp(t->group, "tcp") || !strcmp(t->group, "net") || !strcmp(t->name, "worker-network"))) {
            net_waited = true;
            if (!net_wait_up(20000)) printf("runtests: the network did not come up\n");
        }
        run(t);
    }
    printf("TESTS DONE pass=%d fail=%d skip=%d (%lu s)\n", npass, nfail, nskip, (unsigned long)((uptime_ms() - t0) / 1000));
    return nfail != 0;
}
