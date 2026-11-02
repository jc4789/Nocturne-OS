/* WPT's Server-sent events tests (the eventsource/ files below, run by WPT's own
 * resources/testharness.js) against the production EventSource (js_event_source.js) in two node
 * vm realms, each built from its bootstrap's own lines:
 *   a Window's: js_bootstrap.js from its js_abort.js include to the Fetch globals (js_abort.js,
 *     js_fetch.js and the Window's transport port), then its EventSource port (the
 *     js_event_source.js include to the EventSource global);
 *   a Worker's: js_worker_runtime.js's Event and EventTarget, its event handler lines, its
 *     AbortSignal and Fetch (js_fetch.js, workerFetchTransport, the fetchBridge port) and its
 *     EventSource port.
 * Both realms' DOM events are the Worker runtime's Event, EventTarget and handlers, and their
 * MessageEvent is js_worker_messaging.js's (its structured clone is a stand-in with only the
 * transfer list it reads). A Window's document is a stub; URLs are node's parser (the realms'
 * urlBridge.encodingParse), UTF-8 decoding is node's TextDecoder behind the native decoder's
 * contract (host.textDecoder: a BOM is stripped unless ignoreBOM).
 * The native transports are doubles: host.fetch's notify frames (js.c) for a Window, NJW_FRAME
 * through workerFetchTransport.frame and NJW_LOADED (js_worker.c) for a Worker, one frame per
 * resume, from the event loop. Behind both is one model of what webnet.c and webfetch.c do with a
 * request, written after Fetch's main fetch, HTTP fetch and HTTP-redirect fetch: only HTTP(S)
 * URLs; the response tainting "cors" for a cross-origin URL in mode "cors", the Origin header,
 * the CORS check of every response (a redirect's too) and a CORS-preflight fetch when an author
 * header is not CORS-safelisted (Last-Event-ID is not: Fetch has no exception for it) or the
 * method is not simple; redirects (301, 302, 303, 307, 308, 20 at most, the tainted origin
 * flag); a cookie jar (host-only, Path=/, Expires and Max-Age in the past delete) used when the
 * credentials mode is "include" or "same-origin" with the tainting "basic"; and webfetch.c's
 * cache_headers (cache mode "no-store" or "reload": Pragma and Cache-Control `no-cache` unless
 * present, never author headers). The server it asks answers as wptserve runs these handlers,
 * written here after their Python (query values percent-decoded to bytes, isomorphic strings, as
 * request.GET): eventsource/resources/message.py (mime, message, newline, sleep), message2.py
 * (its writes as separate chunks, then 2 s, forever), last-event-id.py, last-event-id2.py,
 * reconnect-fail.py, status-error.py, status-reconnect.py, cors.py (its run=message,
 * status-reconnect, redirect and cache-control; a request without Origin is wptserve's 500, as
 * the handler reads request.headers[b"origin"]), cors-cookie.py (for OPTIONS too, as wptserve
 * runs it for any method) and common/redirect.py; accept.event_stream and
 * cache-control.event_stream are text/event-stream (wptserve's type for the extension) with
 * pipe=sub's {{headers[name]}}; any other path is 404. Every subdomain of web-platform.test on
 * port 8000 is that server; any other host is a network error. The page is
 * http://web-platform.test:8000/<file as WPT serves it>. eventsource/contract/*.py are this
 * file's own handlers for the checks after the WPT files (chunks.py: the c values as chunks,
 * each after delay ms, then the end, a network error (error) or nothing (hold); id.py: an id
 * with non-ASCII characters, then the Last-Event-ID header's bytes as hex).
 * Not run: eventsource-request-cancellation.window.js (window.stop() and the load event),
 * eventsource-constructor-document-domain.window.js (document.domain), the .htm files (iframes,
 * a Worker started from a page) and shared-worker/. request-credentials.window.js's reconnection
 * sends Last-Event-ID to another origin, so it is preflighted, and cors-cookie.py's answer to the
 * preflight has no Access-Control-Allow-Headers: its subtests time out here, as Fetch's text
 * says they must. A document's destruction (js.c's
 * web_js_free) is not modeled: each file's realm is torn down after it, its timers and requests
 * cancelled. This is not the guest browser and not wptrunner. It fails when a subtest the Window
 * passes in a file both realms run does not pass in the Worker (one definition), or when a check
 * after the WPT files fails.
 * Run: node tests/eventsource_wpt_contract.cjs [--wpt=DIR] [--verbose] [--only=FILE] */
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
const assert = require('node:assert/strict');
const {streamsSource} = require('./cache_storage_contract.cjs');
const web = 'user/libc/web/';
const option = name => { const a = process.argv.find(x => x.startsWith('--' + name + '=')); return a ? a.slice(name.length + 3) : null; };
const wpt = option('wpt') || process.env.WPT_ROOT || 'D:/Programes/cloned repos/wpt';
const only = option('only'), verbose = process.argv.includes('--verbose');
const HOST = 'web-platform.test', ORIGIN = 'http://web-platform.test:8000';
const read = file => fs.readFileSync(file, 'utf8');
const expand = text => text.replace(/\/\* @include ([a-z_]+\.js) \*\//g, (_, name) => read(web + name));
/* From the line holding start to the end of the line holding end. */
function lines(text, start, end, label) {
    const a = text.indexOf(start);
    assert(a >= 0, label + ': ' + start);
    const from = text.lastIndexOf('\n', a) + 1, b = text.indexOf(end, a);
    assert(b > a, label + ': ' + end);
    const to = text.indexOf('\n', b + end.length - 1);
    return text.slice(from, to < 0 ? text.length : to + 1);
}

/* ---- the checks after the WPT files: testharness files of this test's own ---- */
const contract = {
    'eventsource/contract-request.any.js': String.raw`
async_test(t => {
  const source = new EventSource('resources/message.py?contract=request');
  t.add_cleanup(() => source.close());
  source.onopen = t.step_func_done(() => {
    const q = __queries.find(q => q.url.includes('contract=request'));
    assert_true(!!q, 'a request');
    assert_equals(q.method, 'GET');
    assert_equals(q.headers.get('accept'), 'text/event-stream', 'Accept');
    assert_false(q.headers.has('last-event-id'), 'no Last-Event-ID at first');
    assert_equals(q.mode, 'cors', 'mode');
    assert_equals(q.credentials, 1, 'credentials mode same-origin');
    assert_equals(q.cache, 1, 'cache mode no-store');
    assert_equals(q.redirect, 'follow', 'redirect mode');
    assert_equals(q.destination, '', 'destination');
    assert_true(q.streamed, 'streamed');
    assert_true(__initiators.some(([id, type]) => id === q.id && type === 'other'), 'initiator type other');
  });
}, 'the request: GET, Accept, mode cors, credentials same-origin, cache no-store, initiator type other');
async_test(t => {
  const source = new EventSource('resources/message.py?contract=credentials', {withCredentials: true});
  t.add_cleanup(() => source.close());
  assert_true(source.withCredentials);
  source.onopen = t.step_func_done(() => {
    assert_equals(__queries.find(q => q.url.includes('contract=credentials')).credentials, 2, 'credentials mode include');
  });
}, 'withCredentials: the credentials mode include');
async_test(t => {
  const source = new EventSource('contract/id.py');
  t.add_cleanup(() => source.close());
  let n = 0;
  source.onmessage = t.step_func(e => {
    if (++n === 1) { assert_equals(e.data, 'first'); assert_equals(e.lastEventId, '\u00e9\u2026'); return; }
    assert_equals(e.data, 'c3a9e280a6', 'the header value is the last event ID string, UTF-8 encoded');
    assert_equals(e.lastEventId, '\u00e9\u2026');
    t.done();
  });
}, 'reestablishing: Last-Event-ID');
async_test(t => {
  const source = new EventSource('contract/chunks.py?c=data%3Aa%0D&c=%0Adata%3Ab%0D&c=%0Ddata%3A%C3&c=%A9%0A%0Adata%3Ax%0D&c=data%3Ay%0A%0A');
  t.add_cleanup(() => source.close());
  const seen = [];
  source.onmessage = t.step_func(e => {
    seen.push(e.data);
    if (seen.length === 3) { assert_array_equals(seen, ['a\nb', '\u00e9', 'x\ny']); t.done(); }
  });
  source.onerror = t.unreached_func('error');
}, 'chunk boundaries: a CRLF split between chunks, a CR at a chunk end, a UTF-8 sequence split');
async_test(t => {
  const source = new EventSource('contract/chunks.py?c=data%3Aone%0A%0A&c=data%3Atwo%0A%0A&delay=50&hold&contract=close');
  let n = 0;
  source.onmessage = t.step_func(e => {
    n++;
    assert_equals(e.data, 'one');
    source.close();
    assert_equals(source.readyState, EventSource.CLOSED);
    t.step_timeout(() => {
      assert_true(__queries.find(q => q.url.includes('contract=close')).cancelled, 'the fetch was aborted');
      assert_equals(n, 1, 'nothing after close()');
      t.done();
    }, 200);
  });
  source.onerror = t.unreached_func('error');
}, 'close() aborts the fetch');
async_test(t => {
  const source = new EventSource('resources/message.py?contract=wait&message=retry%3A100%0Adata%3Ax');
  let opened = 0;
  source.onopen = () => { opened++; };
  source.onerror = t.step_func(() => {
    assert_equals(source.readyState, EventSource.CONNECTING);
    source.close();
    t.step_timeout(() => {
      assert_equals(__queries.filter(q => q.url.includes('contract=wait')).length, 1, 'no new request');
      assert_equals(opened, 1);
      t.done();
    }, 400);
  });
}, 'close() in the reconnection time: no new request');
async_test(t => {
  const source = new EventSource('http://nowhere.invalid:8000/contract-unreachable');
  let errors = 0;
  source.onerror = t.step_func(() => {
    errors++;
    assert_equals(source.readyState, EventSource.CLOSED);
    t.step_timeout(() => {
      assert_equals(errors, 1);
      assert_equals(__queries.filter(q => q.url.includes('contract-unreachable')).length, 1, 'no new request');
      t.done();
    }, 300);
  });
}, 'a network error before any connection was announced fails the connection');
async_test(t => {
  const source = new EventSource('contract/chunks.py?c=retry%3A10%0Adata%3Aa%0A%0A&error&contract=drop');
  t.add_cleanup(() => source.close());
  let messages = 0, errors = 0;
  source.onmessage = t.step_func(() => {
    if (++messages < 2) return;
    assert_equals(errors, 1);
    assert_equals(__queries.filter(q => q.url.includes('contract=drop')).length, 2);
    t.done();
  });
  source.onerror = t.step_func(() => { errors++; assert_equals(source.readyState, EventSource.CONNECTING); });
}, 'a network error after the connection was announced reestablishes it');
async_test(t => {
  const url = 'data:text/event-stream,data:x%0A%0A', source = new EventSource(url);
  t.add_cleanup(() => source.close());
  assert_equals(source.url, url);
  source.onmessage = t.step_func_done(e => { assert_equals(e.data, 'x'); assert_equals(e.origin, 'null', 'an opaque origin'); });
  source.onerror = t.unreached_func('error');
}, 'a data: URL');
test(() => {
  assert_equals(EventSource.length, 1);
  assert_equals(EventSource.name, 'EventSource');
  assert_equals(Object.getPrototypeOf(EventSource.prototype), EventTarget.prototype);
  for (const [name, value] of [['CONNECTING', 0], ['OPEN', 1], ['CLOSED', 2]]) for (const o of [EventSource, EventSource.prototype]) {
    const d = Object.getOwnPropertyDescriptor(o, name);
    assert_equals(d.value, value, name);
    assert_false(d.writable, name);
    assert_true(d.enumerable, name);
    assert_false(d.configurable, name);
  }
  for (const name of ['url', 'withCredentials', 'readyState', 'onopen', 'onmessage', 'onerror']) {
    const d = Object.getOwnPropertyDescriptor(EventSource.prototype, name);
    assert_equals(typeof d.get, 'function', name);
    assert_true(d.enumerable, name);
    assert_throws_js(TypeError, () => d.get.call({}), name + ' of another object');
  }
  assert_throws_js(TypeError, () => EventSource.prototype.close.call({}));
  assert_throws_js(TypeError, () => new EventSource());
  assert_throws_js(TypeError, () => new EventSource(Symbol()));
  assert_throws_js(TypeError, () => new EventSource('x', 5), 'a dictionary');
  assert_throws_dom('SyntaxError', () => new EventSource('http://[bad'));
  const s = new EventSource('resources/message.py?contract=idl', {withCredentials: 1});
  assert_equals(Object.prototype.toString.call(s), '[object EventSource]');
  assert_true(s.withCredentials);
  assert_equals(s.readyState, EventSource.CONNECTING);
  s.close();
  assert_equals(s.readyState, EventSource.CLOSED);
  s.close();
  assert_false(new EventSource('resources/message.py?contract=idl2', null).withCredentials);
}, 'the interface: constants, attributes, brand checks, argument conversion');
done();`,
    /* A Worker's close(): the runtime's (closed, workerFetchTransport.close(), its timers
       cleared); the parent then cancels the Worker's requests. */
    'eventsource/contract-terminate.worker.js': String.raw`
async_test(t => {
  const source = new EventSource('contract/chunks.py?c=data%3Aone%0A%0A&c=data%3Atwo%0A%0A&delay=50&hold&contract=terminate');
  let events = 0;
  source.onmessage = t.step_func(() => {
    events++;
    __terminate();
    t.step_timeout(() => {
      assert_equals(events, 1, 'nothing is dispatched after the Worker closed');
      assert_equals(__queries.filter(q => q.url.includes('contract=terminate')).length, 1, 'no new request');
      t.done();
    }, 300);
  });
  source.onerror = () => { events++; };
}, 'a Worker that closes ends its EventSource fetch and dispatches nothing more');
done();`,
};

/* ---- the files ---- */
const BOTH = ['window', 'worker'];
const files = [
    ...['event-data', 'eventsource-constructor-empty-url', 'eventsource-constructor-url-bogus', 'eventsource-cross-origin-redirect-origin',
        'eventsource-eventtarget', 'eventsource-onmessage-trusted', 'eventsource-onmessage', 'eventsource-onopen', 'eventsource-prototype',
        'eventsource-url', 'format-bom-2', 'format-bom', 'format-comments', 'format-data-before-final-empty-line', 'format-field-data',
        'format-field-event-empty', 'format-field-event', 'format-field-id-2', 'format-field-id', 'format-field-parsing',
        'format-field-retry-bogus', 'format-field-retry-empty', 'format-field-retry', 'format-field-unknown', 'format-leading-space',
        'format-mime-bogus', 'format-mime-trailing-semicolon', 'format-mime-valid-bogus', 'format-newlines', 'format-null-character',
        'format-utf-8', 'request-accept', 'request-cache-control'].map(name => ({file: 'eventsource/' + name + '.any.js', kinds: BOTH})),
    {file: 'eventsource/dedicated-worker/eventsource-constructor-no-new.any.js', kinds: BOTH},
    ...['eventsource-close', 'eventsource-constructor-non-same-origin', 'eventsource-constructor-stringify', 'eventsource-cross-origin',
        'eventsource-reconnect', 'format-field-id-3', 'format-field-id-null', 'request-credentials', 'request-redirect',
        'request-status-error'].map(name => ({file: 'eventsource/' + name + '.window.js', kinds: ['window']})),
    {file: 'eventsource/dedicated-worker/eventsource-eventtarget.worker.js', kinds: ['worker']},
    {file: 'eventsource/contract-request.any.js', kinds: BOTH, own: true},
    {file: 'eventsource/contract-terminate.worker.js', kinds: ['worker'], own: true},
].filter(entry => !only || entry.file.includes(only));

/* ---- the server ---- */
const latin1 = text => Buffer.from(String(text), 'latin1');
/* wptserve's request.GET: name -> values, percent-decoded bytes as isomorphic strings. */
function query(u) {
    const params = new Map(), bytes = s => Buffer.from(s.replace(/\+/g, ' ').replace(/%([0-9a-fA-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))), 'latin1').toString('latin1');
    for (const pair of u.search.slice(1).split('&')) {
        if (!pair) continue;
        const eq = pair.indexOf('='), key = bytes(eq < 0 ? pair : pair.slice(0, eq)), value = bytes(eq < 0 ? '' : pair.slice(eq + 1));
        if (!params.has(key)) params.set(key, []);
        params.get(key).push(value);
    }
    return params;
}
/* A response: status, statusText, headers [[name, value]], headDelay (ms before the headers),
   chunks [{delay, bytes}] (an iterable), then tail: 'end', 'error' (a network error after the
   headers) or 'hold' (no end); or {error} for a network error. */
function respond(status, statusText, headers, body, extra = {}) {
    const bytes = Buffer.isBuffer(body) ? body : latin1(body ?? '');
    return {status, statusText, headers, headDelay: extra.headDelay || 0, chunks: extra.chunks || (bytes.length ? [{delay: 0, bytes}] : []), tail: extra.tail || 'end'};
}
const setCookie = (name, value) => ['Set-Cookie', name + '=' + value + '; Path=/'];
const deleteCookie = name => ['Set-Cookie', name + '=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0'];
const eventStream = ['Content-Type', 'text/event-stream'];
function* message2() {
    const writes = ['data:msg', '\n', 'data: msg', '\n\n', ':', '\n', 'falsefield:msg', '\n\n', 'falsefield:msg', '\n', 'Data:data', '\n\n', 'data', '\n\n', 'data:end', '\n\n'];
    for (;;) { for (const w of writes) yield {delay: 0, bytes: latin1(w)}; yield {delay: 2000, bytes: null}; }
}
function serve(req) {
    let u;try{u = new URL(req.url);}catch(_){return {error: 'invalid URL'};}
    if (u.port !== '8000' || (u.hostname !== HOST && !u.hostname.endsWith('.' + HOST))) return {error: 'host not found: ' + u.host};
    const GET = query(u), first = (name, fallback) => GET.has(name) ? GET.get(name)[0] : fallback;
    const header = name => req.headers.has(name) ? req.headers.get(name) : undefined;
    const cookies = new Map();
    for (const part of (header('cookie') || '').split(';')) { const eq = part.indexOf('='); if (eq > 0) cookies.set(part.slice(0, eq).trim(), part.slice(eq + 1).trim()); }
    const fail = () => respond(500, 'Internal Server Error', [], '');
    /* message.py */
    const message = (extra = []) => {
        const mime = first('mime', 'text/event-stream'), body = first('message', 'data: data') + (first('newline', null) === 'none' ? '' : '\n\n') + '\n';
        return respond(200, 'OK', [...extra, ['Content-Type', mime]], body, {headDelay: Number(first('sleep', '0'))});
    };
    /* status-reconnect.py */
    const statusReconnect = (extra = []) => {
        const code = first('status', '204'), name = first('id', code), cookie = 'request' + name;
        if ((cookies.get(cookie) ?? '') === code) return respond(200, 'OK', [...extra, eventStream, deleteCookie(cookie)], 'data: data\n\n');
        return respond(Number(code), 'TEST', [...extra, eventStream, setCookie(cookie, code)], 'retry: 2\n' + (GET.has('ok_first') ? 'data: ok\n\n' : ''));
    };
    /* common/redirect.py */
    const redirect = (extra = []) => {
        const status = /^\s*[+-]?\d+\s*$/.test(first('status', '')) ? Number(first('status')) : 302, location = first('location');
        if (location === undefined) return fail();
        const headers = [...extra, ['Location', location]], origin = header('origin');
        if (GET.has('enable-cors') && origin) headers.push(['Content-Type', 'text/plain'], ['Access-Control-Allow-Origin', origin], ['Access-Control-Allow-Credentials', 'true']);
        return respond(status, 'Found', headers, '');
    };
    /* wptserve's sub pipe, its {{headers[name]}} */
    const headersSub = text => text.replace(/\{\{headers\[([^\]]+)\]\}\}/g, (_, name) => header(name.toLowerCase()) ?? '');
    const sub = text => GET.has('pipe') && first('pipe') === 'sub' ? headersSub(text) : text;
    switch (decodeURIComponent(u.pathname)) {
    case '/eventsource/resources/message.py': return message();
    case '/eventsource/resources/message2.py': return respond(200, 'OK', [eventStream, ['Cache-Control', 'no-cache']], '', {chunks: message2()});
    case '/eventsource/resources/last-event-id.py': {
        const last = header('last-event-id') ?? '';
        if (last) return respond(200, 'OK', [eventStream], 'data: ' + last + '\n\n');
        return respond(200, 'OK', [eventStream], 'id: ' + first('idvalue', '\xe2\x80\xa6') + '\nretry: 200\ndata: hello\n\n');
    }
    case '/eventsource/resources/last-event-id2.py': {
        const type = /^\s*[+-]?\d+\s*$/.test(first('type', '1')) ? Number(first('type', '1')) : 1;
        const bodies = {1: 'id: 1\ndata: 1\n\ndata: 2\n\nid: 2\ndata:3\n\ndata:4\n\n', 2: 'id: 1\ndata: 1\n\nid:\ndata:2\n\ndata:3\n\n', 3: 'id: 1\ndata: 1\n\nid\ndata:2\n\ndata:3\n\n'};
        return respond(200, 'OK', [eventStream], bodies[type] ?? 'data: invalid_test\n\n');
    }
    case '/eventsource/resources/reconnect-fail.py': {
        if (!GET.has('id')) return fail();
        const name = 'recon_fail_' + first('id'), state = cookies.get(name);
        if (state === 'opened') return respond(200, 'RECONNECT', [eventStream, setCookie(name, 'reconnected')], 'data: reconnected\n\n');
        if (state === 'reconnected') return respond(204, 'NO CONTENT (CLOSE)', [eventStream, deleteCookie(name)], 'data: closed\n\n');
        return respond(200, 'OPEN', [eventStream, setCookie(name, 'opened')], 'retry: 2\ndata: opened\n\n');
    }
    case '/eventsource/resources/status-error.py': {
        const status = first('status', '404');
        return respond(Number(status), 'HAHAHAHA', [eventStream], status === '204' || status === '205' ? '' : 'data: data\n\n');
    }
    case '/eventsource/resources/status-reconnect.py': return statusReconnect();
    case '/eventsource/resources/cors.py': {
        if (header('origin') === undefined) return fail();
        const cors = [['Access-Control-Allow-Origin', first('origin', header('origin'))], ['Access-Control-Allow-Credentials', first('credentials', 'true')]];
        const run = first('run');
        /* run=cache-control: the file through pipes.sub, whatever the query says. */
        if (run === 'cache-control') return respond(200, 'OK', [...cors, eventStream], headersSub(read(path.join(wpt, 'eventsource/resources/cache-control.event_stream'))));
        if (run === 'redirect') return redirect(cors);
        if (run === 'message') return message(cors);
        if (run === 'status-reconnect') return statusReconnect(cors);
        return respond(200, 'OK', cors, '');
    }
    case '/eventsource/resources/cors-cookie.py': {
        if (header('origin') === undefined) return fail();
        const last = header('last-event-id') ?? '', ident = first('ident', 'test'), cookie = cookies.has(ident) ? 'COOKIE' : 'NO_COOKIE';
        const origin = first('origin', header('origin')), credentials = first('credentials', 'true'), headers = [];
        if (origin !== 'none') headers.push(['Access-Control-Allow-Origin', origin]);
        if (credentials !== 'none') headers.push(['Access-Control-Allow-Credentials', credentials]);
        if (last === '') return respond(200, 'OK', [...headers, eventStream, setCookie(ident, 'COOKIE')], 'id: 1\nretry: 200\ndata: first ' + cookie + '\n\n');
        if (last === '1') return respond(200, 'OK', [...headers, eventStream, ['Set-Cookie', ident + '=COOKIE; Path=/; Expires=Fri, 27 Jul 2001 00:00:00 GMT']], 'id: 2\ndata: second ' + cookie + '\n\n');
        return respond(200, 'OK', [...headers, ['Content-Type', 'stop']], 'data: ' + last + cookie + '\n\n');
    }
    case '/common/redirect.py': return redirect();
    case '/eventsource/resources/accept.event_stream':
    case '/eventsource/resources/cache-control.event_stream':
        return respond(200, 'OK', [eventStream], sub(read(path.join(wpt, u.pathname))));
    /* This file's own handlers. */
    case '/eventsource/contract/chunks.py': {
        const delay = Number(first('delay', '0'));
        return respond(200, 'OK', [eventStream], '', {chunks: (GET.get('c') || []).map(c => ({delay, bytes: latin1(c)})), tail: GET.has('error') ? 'error' : GET.has('hold') ? 'hold' : 'end'});
    }
    case '/eventsource/contract/id.py': {
        const last = header('last-event-id');
        if (last === undefined) return respond(200, 'OK', [eventStream], 'id: \xc3\xa9\xe2\x80\xa6\nretry: 10\ndata: first\n\n');
        return respond(200, 'OK', [eventStream], 'data: ' + latin1(last).toString('hex') + '\n\n');
    }
    }
    return respond(404, 'Not Found', [['Content-Type', 'text/plain']], 'Not Found');
}
const field = (res, name) => { const values = res.headers.filter(([n]) => n.toLowerCase() === name).map(([, v]) => v); return values.length ? values.join(', ') : null; };
const rawHeaders = res => 'HTTP/1.1 ' + res.status + ' ' + res.statusText + '\r\n' + res.headers.map(([n, v]) => n + ': ' + v + '\r\n').join('');

/* ---- the native layer's model (webnet.c, webfetch.c) ---- */
/* A cookie jar: host-only cookies with Path=/. */
function cookieJar() {
    const hosts = new Map();
    return {
        header(host) { const m = hosts.get(host); return m && m.size ? [...m].map(([n, v]) => n + '=' + v).join('; ') : null; },
        store(host, headers) {
            for (const [n, v] of headers) {
                if (n.toLowerCase() !== 'set-cookie') continue;
                const [pair, ...attributes] = v.split(';'), eq = pair.indexOf('=');
                if (eq <= 0) continue;
                let expired = false;
                for (const a of attributes) {
                    const eqa = a.indexOf('='), key = (eqa < 0 ? a : a.slice(0, eqa)).trim().toLowerCase(), value = eqa < 0 ? '' : a.slice(eqa + 1).trim();
                    if ((key === 'max-age' && Number(value) <= 0) || (key === 'expires' && Date.parse(value) <= Date.now())) expired = true;
                }
                if (!hosts.has(host)) hosts.set(host, new Map());
                if (expired) hosts.get(host).delete(pair.slice(0, eq).trim()); else hosts.get(host).set(pair.slice(0, eq).trim(), pair.slice(eq + 1).trim());
            }
        },
    };
}
/* Fetch's CORS-unsafe request-header names of the author headers, sorted. */
function unsafeNames(headers) {
    const out = [];
    for (const [name, value] of headers) {
        if (name === 'accept' && value.length <= 128 && !/[\x00-\x08\x0a-\x1f"():<>?@[\\\]{}\x7f]/.test(value)) continue;
        if ((name === 'accept-language' || name === 'content-language') && value.length <= 128 && /^[0-9A-Za-z *,\-.;=]*$/.test(value)) continue;
        if (name === 'content-type' && /^(application\/x-www-form-urlencoded|multipart\/form-data|text\/plain)\s*(;|$)/i.test(value)) continue;
        out.push(name);
    }
    return [...new Set(out)].sort();
}
/* Fetch's CORS check. */
function corsCheck(res, origin, include) {
    const allow = field(res, 'access-control-allow-origin');
    if (allow === null) return false;
    if (!include && allow === '*') return true;
    if (allow !== origin) return false;
    return !include || field(res, 'access-control-allow-credentials') === 'true';
}
const redirectStatuses = new Set([301, 302, 303, 307, 308]);
/* A request (url, method, author headers, credentials 0 omit / 1 same-origin / 2 include, mode,
   cache mode index, redirect mode) to its final response: {res, url, redirected, cors}, or
   {error}. */
function network(q, jar) {
    let url;try{url = new URL(q.url);}catch(_){return {error: 'invalid URL'};}
    let method = q.method, tainting = 'basic', taintedOrigin = false, redirected = false;
    for (let count = 0; ; count++) {
        if (url.protocol !== 'http:' && url.protocol !== 'https:') return {error: 'Only HTTP(S) URLs can be fetched'};
        if (url.origin !== ORIGIN) {
            if (q.mode === 'same-origin') return {error: 'Cross-origin request in mode same-origin'};
            if (q.mode === 'cors') tainting = 'cors';
        }
        const origin = taintedOrigin ? 'null' : ORIGIN, headers = new Map(q.headers);
        if (tainting === 'cors') headers.set('origin', origin);
        const credentialed = q.credentials === 2 || (q.credentials === 1 && tainting === 'basic');
        if (tainting === 'cors') {
            const unsafe = unsafeNames(q.headers);
            if (q.forcePreflight || unsafe.length || !['GET', 'HEAD', 'POST'].includes(method)) {
                const ask = new Map([['origin', origin], ['access-control-request-method', method]]);
                if (unsafe.length) ask.set('access-control-request-headers', unsafe.join(','));
                const pre = serve({method: 'OPTIONS', url: url.href, headers: ask});
                if (pre.error || pre.status < 200 || pre.status > 299 || !corsCheck(pre, origin, q.credentials === 2)) return {error: 'CORS preflight failed'};
                const allowed = (field(pre, 'access-control-allow-headers') || '').split(',').map(s => s.trim().toLowerCase()).filter(Boolean);
                for (const name of unsafe) if (!allowed.includes(name) && !(allowed.includes('*') && q.credentials !== 2)) return {error: 'CORS preflight does not allow ' + name};
            }
        }
        /* webfetch.c's cache_headers */
        if (q.cache === 1 || q.cache === 2) {
            if (!headers.has('pragma')) headers.set('pragma', 'no-cache');
            if (!headers.has('cache-control')) headers.set('cache-control', 'no-cache');
        }
        if (credentialed) { const cookie = jar.header(url.hostname); if (cookie) headers.set('cookie', cookie); }
        const res = serve({method, url: url.href, headers});
        if (res.error) return res;
        if (credentialed) jar.store(url.hostname, res.headers);
        if (tainting === 'cors' && !corsCheck(res, origin, q.credentials === 2)) return {error: 'CORS check failed'};
        const location = field(res, 'location');
        if (redirectStatuses.has(res.status) && location !== null && q.redirect !== 'manual') {
            if (q.redirect === 'error') return {error: 'Redirect in redirect mode error'};
            if (count >= 20) return {error: 'Too many redirects'};
            let next;try{next = new URL(location, url);}catch(_){return {error: 'Invalid Location'};}
            if (next.origin !== url.origin && ORIGIN !== url.origin) taintedOrigin = true;
            if ((res.status === 303 && method !== 'GET' && method !== 'HEAD') || ((res.status === 301 || res.status === 302) && method === 'POST')) method = 'GET';
            url = next;redirected = true;continue;
        }
        return {res, url: url.href, redirected, cors: tainting === 'cors'};
    }
}
/* A request's header block, as the native transports carry it: Name: value lines. */
function headerMap(raw) {
    const map = new Map();
    if (typeof raw === 'string') for (const line of raw.split(/\r\n/)) {
        const colon = line.indexOf(':');
        if (colon <= 0) continue;
        const name = line.slice(0, colon).trim().toLowerCase(), value = line.slice(colon + 1).trim();
        map.set(name, map.has(name) ? map.get(name) + ', ' + value : value);
    }
    return map;
}
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
/* One response delivered: deliver(kind, value) returns a promise for the consumer's resume. */
async function exchange(res, deliver, control) {
    if (res.error) { await sleep(0); if (!control.cancelled) deliver('error', res.error); return; }
    await sleep(res.headDelay); if (control.cancelled) return;
    await deliver('headers', res); if (control.cancelled) return;
    for (const chunk of res.chunks) {
        await sleep(chunk.delay); if (control.cancelled) return;
        if (chunk.bytes && chunk.bytes.length) { await deliver('chunk', chunk.bytes); if (control.cancelled) return; }
    }
    if (res.tail === 'hold') return;
    if (res.tail === 'error') { deliver('error', 'The connection was reset'); return; }
    deliver('end');
}
/* One resume per frame: a credit taken now or when it arrives. */
function credits() {
    const c = {count: 0, waiter: null, cancelled: false};
    c.give = () => { if (c.waiter) { const w = c.waiter; c.waiter = null; w(); } else c.count++; };
    c.take = () => c.count ? (c.count--, Promise.resolve()) : new Promise(resolve => { c.waiter = resolve; });
    return c;
}
const modeOf = (sameOrigin, noCors) => sameOrigin ? 'same-origin' : noCors ? 'no-cors' : 'cors';
/* js.c's side for a Window: host.fetch(...) -> {id, promise}; notify(1, Response) / (2,
   ArrayBuffer) / (3, error or undefined), the next frame after notify answers true or
   host.resumeFetch(id). */
function windowTransport(c, jar) {
    const t = {queries: [], controls: new Map(), bridge: null};
    let next = 0;
    t.host = {
        fetch(url, method, headers, body, sameOrigin, credentials, forcePreflight, redirect, cache, keepalive, noCors, noReferrer, notify,
              referrer, referrerPolicy, destination, synchronous) {
            if (synchronous) throw new c.__TypeError('A synchronous request is not modeled here');
            const id = ++next, control = credits();
            const q = {id, url, method, headers: headerMap(headers), credentials, mode: modeOf(sameOrigin, noCors), cache,
                redirect: ['follow', 'error', 'manual'][redirect], forcePreflight: !!forcePreflight, destination, streamed: !!notify, cancelled: false, control};
            t.queries.push(q);t.controls.set(id, control);
            let resolve, reject;const promise = new c.__Promise((a, b) => { resolve = a; reject = b; });
            const frame = (event, value, then) => { setTimeout(() => { if (control.cancelled) return; if (notify(event, value)) control.give(); if (then) then(); }, 0); return control.take(); };
            const result = network(q, jar), res = result.res || result, received = [];
            let head = null;
            exchange(res, (kind, value) => {
                /* After the frames already sent (js.c's records keep their order; a Fetch
                   observer's extra resumes give credits ahead). */
                if (kind === 'error') { const e = new c.__TypeError(value); setTimeout(() => { if (control.cancelled) return; if (notify) notify(3, e); reject(e); }, 0); return Promise.resolve(); }
                const response = (r, bytes) => t.bridge.response(r.status, result.url, rawHeaders(r), undefined, c.__arrayBuffer(bytes), result.redirected, r.statusText, result.cors ? 'cors' : 'basic');
                if (!notify) {
                    if (kind === 'headers') head = value; else if (kind === 'chunk') received.push(value); else resolve(response(head, Buffer.concat(received)));
                    return Promise.resolve();
                }
                if (kind === 'end') { setTimeout(() => { if (!control.cancelled) notify(3, undefined); }, 0); return Promise.resolve(); }
                if (kind === 'headers') { const r = response(value, Buffer.alloc(0)); return frame(1, r, () => resolve(r)); }
                return frame(2, c.__arrayBuffer(value));
            }, control);
            return {id, promise};
        },
        resumeFetch(id) { const control = t.controls.get(id); if (control) control.give(); },
        cancel(id) { const control = t.controls.get(id); if (!control) return; control.cancelled = true; t.queries.find(q => q.id === id).cancelled = true; },
    };
    t.cancelAll = () => { for (const control of t.controls.values()) control.cancelled = true; };
    return t;
}
/* js_worker.c's side for a Worker: host.request(query, stream) -> id; NJW_FRAME through
   workerFetchTransport.frame (headers [status, url, raw] with the cors and redirected flags, a
   chunk, the end), the next after each host.resume(id); a network error is NJW_LOADED's. */
function workerTransport(c, jar) {
    const t = {queries: [], controls: new Map(), port: null};
    let next = 0;
    t.host = {
        request(r, stream) {
            const id = ++next, control = credits();
            const q = {id, url: r.url, method: r.method, headers: headerMap(r.headers), credentials: r.credentials, mode: modeOf(r.sameOrigin, r.noCors), cache: r.cache,
                redirect: r.redirectError ? 'error' : r.redirectManual ? 'manual' : 'follow', forcePreflight: !!r.forcePreflight, destination: r.destination,
                streamed: !!stream, cancelled: false, control};
            t.queries.push(q);t.controls.set(id, control);
            const result = network(q, jar), res = result.res || result, flags = (result.cors ? 1 : 0) | (result.redirected ? 2 : 0), body = [];
            let head = null;
            const frame = (kind, value) => { setTimeout(() => { if (!control.cancelled) t.port.frame(id, value, kind); }, 0); return control.take(); };
            exchange(res, (kind, value) => {
                if (kind === 'error') { setTimeout(() => { if (!control.cancelled) t.port.loaded(id, [0, '', '', null, value], 0); }, 0); return Promise.resolve(); }
                if (!stream) {
                    if (kind === 'headers') head = value; else if (kind === 'chunk') body.push(value);
                    else t.port.loaded(id, [head.status, result.url, rawHeaders(head), c.__arrayBuffer(Buffer.concat(body)), ''], flags);
                    return Promise.resolve();
                }
                if (kind === 'headers') return frame((1 << 8) | flags, [value.status, result.url, rawHeaders(value)]);
                if (kind === 'chunk') return frame(2 << 8, c.__arrayBuffer(value));
                frame(3 << 8, '');return Promise.resolve();
            }, control);
            return id;
        },
        resume(id) { const control = t.controls.get(id); if (control) control.give(); },
        cancel(id) { const control = t.controls.get(id); if (!control) return; control.cancelled = true; t.queries.find(q => q.id === id).cancelled = true; },
    };
    t.cancelAll = () => { for (const control of t.controls.values()) control.cancelled = true; };
    return t;
}

/* ---- the realms ---- */
/* The native decoder's contract (js_encoding.c): a BOM is stripped unless ignoreBOM. */
const decoder = {
    textDecoder: (label, fatal, ignoreBOM) => ({inner: new TextDecoder(String(label), {fatal: !!fatal, ignoreBOM: !!ignoreBOM})}),
    textDecode: (d, bytes, offset, length, stream) => d.inner.decode(bytes ? Buffer.from(bytes, offset, length) : Buffer.alloc(0), {stream: !!stream}),
};
const dataURL = url => {
    const comma = url.indexOf(','), meta = url.slice(5, comma), body = url.slice(comma + 1);
    const b64 = /;base64$/i.test(meta), type = (b64 ? meta.slice(0, -7) : meta) || 'text/plain;charset=US-ASCII', octets = [];
    for (let i = 0; i < body.length; i++) { const hex = body.slice(i + 1, i + 3);
        if (body[i] === '%' && /^[0-9a-fA-F]{2}$/.test(hex)) { octets.push(parseInt(hex, 16)); i += 2; } else octets.push(...Buffer.from(body[i], 'utf8')); }
    const bytes = b64 ? Buffer.from(Buffer.from(octets).toString('latin1'), 'base64') : Buffer.from(octets);
    return [arrayBuffer(bytes), type];
};
const runtime = read(web + 'js_worker_runtime.js'), bootstrap = read(web + 'js_bootstrap.js');
const eventLines = expand(lines(runtime, '    const eventStates=new WeakMap()', '    Object.assign(globalThis,{DOMException,QuotaExceededError,Event,', 'worker events'));
/* js_worker_messaging.js's MessageEvent; its structured clone here has the transfer list only. */
const messaging = `const cloneData={transferList:value=>value===undefined?[]:Array.from(value),registerTransfer(){},registerUncloneable(){}};
    ${read(web + 'js_worker_messaging.js')}`;
const blobLines = `const blobBridge={brand:()=>false,bytes:()=>new ArrayBuffer(0),type:()=>'',fromBytes:(bytes,type)=>new Blob([bytes],{type})};
    const urlBridge={encodingParse(url,base){try{return new URL(url,base);}catch(_){return null;}}};`;
function context(kind, entry, jar) {
    const c = {console, URL, URLSearchParams, TextEncoder, TextDecoder, setTimeout, clearTimeout, Blob, File, queueMicrotask, Buffer};
    vm.createContext(c);
    c.self = vm.runInContext('globalThis', c);
    const page = kind === 'worker' ? entry.file.replace(/\.any\.js$/, '.any.worker.js') : entry.file.replace(/\.js$/, '.html');
    const url = ORIGIN + '/' + page, text = entry.own ? contract[entry.file] : read(path.join(wpt, entry.file)), title = /^\/\/ META: title=(.*)$/m.exec(text);
    Object.assign(c, {__url: url, __title: title ? title[1].trim() : '', __reported: [], __initiators: [], __text: text});
    if (title) c.META_TITLE = title[1].trim();
    c.__arrayBuffer = vm.runInContext('bytes=>{const out=new ArrayBuffer(bytes.length);new Uint8Array(out).set(bytes);return out;}', c);
    c.__TypeError = vm.runInContext('TypeError', c);c.__Promise = vm.runInContext('Promise', c);
    c.GLOBAL = {isWindow: () => kind === 'window', isWorker: () => kind === 'worker', isShadowRealm: () => false};
    const location = new URL(url);
    c.location = {href: url, origin: location.origin, protocol: location.protocol, host: location.host, hostname: location.hostname,
        port: location.port, pathname: location.pathname, search: '', hash: '', toString: () => url};
    /* The realm's timers; none run once it is torn down. */
    const timers = new Set();
    c.__timer = (_, fn, ms, args) => {
        if (c.__destroyed) return 0;
        const handle = setTimeout(() => { timers.delete(handle); if (!c.__destroyed) fn(...(args || [])); }, ms);
        timers.add(handle);return handle;
    };
    c.__clear = handle => { clearTimeout(handle); timers.delete(handle); };
    c.__stopTimers = () => { c.__destroyed = true; for (const handle of timers) clearTimeout(handle); timers.clear(); };
    const transport = (kind === 'worker' ? workerTransport : windowTransport)(c, jar);
    c.__transport = transport;c.__queries = transport.queries;
    vm.runInContext(read(web + 'js_dom_exception.js'), c);
    vm.runInContext(`globalThis.DOMException=domExceptionBridge.DOMException;
        const host={now:()=>Date.now(),promiseThen:(p,f,r)=>Promise.prototype.then.call(p,f,r),textDecoder:__decoder.textDecoder,textDecode:__decoder.textDecode};`,
        Object.assign(c, {__decoder: decoder}));
    vm.runInContext(streamsSource(), c);
    return c;
}
const realms = {
    /* js_bootstrap.js's Fetch and EventSource ports over a Window transport double. */
    window(entry, jar) {
        const c = context('window', entry, jar);
        const fetchPort = expand(lines(bootstrap, '    /* @include js_abort.js */', '    const {Headers,Request,Response,fetch}=fetchBridge;', 'window fetch'));
        const eventSourcePort = expand(lines(bootstrap, '    /* @include js_event_source.js */', "    Object.defineProperty(globalThis,'EventSource'", 'window event source'));
        const handlers = lines(runtime, '    function installHandlers(prototype,types){', '}});}', 'worker handlers');
        vm.runInContext(`(function(){
            const apply=Reflect.apply,DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
            function report(e){__reported.push(e);}
            let abortHandlerTarget=()=>false,abortSignals,abortListen,eventSourceHandlerTarget=()=>false;
            ${eventLines}
            const target=new EventTarget();
            /* The Window's event handler IDL attributes and dispatch, as a Worker has them. */
            const fileHandlers=new WeakMap(),handlerTarget=(v,t)=>eventSourceHandlerTarget(v,t)||abortHandlerTarget(v,t);
            ${handlers}
            const dispatch=(target,event)=>dispatchWorkerEvent(target,event);
            ${messaging}
            globalThis.MessageEvent=workerMessagingBridge.MessageEvent;
            const document={title:__title,getElementsByTagName:()=>[]},performanceBridge={initiator(id,type){__initiators.push([id,type]);}};
            const rawDom={get:(d,k)=>({baseURI:__url,fetchOrigin:'${ORIGIN}',fetchClientReferrer:__url,referrerPolicy:0,characterSet:'UTF-8'})[k]};
            ${blobLines}
            Object.assign(host,{url:()=>__url,timer:__timer,clear:__clear,microtask:fn=>queueMicrotask(fn),log(){},
                dataURL:(arrayBuffer=>${dataURL.toString()})(__arrayBuffer),cryptoDigest:()=>new ArrayBuffer(32),noCorsFetch:true,streamingFetch:true},__transport.host);
            ${fetchPort}
            fetchBridge.initialize();fetchBridge.initializeStreams(streamBridge);fetchBridge.initializeBlobs(blobBridge);
            __transport.bridge=fetchBridge;
            ${eventSourcePort}
            Object.assign(globalThis,{Event,EventTarget,Headers,Request,Response,fetch,AbortController,AbortSignal});
            globalThis.__document=document;
            /* The realm torn down: its timers and its requests cancelled. */
            globalThis.__teardown=()=>{__stopTimers();__transport.cancelAll();};
        })();`, c, {filename: 'js_bootstrap.js (Fetch, EventSource)'});
        return c;
    },
    /* js_worker_runtime.js's events, Fetch and EventSource lines over a Worker transport double. */
    worker(entry, jar) {
        const c = context('worker', entry, jar);
        const handlerLines = expand(lines(runtime, '    /* XMLHttpRequest and ProgressEvent are the Window\'s', '        return found;\n    }', 'worker handlers'));
        const fetchPart = expand(lines(runtime, '    /* @include js_abort.js */', '    const {Headers,Request,Response,fetch}=fetchBridge;', 'worker fetch'));
        const eventSourcePort = expand(lines(runtime, '    /* @include js_event_source.js */', "    Object.defineProperty(globalThis,'EventSource'", 'worker event source'));
        vm.runInContext(`(function(){
            const apply=Reflect.apply,DOMException=domExceptionBridge.DOMException,QuotaExceededError=domExceptionBridge.QuotaExceededError;
            let closed=false,address=__url,workerCanCancel=true,workerStreaming=true;
            const fetches=new Map();
            /* The Worker's performance timeline (js_performance.js) is not run here. */
            const performanceBridge={initiator(id,type){__initiators.push([id,type]);},monotonicResource(){}};
            function report(e){__reported.push(e);}
            function timer(fn,ms,args){return closed?0:__timer(0,fn,ms,args||[]);}
            function clearTimer(id){__clear(id);}
            Object.assign(host,{timer:(_,fn,ms,args)=>timer(fn,ms,args,false),clear:clearTimer,securityURL:()=>address,referrerPolicy:()=>0,
                dataURL:(arrayBuffer=>${dataURL.toString()})(__arrayBuffer),cryptoDigest:()=>new ArrayBuffer(32),send(){}},__transport.host);
            ${eventLines}
            const target=new EventTarget();
            ${handlerLines}
            ${messaging}
            const MessageEvent=workerMessagingBridge.MessageEvent;
            ${blobLines}
            ${fetchPart}
            __transport.port=workerFetchTransport;
            ${eventSourcePort}
            Object.assign(globalThis,{Event,EventTarget,MessageEvent,Headers,Request,Response,fetch,AbortController,AbortSignal,importScripts(){}});
            /* The runtime's close() (closed, workerFetchTransport.close(), its timers cleared), then
               the parent's cancelling of the Worker's requests. */
            globalThis.__terminate=()=>{if(closed)return;closed=true;workerFetchTransport.close();__stopTimers();__transport.cancelAll();};
            globalThis.__teardown=globalThis.__terminate;
        })();`, c, {filename: 'js_worker_runtime.js (events, Fetch, EventSource)'});
        return c;
    },
};

/* ---- running ---- */
const testharness = read(path.join(wpt, 'resources/testharness.js'));
const substitute = text => text.replace(/\{\{domains\[([^\]]*)\]\}\}/g, (m, sub) => (sub ? sub + '.' : '') + HOST)
    .replace(/\{\{host\}\}/g, HOST)
    .replace(/\{\{ports\[(https?)\]\[([01])\]\}\}/g, (m, scheme, i) => String({http: [8000, 8001], https: [8443, 8444]}[scheme][i]));
const statusName = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
/* One file in a new realm: its subtests' names and statuses. */
function runFile(kind, entry, jar) {
    return new Promise(resolve => {
        const results = new Map(), full = path.join(wpt, entry.file);
        let c;
        try { c = realms[kind](entry, jar); }
        catch (e) { results.set('(realm) ' + entry.file, 'ERROR ' + e.message); resolve(results); return; }
        const text = c.__text;
        let timer;
        c.__done = (tests, status) => {
            clearTimeout(timer);
            for (const t of tests) results.set(t.name, statusName[t.status] + (t.status && verbose && t.message ? ' ' + t.message : ''));
            if (status.status !== 0 && !tests.length) results.set('(harness) ' + entry.file, 'ERROR ' + status.message);
            for (const e of c.__reported) results.set('(reported) ' + String(e && e.stack || e).split('\n')[0], 'ERROR');
            /* After the harness: the realm is torn down. */
            setTimeout(() => { try { vm.runInContext('__teardown()', c); } catch (_) {} resolve(results); }, 0);
        };
        vm.runInContext(testharness, c, {filename: 'testharness.js'});
        vm.runInContext('add_completion_callback((tests,status)=>__done(tests,status));if(globalThis.__document)globalThis.document=__document;', c);
        try {
            for (const line of text.split('\n')) {
                const m = /^\/\/ META: script=(.*)$/.exec(line.trim());
                if (!m) continue;
                const script = read(m[1].startsWith('/') ? path.join(wpt, m[1]) : path.join(path.dirname(full), m[1]));
                vm.runInContext(m[1].includes('.sub.') ? substitute(script) : script, c, {filename: m[1]});
            }
            vm.runInContext(text, c, {filename: entry.file});
        } catch (e) { results.set('(load) ' + entry.file, 'ERROR ' + e.message); }
        vm.runInContext('done()', c);
        timer = setTimeout(() => vm.runInContext('timeout()', c), /META: timeout=long/.test(text) ? 30000 : 10000);
    });
}
async function runRealm(kind) {
    const all = new Map(), jar = cookieJar();
    await Promise.all(files.filter(entry => entry.kinds.includes(kind)).map(async entry => {
        for (const [name, status] of await runFile(kind, entry, jar)) all.set(entry.file + ' | ' + name, status);
    }));
    return new Map([...all].sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
}
const passes = status => status.startsWith('PASS');
async function main() {
    const [window, worker] = await Promise.all([runRealm('window'), runRealm('worker')]);
    let failed = false;
    for (const [label, results] of [['Window (js_event_source.js)', window], ['Worker (js_event_source.js)', worker]]) {
        const wptOnly = [...results].filter(([name]) => !name.startsWith('eventsource/contract-'));
        const own = [...results].filter(([name]) => name.startsWith('eventsource/contract-'));
        console.log(`${label}: WPT ${wptOnly.filter(([, s]) => passes(s)).length}/${wptOnly.length} subtests pass, checks ${own.filter(([, s]) => passes(s)).length}/${own.length}`);
        for (const [name, s] of results) {
            if (passes(s)) continue;
            console.log(`  ${s}: ${name}`);
            if (name.startsWith('eventsource/contract-') || name.includes('(reported)') || name.includes('(realm)') || name.includes('(load)')) failed = true;
        }
    }
    /* One definition: a subtest the Window passes passes in the Worker too. */
    for (const [name, s] of window) {
        if (!worker.has(name) || passes(s) === passes(worker.get(name))) continue;
        console.log(`  differs: ${name}: Window ${s}, Worker ${worker.get(name)}`);
        if (passes(s)) failed = true;
    }
    process.exit(failed ? 1 : 0);
}
if (require.main === module) {
    process.on('unhandledRejection', () => {});
    main().catch(e => { console.error(e); process.exit(1); });
}
