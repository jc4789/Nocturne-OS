"""One new command boundary run, actual helpers with form/DOM fixture storage."""
from pathlib import Path
import hashlib
import os
import subprocess
from datetime import datetime, timezone

root = Path(__file__).resolve().parent.parent
out = root / 'build/goal-20261009'
js = """(function(){'use strict';
const apply=Reflect.apply;
class Document{};Object.setPrototypeOf(document,Document.prototype);
const documentBridge={brand:nativeBrand};
class Event{constructor(type,init={}){this.type=String(type);this.bubbles=!!init.bubbles;this.cancelable=!!init.cancelable;this.composed=!!init.composed;this.defaultPrevented=false;}preventDefault(){if(this.cancelable)this.defaultPrevented=true;}}
class UIEvent extends Event{}
let events=[],before=null;function dispatch(target,event){event.target=target;events.push(event);if(event.type==='beforeinput'&&before)before(event);return !event.defaultPrevented;}
"""
for name in ['js_input_event.js','js_document_commands.js']:
    js += (root / 'user/libc/web' / name).read_text(encoding='utf-8')+'\n'
js += """
let checks=0,failed=0;function ok(value){checks++;if(!value)failed++;}
function throws(fn){try{fn();return false;}catch(e){return e instanceof TypeError;}}
for(const name of ['cut','copy','paste','bold','undo'])ok(!document.queryCommandSupported(name)&&!document.execCommand(name)&&value()==='ab😀cd');
ok(document.queryCommandSupported('SeLeCtAlL')&&document.queryCommandSupported('insertText'));
ok(!document.queryCommandSupported('selectall\\0')&&!document.queryCommandSupported('ſelectall'));
ok(throws(()=>document.queryCommandSupported()));ok(throws(()=>document.execCommand()));
ok(throws(()=>Document.prototype.queryCommandSupported.call({},'copy')));
ok(throws(()=>document.queryCommandSupported(Symbol()))&&throws(()=>document.execCommand('insertText',false,Symbol())));
ok(document.queryCommandSupported({toString:()=> 'INSERTTEXT'}));
ok(document.execCommand('selectAll')&&caret()===6);
flag('readonly',true);ok(document.queryCommandEnabled('selectall')&&!document.queryCommandEnabled('inserttext')&&!document.execCommand('inserttext',false,'x'));flag('readonly',false);
for(const key of ['disabled','inert']){flag(key,true);ok(!document.queryCommandEnabled('inserttext')&&!document.execCommand('inserttext',false,'x'));flag(key,false);}
for(const key of ['focus','live']){flag(key,false);ok(!document.queryCommandEnabled('inserttext')&&!document.execCommand('inserttext',false,'x'));flag(key,true);}
flag('range',2,4);ok(document.execCommand('insertText',false,'X')&&value()==='abXcd'&&caret()===3);
ok(events.length===2&&events[0] instanceof InputEvent&&events[0] instanceof UIEvent&&events[0].type==='beforeinput'&&events[0].cancelable&&events[0].composed&&events[0].isTrusted&&events[0].data==='X'&&events[0].inputType==='insertText'&&events[0].target===control&&events[1].type==='input'&&!events[1].cancelable);
flag('value','');flag('textarea',true);ok(document.execCommand('insertText',false,'a\\r\\nb\\rc')&&value()==='a\\nb\\nc');
flag('value','');flag('textarea',false);ok(document.execCommand('insertText',false,'a\\r\\nb\\rc')&&value()==='abc');
flag('value','');flag('url',true);ok(document.execCommand('insertText',false,'  https://example.test/  ')&&value()==='https://example.test/'&&caret()===21);flag('url',false);
flag('value','abcd');flag('maxlength',true);events=[];ok(!document.execCommand('insertText',false,'x')&&value()==='abcd'&&events.length===0);
flag('range',1,3);ok(document.execCommand('insertText',false,'x')&&value()==='axd');
flag('value','abcdef');flag('range',1,4);ok(document.execCommand('insertText',false,'x')&&value()==='axef');flag('maxlength',false);
flag('value','abc');events=[];before=e=>e.preventDefault();ok(!document.execCommand('insertText',false,'x')&&value()==='abc'&&caret()===3&&events.length===1);before=null;
before=()=>flag('value','handler');ok(!document.execCommand('insertText',false,'x')&&value()==='handler');before=null;
let nested;before=()=>{nested=document.execCommand('insertText',false,'nested');};ok(document.execCommand('insertText',false,'x')&&!nested&&value()==='handlerx');before=null;
before=()=>flag('readonly',true);ok(!document.execCommand('insertText',false,'x')&&value()==='handlerx');before=null;flag('readonly',false);
before=()=>flag('live',false);ok(!document.execCommand('insertText',false,'x')&&value()==='handlerx');before=null;flag('live',true);
ok(!document.execCommand('insertText',false,'\\0')&&value()==='handlerx');
ok(document.queryCommandState('selectall')===false&&document.queryCommandIndeterm('inserttext')===false&&document.queryCommandValue('inserttext')==='');
ok(throws(()=>new InputEvent()));ok(throws(()=>new InputEvent(Symbol())));
const ie=new InputEvent('input',null);ok(ie.data===null&&ie.inputType===''&&!ie.isComposing&&ie instanceof UIEvent);
const typed=new InputEvent('beforeinput',{data:12,inputType:'insertText',isComposing:1});ok(typed.data==='12'&&typed.inputType==='insertText'&&typed.isComposing&&typed.dataTransfer===null);
ok(throws(()=>{typed.data='bad';})&&typed.data==='12');
ok(throws(()=>Object.getOwnPropertyDescriptor(InputEvent.prototype,'data').get.call({}))&&throws(()=>InputEvent.prototype.getTargetRanges.call({})));
ok(typed.getTargetRanges().length===0&&typed.getTargetRanges()!==typed.getTargetRanges());
ok(throws(()=>new InputEvent('input',{dataTransfer:{}}))&&throws(()=>new InputEvent('input',{targetRanges:[{}]})));
ok(throws(()=>new InputEvent('input',{data:Symbol()}))&&throws(()=>new InputEvent('input',{inputType:Symbol()})));
return {documentCommandEvent:documentCommandBridge.event,checks,failed};})();
"""
boundary=js.index('let checks=0,failed=0;')
setup=js[:boundary]+'return {documentCommandEvent:documentCommandBridge.event,run(){'+js[boundary:]
setup=setup.replace('return {documentCommandEvent:documentCommandBridge.event,checks,failed};})();','return {checks,failed};}};})();')
# Preserve the original failed run. This two-phase fixture repair is unexecuted.
(out / 'document-command-bindings.js').write_text(setup,encoding='utf-8')
cc=root / 'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[]
log=out / 'document-commands-new-boundary.log'
with log.open('w',encoding='utf-8') as f:
    f.write('compile start UTC '+datetime.now(timezone.utc).isoformat()+'\n')
    for name in ['quickjs','dtoa','libregexp','libunicode','cutils']:
        data=(root / 'third_party/quickjs' / (name+'.c')).read_bytes()
        copied=out / ('command-engine-'+name+'.c');copied.write_bytes(data);engine.append(copied)
        f.write(name+' SHA256 '+hashlib.sha256(data).hexdigest()+'\n')
    f.flush()
    exe=out / 'document-commands-native.exe'
    subprocess.run([str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include','-I.',str(root / 'tests/document_commands_native.c'),*map(str,engine),'-lm','-o',str(exe)],cwd=root,env=env,check=True,stdout=f,stderr=subprocess.STDOUT)
    f.write('compile complete UTC '+datetime.now(timezone.utc).isoformat()+'\n');f.flush()
    result=subprocess.run([str(exe)],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=20)
    f.write('exit '+str(result.returncode)+'\n')
print(log.read_text(encoding='utf-8'))
