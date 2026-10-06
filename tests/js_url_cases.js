/* Host-independent URL regression checks; run after js_encoding.js and js_url.js.
 * Returns a JSON report. These API checks are not real-website acceptance tests. */
(() => {
    'use strict';
    const failures = []; let total = 0;
    const equal = (a, b) => { if (!Object.is(a, b)) throw new Error(JSON.stringify(a) + ' != ' + JSON.stringify(b)); };
    const rejects = f => { try { f(); } catch (e) { if (e instanceof TypeError) return; throw e; } throw new Error('Expected TypeError'); };
    const check = (name, f) => { total++; try { f(); } catch (e) { failures.push({name, error:String(e)}); } };
    check('absolute URL and default port', () => {
        const u = new URL('HTTPS://User:pass@EXAMPLE.COM:443/a/../b?x=1#f');
        equal(u.href, 'https://User:pass@example.com/b?x=1#f'); equal(u.origin, 'https://example.com');
        equal(u.host, 'example.com'); equal(u.port, ''); equal(u.username, 'User'); equal(u.password, 'pass');
    });
    check('relative and encoded dot segments', () => equal(new URL('../%2e%2e/z', 'https://a.test/a/b/c').href, 'https://a.test/z'));
    check('query-only and fragment-only resolution', () => {
        equal(new URL('?q=2', 'https://a.test/p?q=1#x').href, 'https://a.test/p?q=2');
        equal(new URL('#y', 'https://a.test/p?q=1#x').href, 'https://a.test/p?q=1#y');
    });
    check('special URL backslashes', () => equal(new URL('http:\\example.com\\a').href, 'http://example.com/a'));
    check('IPv4 alternative forms canonicalize', () => equal(new URL('http://0x7f.1/').origin, 'http://127.0.0.1'));
    check('IPv6 compression and default port', () => equal(new URL('https://[2001:0db8:0:0:0:0:0:1]:443/').origin, 'https://[2001:db8::1]'));
    check('IPv6 embedded IPv4', () => equal(new URL('http://[::ffff:192.0.2.1]/').hostname, '[::ffff:c000:201]'));
    check('IDN punycode', () => equal(new URL('https://例え.テスト/').hostname, 'xn--r8jz45g.xn--zckzah'));
    check('IDNA NFC maps equivalent origins', () => equal(new URL('https://e\u0301.test/').origin, new URL('https://é.test/').origin));
    check('IDNA width mapping', () => equal(new URL('https://ＥＸＡＭＰＬＥ。ＣＯＭ/').origin, 'https://example.com'));
    check('IDNA rejects invalid joiner', () => rejects(() => new URL('https://a\u200db.test/')));
    check('percent-encoded host', () => equal(new URL('http://%65xample.com/').host, 'example.com'));
    check('non-ASCII path/query/fragment', () => equal(new URL('https://a.test/雪?q=é#😀').href, 'https://a.test/%E9%9B%AA?q=%C3%A9#%F0%9F%98%80'));
    check('opaque origin and path', () => {
        const u = new URL('javascript:void(0)', 'https://news.ycombinator.com/');
        equal(u.origin, 'null'); equal(u.pathname, 'void(0)'); equal(u.href, 'javascript:void(0)');
    });
    check('blob URL origin', () => equal(new URL('blob:https://example.com/id').origin, 'https://example.com'));
    check('file URL parsing is syntax not filesystem access', () => {
        const u = new URL('file:///C:/a/../b'); equal(u.href, 'file:///C:/b'); equal(u.origin, 'null');
    });
    check('invalid URL constructor', () => {
        for (const s of ['relative', 'https://[::1', 'https://a.test:65536/', 'https://a b/']) rejects(() => new URL(s));
        rejects(() => new URL('https://valid.test/', 'not a base'));
    });
    check('setters and origin normalization', () => {
        const u = new URL('https://a.test/'); u.hostname = 'BÜCHER.test'; u.port = '443';
        equal(u.origin, 'https://xn--bcher-kva.test'); u.protocol = 'http:'; u.port = '80';
        equal(u.origin, 'http://xn--bcher-kva.test'); u.pathname = '/a b/雪';
        equal(u.pathname, '/a%20b/%E9%9B%AA');
    });
    check('invalid setters leave URL unchanged', () => {
        const u = new URL('https://a.test/'); u.hostname = '[bad'; u.port = '65536'; u.protocol = 'data:';
        equal(u.href, 'https://a.test/'); rejects(() => { u.href = 'bad'; }); equal(u.href, 'https://a.test/');
    });
    check('credentials encoded by setters', () => {
        const u = new URL('https://a.test'); u.username = 'a@b'; u.password = 'c:d';
        equal(u.href, 'https://a%40b:c%3Ad@a.test/');
    });
    check('searchParams keeps identity after href/search changes', () => {
        const u = new URL('https://a.test/?a=1'); const p = u.searchParams;
        u.href = 'https://b.test/?b=2'; equal(p, u.searchParams); equal(p.get('b'), '2');
        u.search = '?c=3'; equal(p.get('b'), null); equal(p.get('c'), '3');
        p.append('d', 'a b'); equal(u.search, '?c=3&d=a+b');
    });
    check('searchParams empty synchronization', () => {
        const u = new URL('https://a.test/?a=1#f'); u.searchParams.delete('a'); equal(u.href, 'https://a.test/#f');
        u.search = '?'; equal(u.href, 'https://a.test/?#f'); u.searchParams.sort(); equal(u.href, 'https://a.test/#f');
    });
    check('form encoding plus/percent and malformed UTF8', () => {
        const p = new URLSearchParams('a=x+y&b=%2B&c=%E0%A4&d=%FF');
        equal(p.get('a'), 'x y'); equal(p.get('b'), '+'); equal(p.get('c'), '\ufffd'); equal(p.get('d'), '\ufffd');
        equal(new URLSearchParams({a:'~!*()'}).toString(), 'a=%7E%21*%28%29');
    });
    check('query UTF8 BOM is retained', () => equal(new URLSearchParams('x=%EF%BB%BFz').get('x'), '\ufeffz'));
    check('USVString lone surrogates', () => {
        equal(new URLSearchParams([['x', '\ud800']]).get('x'), '\ufffd');
        equal(new URL('https://a.test/\ud800').pathname, '/%EF%BF%BD');
        rejects(() => new URL(Symbol('url'))); rejects(() => new URLSearchParams(Symbol('params')));
    });
    check('record, iterable and repeated keys', () => {
        const p = new URLSearchParams(new Map([['a', 1], ['b', 2]])); p.append('a', 3);
        equal(p.size, 3); equal(p.getAll('a').join(','), '1,3'); p.set('a', 4);
        equal(p.toString(), 'a=4&b=2'); rejects(() => new URLSearchParams([['a']]));
    });
    check('has/delete value overload', () => {
        const p = new URLSearchParams('a=1&a=2&a=1'); equal(p.has('a','2'), true);
        p.delete('a','1'); equal(p.toString(), 'a=2'); equal(p.has('a','1'), false);
    });
    check('stable UTF16 sort', () => {
        const p = new URLSearchParams('z=0&a=2&a=1'); p.sort(); equal(p.toString(), 'a=2&a=1&z=0');
    });
    check('iterators observe URL replacement', () => {
        const u = new URL('https://a.test/?a=1&b=2&c=3'); const it = u.searchParams.entries();
        equal(it.next().value.join('='), 'a=1'); u.search = '?x=4&y=5';
        equal(it.next().value.join('='), 'y=5'); equal(it.next().done, true);
    });
    check('forEach receiver and live mutation', () => {
        const p = new URLSearchParams('a=1&b=2'); const receiver = {}; const seen = [];
        p.forEach(function(value, key, owner) {
            equal(this, receiver); equal(owner, p); seen.push(key + value);
            if (key === 'a') p.append('c', 3);
        }, receiver); equal(seen.join(','), 'a1,b2,c3');
    });
    check('URLSearchParams branding', () => {
        rejects(() => URLSearchParams.prototype.append.call({}, 'x', 'y'));
        rejects(() => Object.getOwnPropertyDescriptor(URL.prototype, 'href').get.call({}));
        rejects(() => URL('https://a.test/')); rejects(() => URLSearchParams('x=1'));
    });
    check('static parsing and JSON', () => {
        equal(URL.canParse('/x', 'https://a.test/'), true); equal(URL.canParse('x'), false);
        equal(URL.parse('x'), null); equal(URL.parse('/x', 'https://a.test/').href, 'https://a.test/x');
        equal(JSON.stringify(new URL('https://a.test/')), '"https://a.test/"');
    });
    const result = {total, passed:total-failures.length, failures};
    globalThis.__urlTestResults = result;
    return JSON.stringify(result);
})();
