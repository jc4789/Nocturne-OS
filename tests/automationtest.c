/* Self-check of the WPT automation tool (--wpt-automation), not of the browser's behaviour:
 * the step-to-event conversion of user/apps/browser_automation.h, and that a real web_live
 * realm has __nocturne_automation and navigator.webdriver only with a host that drives input
 * (web_host.automation), never in a frame, and settles its promise only through
 * web_automation_done in a task. What the browser does with the events is for WPT's
 * infrastructure/testdriver/ tests in the guest. */
#include <nocturne.h>
#include <web.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser_automation.h"

static int checks, failures;
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL automation %s\n", name); }
}

static void pure(void) {
    static const uint32_t bits[] = {1, 4, 2, 8, 16, 0};
    for (uint32_t b = 0; b < 6; b++) check("WebDriver button as native mask", automation_button(b) == bits[b]);
    static const struct { uint32_t raw, key; unsigned modifier; } keys[] = {
        {'a', 'a', 0}, {'A', 'A', 0}, {'\r', NKEY_ENTER, 0}, {'\n', NKEY_ENTER, 0}, {0x3042, 0x3042, 0},
        {0xE003, NKEY_BACKSPACE, 0}, {0xE004, NKEY_TAB, 0}, {0xE006, NKEY_ENTER, 0}, {0xE007, NKEY_ENTER, 0},
        {0xE00C, NKEY_ESC, 0}, {0xE00D, ' ', 0}, {0xE00E, NKEY_PGUP, 0}, {0xE012, NKEY_LEFT, 0},
        {0xE015, NKEY_DOWN, 0}, {0xE017, NKEY_DELETE, 0}, {0xE01A, '0', 0}, {0xE023, '9', 0}, {0xE029, '/', 0},
        {0xE031, NKEY_F1, 0}, {0xE03C, NKEY_F12, 0}, {0xE054, NKEY_PGUP, 0}, {0xE05D, NKEY_DELETE, 0},
        {0xE008, 0, AUTOMATION_SHIFT}, {0xE050, 0, AUTOMATION_SHIFT}, {0xE009, 0, AUTOMATION_CONTROL},
        {0xE051, 0, AUTOMATION_CONTROL}, {0xE00A, 0, AUTOMATION_ALT}, {0xE052, 0, AUTOMATION_ALT},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        uint32_t key; unsigned modifier;
        const char *why = automation_key(keys[i].raw, &key, &modifier);
        check("WebDriver key as native key", !why && key == keys[i].key && modifier == keys[i].modifier);
    }
    static const uint32_t untypeable[] = {0xE000, 0xE001, 0xE002, 0xE005, 0xE00B, 0xE03D, 0xE053, 0xE040,
                                          0x100, 0x1FF, 0xD800, 0xDFFF, 0x110000};
    for (size_t i = 0; i < sizeof untypeable / sizeof *untypeable; i++) {
        uint32_t key; unsigned modifier;
        check("a key the native keyboard lacks is refused", automation_key(untypeable[i], &key, &modifier) != NULL);
    }
    check("wheel lines round", automation_lines(120, 48) == 3 && automation_lines(96, 48) == 2 && automation_lines(-120, 48) == -3);
    check("a small wheel delta is one line", automation_lines(10, 48) == 1 && automation_lines(-10, 48) == -1);
    check("no wheel delta is no line", automation_lines(0, 48) == 0);

    struct automation_device d = {0};
    struct gui_event e[3];
    const char *error;
    struct web_automation_step s = {.kind = WEB_AUTOMATION_MOVE, .x = 3.7, .y = 4.2};
    int n = automation_events(&d, &s, 48, e, &error);
    check("a move is one floored move", n == 1 && e[0].type == EV_MOUSE_MOVE && e[0].x == 3 && e[0].y == 4 && !e[0].buttons);
    check("a move to the same place is nothing", automation_events(&d, &s, 48, e, &error) == 0);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_DOWN, .value = 0, .x = 10, .y = 10};
    n = automation_events(&d, &s, 48, e, &error);
    check("a press away from the pointer moves first", n == 2 && e[0].type == EV_MOUSE_MOVE && e[0].x == 10 &&
          e[1].type == EV_MOUSE_DOWN && e[1].buttons == 1 && e[1].x == 10 && e[1].y == 10);
    check("pressing a held button is nothing", automation_events(&d, &s, 48, e, &error) == 0);
    s.value = 2;
    n = automation_events(&d, &s, 48, e, &error);
    check("a second button adds to the mask", n == 1 && e[0].type == EV_MOUSE_DOWN && e[0].buttons == 3);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_UP, .value = 0, .x = 10, .y = 10};
    n = automation_events(&d, &s, 48, e, &error);
    check("a release carries the buttons after it", n == 1 && e[0].type == EV_MOUSE_UP && e[0].buttons == 2);
    check("releasing a button not held is nothing", automation_events(&d, &s, 48, e, &error) == 0);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_KEY_DOWN, .value = 0xE008};
    check("a modifier key is no event", automation_events(&d, &s, 48, e, &error) == 0 && d.modifiers == AUTOMATION_SHIFT);
    s.value = 'A';
    n = automation_events(&d, &s, 48, e, &error);
    check("a key carries the held modifiers and the pointer", n == 1 && e[0].type == EV_KEY && e[0].key == 'A' &&
          e[0].mods == NMOD_SHIFT && e[0].pressed && e[0].x == 10 && e[0].y == 10);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_KEY_UP, .value = 0xE050};
    check("right Shift releases Shift", automation_events(&d, &s, 48, e, &error) == 0 && d.modifiers == 0);
    s.value = 'A';
    n = automation_events(&d, &s, 48, e, &error);
    check("a key release", n == 1 && e[0].type == EV_KEY && !e[0].pressed && e[0].mods == 0);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_WHEEL, .x = 5, .y = 6, .dx = 50, .dy = -100};
    n = automation_events(&d, &s, 48, e, &error);
    check("a wheel is vertical lines, then across with Shift", n == 2 && e[0].type == EV_WHEEL && e[0].wheel == -2 &&
          !(e[0].mods & NMOD_SHIFT) && e[1].wheel == 1 && (e[1].mods & NMOD_SHIFT) && e[1].x == 5 && e[1].y == 6);
    check("a wheel does not move the pointer", d.x == 10 && d.y == 10);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_KEY_DOWN, .value = 0xE03D};
    check("Meta is refused", automation_events(&d, &s, 48, e, &error) < 0 && error && d.modifiers == 0);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_DOWN, .value = 7};
    check("an unknown button is refused", automation_step_error(&s) != NULL);
    s = (struct web_automation_step){.kind = WEB_AUTOMATION_MOVE, .x = NAN};
    check("a position that is not finite is refused", automation_step_error(&s) != NULL);
    s = (struct web_automation_step){.kind = (enum web_automation_kind)99};
    check("an unknown step is refused", automation_step_error(&s) != NULL);
}

struct fixture {
    bool done, frame_done, resolved, rejected_refusal, rejected_error;
    int calls, page_checks, page_failed;
    uint64_t ids[4];
    struct web_automation_step first[3];
    size_t first_count;
    uint32_t first_duration;
    web_doc *document_seen;
};
static void log_line(void *opaque, int level, const char *text) {
    struct fixture *f = opaque;
    (void)level;
    if (!strcmp(text, "PAGE-DONE")) f->done = true;
    else if (!strcmp(text, "FRAME-DONE")) f->frame_done = true;
    else if (!strcmp(text, "RESOLVED")) f->resolved = true;
    else if (!strcmp(text, "REFUSED")) f->rejected_refusal = true;
    else if (!strcmp(text, "FAILED-AS-TOLD")) f->rejected_error = true;
    else if (!strncmp(text, "OK ", 3)) f->page_checks++;
    else if (!strncmp(text, "FAIL ", 5)) { f->page_checks++; f->page_failed++; printf("FAIL automation page %s\n", text + 5); }
}
static const char *drive(void *opaque, web_doc *document, uint64_t id,
                         const struct web_automation_step *steps, size_t count, uint32_t duration) {
    struct fixture *f = opaque;
    if (f->calls < 4) f->ids[f->calls] = id;
    f->calls++;
    f->document_seen = document;
    if (count == 1 && steps[0].kind == WEB_AUTOMATION_KEY_DOWN && steps[0].value == 'r') return "refused for the test";
    if (f->calls == 1 && count <= 3) {
        memcpy(f->first, steps, count * sizeof *steps);
        f->first_count = count; f->first_duration = duration;
    }
    return NULL;
}
static const char page[] =
    "<!doctype html><script>"
    "function check(n,v){console.log((v?'OK ':'FAIL ')+n);}"
    "const automated=typeof __nocturne_automation==='function';"
    "check('webdriver follows the host',navigator.webdriver===automated);"
    "check('the name is not enumerable',Object.keys(window).indexOf('__nocturne_automation')<0);"
    "if(automated){"
    "  __nocturne_automation([{type:'move',at:0,x:3.5,y:4},{type:'down',at:5,x:3.5,y:4,button:2},{type:'keyDown',at:5,key:0x61}],20)"
    "    .then(()=>console.log('RESOLVED'),e=>console.log('FAIL first run rejected '+e.message));"
    "  __nocturne_automation([{type:'keyDown',at:0,key:0x72}],0).then(()=>console.log('FAIL refused run resolved'),"
    "    e=>{if(e instanceof Error&&e.message==='refused for the test')console.log('REFUSED');else console.log('FAIL refusal '+e);});"
    "  try{__nocturne_automation([{type:'bogus',at:0}]);check('unknown type throws',false);}catch(e){check('unknown type throws',e instanceof TypeError);}"
    "  try{__nocturne_automation([{type:'move',at:5},{type:'move',at:1}]);check('time order',false);}catch(e){check('time order',e instanceof RangeError);}"
    "  try{__nocturne_automation([{type:'down',at:0,button:9}]);check('button range',false);}catch(e){check('button range',e instanceof RangeError);}"
    "}else check('no name at all',!('__nocturne_automation' in window));"
    "addEventListener('message',e=>{if(!e.data||e.data.kind!=='frame')return;"
    "  check('a frame has no port',e.data.port==='undefined'&&!e.data.inside);"
    "  check('a frame sees webdriver too',e.data.webdriver===automated);console.log('FRAME-DONE');});"
    "const frame=document.createElement('iframe');"
    "frame.srcdoc='<script>parent.postMessage({kind:\"frame\",port:typeof __nocturne_automation,inside:(\"__nocturne_automation\" in window),webdriver:navigator.webdriver},\"*\")<\\/script>';"
    "document.documentElement.appendChild(frame);"
    "console.log('PAGE-DONE');"
    "</script>";
static void tick_until(web_doc *doc, const bool *flag, int ms) {
    uint64_t deadline = uptime_ms() + (uint64_t)ms;
    while (!*flag && uptime_ms() < deadline) { web_tick(doc, uptime_ms()); n_sleep_ms(1); }
}
static void realm(bool automated) {
    struct fixture f = {0};
    struct web_host host = {.opaque = &f, .console = log_line};
    if (automated) host.automation = drive;
    web_doc *doc = web_live(page, sizeof page - 1, "https://automation.test/", "utf-8", &host);
    check("the page loads", doc != NULL);
    if (!doc) return;
    tick_until(doc, &f.done, 15000);
    tick_until(doc, &f.frame_done, 15000);
    check("the page ran", f.done);
    check("the frame reported", f.frame_done);
    if (!automated) {
        check("without the host nothing is asked", f.calls == 0);
        check("page checks without the host", f.page_checks == 5 && !f.page_failed);
        web_free(doc);
        return;
    }
    tick_until(doc, &f.rejected_refusal, 5000);
    check("a refused run rejects at once with the host's reason", f.rejected_refusal);
    check("the host got the runs the page made", f.calls == 2 && f.document_seen == doc);
    check("the host got the steps", f.first_count == 3 && f.first_duration == 20 &&
          f.first[0].kind == WEB_AUTOMATION_MOVE && f.first[0].x == 3.5 && f.first[0].y == 4 && f.first[0].at == 0 &&
          f.first[1].kind == WEB_AUTOMATION_DOWN && f.first[1].value == 2 && f.first[1].at == 5 &&
          f.first[2].kind == WEB_AUTOMATION_KEY_DOWN && f.first[2].value == 0x61);
    for (int i = 0; i < 20; i++) { web_tick(doc, uptime_ms()); n_sleep_ms(1); }
    check("a run waits for the host", !f.resolved);
    web_automation_done(doc, f.ids[0] + 1000, NULL); /* no such run */
    for (int i = 0; i < 20; i++) { web_tick(doc, uptime_ms()); n_sleep_ms(1); }
    check("another id settles nothing", !f.resolved);
    web_automation_done(doc, f.ids[0], NULL);
    check("done is due at once", web_deadline(doc) >= 0 && web_deadline(doc) <= (int64_t)uptime_ms());
    tick_until(doc, &f.resolved, 5000);
    check("done resolves the run in a task", f.resolved);
    const char *more = "__nocturne_automation([],0).then(()=>console.log('FAIL resolved'),"
                       "e=>console.log(e.message==='The document went away'?'FAILED-AS-TOLD':'FAIL message '+e.message))";
    check("a third run", web_console_eval(doc, more, strlen(more)) && f.calls == 3);
    web_automation_done(doc, f.ids[2], "The document went away");
    tick_until(doc, &f.rejected_error, 5000);
    check("done with an error rejects with it", f.rejected_error);
    const char *fourth = "__nocturne_automation([{type:'move',at:0,x:1,y:1}],0)";
    check("a run left open", web_console_eval(doc, fourth, strlen(fourth)) && f.calls == 4);
    check("page checks with the host", f.page_checks == 7 && !f.page_failed);
    web_free(doc); /* the open run's record goes with its document */
}

int main(void) {
    pure();
    realm(false);
    realm(true);
    printf("automation: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
