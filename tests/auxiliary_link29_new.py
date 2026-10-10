"""新29だけを初回一度。旧 Worker/site suite は実行しない。"""
from pathlib import Path
import datetime,hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build/goal-20261009/auxiliary-link29-new-once'
if out.exists():raise SystemExit('同検査再実行禁止')
out.mkdir()
def function(text,marker):
    start=text.index(marker);brace=text.index('{',start);depth=0;quote=None;comment=None;i=brace
    while i<len(text):
        c=text[i];n=text[i:i+2]
        if comment=='line':
            if c=='\n':comment=None
        elif comment=='block':
            if n=='*/':comment=None;i+=1
        elif quote:
            if c=='\\':i+=1
            elif c==quote:quote=None
        elif n=='//':comment='line';i+=1
        elif n=='/*':comment='block';i+=1
        elif c in ('"',"'"):quote=c
        elif c=='{':depth+=1
        elif c=='}':
            depth-=1
            if depth==0:return text[start:i+1]
        i+=1
    raise ValueError('製品関数終端なし')
frame=(root/'user/libc/web/frame.c').read_text(encoding='utf-8')
js=(root/'user/libc/web/js.c').read_text(encoding='utf-8')
browser=(root/'user/apps/browser.c').read_text(encoding='utf-8')
pieces=[function(browser,'static void host_navigate_form('),function(js,'bool web_js_auxiliary_link(')]
pieces += [function(frame,marker) for marker in ['static web_doc *named_frame(','static bool frame_link_space(','static bool frame_link_token(','static const char *frame_link_target(','static bool frame_auxiliary_allowed(','bool web_frame_navigate(']]
snapshot=out/'auxiliary_link29_product.inc';snapshot.write_text('\n\n'.join(pieces),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
commands=[[str(cc),'-std=gnu11','-I'+str(out),'tests/auxiliary_link29_new.c','-o',str(out/'auxiliary-link29.exe')],[str(out/'auxiliary-link29.exe')],['C:/nvm4w/nodejs/node.exe','tests/auxiliary_link29_new.js']]
checks=[]
for cmd in commands:
    p=subprocess.run(cmd,cwd=root,env=env,capture_output=True,text=True,encoding='utf-8')
    checks.append(dict(command=cmd,exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
    if p.returncode:break
record=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),checks=checks,snapshot_sha256=hashlib.sha256(snapshot.read_bytes()).hexdigest(),scope='製品 native link/host spawn 呼出境界を抽出。実OS spawn/GUI/サイトの受入ではない。JSは製品 realm descriptor と adapter 委譲だけ。')
(out/'result.json').write_text(json.dumps(record,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(record,ensure_ascii=False,indent=2))
if len(checks)!=3 or any(c['exit'] for c in checks):raise SystemExit('新29原検査失敗を保存。再走は禁止。')
