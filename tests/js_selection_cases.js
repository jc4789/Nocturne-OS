/* These cases run against the real web_live/QuickJS native DOM. They are
 * supplementary API regressions, not evidence that DuckDuckGo works. */
async function runSelectionCases() {
    let count=0;
    function check(name,ok) { count++;if(!ok)throw new Error('selection: '+name); }
    function throws(name,fn,kind) {
        try { fn();check(name,false); } catch(error) { check(name,error.name===kind); }
    }
    function state(name,node,start,end,direction) {
        check(name+' start',node.selectionStart===start);
        check(name+' end',node.selectionEnd===end);
        check(name+' direction',node.selectionDirection===direction);
    }
    check('no HTMLElement selection',!('selectionStart' in HTMLElement.prototype));
    for (const tag of ['input','textarea']) {
        const input=document.createElement(tag);
        state(tag+' initial',input,0,0,'none');
        const proto=tag==='input'?HTMLInputElement.prototype:HTMLTextAreaElement.prototype;
        for(const name of ['selectionStart','selectionEnd','selectionDirection']) {
            const descriptor=Object.getOwnPropertyDescriptor(proto,name);
            check(tag+' descriptor '+name,typeof descriptor.get==='function'&&typeof descriptor.set==='function'&&descriptor.enumerable&&descriptor.configurable);
            throws(tag+' getter brand '+name,()=>descriptor.get.call(document.createElement('div')),'TypeError');
        }
        throws(tag+' range brand',()=>proto.setSelectionRange.call(document.createElement('div'),0,1),'TypeError');
        throws(tag+' select brand',()=>proto.select.call({}),'TypeError');
        throws(tag+' missing endpoint',()=>input.setSelectionRange(0),'TypeError');
        input.value='a日本😀z';
        state(tag+' value UTF16 end',input,6,6,'none');
        input.setSelectionRange(1,5,'backward');
        state(tag+' UTF16 range',input,1,5,'backward');
        check(tag+' selected actual value',input.value.slice(input.selectionStart,input.selectionEnd)==='日本😀');
        input.setSelectionRange(3,4);
        state(tag+' half surrogate',input,3,4,'none');
        input.focus();
        state(tag+' detached focus preserves selection',input,3,4,'none');
        input.blur();
        state(tag+' blur preserves selection',input,3,4,'none');
        input.value=input.value;
        state(tag+' same value preserves',input,3,4,'none');
        input.selectionStart=5;
        state(tag+' start expands end',input,5,5,'none');
        input.selectionEnd=2;
        state(tag+' end collapses start',input,2,2,'none');
        input.selectionDirection='backward';
        state(tag+' setter direction',input,2,2,'backward');
        input.selectionDirection='BACKWARD';
        state(tag+' invalid direction defaults none',input,2,2,'none');
        input.setSelectionRange(7,3,'forward');
        state(tag+' reversed range',input,3,3,'forward');
        input.setSelectionRange(-1,2);
        state(tag+' unsigned range conversion',input,2,2,'none');
        input.setSelectionRange(4294967297,5.9,'forward');
        state(tag+' unsigned modulo fractional',input,1,5,'forward');
        input.setSelectionRange(null,undefined);
        state(tag+' nullable and undefined',input,0,0,'none');
        input.setSelectionRange(1,99,'anything');
        state(tag+' clamped end invalid direction',input,1,6,'none');
        input.select();
        state(tag+' select full',input,0,6,'none');
        input.value='abc';
        state(tag+' changed value moves end',input,3,3,'none');
        input.setSelectionRange(1,2,'backward');
        input.value='abc';
        state(tag+' unchanged keeps direction',input,1,2,'backward');
        input.setRangeText('X');
        check(tag+' range replace',input.value==='aXc');
        state(tag+' range preserve',input,1,2,'none');
        input.setRangeText('pq',1,2,'select');
        check(tag+' range expanded',input.value==='apqc');
        state(tag+' range select',input,1,3,'none');
        input.setRangeText('日本',1,3,'end');
        state(tag+' range end',input,3,3,'none');
        input.setRangeText('😀',1,3,'start');
        check(tag+' range surrogate replacement',input.value==='a😀c');
        state(tag+' range start',input,1,1,'none');
        input.setRangeText('X',1,2,'end');
        check(tag+' half surrogate replacement',input.value==='aX\ude00c');
        state(tag+' half surrogate result',input,2,2,'none');
        throws(tag+' range inverted',()=>input.setRangeText('x',2,1),'IndexSizeError');
        throws(tag+' range invalid mode',()=>input.setRangeText('x',0,0,'wat'),'TypeError');
        throws(tag+' BigInt conversion',()=>input.setSelectionRange(1n,2),'TypeError');
        throws(tag+' Symbol direction',()=>input.setSelectionRange(0,1,Symbol()),'TypeError');
        input.value='a\r\nb\rc\nd';
        check(tag+' newlines',input.value===(tag==='input'?'abcd':'a\nb\nc\nd'));
        state(tag+' normalized value length',input,input.value.length,input.value.length,'none');
    }
    const input=document.createElement('input');
    input.value='abcd';
    for(const type of ['text','search','url','tel','password','TeXt','unknown','']) {
        input.type=type;input.setSelectionRange(1,3,'forward');
        state('supported type '+type,input,1,3,'forward');
    }
    for(const type of ['number','email','date','month','week','time','datetime-local','range','color','checkbox','radio','file','submit','image','reset','button','hidden']) {
        input.type=type;
        check('unsupported start '+type,input.selectionStart===null);
        check('unsupported end '+type,input.selectionEnd===null);
        check('unsupported direction '+type,input.selectionDirection===null);
        throws('unsupported range '+type,()=>input.setSelectionRange(0,1),'InvalidStateError');
        throws('unsupported start write '+type,()=>input.selectionStart=0,'InvalidStateError');
        throws('unsupported end write '+type,()=>input.selectionEnd=0,'InvalidStateError');
        throws('unsupported direction write '+type,()=>input.selectionDirection='forward','InvalidStateError');
        input.select();check('select no throw '+type,true);
    }
    input.type='text';
    state('type becomes selectable resets cursor',input,0,0,'none');
    input.setSelectionRange(1,2);input.remove();
    state('detached state retained',input,1,2,'none');
    document.body.appendChild(input);input.focus();
    check('focus native active element',document.activeElement===input);
    state('connected focus range retained',input,1,2,'none');
    input.blur();
    const clone=input.cloneNode();
    state('clone begins fresh selection',clone,0,0,'none');
    input.setAttribute('readonly','');input.setSelectionRange(1,3,'backward');
    state('readonly programmatic selection',input,1,3,'backward');
    input.removeAttribute('readonly');input.setAttribute('disabled','');
    input.setSelectionRange(0,2,'forward');
    state('disabled programmatic selection',input,0,2,'forward');
    input.focus();check('disabled focus ignored',document.activeElement!==input);
    input.removeAttribute('disabled');
    const pristine=document.createElement('input');pristine.setAttribute('value','hi');document.body.appendChild(pristine);
    state('pristine initial DOM cursor',pristine,0,0,'none');
    pristine.focus();state('Nocturne pristine focus at end',pristine,2,2,'none');pristine.blur();
    pristine.setSelectionRange(0,0);pristine.focus();state('explicit zero overrides focus default',pristine,0,0,'none');pristine.blur();pristine.remove();

    const eventInput=document.createElement('input');
    eventInput.value='abcd';document.body.appendChild(eventInput);
    const received=[];
    eventInput.addEventListener('select',event=>received.push([event.bubbles,event.cancelable,event.isTrusted,event.target]));
    let bubbled=0;
    const listener=event=>{if(event.target===eventInput)bubbled++;};
    document.body.addEventListener('select',listener);
    eventInput.setSelectionRange(1,3,'forward');
    eventInput.setSelectionRange(1,3,'forward');
    eventInput.selectionDirection='backward';
    eventInput.select();
    check('select not synchronous',received.length===0);
    await Promise.resolve();
    check('select not microtask',received.length===0);
    await new Promise(resolve=>setTimeout(resolve,0));
    check('select change tasks exactly',received.length===3);
    check('select event fields',received.every(event=>event[0]===true&&event[1]===false&&event[2]===true&&event[3]===eventInput));
    check('select bubbles to body',bubbled===3);
    document.body.removeEventListener('select',listener);
    eventInput.remove();input.remove();
    return count;
}
