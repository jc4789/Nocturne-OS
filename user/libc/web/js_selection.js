/* Text-control selection belongs to the native node, including detached nodes.
 * Document.getSelection()/Range are a separate API, not a fallback here. */
(() => {
    const string = value => {
        if (typeof value === 'symbol') throw new TypeError('Cannot convert a Symbol to a string');
        return String(value);
    };
    const number = value => value === null ? 0 : (+value >>> 0);
    const direction = value => value === 'forward' ? 1 : value === 'backward' ? 2 : 0;
    function install(proto, brand) {
        const get = (target, op) => dom('selection', target, brand, op);
        const requireText = target => {
            if (get(target, 'start') === null) throw new DOMException('This input type does not support text selection', 'InvalidStateError');
        };
        const range = (target, start, end, dir) => {
            requireText(target); dom('selection', target, brand, 'range', start, end, dir);
        };
        Object.defineProperties(proto, {
            selectionStart: {configurable:true, enumerable:true,
                get() { return get(this, 'start'); },
                set(value) { const start=number(value);requireText(this);const end=get(this,'end');range(this,start,Math.max(start,end),direction(get(this,'direction'))); }},
            selectionEnd: {configurable:true, enumerable:true,
                get() { return get(this, 'end'); },
                set(value) { const end=number(value);requireText(this);range(this,get(this,'start'),end,direction(get(this,'direction'))); }},
            selectionDirection: {configurable:true, enumerable:true,
                get() { return get(this, 'direction'); },
                set(value) { const dir=direction(string(value));requireText(this);range(this,get(this,'start'),get(this,'end'),dir); }},
            setSelectionRange: {configurable:true, enumerable:true, writable:true,
                value: function setSelectionRange(start,end) {
                    if (arguments.length < 2) throw new TypeError('Two selection endpoints are required');
                    start=number(start);end=number(end);
                    const dir=arguments.length>2 && arguments[2]!==undefined?direction(string(arguments[2])):0;
                    range(this,start,end,dir);
                }},
            select: {configurable:true, enumerable:true, writable:true,
                value: function select() {
                    dom('selection',this,brand,'select');
                }},
            setRangeText: {configurable:true, enumerable:true, writable:true,
                value: function setRangeText(replacement) {
                    if (!arguments.length || arguments.length===2) throw new TypeError('Replacement and either zero or two endpoints are required');
                    replacement=string(replacement);
                    let start,end,mode='preserve';
                    if (arguments.length>=3) {
                        start=number(arguments[1]);end=number(arguments[2]);
                        if (arguments.length>3 && arguments[3]!==undefined) mode=string(arguments[3]);
                    }
                    if (!['select','start','end','preserve'].includes(mode)) throw new TypeError('Invalid selection mode');
                    requireText(this);
                    let selectionStart=get(this,'start'),selectionEnd=get(this,'end');
                    if (arguments.length===1) { start=selectionStart;end=selectionEnd; }
                    if (start>end) throw new DOMException('The start endpoint exceeds the end endpoint','IndexSizeError');
                    const value=dom('get',this,'value');
                    start=Math.min(start,value.length);end=Math.min(end,value.length);
                    /* JS slicing preserves half-surrogate boundaries exactly.
                       The value setter stores the result in the native control. */
                    dom('set',this,'value',value.slice(0,start)+replacement+value.slice(end));
                    const newEnd=start+replacement.length,delta=replacement.length-(end-start);
                    if (mode==='select') { selectionStart=start;selectionEnd=newEnd; }
                    else if (mode==='start') selectionStart=selectionEnd=start;
                    else if (mode==='end') selectionStart=selectionEnd=newEnd;
                    else {
                        if (selectionStart>end) selectionStart+=delta;else if (selectionStart>start) selectionStart=start;
                        if (selectionEnd>end) selectionEnd+=delta;else if (selectionEnd>start) selectionEnd=newEnd;
                    }
                    range(this,selectionStart,selectionEnd,0);
                }}
        });
    }
    install(HTMLInputElement.prototype,'input');
    install(HTMLTextAreaElement.prototype,'textarea');
})();
