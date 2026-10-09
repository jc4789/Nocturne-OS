/* New native web_live geometry boundaries. Saved, not executed by this agent. */
globalThis.runFrameInlineGeometryCases = async function () {
    let count=0;
    const verify=(ok,name)=>{count++;check('frame-inline-geometry-'+name,ok);if(!ok)throw new Error(name);};
    const frame=document.createElement('iframe');
    frame.style.cssText='position:absolute;left:30px;top:40px;width:160px;height:100px;border:0';
    document.body.appendChild(frame);
    try {
        const win=frame.contentWindow,child=frame.contentDocument;
        const ready=new Promise((resolve,reject)=>{
            const id=setTimeout(()=>reject(new Error('new geometry child load missing')),1000);
            frame.addEventListener('load',()=>{clearTimeout(id);resolve();},{once:true});
        });
        child.open();child.write('<!doctype html><body style="margin:0"><a id="plain" href="/geometry-destination" style="white-space:nowrap">plain link</a><div style="height:300px"></div>');child.close();
        await ready;
        const a=child.getElementById('plain'),r=a.getBoundingClientRect();
        verify(r.width>20&&r.height>5,'plain-backgroundless-link-has-real-size');
        verify(r.left>=0&&r.top>=0&&r.right<=160,'child-local-fragment-coordinates');
        verify(a.contains(child.elementFromPoint(r.left+r.width/2,r.top+r.height/2)),'child-document-hit-matches-link');
        const fr=frame.getBoundingClientRect();
        verify(document.elementFromPoint(fr.left+r.left+r.width/2,fr.top+r.top+r.height/2)===frame,'parent-document-retargets-child-hit');
        verify(win.frameElement===frame&&child.defaultView===win&&win.parent.document===document,'same-origin-frame-identity');
        const borrowed=Element.prototype.getBoundingClientRect.call(a);
        verify(borrowed.left===r.left&&borrowed.top===r.top&&borrowed.width===r.width,'borrowed-parent-method-uses-owning-document');
        verify(a.contains(Document.prototype.elementFromPoint.call(child,r.left+r.width/2,r.top+r.height/2)),'borrowed-parent-document-method-uses-owning-viewport');
        win.scrollTo(0,10);
        const scrolled=a.getBoundingClientRect();
        verify(scrolled.top===r.top-10,'child-scroll-not-parent-scroll');
        a.style.display='none';const hidden=a.getBoundingClientRect();
        verify(hidden.width===0&&hidden.height===0,'nonrendered-inline-empty');
        a.style.display='inline';a.remove();const detached=a.getBoundingClientRect();
        verify(detached.width===0&&detached.height===0,'detached-inline-empty');
    } finally {frame.remove();}
    return count;
};
