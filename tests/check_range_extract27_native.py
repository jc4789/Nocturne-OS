"""New extraction-only supplement, not a site/rendering/complete DOM claim.
Actual js_range bindings + native may_insert/detach/doc_node_move/remove bodies.
Node construction/cloning/data, CE notification, layout/host are fixture edges.
Old deletion suite is never evaluated. Guard preserves the original outcome.
"""
from pathlib import Path
import datetime,json,os,subprocess
root=Path(__file__).resolve().parent.parent;out=root/'build/goal-20261009/root-range-extract27-new-once'
out.mkdir(parents=True,exist_ok=True)
result=out/'result.json'
if result.exists():raise SystemExit('新抽出境界は保存済み。再走しません。')
result.write_text(json.dumps({'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'state':'started'}),encoding='utf-8')
dom=(root/'user/libc/web/dom.c').read_text(encoding='utf-8')
def fn(signature):
 a=dom.index(signature);return dom[a:dom.index('\n}',a)+2]+'\n'
c=(root/'tests/range_delete_native_cases.c').read_text(encoding='utf-8')
c=c.replace('*slot_assigned_first;','*slot_assigned_first,*template_host,*document;')
c=c.replace('struct web_doc {node_t *focus;int caret;};','struct web_doc {node_t *focus;int caret;web_doc *dom_family;struct {unsigned inserts;} profile;};\nenum{N_ELEM=1,N_ATTR=2,N_TEXT=3,N_DOC=9,N_DOCTYPE=10,N_FRAGMENT=11};')
c=c.replace('/* PRODUCT_REMOVE */', '''static bool under(node_t *n,node_t *p){for(;n;n=n->parent)if(n==p)return true;return false;}
static bool web_js_nodes_same_forest(web_doc *d,node_t *a,node_t *b){return false;}
static bool doc_node_adopt(web_doc *d,node_t *n){return false;}
static void web_dialog_removed(web_doc *d,node_t *n){}
static void structure_changed_lifetime(web_doc *d,node_t *p,node_t *n,bool b){removals++;}
static void doc_details_inserted(node_t *n){}
'''+fn('static void detach(')+fn('static bool may_insert(')+fn('bool doc_node_move(')+fn('void doc_node_remove('))
c=c.replace('n->type=type;','n->type=type;n->document=type==9?n:argc>1?unwrap(argv[1]):NULL;')
c=c.replace('node_t *d=all;while(d&&d->type!=9)d=d->all_next;result=wrap(d);','result=wrap(n->document);')
old='if(!child)result=JS_ThrowTypeError(ctx,"Native child required");else link_node(n,child);'
new='''if(!child)result=JS_ThrowTypeError(ctx,"Native child required");
        else if(fail_mutation){fail_mutation=false;result=JS_ThrowInternalError(ctx,"fixture actual mutation rejection");}
        else if(!doc_node_move(n->owner,n,child,argc>3?unwrap(argv[3]):NULL))result=JS_ThrowInternalError(ctx,"native move rejected");'''
if old not in c:raise SystemExit('fixture insert anchor missing')
c=c.replace(old,new)
c=c.replace('}else if(!strcmp(op,"remove")){','''}else if(!strcmp(op,"create")){
        JSValue args[]={argv[2],argv[1],JS_NewString(ctx,"")};result=make_node(ctx,JS_UNDEFINED,3,args);JS_FreeValue(ctx,args[2]);
    }else if(!strcmp(op,"clone")){
        JSValue args[]={JS_NewInt32(ctx,n->type),wrap(n->document),JS_DupValue(ctx,n->data)};
        result=make_node(ctx,JS_UNDEFINED,3,args);for(int i=0;i<3;i++)JS_FreeValue(ctx,args[i]);
    }else if(!strcmp(op,"remove")){''')
c=c.replace('build/goal-20261009/range-delete-native-bindings.js','build/goal-20261009/root-range-extract27-new-once/bindings.js').replace('range-delete-new-bindings','range-extract27-new-bindings').replace('range-delete-new-boundaries','range-extract27-new-boundaries')
(out/'native-generated.c').write_text(c,encoding='utf-8')
prefix=(root/'tests/range_delete_native_prefix.js').read_text(encoding='utf-8')
prefix=prefix.replace('let onRemoved=null;','let onRemoved=null,onMoved=null;')
prefix=prefix.replace('const before=rangeBridge.before(...args),result=rawDom(...args);','const oldParent=args[0]===\'insert\'?rawDom(\'get\',args[2],\'parentNode\'):null;const before=rangeBridge.before(...args),result=rawDom(...args);')
prefix=prefix.replace('return result;};','if(oldParent&&onMoved)reactions.push(()=>onMoved(args[2],oldParent));return result;};')
js=prefix+(root/'user/libc/web/js_range.js').read_text(encoding='utf-8')+'\n'+(root/'tests/js_range_extract27_cases.js').read_text(encoding='utf-8')+'\n})();'
(out/'bindings.js').write_text(js,encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';exe=out/'native.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[root/('third_party/quickjs/'+n+'.c') for n in ('quickjs','dtoa','libregexp','libunicode','cutils')]
argv=[str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(out/'native-generated.c'),*map(str,engine),'-lm','-o',str(exe)]
compiled=subprocess.run(argv,cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace');(out/'compile.log').write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
record={'compile_exit':compiled.returncode,'scope':'actual Range/native move bodies; fixture clone/create/data/notifications; no site acceptance'}
if compiled.returncode==0:
 run=subprocess.run([str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
 record['runtime_exit']=run.returncode;(out/'runtime-original.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout+run.stderr)
else:print(compiled.stdout+compiled.stderr)
result.write_text(json.dumps(record,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(record,ensure_ascii=False))
raise SystemExit(0 if record.get('runtime_exit')==0 else 1)
