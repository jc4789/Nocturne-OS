/* Actual Nocturne DOM/QuickJS/private IPC/child/FAT path. No IndexedDB host
 * adapter or fabricated Promise resolution participates in this acceptance. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main

int main(void) {
    const char *page=START "</head><body><script>"
        "const requestValue=q=>new Promise((resolve,reject)=>{q.onsuccess=()=>resolve(q.result);q.onerror=()=>reject(q.error)});"
        "const finished=t=>new Promise((resolve,reject)=>{t.oncomplete=resolve;t.onabort=()=>reject(t.error||new Error('abort'));t.onerror=()=>{}});"
        "(async()=>{const name='native-broker-'+Date.now(),open=indexedDB.open(name,1);"
        "open.onupgradeneeded=()=>{const s=open.result.createObjectStore('records',{keyPath:'id'});s.createIndex('label','label',{unique:true})};"
        "const db=await requestValue(open);mark('idb-broker-open');let heartbeats=0;const beat=setInterval(()=>heartbeats++,5);"
        "const write=db.transaction('records','readwrite'),writeDone=finished(write),store=write.objectStore('records');let successes=0;"
        "for(let id=0;id<80;id++){const q=store.put({id,label:'key-'+id,bytes:new Uint8Array([id,255-id])});q.onsuccess=()=>successes++}"
        "await writeDone;clearInterval(beat);check('idb-broker-write-order',successes===80);check('idb-broker-GUI-task-progress',heartbeats>0);"
        "const read=db.transaction('records'),readDone=finished(read),all=await requestValue(read.objectStore('records').getAll());await readDone;"
        "check('idb-broker-batch-values',all.length===80&&all.every((v,i)=>v.id===i&&v.bytes[0]===i&&v.bytes[1]===255-i));"
        "const abort=db.transaction('records','readwrite'),aborted=finished(abort).then(()=>false,()=>true);abort.objectStore('records').put({id:100,label:'aborted'});abort.abort();check('idb-broker-abort',await aborted);"
        "const absent=db.transaction('records'),absentDone=finished(absent),notStored=await requestValue(absent.objectStore('records').get(100));await absentDone;check('idb-broker-abort-atomic',notStored===undefined);"
        "const cursorTx=db.transaction('records'),cursorDone=finished(cursorTx);let cursorCount=0;await new Promise((resolve,reject)=>{const q=cursorTx.objectStore('records').openCursor();q.onsuccess=()=>{const c=q.result;if(!c){resolve();return}if(c.value.id!==cursorCount){reject(new Error('cursor order'));return}cursorCount++;c.continue()};q.onerror=()=>reject(q.error)});await cursorDone;check('idb-broker-cursor-blocks',cursorCount===80);"
        "const inactive=db.transaction('records'),inactiveDone=finished(inactive);await new Promise(resolve=>setTimeout(resolve,0));let rejected=false;try{inactive.objectStore('records').get(0)}catch(e){rejected=e.name==='TransactionInactiveError'}check('idb-broker-task-inactive',rejected);await inactiveDone;"
        "const renamed=indexedDB.open(name,2);renamed.onupgradeneeded=()=>renamed.transaction.objectStore('records').createIndex('byteId','id',{unique:true});"
        "let notified=false;db.onversionchange=()=>{notified=true;db.close()};const upgraded=await requestValue(renamed);check('idb-broker-versionchange',notified&&upgraded.version===2&&upgraded.transaction('records').objectStore('records').indexNames.contains('byteId'));"
        "upgraded.close();await requestValue(indexedDB.deleteDatabase(name));check('idb-broker-delete',true);mark('api-done')"
        "})().catch(e=>{console.log('FAIL IndexedDB broker '+e+' '+e.stack);mark('api-done')});</script></body>";
    if(open_case(page,false)){
        test_check("idb-broker-open",pump("idb-broker-open",30000));
        struct n_procinfo rows[128];int count=proclist(rows,128);bool found=false;
        for(int i=0;i<count&&i<128;i++)if(!strcmp(rows[i].name,"browserstorageworker"))found=true;
        test_check("idb-real-native-helper",found);
        test_check("idb-broker-finished",pump("api-done",60000));
        test_check("idb-broker-no-js-errors",fixture.errors==0&&fixture.js_failures==0);close_case();
    }
    printf("indexeddbguesttest: %d checks, %d failed\n",total,failed);return failed!=0;
}
