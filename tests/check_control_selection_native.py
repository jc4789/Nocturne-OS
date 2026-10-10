"""Exercise current native selection with real, non-terminating sbuf helpers.

The JS beforeinput mock is only a transaction permission oracle. This is not a
browser integration claim; the isolated guest selection-native case covers it.
"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/parallel-selection-contract-20261010'
out.mkdir(parents=True, exist_ok=True)
doc = (root / 'user/libc/web/doc.c').read_text(encoding='utf-8')
util = (root / 'user/libc/web/util.c').read_text(encoding='utf-8')
prefix = (root / 'tests/control_physical_edit_prefix.c').read_text(encoding='utf-8')
prefix = prefix[:prefix.index('int main(void)')]
start = prefix.index('static void sb_put(')
end = prefix.index('static enum web_input_kind web_input_type(')
helpers = util[util.index('static void sb_grow('):util.index('int utf8_put(')]
helpers += util[util.index('char *sb_cstr('):util.index('void pv_push(')]
prefix = prefix[:start] + helpers + prefix[end:]
# Deterministically poison spare bytes while leaving a boundary sentinel. A
# zero-filled allocator would hide a missing terminator before the UTF-16 scan.
old = 'return malloc(n);'
prefix = prefix.replace(old, 'void *p=malloc(n);if(p&&n){memset(p,0x5a,n);((char *)p)[n-1]=0;}return p;', 1)
utf = doc[doc.index('static uint32_t control_decode('):doc.index('static bool focus_under(')]
keys = doc[doc.index('static bool readonly('):doc.index('static node_t *form_id(')]
prefix = prefix.replace('/* ACTUAL_CONTROL_FUNCTIONS */', utf + '\n' + keys)
prefix = prefix.replace('return web_key(&document,&e);', 'return web_key_local(&document,&e);')
main = r'''
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    reset("abcdef",1,4);
    check(key('Z',0)==1&&!strcmp(control.value,"aZef"),"非ゼロ開始位置の置換本文");
    check(control.selection_start==2&&control.selection_end==2,"非終端sbuf置換caret");
    check(before_count==1&&input_count==1,"置換beforeinput/input順序");
    reset("a\xf0\x9f\x98\x80" "b",1,3);
    check(key(NKEY_DELETE,0)==1&&!strcmp(control.value,"ab"),"非ゼロ開始位置の削除本文");
    check(control.selection_start==1&&control.selection_end==1,"空挿入の非終端sbufcaret");
    reset("abcd",1,3);control.maxlength="4";
    check(key('X',0)==1&&!strcmp(control.value,"aXd"),"maxlength選択置換本文");
    check(control.selection_start==2&&control.selection_end==2,"maxlength選択置換caret");
    reset("abcd",2,2);control.maxlength="4";
    check(key('Y',0)==1&&!strcmp(control.value,"abcd")&&control.selection_start==2&&before_count==0,"maxlength拒否キー消費");
    reset("ab",1,1);mutation=1;
    check(key('X',0)==1&&!strcmp(control.value,"ab")&&input_count==0,"beforeinput取消を維持");
    reset("ab",1,1);mutation=3;
    check(key('X',0)==1&&!strcmp(control.value,"handler")&&input_count==0,"beforeinput値変更を上書きしない");
    printf("selection native boundary: %u checks, %u failed\n",checks,failed);
    free(control.value);return failed!=0;
}
'''
generated = out / 'selection-native-generated.c'
generated.write_text(prefix + main, encoding='utf-8', newline='\n')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
exe = out / 'selection-native.exe'
log = out / 'selection-native.log'
with log.open('w', encoding='utf-8') as f:
    for file in [root / 'user/libc/web/doc.c', root / 'user/libc/web/control_edit.h', root / 'user/libc/web/util.c', generated]:
        f.write(file.name + ' SHA256 ' + hashlib.sha256(file.read_bytes()).hexdigest() + '\n')
    f.flush()
    result = subprocess.run([str(cc), '-std=gnu11', '-O1', '-I.', '-Iuser/libc/web', str(generated), '-o', str(exe)], cwd=root, env=env, stdout=f, stderr=subprocess.STDOUT)
    if result.returncode == 0:
        result = subprocess.run([str(exe)], cwd=root, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=20)
    f.write('exit ' + str(result.returncode) + '\n')
print(log.read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
