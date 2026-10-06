/* The native CSS evaluator supplies actual viewport/device matches. No page
 * script receives the host hook or the change-notification entry point. */
const mediaBridge = (() => {
    const states = new WeakMap(), eventStates = new WeakMap(), active = new Set();
    let refs = [];
    const token = {}, add = EventTarget.prototype.addEventListener, remove = EventTarget.prototype.removeEventListener;
    const evaluate = host.media;
    const get = target => { const s = states.get(target); if (!s) throw new TypeError('Illegal invocation'); return s; };
    function retain(target) {
        const s = get(target), listeners = listenerMap.get(target);
        if (s.handler || (listeners && listeners.some(x => !x.removed && x.type === 'change'))) active.add(target);
        else active.delete(target);
    }
    class MediaQueryListEvent extends Event {
        constructor(type, init = {}) {
            if (!arguments.length) throw new TypeError('Event type is required');
            init = init == null ? {} : Object(init); super(type, init);
            eventStates.set(this, {media:init.media === undefined ? '' : String(init.media), matches:!!init.matches});
        }
        get media() { const s=eventStates.get(this);if(!s)throw new TypeError('Illegal invocation');return s.media; }
        get matches() { const s=eventStates.get(this);if(!s)throw new TypeError('Illegal invocation');return s.matches; }
    }
    class MediaQueryList extends EventTarget {
        constructor(key, query) {
            super(); if (key !== token) throw new TypeError('Illegal constructor');
            if (refs.length >= 4096) refs = refs.filter(x => x.deref() !== undefined);
            if (refs.length >= 4096) throw new RangeError('Too many live media query lists');
            const info = evaluate(query, true);
            states.set(this, {query, media:info.media, last:info.matches, handler:null}); refs.push(new WeakRef(this));
        }
        get media() { return get(this).media; }
        get matches() { return evaluate(get(this).query); }
        get onchange() { return get(this).handler; }
        set onchange(value) { get(this).handler = typeof value === 'function' ? value : null; retain(this); }
        addEventListener(type, callback, options) { get(this); add.call(this,type,callback,options); retain(this); }
        removeEventListener(type, callback, options) { get(this); remove.call(this,type,callback,options); retain(this); }
        addListener(callback) { get(this);add.call(this,'change',callback);retain(this); }
        removeListener(callback) { get(this);remove.call(this,'change',callback);retain(this); }
    }
    Object.defineProperty(MediaQueryList.prototype,Symbol.toStringTag,{value:'MediaQueryList',configurable:true});
    Object.defineProperty(MediaQueryListEvent.prototype,Symbol.toStringTag,{value:'MediaQueryListEvent',configurable:true});
    function matchMedia(query) {
        if (!arguments.length) throw new TypeError('Media query is required');
        if (typeof query === 'symbol') throw new TypeError('Cannot convert a Symbol to a string');
        return new MediaQueryList(token, String(query));
    }
    Object.assign(globalThis,{matchMedia,MediaQueryList,MediaQueryListEvent});
    return {changed() {
        const snapshot = refs.slice(); refs = refs.filter(x => x.deref() !== undefined);
        for (const ref of snapshot) {
            const target=ref.deref();if(!target)continue;
            const s=get(target), matches=evaluate(s.query);
            if (matches !== s.last) {
                s.last=matches;const event=new MediaQueryListEvent('change',{media:s.media,matches});
                event.isTrusted=true;dispatch(target,event);
            }
            retain(target);
        }
    }};
})();
