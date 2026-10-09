/* Only implemented native text-control commands are advertised. Clipboard,
 * undo/formatting and arbitrary contenteditable remain unsupported. */
const documentCommandBridge=(()=>{
    const apply=Reflect.apply,replace=String.prototype.replace,slice=String.prototype.slice;
    const fromCharCode=String.fromCharCode,charCodeAt=String.prototype.charCodeAt;
    const regexpExec=RegExp.prototype.exec,min=Math.min,max=Math.max;
    const StringType=String,TypeErr=TypeError,InputEventType=InputEvent,define=Object.defineProperty,realm=globalThis;
    function string(value){if(typeof value==='symbol')throw new TypeErr('Cannot convert Symbol to DOMString');return StringType(value);}
    function command(value){return apply(replace,string(value),[/[A-Z]/g,c=>fromCharCode(apply(charCodeAt,c,[0])+32)]);}
    function args(receiver,count,value){documentBridge.brand(receiver);if(!count)throw new TypeErr('Command required');return command(value);}
    for(const [name,mode] of [['queryCommandSupported','supported'],['queryCommandEnabled','enabled'],
        ['queryCommandState','state'],['queryCommandIndeterm','indeterm'],['queryCommandValue','value']])
        define(Document.prototype,name,{configurable:true,writable:true,value:function(commandId){
            return rawDom('documentCommand',this,mode,args(this,arguments.length,commandId));
        }});
    define(Document.prototype,'execCommand',{configurable:true,writable:true,value:function(commandId,showUI=false,value=''){
        const cmd=args(this,arguments.length,commandId);showUI=!!showUI;value=string(value);
        const snapshot=rawDom('documentCommand',this,'prepare',cmd);
        if(!snapshot)return false;
        if(cmd==='selectall')return rawDom('documentCommand',this,'execute',cmd,snapshot,'',0,'');
        let text=snapshot.multiline?apply(replace,value,[/\r\n?/g,'\n']):apply(replace,value,[/[\r\n]/g,'']);
        let next=apply(slice,snapshot.value,[0,snapshot.start])+text+apply(slice,snapshot.value,[snapshot.end]);
        let caret=snapshot.start+text.length;
        if(snapshot.trimURL){
            const leading=apply(regexpExec,/^[\t\n\f\r ]*/,[next])[0].length;
            next=apply(replace,next,[/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g,'']);
            caret=max(0,min(next.length,caret-leading));
        }
        return rawDom('documentCommand',this,'execute',cmd,snapshot,next,caret,value);
    }});
    return {event(target,type,data,inputType='insertText'){
        const event=new InputEventType(type,{bubbles:true,cancelable:type==='beforeinput',composed:true,view:realm,
            data,inputType,isComposing:false});
        event.isTrusted=true;return dispatch(target,event);
    }};
})();
