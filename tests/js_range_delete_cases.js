// New deletion boundaries only. Used by the native C-tree supplement.
{
    function caret(r,n,o){return r.collapsed&&r.startContainer===n&&r.endContainer===n&&r.startOffset===o&&r.endOffset===o;}
    function throws(fn,name){try{fn();return false;}catch(e){return e.name===name;}}
    check(throws(()=>Range.prototype.deleteContents.call({}),'TypeError'),'range-delete-brand');
    check(throws(()=>Selection.prototype.deleteFromDocument.call({}),'TypeError'),'selection-delete-brand');
    const empty=document.createRange(),before=nativeCounts();check(empty.deleteContents()===undefined,'collapsed-return-undefined');
    check(nativeCounts()[0]===before[0]&&nativeCounts()[1]===before[1],'collapsed-no-native-mutation');
    for(const kind of [3,4,7,8]){
        const n=make(kind,'a\ud83d\ude00bc'),r=range(n,1,n,3),other=range(n,4,n,5);
        check(r.deleteContents()===undefined&&text(n)==='abc','character-kind-utf16-'+kind);
        check(caret(r,n,1),'character-caret-'+kind);
        check(other.startOffset===2&&other.endOffset===3,'character-other-live-splice-'+kind);
    }
    const forest=make(11),a=make(1),mid=make(1),b=make(1),at=make(3,'abc'),inside=make(3,'middle'),bt=make(3,'def');
    append(a,at);append(mid,inside);append(b,bt);append(forest,a,mid,b);
    const crossing=range(at,1,bt,2),removedRange=range(inside,2,inside,4),tailRange=range(forest,3,forest,3);
    let reactionsSeen=0;
    onRemoved=n=>{reactionsSeen++;check(n===mid&&caret(crossing,forest,1)&&text(at)==='a'&&text(bt)==='f','reaction-after-final-deletion-state');};
    const count=nativeCounts();crossing.deleteContents();onRemoved=null;
    check(kids(forest).length===2&&kids(forest)[0]===a&&kids(forest)[1]===b,'partial-ancestors-preserved');
    check(kids(mid)[0]===inside&&rawDom('get',mid,'parentNode')===null,'contained-subtree-removed-not-descendants');
    check(nativeCounts()[0]===count[0]+1&&reactionsSeen===1,'topmost-only-native-remove');
    check(caret(crossing,forest,1),'cross-branch-caret');check(caret(removedRange,forest,1),'removed-descendant-live-boundaries');check(caret(tailRange,forest,2),'later-native-index-live-boundary');
    const ancestor=make(11),left=make(1),lt=make(3,'left'),right=make(3,'xyz');append(left,lt);append(ancestor,left,right);
    const ancestorRange=range(ancestor,0,right,1);ancestorRange.deleteContents();
    check(kids(ancestor).length===1&&kids(ancestor)[0]===right&&text(right)==='yz'&&caret(ancestorRange,ancestor,0),'start-ancestor-collapse-and-end-splice');
    const endAncestor=make(11),ea=make(3,'123'),whole=make(1),trailing=make(3,'retain');append(endAncestor,ea,whole,trailing);
    const endRange=range(ea,1,endAncestor,2);endRange.deleteContents();
    check(text(ea)==='1'&&kids(endAncestor).length===2&&kids(endAncestor)[1]===trailing&&caret(endRange,endAncestor,1),'end-ancestor-collapse-before-removal');
    const docLike=make(9),doctype=make(10),html=make(1);append(docLike,doctype,html);const docRange=range(docLike,0,docLike,1);docRange.deleteContents();
    check(kids(docLike).length===1&&kids(docLike)[0]===html&&caret(docRange,docLike,0),'doctype-removal-is-allowed');
    const leaf=make(3,'equal'),zero=range(leaf,2,leaf,2);const zeroBefore=nativeCounts();zero.deleteContents();check(text(leaf)==='equal'&&nativeCounts()[1]===zeroBefore[1],'equal-character-empty-no-op');
    const failureRoot=make(11),failureNode=make(1);append(failureRoot,failureNode);const failureRange=range(failureRoot,0,failureRoot,1);
    rejectNextMutation();check(throws(()=>failureRange.deleteContents(),'InternalError'),'native-mutation-failure-not-fake-success');
    check(kids(failureRoot)[0]===failureNode&&caret(failureRange,failureRoot,0),'failed-delete-collapse-spec-before-mutation');
    const sel=getSelection();sel.removeAllRanges();flushSelection();const eventCount=selectionEvents.length;sel.deleteFromDocument();flushSelection();
    check(selectionEvents.length===eventCount,'empty-selection-no-notification');
    const selected=make(3,'abcdef');append(document,selected);const selectedRange=range(selected,1,selected,4);sel.addRange(selectedRange);flushSelection();
    selectedRange.deleteContents=()=>{throw Error('author override must not run');};
    sel.deleteFromDocument();
    check(text(selected)==='aef'&&sel.getRangeAt(0)===selectedRange&&caret(selectedRange,selected,1),'selection-uses-captured-real-range-and-identity');
    check(sel.anchorNode===selected&&sel.focusNode===selected&&sel.anchorOffset===1&&sel.focusOffset===1,'selection-live-endpoints');
    flushSelection();check(selectionEvents.length===eventCount+2,'selection-notice-once-per-change-batch');
    const invisible=make(3,'shadow-like'),invisibleRoot=make(11);append(invisibleRoot,invisible);sel.removeAllRanges();const invisibleRange=range(invisible,0,invisible,6);sel.addRange(invisibleRange);sel.deleteFromDocument();
    check(text(invisible)==='shadow-like'&&sel.rangeCount===0,'detached-range-not-document-selection');
    const detachedSelected=make(3,'keep');append(document,detachedSelected);const displaced=range(detachedSelected,0,detachedSelected,4);sel.addRange(displaced);rawDom('remove',detachedSelected);sel.deleteFromDocument();
    check(text(detachedSelected)==='keep'&&sel.rangeCount===0,'invisible-associated-selection-no-delete');
    sel.removeAllRanges();sel.setBaseAndExtent(selected,3,selected,0);sel.deleteFromDocument();
    check(text(selected)===''&&sel.isCollapsed&&sel.direction==='backward','backward-selection-preserves-direction');
    const otherDoc=make(9),foreign=make(3,'foreign');append(otherDoc,foreign);const foreignRange=range(foreign,1,foreign,4);foreignRange.deleteContents();
    check(text(foreign)==='fign'&&caret(foreignRange,foreign,1),'foreign-document-native-range-not-global-document');
}
