// New extraction boundaries only: actual Range bindings + native C-tree move.
{
    const type=n=>rawDom('get',n,'nodeType'),owner=n=>rawDom('get',n,'ownerDocument');
    const caret=(r,n,o)=>r.collapsed&&r.startContainer===n&&r.startOffset===o&&r.endContainer===n&&r.endOffset===o;
    const throws=(fn,name)=>{try{fn();return false;}catch(e){return e.name===name;}};
    check(throws(()=>Range.prototype.extractContents.call({}),'TypeError'),'extract-brand');
    check(Object.getOwnPropertyDescriptor(Range.prototype,'extractContents').enumerable,'extract-enumerable');
    const empty=document.createRange(),before=nativeCounts(),ef=empty.extractContents();
    check(type(ef)===11&&owner(ef)===document&&!kids(ef).length,'collapsed-native-fragment');
    check(nativeCounts()[0]===before[0]&&nativeCounts()[1]===before[1],'collapsed-no-source-mutation');
    for(const kind of [3,4,7,8]){
        const n=make(kind,'a\ud83d\ude00bc'),r=range(n,1,n,3),other=range(n,4,n,5),f=r.extractContents();
        check(kids(f).length===1&&type(kids(f)[0])===kind&&text(kids(f)[0])==='\ud83d\ude00','extract-character-utf16-'+kind);
        check(kids(f)[0]!==n&&text(n)==='abc'&&caret(r,n,1),'extract-character-original-caret-'+kind);
        check(other.startOffset===2&&other.endOffset===3,'extract-character-other-live-'+kind);
    }
    const source=make(1),a=make(1),b=make(1),c=make(8,'comment');append(source,a,b,c);b.authorIdentity={live:true};
    const allRange=range(source,0,source,3),inside=range(b,0,b,0),later=range(source,3,source,3);
    const moved=allRange.extractContents();
    check(!kids(source).length&&kids(moved).length===3&&kids(moved)[0]===a&&kids(moved)[1]===b&&kids(moved)[2]===c,'whole-native-identity-not-clones');
    check(b.authorIdentity.live&&rawDom('get',b,'parentNode')===moved,'whole-author-identity-preserved');
    check(caret(allRange,source,0)&&caret(inside,source,0)&&caret(later,source,0),'whole-live-removal-boundaries');
    const tree=make(11),left=make(1),middle=make(1),right=make(1),lt=make(3,'abc'),rt=make(3,'def');
    append(left,lt);append(right,rt);append(tree,left,middle,right);middle.authorIdentity=17;
    const crossing=range(lt,1,rt,2),fragment=crossing.extractContents(),result=kids(fragment);
    check(result.length===3&&result[0]!==left&&result[1]===middle&&result[2]!==right,'partial-shells-cloned-middle-moved');
    check(text(kids(result[0])[0])==='bc'&&text(kids(result[2])[0])==='de'&&middle.authorIdentity===17,'partial-fragment-contents');
    check(text(lt)==='a'&&text(rt)==='f'&&kids(tree).length===2&&kids(tree)[0]===left&&kids(tree)[1]===right,'partial-source-retained');
    check(caret(crossing,tree,1),'partial-extraction-caret');
    const nested=make(11),nl=make(1),nl2=make(1),nt=make(3,'start'),retained=make(1),whole=make(1),nr=make(1),nr2=make(1),end=make(3,'finish');
    append(nl2,nt,retained);append(nl,nl2);append(nr2,end);append(nr,nr2);append(nested,nl,whole,nr);
    const nestedRange=range(nt,2,end,3),nestedFragment=nestedRange.extractContents(),nestedKids=kids(nestedFragment);
    check(nestedKids[1]===whole&&kids(kids(nestedKids[0])[0])[1]===retained,'nested-contained-identity-moved');
    check(text(nt)==='st'&&text(end)==='ish'&&kids(nl2).length===1&&caret(nestedRange,nested,1),'nested-partial-source-and-caret');
    const ancestor=make(11),first=make(1),last=make(3,'xyz');append(ancestor,first,last);
    const ancestorRange=range(ancestor,0,last,1),ancestorFragment=ancestorRange.extractContents();
    check(kids(ancestorFragment)[0]===first&&text(kids(ancestorFragment)[1])==='x'&&text(last)==='yz'&&caret(ancestorRange,ancestor,0),'start-ancestor-extraction');
    const endAncestor=make(11),ea=make(3,'123'),em=make(1),tail=make(3,'retain');append(endAncestor,ea,em,tail);
    const endRange=range(ea,1,endAncestor,2),endFragment=endRange.extractContents();
    check(text(kids(endFragment)[0])==='23'&&kids(endFragment)[1]===em&&text(ea)==='1'&&kids(endAncestor)[1]===tail&&caret(endRange,endAncestor,1),'end-ancestor-extraction');
    const docLike=make(9),doctype=make(10,'',docLike),html=make(1,'',docLike);append(docLike,doctype,html);
    const docRange=range(docLike,0,docLike,1),doctypeBefore=nativeCounts();
    check(throws(()=>docRange.extractContents(),'HierarchyRequestError'),'doctype-extraction-rejected');
    check(kids(docLike)[0]===doctype&&!docRange.collapsed&&nativeCounts()[0]===doctypeBefore[0]&&nativeCounts()[1]===doctypeBefore[1],'doctype-no-source-mutation');
    const foreignDoc=make(9),foreign=make(3,'foreign',foreignDoc),foreignRange=range(foreign,1,foreign,4),foreignFragment=foreignRange.extractContents();
    check(owner(foreignFragment)===foreignDoc&&owner(kids(foreignFragment)[0])===foreignDoc&&text(foreign)==='fign','foreign-document-owner');
    const failureSource=make(1),failureChild=make(1);append(failureSource,failureChild);const failureRange=range(failureSource,0,failureSource,1);
    rejectNextMutation();check(throws(()=>failureRange.extractContents(),'InternalError'),'native-move-failure-propagates');
    check(kids(failureSource)[0]===failureChild&&caret(failureRange,failureSource,0),'failure-retains-source-after-spec-collapse');
    const signalRoot=make(1),signalChild=make(1);append(signalRoot,signalChild);const signalRange=range(signalRoot,0,signalRoot,1);let signals=0;
    onMoved=(n,p)=>{signals++;check(n===signalChild&&p===signalRoot&&!kids(signalRoot).length&&caret(signalRange,signalRoot,0),'reaction-after-final-extraction');};
    signalRange.extractContents();onMoved=null;check(signals===1,'reaction-single-whole-move');
    const many=make(1),nodes=[];for(let i=0;i<1050;i++){const n=make(1);nodes.push(n);append(many,n);}
    const manyRange=range(many,0,many,1050),manyFragment=manyRange.extractContents();
    check(kids(manyFragment).length===1050&&kids(manyFragment)[1049]===nodes[1049]&&!kids(many).length,'no-small-extraction-quota');
}
