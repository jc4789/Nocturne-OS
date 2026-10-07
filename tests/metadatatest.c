/* Internal regression: DOM/URL reads must not rebuild authored CSS. This
   supplements the public-site QEMU comparison; it is not website acceptance. */
#include <stdio.h>
#include "nocturne.h"
#include "webi.h"
static web_doc *doc;
static uint64_t before;
static int failures, checks;
static bool done;
static void log_message(void *opaque, int level, const char *message) {
    (void)opaque;
    if (level == 2 || !strncmp(message, "FAIL ", 5)) { failures++; printf("%s\n", message); }
    if (!strcmp(message, "META-BEGIN")) before = doc->profile.rescans;
    if (!strcmp(message, "META-READS")) {
        checks++;
        if (doc->profile.rescans != before) { failures++; puts("FAIL metadata read rebuilt resources"); }
    }
    if (!strncmp(message, "PASS ", 5)) checks++;
    if (!strcmp(message, "META-DONE")) done = true;
}
int main(void) {
    const char *html = "<!doctype html><html><head><style id=s>#probe{width:17px}</style>"
        "</head><body><div id=probe></div><script>"
        "function check(n,v){console.log((v?'PASS ':'FAIL ')+n)}"
        "setTimeout(()=>{console.log('META-BEGIN');"
        "const style=document.getElementById('s'),probe=document.getElementById('probe');"
        "style.textContent='#probe{width:37px}';"
        "const base=document.createElement('base');base.href='http://metadata.test/new/';document.head.appendChild(base);"
        "for(let i=0;i<40;i++){const n=document.createElement('div');document.body.appendChild(n);"
        "check('body-head-'+i,document.documentElement.contains(document.body)&&document.head.contains(style));"
        "check('base-'+i,n.baseURI==='http://metadata.test/new/');}"
        "const script=document.createElement('script');script.textContent='globalThis.metadataScript=42';document.body.appendChild(script);"
        "check('dynamic-inline',globalThis.metadataScript===42);"
        "base.remove();check('removed-base',document.baseURI==='http://metadata.test/old/index.html');"
        "console.log('META-READS');"
        "check('pending-css-preserved',getComputedStyle(probe).width==='37px');"
        "style.textContent='#probe{width:53px}';void document.body;"
        "check('layout-css-preserved',probe.getBoundingClientRect().width===53);"
        "console.log('META-DONE');},0);</script></body></html>";
    struct web_host host = {.console=log_message};
    doc = web_live(html, strlen(html), "http://metadata.test/old/index.html", "utf-8", &host);
    if (!doc) { puts("FAIL metadata document allocation"); return 1; }
    web_layout(doc, 800, 600);
    uint64_t deadline = uptime_ms() + 10000;
    while (!done && uptime_ms() < deadline) { web_tick(doc, uptime_ms()); msleep(1); }
    if (!done || checks != 85) { failures++; printf("FAIL metadata completion: done=%d checks=%d\n", done, checks); }
    web_free(doc);
    printf("metadatatest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
