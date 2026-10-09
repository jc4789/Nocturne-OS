/* New real web_live CE/MO boundary only. Saved for the parent's next native
 * run; this has not been executed by the host fixture or a separate VM. */
globalThis.runRangeDeletionIntegrationCases=async function(){
    let count=0;const ok=(name,value)=>{count++;check(name,value);};
    const host=document.createElement('div'),left=document.createElement('span'),right=document.createElement('span');
    const start=document.createTextNode('abc'),end=document.createTextNode('xyz');
    left.appendChild(start);right.appendChild(end);
    let range,disconnected=0,stateAtReaction=false;
    const name='x-native-range-delete-integration';
    customElements.define(name,class extends HTMLElement{
        disconnectedCallback(){disconnected++;stateAtReaction=range.collapsed&&range.startContainer===host&&range.startOffset===1&&start.data==='a'&&end.data==='yz'&&host.childNodes.length===2;}
    });
    const middle=document.createElement(name),inside=document.createTextNode('middle');middle.appendChild(inside);
    host.append(left,middle,right);document.body.appendChild(host);
    range=document.createRange();range.setStart(start,1);range.setEnd(end,1);
    const live=document.createRange();live.setStart(inside,2);live.setEnd(inside,4);
    const records=[],observer=new MutationObserver(batch=>records.push(...batch));
    observer.observe(host,{subtree:true,childList:true,characterData:true,characterDataOldValue:true});
    range.deleteContents();
    ok('delete-native-ce-reaction-after-final-state',disconnected===1&&stateAtReaction);
    ok('delete-native-contained-subtree-retained',middle.parentNode===null&&middle.firstChild===inside);
    ok('delete-native-other-live-range',live.collapsed&&live.startContainer===host&&live.startOffset===1);
    await new Promise(resolve=>setTimeout(resolve,0));
    ok('delete-native-mo-three-records',records.length===3);
    ok('delete-native-mo-start-character',records[0]?.type==='characterData'&&records[0].target===start&&records[0].oldValue==='abc');
    ok('delete-native-mo-contained-child',records[1]?.type==='childList'&&records[1].target===host&&records[1].removedNodes.length===1&&records[1].removedNodes[0]===middle);
    ok('delete-native-mo-end-character',records[2]?.type==='characterData'&&records[2].target===end&&records[2].oldValue==='xyz');
    observer.disconnect();
    const selection=document.getSelection();selection.removeAllRanges();selection.addRange(range);
    selection.setBaseAndExtent(end,2,end,0);const selected=selection.getRangeAt(0);
    selected.deleteContents=()=>{throw Error('Selected Range author override must not be called');};
    selection.deleteFromDocument();
    ok('delete-native-selection-captured-method',end.data===''&&selection.getRangeAt(0)===selected&&selected.collapsed);
    ok('delete-native-selection-backward-caret',selection.direction==='backward'&&selection.anchorNode===end&&selection.anchorOffset===0&&selection.focusOffset===0);
    selection.removeAllRanges();host.remove();return count;
};
