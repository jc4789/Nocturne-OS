/* Editing: HTML's design mode, contentEditable and isContentEditable, the
 * Document command methods of the execCommand draft, and HTML's "editing host"
 * and "editable", read from the native tree.  The commands are one layer,
 * js_editing.js (the draft's own implementation, third_party/editing), which
 * runs the first time a page asks about a command or the user edits an editing
 * host; execCommand, the editing keys (doc.c) and the browser's clipboard keys
 * all take its actions.  A focused input or textarea is its own editor: the
 * plain-text commands edit its value natively (js_document_commands.h), and the
 * rich-text commands are not enabled while it has the focus. */
const documentCommandBridge=(()=>{
    const apply=Reflect.apply,replace=String.prototype.replace,slice=String.prototype.slice;
    const fromCharCode=String.fromCharCode,charCodeAt=String.prototype.charCodeAt;
    const regexpExec=RegExp.prototype.exec,min=Math.min,max=Math.max,createObject=Object.create,assign=Object.assign;
    const StringType=String,TypeErr=TypeError,DOMErr=DOMException,InputEventType=InputEvent,define=Object.defineProperty,realm=globalThis;
    const ObserverType=MutationObserver,NodeType=Node,realmDocument=document;
    const weakGet=WeakMap.prototype.get,weakSet=WeakMap.prototype.set,designModes=new WeakMap();
    /* The realm's Selection, through its own methods, not the page-replaceable ones. */
    const selectionOf=Document.prototype.getSelection,selectionMethods=Selection.prototype;
    const selectionSlots=createObject(null);
    for(const name of ['collapse','toString'])selectionSlots[name]=selectionMethods[name];
    const selectionGetters=createObject(null);
    for(const name of ['anchorNode','focusNode','rangeCount'])selectionGetters[name]=Object.getOwnPropertyDescriptor(selectionMethods,name).get;
    const selection=()=>apply(selectionOf,realmDocument,[]);
    const HTML='http://www.w3.org/1999/xhtml',SVG='http://www.w3.org/2000/svg',MATHML='http://www.w3.org/1998/Math/MathML';
    const get=(n,key)=>rawDom.get(n,key),isNode=n=>n!==null&&typeof n==='object'&&rawDom.isNode(null,n);
    function string(value){if(typeof value==='symbol')throw new TypeErr('Cannot convert Symbol to DOMString');return StringType(value);}
    function lower(text){return apply(replace,text,[/[A-Z]/g,c=>fromCharCode(apply(charCodeAt,c,[0])+32)]);}
    function command(value){return lower(string(value));}
    function args(receiver,count,value){documentBridge.brand(receiver);if(!count)throw new TypeErr('Command required');return command(value);}

    function htmlElement(n){return get(n,'nodeType')===1&&get(n,'namespaceURI')===HTML;}
    /* HTML: the contenteditable attribute's state: 'true', 'false' or
       'plaintext-only'; null is the Inherit state (missing or invalid value). */
    function editableState(n){
        if(!isNode(n)||!htmlElement(n))return null;
        const value=rawDom.attrNS(n,null,'contenteditable');if(value===null)return null;
        const keyword=lower(value);
        return keyword===''||keyword==='true'?'true':keyword==='false'||keyword==='plaintext-only'?keyword:null;
    }
    function designModeEnabled(doc){return apply(weakGet,designModes,[doc])===true;}
    /* HTML: "An editing host is either an HTML element with its contenteditable
       attribute in the true state or plaintext-only state, or a child HTML
       element of a Document whose design mode enabled is true." */
    function editingHost(n){
        if(!isNode(n)||!htmlElement(n))return false;
        const state=editableState(n);
        if(state==='true'||state==='plaintext-only')return true;
        const parent=get(n,'parentNode');
        return parent!==null&&get(parent,'nodeType')===9&&designModeEnabled(parent);
    }
    /* HTML: something is editable if it is a node; it is not an editing host; it
       does not have a contenteditable attribute set to the false state; its
       parent is an editing host or editable; and either it is an HTML element,
       or it is an svg or math element, or it is not an Element and its parent is
       an HTML element.  Up the ancestors until an editing host, or a node that
       fails the steps. */
    function editable(n){
        if(!isNode(n)||editingHost(n))return false;
        for(;;){
            const parent=get(n,'parentNode');
            if(parent===null||editableState(n)==='false')return false;
            if(get(n,'nodeType')===1){
                const ns=get(n,'namespaceURI'),name=get(n,'localName');
                if(ns!==HTML&&!(ns===SVG&&name==='svg')&&!(ns===MATHML&&name==='math'))return false;
            }else if(!htmlElement(parent))return false;
            if(editingHost(parent))return true;
            n=parent;
        }
    }

    /* The plain-text commands a focused input or textarea takes, with the
       inputType of their input event. */
    const textCommands=assign(createObject(null),{inserttext:'insertText',selectall:'',delete:'deleteContentBackward',
        forwarddelete:'deleteContentForward',insertparagraph:'insertParagraph',insertlinebreak:'insertLineBreak'});
    /* The draft's Miscellaneous commands, which no editing host enables. */
    const miscellaneous=assign(createObject(null),{copy:true,cut:true,defaultparagraphseparator:true,paste:true,
        redo:true,selectall:true,stylewithcss:true,undo:true,usecss:true});
    /* copy, cut and paste are the optional commands, and unsupported: running
       them needs the Clipboard API's events (ClipboardEvent, DataTransfer) and a
       clipboard the page reaches only with the user's activation, which these
       bindings do not have.  The browser's own Ctrl+C/X/V keys copy, cut and paste. */
    const clipboard=assign(createObject(null),{copy:true,cut:true,paste:true});
    function textControl(doc){return rawDom.documentCommand(doc,'textControl','');}
    function codeUnitsBefore(text,index){
        const low=apply(charCodeAt,text,[index-1]),high=index>1?apply(charCodeAt,text,[index-2]):0;
        return low>=0xdc00&&low<=0xdfff&&high>=0xd800&&high<=0xdbff?2:1;
    }
    function codeUnitsAfter(text,index){
        const high=apply(charCodeAt,text,[index]),low=index+1<text.length?apply(charCodeAt,text,[index+1]):0;
        let units=high>=0xd800&&high<=0xdbff&&low>=0xdc00&&low<=0xdfff?2:1;
        /* The combining marks that follow go with it, as the draft's forwardDelete does. */
        const marks=apply(regexpExec,/^\p{M}+/u,[apply(slice,text,[index+units])]);
        return units+(marks?marks[0].length:0);
    }
    /* The command on the focused text control's value: a snapshot of it, the
       value the command makes, and the native write, which checks the snapshot
       again and fires input (no beforeinput: Input Events fires that only for
       the user's own input). */
    function textControlEdit(doc,cmd,value){
        const snapshot=rawDom.documentCommand(doc,'prepare',cmd);
        if(!snapshot)return false;
        const inputType=textCommands[cmd];
        if(cmd==='selectall')return rawDom.documentCommand(doc,'execute',cmd,snapshot,'',0,null,inputType);
        const old=snapshot.value;let start=snapshot.start,end=snapshot.end,text='',data=null;
        if(cmd==='inserttext'){
            text=snapshot.multiline?apply(replace,value,[/\r\n?/g,'\n']):apply(replace,value,[/[\r\n]/g,'']);data=value;
        }else if(cmd==='insertparagraph'||cmd==='insertlinebreak'){if(!snapshot.multiline)return false;text='\n';}
        else if(start===end){
            if(cmd==='delete'){if(!start)return false;start-=codeUnitsBefore(old,start);}
            else{if(end>=old.length)return false;end+=codeUnitsAfter(old,end);}
        }
        let next=apply(slice,old,[0,start])+text+apply(slice,old,[end]),caret=start+text.length;
        if(snapshot.trimURL){
            const leading=apply(regexpExec,/^[\t\n\f\r ]*/,[next])[0].length;
            next=apply(replace,next,[/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g,'']);
            caret=max(0,min(next.length,caret-leading));
        }
        return rawDom.documentCommand(doc,'execute',cmd,snapshot,next,caret,data,inputType);
    }

    /* The editing layer of this realm: js_editing.js's function, called once with
       what it reads through. */
    let layer=null;
    function editing(){
        if(layer)return layer;
        const program=host.editing();
        return layer=program({
            document:realmDocument,Node:NodeType,MutationObserver:ObserverType,
            selection,
            computedStyle:element=>new CSSStyleProperties(element,true,styleToken,null),
            color:text=>rawDom.documentCommand(realmDocument,'color',string(text)),
            fontKeywords:rawDom.documentCommand(realmDocument,'fontKeywords',''),
            editingHost,editable,editableState,
            event,
            report:error=>host.log(3,'Editing:',error),
        });
    }
    /* A command question: the focused text control answers for the plain-text
       commands, and enables no other but the miscellaneous ones; a document of
       no browsing context of this realm has no selection, so no active range. */
    function query(doc,mode,cmd){
        if(clipboard[cmd])return mode==='value'?'':false;
        if(mode==='supported')return editing().supported(cmd);
        const control=textControl(doc);
        if(control&&textCommands[cmd]!==undefined)return rawDom.documentCommand(doc,mode,cmd);
        if(doc!==realmDocument||(control&&(!miscellaneous[cmd]||cmd==='undo'||cmd==='redo')))
            return mode==='value'?'':false;
        return editing()[mode](cmd);
    }
    for(const [name,mode] of [['queryCommandSupported','supported'],['queryCommandEnabled','enabled'],
        ['queryCommandState','state'],['queryCommandIndeterm','indeterm'],['queryCommandValue','value']])
        define(Document.prototype,name,{configurable:true,writable:true,value:{[name](commandId){
            return query(this,mode,args(this,arguments.length,commandId));
        }}[name]});
    define(Document.prototype,'execCommand',{configurable:true,writable:true,value:{execCommand(commandId,showUI=false,value=''){
        const cmd=args(this,arguments.length,commandId);showUI=!!showUI;value=string(value);
        if(clipboard[cmd])return false;
        const control=textControl(this);
        if(control&&textCommands[cmd]!==undefined)return textControlEdit(this,cmd,value);
        if(this!==realmDocument||(control&&(!miscellaneous[cmd]||cmd==='undo'||cmd==='redo')))return false;
        return editing().exec(cmd,showUI,value);
    }}.execCommand});

    /* HTML: designMode.  On: reset the active range's boundary points to the
       start of the Document and run the focusing steps for the document
       element.  The native copy is for :read-write and the editing keys. */
    define(Document.prototype,'designMode',{configurable:true,enumerable:true,
        get(){documentBridge.brand(this);return designModeEnabled(this)?'on':'off';},
        set(value){
            documentBridge.brand(this);value=lower(string(value));
            if(value==='on'&&!designModeEnabled(this)){
                apply(weakSet,designModes,[this,true]);rawDom.documentCommand(this,'designMode','',true);
                if(this===realmDocument&&apply(selectionGetters.rangeCount,selection(),[]))
                    apply(selectionSlots.collapse,selection(),[this,0]);
                const element=get(this,'documentElement');
                if(element)dom('focus',element);
            }else if(value==='off'&&designModeEnabled(this)){
                apply(weakSet,designModes,[this,false]);rawDom.documentCommand(this,'designMode','',false);
            }
        }});
    /* HTML: ElementContentEditable on HTMLElement. */
    function htmlReceiver(value){if(!isNode(value)||!htmlElement(value))throw new TypeErr('Illegal HTMLElement receiver');return value;}
    define(HTMLElement.prototype,'contentEditable',{configurable:true,enumerable:true,
        get(){const state=editableState(htmlReceiver(this));return state===null?'inherit':state;},
        set(value){
            htmlReceiver(this);const keyword=lower(string(value));
            if(keyword==='inherit')dom('attrNS',this,null,'contenteditable',null);
            else if(keyword==='true'||keyword==='false'||keyword==='plaintext-only')dom('attrNS',this,null,'contenteditable',keyword);
            else throw new DOMErr('contentEditable must be "true", "false", "plaintext-only" or "inherit"','SyntaxError');
        }});
    define(HTMLElement.prototype,'isContentEditable',{configurable:true,enumerable:true,
        get(){htmlReceiver(this);return editingHost(this)||editable(this);}});

    /* A focused editing host takes the selection when it is outside it, so that
       the user's typing lands in it: a caret at its start. */
    function focused(element){
        if(!editingHost(element)&&!editable(element))return;
        const s=selection();
        for(let n=apply(selectionGetters.focusNode,s,[]);n;n=get(n,'parentNode'))if(n===element)return;
        apply(selectionSlots.collapse,s,[element,0]);
    }
    function event(target,type,data,inputType='insertText'){
        const event=new InputEventType(type,{bubbles:true,cancelable:type==='beforeinput',composed:true,view:realm,
            data,inputType,isComposing:false});
        eventState(event).isTrusted=true;return dispatch(target,event);
    }
    return {event,editingHost,editable,
        /* The user's edit of an editing host (doc.c's editing keys, the browser's
           clipboard keys): true when the editing layer took it. */
        userAction(action,extend,data){return editing().user(string(action),!!extend,data==null?null:string(data));},
        focused,
        selectionText(){return apply(selectionSlots.toString,selection(),[]);}};
})();
