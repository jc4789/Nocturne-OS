"""The text-control path of the editing commands: the product's js_input_event.js and
js_document_commands.js with the native js_document_commands.h over the fixture's form-control
storage (tests/document_commands_native.c), and the product's editing program (from the generated
js_bootstrap.inc) answering queryCommandSupported.  A focused input or textarea takes the
plain-text commands (insertText, delete, forwardDelete, insertParagraph, insertLineBreak,
selectAll); a script's command fires input and no beforeinput (Input Events).
Not covered here: editing hosts (tests/check_editing.py), the guest (tests/jstest.c, editing).
    python -X utf8 tests/check_document_commands_native.py
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / 'build/goal-20261009'
out.mkdir(parents=True, exist_ok=True)
js = """(function(){'use strict';
const apply=Reflect.apply,nativeDom=globalThis.rawDom;
class Document{getSelection(){return selection;}};Object.setPrototypeOf(document,Document.prototype);
class Selection{get anchorNode(){return null;}get focusNode(){return null;}get rangeCount(){return 0;}collapse(){}toString(){return '';}}
const selection=new Selection();
class HTMLElement{};class Node{};class MutationObserver{};class DOMException extends Error{constructor(m,n){super(m);this.name=n;}}
const documentBridge={brand:nativeBrand};
const rawDom={documentCommand:(...args)=>nativeDom('documentCommand',...args),get:()=>null,attrNS:()=>null,isNode:()=>false};
function dom(){throw new Error('no DOM in this fixture');}
const host={editing:()=>editingProgram,log(){}};
class Event{constructor(type,init={}){this.type=String(type);this.bubbles=!!init.bubbles;this.cancelable=!!init.cancelable;this.composed=!!init.composed;this.defaultPrevented=false;}preventDefault(){if(this.cancelable)this.defaultPrevented=true;}}
class UIEvent extends Event{}
const states=new WeakMap();function eventState(e){let s=states.get(e);if(!s)states.set(e,s={});return s;}
let events=[],after=null;function dispatch(target,event){event.target=target;event.isTrusted=!!eventState(event).isTrusted;events.push(event);if(after)after(event);return !event.defaultPrevented;}
"""
for name in ['js_input_event.js', 'js_document_commands.js']:
    js += (root / 'user/libc/web' / name).read_text(encoding='utf-8') + '\n'
js += """
let checks=0,failed=0;function ok(value,label){checks++;if(!value){failed++;print('FAIL '+label);}}
function throws(fn){try{fn();return false;}catch(e){return e instanceof TypeError;}}
return {documentCommandEvent:documentCommandBridge.event,run(){
for(const name of ['cut','copy','paste'])ok(!document.queryCommandSupported(name)&&!document.execCommand(name)&&value()==='ab😀cd','clipboard '+name);
for(const name of ['bold','undo','formatBlock'])ok(document.queryCommandSupported(name)&&!document.queryCommandEnabled(name)&&!document.execCommand(name)&&value()==='ab😀cd','rich text in a text control '+name);
ok(document.queryCommandSupported('SeLeCtAlL')&&document.queryCommandSupported('insertText'),'supported');
ok(!document.queryCommandSupported('selectall\\0')&&!document.queryCommandSupported('ſelectall'),'ASCII only');
ok(throws(()=>document.queryCommandSupported())&&throws(()=>document.execCommand()),'command required');
ok(throws(()=>Document.prototype.queryCommandSupported.call({},'copy')),'receiver');
ok(throws(()=>document.queryCommandSupported(Symbol()))&&throws(()=>document.execCommand('insertText',false,Symbol())),'Symbol');
ok(document.queryCommandSupported({toString:()=> 'INSERTTEXT'}),'stringified');
ok(document.execCommand('selectAll')&&caret()===6,'selectAll');
flag('readonly',true);ok(document.queryCommandEnabled('selectall')&&!document.queryCommandEnabled('inserttext')&&!document.execCommand('inserttext',false,'x')&&!document.execCommand('delete'),'readonly');flag('readonly',false);
for(const key of ['disabled','inert']){flag(key,true);ok(!document.queryCommandEnabled('inserttext')&&!document.execCommand('inserttext',false,'x'),key);flag(key,false);}
for(const key of ['focus','live']){flag(key,false);ok(!document.queryCommandEnabled('inserttext')&&!document.execCommand('inserttext',false,'x'),key);flag(key,true);}
events=[];flag('range',2,4);ok(document.execCommand('insertText',false,'X')&&value()==='abXcd'&&caret()===3,'insertText');
ok(events.length===1&&events[0] instanceof InputEvent&&events[0] instanceof UIEvent&&events[0].type==='input'&&!events[0].cancelable&&events[0].isTrusted&&events[0].data==='X'&&events[0].inputType==='insertText'&&events[0].target===control,'insertText fires input, no beforeinput');
flag('value','');flag('textarea',true);ok(document.execCommand('insertText',false,'a\\r\\nb\\rc')&&value()==='a\\nb\\nc','textarea newlines');
flag('value','');flag('textarea',false);ok(document.execCommand('insertText',false,'a\\r\\nb\\rc')&&value()==='abc','input newlines');
flag('value','');flag('url',true);ok(document.execCommand('insertText',false,'  https://example.test/  ')&&value()==='https://example.test/'&&caret()===21,'url trim');flag('url',false);
flag('value','abcd');flag('maxlength',true);events=[];ok(!document.execCommand('insertText',false,'x')&&value()==='abcd'&&events.length===0,'maxlength');
flag('range',1,3);ok(document.execCommand('insertText',false,'x')&&value()==='axd','maxlength replacement');
flag('value','abcdef');flag('range',1,4);ok(document.execCommand('insertText',false,'x')&&value()==='axef','maxlength shrink');flag('maxlength',false);
flag('value','ab😀');events=[];ok(document.execCommand('delete')&&value()==='ab'&&caret()===2,'delete a surrogate pair');
ok(events.length===1&&events[0].inputType==='deleteContentBackward'&&events[0].data===null,'delete event');
flag('value','abc');flag('range',0,0);ok(!document.execCommand('delete')&&value()==='abc','delete at the start');
ok(document.execCommand('forwardDelete')&&value()==='bc'&&caret()===0,'forwardDelete');
flag('value','e\\u0301x');flag('range',0,0);ok(document.execCommand('forwardDelete')&&value()==='x','forwardDelete takes the combining mark');
flag('value','abc');flag('range',1,2);ok(document.execCommand('delete')&&value()==='ac'&&caret()===1,'delete the selection');
flag('value','ab');flag('range',1,1);ok(!document.execCommand('insertParagraph')&&value()==='ab','insertParagraph in an input');
flag('textarea',true);events=[];ok(document.execCommand('insertParagraph')&&value()==='a\\nb'&&events[0].inputType==='insertParagraph','insertParagraph in a textarea');
ok(document.execCommand('insertLineBreak')&&value()==='a\\n\\nb'&&events[1].inputType==='insertLineBreak','insertLineBreak in a textarea');flag('textarea',false);
flag('value','abc');let nested;after=e=>{if(e.type==='input')nested=document.execCommand('insertText',false,'nested');};
ok(document.execCommand('insertText',false,'x')&&nested===false&&value()==='abcx','no command inside its own input event');after=null;
ok(!document.execCommand('insertText',false,'\\0')&&value()==='abcx','NUL');
ok(document.queryCommandState('selectall')===false&&document.queryCommandIndeterm('inserttext')===false&&document.queryCommandValue('inserttext')==='','state, indeterm, value');
ok(throws(()=>new InputEvent())&&throws(()=>new InputEvent(Symbol())),'InputEvent type');
const ie=new InputEvent('input',null);ok(ie.data===null&&ie.inputType===''&&!ie.isComposing&&ie instanceof UIEvent,'InputEvent defaults');
const typed=new InputEvent('beforeinput',{data:12,inputType:'insertText',isComposing:1});ok(typed.data==='12'&&typed.inputType==='insertText'&&typed.isComposing&&typed.dataTransfer===null,'InputEvent init');
ok(throws(()=>{typed.data='bad';})&&typed.data==='12','readonly data');
ok(throws(()=>Object.getOwnPropertyDescriptor(InputEvent.prototype,'data').get.call({}))&&throws(()=>InputEvent.prototype.getTargetRanges.call({})),'InputEvent receiver');
ok(typed.getTargetRanges().length===0&&typed.getTargetRanges()!==typed.getTargetRanges(),'getTargetRanges');
ok(throws(()=>new InputEvent('input',{dataTransfer:{}}))&&throws(()=>new InputEvent('input',{targetRanges:[{}]})),'no DataTransfer, StaticRange');
ok(throws(()=>new InputEvent('input',{data:Symbol()}))&&throws(()=>new InputEvent('input',{inputType:Symbol()})),'Symbol init');
return {checks,failed};}};})();
"""
(out / 'document-command-bindings.js').write_text(js, encoding='utf-8')
clang = next((p / 'tools/msys64/ucrt64/bin/clang.exe' for p in (root, *root.parents)
              if (p / 'tools/msys64/ucrt64/bin/clang.exe').exists()), None)
if clang is None:
    sys.exit('tools/msys64/ucrt64/bin/clang.exe not found')
env = dict(os.environ, PATH=str(clang.parent) + os.pathsep + os.environ.get('PATH', ''))
engine = [root / 'third_party/quickjs' / (name + '.c') for name in ['quickjs', 'dtoa', 'libregexp', 'libunicode', 'cutils']]
exe = out / 'document-commands-native.exe'
subprocess.run([str(clang), '-std=gnu11', '-O1', '-w', '-fwrapv', '-funsigned-char', '-D_GNU_SOURCE',
                '-DCONFIG_VERSION="Nocturne-test"', '-I' + str(root / 'third_party/quickjs'), '-I' + str(root),
                str(root / 'tests/document_commands_native.c'), *map(str, engine), '-lm', '-o', str(exe)],
               check=True, env=env)
result = subprocess.run([str(exe)], cwd=root, env=env, capture_output=True, text=True, encoding='utf-8', timeout=60)
print(result.stdout, end='')
sys.exit(result.returncode)
