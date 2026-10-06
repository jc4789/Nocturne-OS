/* Auxiliary contracts for native mouse movement; not website acceptance. The
   embedder advances the generator between JS tasks, then hit-tests its yield. */
globalThis.runHoverCases = function* () {
    let checks=0, records=[], captured=[], bubbled=0, outerTargets=0;
    function check(value,name) { checks++; if(!value) throw new Error('hover: '+name); }
    function element(id,parent,left,top,width,height) {
        const n=document.createElement('div'); n.id=id;
        n.style.cssText='position:absolute;left:'+left+'px;top:'+top+'px;width:'+width+'px;height:'+height+'px';
        parent.appendChild(n); return n;
    }
    const outer=element('outer',document.body,20,20,260,180);
    const a=element('a',outer,0,0,90,70), child=element('child',a,10,10,30,25);
    const b=element('b',outer,130,0,70,60);
    const other=element('other',document.body,20,240,260,180);
    const nodes=[outer,a,child,b,other], types=['mouseout','mouseleave','mouseover','mouseenter','mousemove'];
    for(const n of nodes) for(const type of types) n.addEventListener(type,e=>{
        const boundary=type==='mouseenter'||type==='mouseleave';
        if(boundary && e.target!==n) bubbled++;
        if(e.target!==n) return;
        check(e instanceof MouseEvent,'MouseEvent brand'); check(e.isTrusted,'trusted');
        check(e.bubbles===!boundary && e.cancelable===!boundary && e.composed===!boundary,'event flags');
        check(e.currentTarget===n && e.eventPhase===2,'at target');
        check(e.buttons===5 && e.button===0 && e.ctrlKey && e.shiftKey && !e.altKey,'native button/modifiers');
        check(e.pageX-e.clientX===5 && e.pageY-e.clientY===7,'native coordinates');
        if(boundary){e.preventDefault();check(!e.defaultPrevented,'boundary cannot cancel');}
        const path=e.composedPath();
        records.push({type,node:n,related:e.relatedTarget,path:path.slice()});
        path.length=0; check(e.composedPath()[0]===n,'composedPath copy');
    });
    for(const type of ['mouseenter','mouseleave']) {
        outer.addEventListener(type,e=>{if(e.target!==outer){check(e.eventPhase===1,'ancestor capture');captured.push(e.target);}},true);
        document.addEventListener(type,e=>{if(e.target===document)outerTargets++;});
        window.addEventListener(type,e=>{if(e.target===window)outerTargets++;});
    }
    function expect(sequence,from,to) {
        check(records.map(r=>r.type+':'+r.node.id).join(',')===sequence,'order '+sequence);
        for(const r of records) {
            const related=r.type==='mousemove'?null:r.type==='mouseout'||r.type==='mouseleave'?to:from;
            check(r.related===related,'relatedTarget '+r.type+':'+r.node.id);
        }
        check(bubbled===0,'enter/leave do not bubble');
        check(outerTargets===0,'document/window not entered as elements');
    }
    function clear(){records=[];captured=[];}
    check(new MouseEvent('mousemove').relatedTarget===null,'constructor relatedTarget default');
    check(new MouseEvent('mouseover',{relatedTarget:a}).relatedTarget===a,'constructor relatedTarget init');
    yield child;
    expect('mouseover:child,mouseenter:outer,mouseenter:a,mouseenter:child,mousemove:child',null,child);
    check(captured.indexOf(a)>=0 && captured.indexOf(child)>=0,'nonbubbling still captures');
    clear(); yield child; expect('mousemove:child',child,child);
    clear(); yield a; expect('mouseout:child,mouseleave:child,mouseover:a,mousemove:a',child,a);
    clear(); yield b; expect('mouseout:a,mouseleave:a,mouseover:b,mouseenter:b,mousemove:b',a,b);
    clear(); yield null; expect('mouseout:b,mouseleave:b,mouseleave:outer',b,null);
    clear(); yield null; expect('',null,null);
    clear(); yield child;
    expect('mouseover:child,mouseenter:outer,mouseenter:a,mouseenter:child,mousemove:child',null,child);
    let microtaskRan=false;
    child.addEventListener('mouseout',()=>{
        outer.remove();other.appendChild(b);
        Promise.resolve().then(()=>{microtaskRan=true;});
    },{once:true});
    b.addEventListener('mouseover',e=>{
        check(microtaskRan,'checkpoint before following native event');
        e.preventDefault(); e.stopImmediatePropagation();
    },{once:true});
    clear(); yield b;
    expect('mouseout:child,mouseleave:child,mouseleave:a,mouseover:b,mouseenter:b,mousemove:b',child,b);
    for(const r of records) {
        check(r.path.indexOf(outer)>=0,'path frozen through handler removal');
        check(r.path.indexOf(other)<0,'new parent excluded from current transition');
    }
    clear(); yield b;
    expect('mouseleave:outer,mouseenter:other,mousemove:b',b,b);
    b.remove();
    clear(); yield null; expect('mouseout:b,mouseleave:b,mouseleave:other',b,null);
    document.body.appendChild(outer);
    outer.addEventListener('mouseenter',e=>e.stopPropagation(),{once:true});
    /* The C ancestry walk must not consult this page-spoofed JS property. */
    Object.defineProperty(child,'parentNode',{configurable:true,value:null});
    clear(); yield child;
    expect('mouseover:child,mouseenter:outer,mouseenter:a,mouseenter:child,mousemove:child',null,child);
    check(captured.indexOf(child)>=0,'parent stop does not suppress separate child enter');
    for(const r of records) if(r.node===child)check(r.path.indexOf(a)>=0,'native ancestry, not parentNode override');
    delete child.parentNode;
    clear(); yield null; expect('mouseout:child,mouseleave:child,mouseleave:a,mouseleave:outer',child,null);
    outer.remove(); other.remove();
    return checks;
};
void 0;
