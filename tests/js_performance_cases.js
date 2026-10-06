/* 実Nocturneバインディングの補助回帰。実サイト操作合格の代替ではない。 */
async function waitForPerformanceCase(done,name) {
    const deadline=performance.now()+2000;
    for(let turns=0;!done();turns++){
        if(turns>=512||performance.now()>=deadline)throw Error('performance待機期限: '+name);
        await new Promise(resolve=>setTimeout(resolve,0));
    }
}
async function runPerformanceCases() {
    let count=0;
    const ok=(name,value)=>{count++;if(!value)throw new Error('performance: '+name);};
    const eq=(name,a,b)=>ok(name,Object.is(a,b));
    const throws=(name,error,fn)=>{let caught;try{fn();}catch(e){caught=e;}ok(name,!!caught&&(typeof error==='string'?caught.name===error:caught instanceof error));};
    const tick=()=>new Promise(resolve=>setTimeout(resolve,8));
    performance.clearMarks();performance.clearMeasures();
    ok('Performance継承',performance instanceof Performance&&performance instanceof EventTarget);
    eq('Performanceタグ',Object.prototype.toString.call(performance),'[object Performance]');
    throws('Performance構築不可',TypeError,()=>new Performance());
    throws('Entry構築不可',TypeError,()=>new PerformanceEntry());
    throws('Measure構築不可',TypeError,()=>new PerformanceMeasure());
    throws('List構築不可',TypeError,()=>new PerformanceObserverEntryList());
    const origin=performance.timeOrigin,before=performance.now();
    ok('実時計',Number.isFinite(before)&&before>=0&&Number.isFinite(origin)&&origin>1700000000000);
    ok('epoch基準',Math.abs(origin+before-Date.now())<1500);
    eq('Performance JSON',performance.toJSON().timeOrigin,origin);
    throws('readonly origin',TypeError,()=>{'use strict';performance.timeOrigin=0;});
    throws('now receiver',TypeError,()=>Performance.prototype.now.call({}));
    throws('mark receiver',TypeError,()=>Performance.prototype.mark.call({},'x'));
    throws('getEntries receiver',TypeError,()=>Performance.prototype.getEntries.call({}));
    const autocomplete=performance.mark('autocomplete');
    ok('DDG autocomplete実記録',autocomplete instanceof PerformanceMark&&autocomplete instanceof PerformanceEntry&&autocomplete.startTime>=before);
    eq('mark duration',autocomplete.duration,0);eq('mark detail default',autocomplete.detail,null);
    eq('mark return identity',performance.getEntriesByName('autocomplete','mark')[0],autocomplete);
    eq('mark tag',Object.prototype.toString.call(autocomplete),'[object PerformanceMark]');
    const isolated=new PerformanceMark('constructed',{startTime:3.25});
    eq('constructor時刻',isolated.startTime,3.25);eq('constructor非記録',performance.getEntriesByName('constructed').length,0);
    for(const prop of ['name','entryType','startTime','duration','detail'])
        throws('readonly '+prop,TypeError,()=>{'use strict';autocomplete[prop]=0;});
    const detail={x:{n:1},map:new Map(),bytes:new Uint8Array([4,5])};detail.self=detail;detail.map.set(detail,detail.x);
    const m=performance.mark('metadata',{startTime:1.5,detail});detail.x.n=9;detail.bytes[0]=88;
    ok('metadata構造複製',m.detail!==detail&&m.detail.self===m.detail&&m.detail.x.n===1&&m.detail.map.get(m.detail)===m.detail.x&&m.detail.bytes[0]===4);
    eq('metadata SameObject',m.detail,m.detail);eq('toJSON detail identity',m.toJSON().detail,m.detail);
    eq('mark JSON値',JSON.stringify(new PerformanceMark('json',{startTime:12,detail:'猫'})),JSON.stringify({name:'json',entryType:'mark',startTime:12,duration:0,detail:'猫'}));
    ok('IDL getter descriptor',Object.getOwnPropertyDescriptor(PerformanceEntry.prototype,'name').enumerable);
    eq('IDL optional length',Performance.prototype.clearMarks.length,0);eq('IDL required length',Performance.prototype.getEntriesByName.length,1);
    throws('mark必須名',TypeError,()=>performance.mark());throws('constructor必須名',TypeError,()=>new PerformanceMark());
    throws('symbol名',TypeError,()=>performance.mark(Symbol()));throws('辞書型',TypeError,()=>performance.mark('x',5));
    for(const time of [-1,NaN,Infinity,-Infinity,1n,Symbol()])throws('不正mark時刻 '+String(time),TypeError,()=>performance.mark('bad',{startTime:time}));
    throws('detail非複製値','DataCloneError',()=>performance.mark('uncloneable',{detail:()=>{}}));
    eq('不正mark非記録',performance.getEntriesByName('uncloneable').length,0);
    const names='navigationStart unloadEventStart unloadEventEnd redirectStart redirectEnd fetchStart domainLookupStart domainLookupEnd connectStart connectEnd secureConnectionStart requestStart responseStart responseEnd domLoading domInteractive domContentLoadedEventStart domContentLoadedEventEnd domComplete loadEventStart loadEventEnd'.split(' ');
    for(const name of names)throws('予約名 '+name,'SyntaxError',()=>performance.mark(name));
    let getters=[];performance.mark('getters',{get detail(){getters.push('detail');return null;},get startTime(){getters.push('startTime');return '2';}});
    eq('mark辞書順序',getters.join(','),'detail,startTime');
    performance.mark('undefined');performance.mark(undefined);performance.mark(null);performance.mark('');
    eq('DOMString undefined',performance.getEntriesByName(undefined).length,2);
    eq('DOMString null',performance.getEntriesByName(null).length,1);
    eq('空名',performance.getEntriesByName('').length,1);
    performance.clearMarks(undefined);eq('undefined clear全消去',performance.getEntries().length,0);
    const late=performance.mark('same',{startTime:30}),early=performance.mark('same',{startTime:10});
    performance.mark('same',{startTime:10});
    const a=performance.getEntriesByType('mark');ok('時系列安定順序',a[0]===early&&a[2]===late);
    a.length=0;eq('取得配列独立',performance.getEntries().length,3);
    const between=performance.measure('between','same','same');
    ok('measure型',between instanceof PerformanceMeasure&&between instanceof PerformanceEntry);
    eq('直近発生mark選択',between.startTime,10);eq('between期間',between.duration,0);
    eq('measureタグ',Object.prototype.toString.call(between),'[object PerformanceMeasure]');
    Object.defineProperty(early,'startTime',{value:9999});
    eq('公開shadowは記録非変更',performance.measure('shadow',{start:'same',end:14}).duration,4);
    eq('slotで取得sort',performance.getEntriesByType('mark')[0],early);
    const forward=performance.measure('forward',{start:10,end:30,detail:{n:1}});
    eq('数値start',forward.startTime,10);eq('数値期間',forward.duration,20);eq('measure detail',forward.detail.n,1);
    const negative=performance.measure('negative',{start:30,end:10});eq('逆向き期間許容',negative.duration,-20);
    const durationEnd=performance.measure('end-duration',{end:5,duration:8});
    eq('計算負start許容',durationEnd.startTime,-3);eq('end-duration',durationEnd.duration,8);
    const durationStart=performance.measure('start-duration',{start:5,duration:8});eq('start-duration',durationStart.duration,8);
    throws('union nullは文字列','SyntaxError',()=>performance.measure('zero',{start:null,end:5}));
    eq('不正measure非記録',performance.getEntriesByName('zero').length,0);
    const nav=performance.measure('since-navigation','navigationStart');eq('navigationStart基準',nav.startTime,0);
    throws('未記録navigation時間','InvalidAccessError',()=>performance.measure('no-native-navigation','responseEnd'));
    throws('存在しないmark','SyntaxError',()=>performance.measure('missing','missing-mark'));
    throws('measure必須名',TypeError,()=>performance.measure());
    for(const o of [{duration:2},{detail:1},{start:1,end:2,duration:1},{start:-1},{end:-1},{start:0,duration:-1},{end:3,duration:-1},{start:NaN},{end:Infinity},{duration:1n}])
        throws('不正measure options',TypeError,()=>performance.measure('bad-measure',o));
    throws('optionsと第3引数',TypeError,()=>performance.measure('bad',{start:1},'same'));
    throws('measure複製失敗','DataCloneError',()=>performance.measure('bad',{start:1,detail:Symbol()}));
    const nullOptions=performance.measure('null-options',null);eq('null辞書start',nullOptions.startTime,0);
    const emptyAndEnd=performance.measure('empty-options-end',{},'same');eq('空辞書とend mark',emptyAndEnd.duration,10);
    getters=[];performance.measure('getters',{get detail(){getters.push('detail');return null;},get duration(){getters.push('duration');return undefined;},get end(){getters.push('end');return 4;},get start(){getters.push('start');return 2;}});
    eq('measure辞書順序',getters.join(','),'detail,duration,end,start');
    const originalNow=performance.now,originalClone=structuredClone,originalMark=PerformanceMark;
    try{
        performance.now=()=>-999;globalThis.structuredClone=()=>({fake:true});globalThis.PerformanceMark=function(){throw Error('fake');};
        const privateMark=performance.mark('private',{detail:{real:true}});
        ok('private原時計とcloneとconstructor',privateMark.startTime>=before&&privateMark.detail.real===true&&privateMark instanceof originalMark);
    }finally{performance.now=originalNow;globalThis.structuredClone=originalClone;globalThis.PerformanceMark=originalMark;}
    eq('name型filter',performance.getEntriesByName('same','measure').length,0);
    eq('未実装resource非偽装',performance.getEntriesByType('resource').length,0);
    throws('type引数必須',TypeError,()=>performance.getEntriesByType());
    throws('name引数必須',TypeError,()=>performance.getEntriesByName());
    throws('type symbol',TypeError,()=>performance.getEntriesByType(Symbol()));
    const measures=performance.getEntriesByType('measure').length;
    performance.clearMarks('same');eq('特定mark消去',performance.getEntriesByName('same').length,0);
    eq('mark消去はmeasure保持',performance.getEntriesByType('measure').length,measures);
    performance.clearMeasures('between');eq('特定measure消去',performance.getEntriesByName('between').length,0);
    performance.clearMarks();performance.clearMeasures();eq('全消去',performance.getEntries().length,0);

    const types=PerformanceObserver.supportedEntryTypes;
    eq('UserTimingだけ広告',types.join(','),'mark,measure');eq('SameObject型配列',types,PerformanceObserver.supportedEntryTypes);
    ok('型配列凍結',Object.isFrozen(types));
    throws('callback必須',TypeError,()=>new PerformanceObserver());
    const invalid=new PerformanceObserver(()=>{});
    for(const o of [undefined,null,{},4,{entryTypes:['mark'],type:'mark'},{entryTypes:['mark'],buffered:false},{entryTypes:null},{entryTypes:'mark'},{entryTypes:{0:'mark',length:1}},{type:Symbol()}])
        throws('observer不正options',TypeError,()=>invalid.observe(o));
    throws('observer receiver',TypeError,()=>PerformanceObserver.prototype.observe.call({}, {type:'mark'}));
    const multiple=new PerformanceObserver(()=>{});multiple.observe({entryTypes:['mark']});
    throws('登録mode変更','InvalidModificationError',()=>multiple.observe({type:'mark'}));
    multiple.disconnect();throws('disconnect後mode保持','InvalidModificationError',()=>multiple.observe({type:'mark'}));
    const single=new PerformanceObserver(()=>{});single.observe({type:'mark'});single.observe({type:'measure'});
    const recMark=performance.mark('record',{startTime:11});const recMeasure=performance.measure('record',{start:3,end:6});
    const records=single.takeRecords();ok('single登録stack',records[0]===recMark&&records[1]===recMeasure);
    eq('takeRecords空化',single.takeRecords().length,0);single.disconnect();
    const order=[],received=[];
    const observer=new PerformanceObserver(function(list,self,options){
        order.push('observer');received.push(list);
        eq('callback this',this,observer);eq('callback observer',self,observer);
        ok('callback entrylist',list instanceof PerformanceObserverEntryList);
        eq('初回drop数',options.droppedEntriesCount,0);
    });
    observer.observe({entryTypes:['mark','mark','measure','resource','paint']});
    const first=performance.mark('observed',{startTime:22});performance.mark('observed',{startTime:12});
    const observedMeasure=performance.measure('observed',{start:15,end:19});
    performance.clearMarks('observed');
    Promise.resolve().then(()=>order.push('microtask'));
    eq('通知同期でない',received.length,0);
    await tick();
    eq('coalesce通知',received.length,1);eq('microtask先行',order.join(','),'microtask,observer');
    const list=received[0],batch=list.getEntries();
    ok('clearは待機通知保持',batch.length===3&&batch[0].startTime===12&&batch[1]===observedMeasure&&batch[2]===first);
    eq('list type filter',list.getEntriesByType('mark').length,2);
    eq('list name filter',list.getEntriesByName('observed','measure')[0],observedMeasure);
    batch.length=0;eq('list独立配列',list.getEntries().length,3);
    throws('list receiver',TypeError,()=>PerformanceObserverEntryList.prototype.getEntries.call({}));
    observer.disconnect();
    performance.mark('after-disconnect');await tick();eq('disconnect通知停止',received.length,1);

    let drained=0;const drain=new PerformanceObserver(()=>drained++);drain.observe({type:'mark'});
    performance.mark('drained');eq('待機takeRecords',drain.takeRecords().length,1);await tick();eq('drain後通知抑止',drained,0);
    performance.mark('disconnected');drain.disconnect();eq('disconnect待機破棄',drain.takeRecords().length,0);await tick();eq('disconnect task抑止',drained,0);
    performance.clearMarks();performance.clearMeasures();
    const stored=performance.mark('buffered',{startTime:7}),bufferedBatches=[];
    const buffered=new PerformanceObserver(list=>bufferedBatches.push(list.getEntries()));
    buffered.observe({type:'mark',buffered:true});eq('buffered通知同期でない',bufferedBatches.length,0);
    await tick();eq('buffered通知',bufferedBatches[0][0],stored);
    const next=performance.mark('next');await tick();eq('bufferedの新規通知',bufferedBatches[1][0],next);
    buffered.observe({type:'mark',buffered:true});await tick();eq('再buffer取得',bufferedBatches[2].length,2);buffered.disconnect();
    let unsupported=0;const unavailable=new PerformanceObserver(()=>unsupported++);unavailable.observe({type:'resource',buffered:true});
    performance.mark('real-mark');await tick();eq('未実装type無通知',unsupported,0);unavailable.disconnect();
    let replaced=[];const replace=new PerformanceObserver(list=>replaced.push(...list.getEntries()));
    replace.observe({entryTypes:['mark']});replace.observe({entryTypes:['measure']});
    performance.mark('not-subscribed');const subscribed=performance.measure('subscribed',{start:0,end:1});
    await tick();ok('multiple登録置換',replaced.length===1&&replaced[0]===subscribed);replace.disconnect();
    eq('origin固定',performance.timeOrigin,origin);ok('単調時計前進',performance.now()>=before);
    performance.clearMarks();performance.clearMeasures();
    return count;
}

/* 第2境界検査。再入とアクセサ副作用を独立に再現する。 */
async function runPerformanceBoundaryCases() {
    let count=0;
    const ok=(name,value)=>{count++;if(!value)throw Error('performance境界: '+name);};
    const eq=(name,a,b)=>ok(name,Object.is(a,b));
    const throws=(name,error,fn)=>{let caught;try{fn();}catch(e){caught=e;}ok(name,!!caught&&(typeof error==='string'?caught.name===error:caught instanceof error));return caught;};
    const tick=()=>new Promise(resolve=>setTimeout(resolve,8));
    const clean=()=>{performance.clearMarks();performance.clearMeasures();};clean();
    const visits=[],aBatches=[],bBatches=[],checkpoints=[];
    const a=new PerformanceObserver(list=>{
        visits.push('a');aBatches.push(list.getEntries());
        eq('callback takeRecords空',a.takeRecords().length,0);
        if(aBatches.length===1){
            Promise.resolve().then(()=>checkpoints.push('checkpoint'));
            performance.mark('reentrant-mark',{startTime:5});
            a.disconnect();a.observe({entryTypes:['measure']});
            performance.measure('reentrant-measure',{start:2,end:3});
        }else {checkpoints.push('second');a.disconnect();}
    });
    const b=new PerformanceObserver(list=>{visits.push('b');bBatches.push(list.getEntries());b.disconnect();});
    a.observe({entryTypes:['mark','measure']});b.observe({entryTypes:['mark','measure']});
    const initialMark=performance.mark('initial-mark',{startTime:20});
    const initialMeasure=performance.measure('initial-measure',{start:1,end:2});
    // 初回notifyが遅れると、再入notifyは古い8ms timerより後になる。
    // 完了を有界に待ち、訪問順・microtask checkpoint・回数は弱めない。
    try{await waitForPerformanceCase(()=>visits.length>=3,'再入notify');}
    catch(e){a.disconnect();b.disconnect();throw e;}
    ok('再入別task通知',visits.join(',')==='a,b,a'&&checkpoints.join(',')==='checkpoint,second');eq('a通知回数',aBatches.length,2);eq('b通知回数',bBatches.length,1);
    eq('a初回範囲',aBatches[0].map(x=>x.name).join(','),'initial-measure,initial-mark');
    eq('後observerは同task追加を取得',bBatches[0].map(x=>x.name).join(','),'initial-measure,reentrant-measure,reentrant-mark,initial-mark');
    eq('再登録で新typeだけ',aBatches[1].map(x=>x.name).join(','),'reentrant-measure');
    eq('初回entry共有identity',aBatches[0][0],initialMeasure);eq('複数observer共有identity',bBatches[0][3],initialMark);
    performance.mark('no-observers');await tick();eq('再入disconnect停止',visits.length,3);clean();

    const prior=performance.mark('prior',{startTime:8});let drainCallbacks=0;
    const draining=new PerformanceObserver(()=>drainCallbacks++);draining.observe({type:'mark',buffered:true});
    performance.clearMarks('prior');eq('clear前buffered待機記録',draining.takeRecords()[0],prior);
    draining.observe({type:'mark',buffered:true});await tick();eq('clear後buffered再取得なし',drainCallbacks,0);draining.disconnect();
    const original=[],later=[];
    const live=new PerformanceObserver(list=>original.push(...list.getEntries()));live.observe({type:'mark'});
    const kept=performance.mark('kept',{startTime:7}),removed=performance.mark('removed',{startTime:6});performance.clearMarks('removed');
    const late=new PerformanceObserver(list=>later.push(...list.getEntries()));late.observe({type:'mark',buffered:true});
    await tick();eq('既存通知はclearで消えない',original.length,2);eq('既存通知時間順',original[0],removed);
    eq('buffered新登録は現bufferだけ',later.length,1);eq('buffered削除filter',later[0],kept);live.disconnect();late.disconnect();clean();

    let observed=[];const taken=new PerformanceObserver(()=>{throw Error('takeRecords後callbackは禁止');});
    const untouched=new PerformanceObserver(list=>observed.push(...list.getEntries()));
    taken.observe({entryTypes:['mark']});untouched.observe({entryTypes:['mark']});
    const high=performance.mark('high',{startTime:30}),low=performance.mark('low',{startTime:10});
    const records=taken.takeRecords();eq('takeRecords発生順1',records[0],high);eq('takeRecords発生順2',records[1],low);
    await tick();eq('空observerで他observer停止しない',observed.length,2);eq('通知の時間順',observed[0],low);
    taken.disconnect();untouched.disconnect();
    let countEmpty=0;const empty=new PerformanceObserver(list=>countEmpty+=list.getEntries().length);
    empty.observe({entryTypes:['mark']});empty.observe({entryTypes:[]});empty.observe({entryTypes:['resource']});
    performance.mark('after-empty-options');await tick();eq('空type登録は既存登録非破壊',countEmpty,1);empty.disconnect();clean();

    const log=[];const name={toString(){log.push('name');return 'accessor-mark';}};
    const own=performance.mark(name,{get detail(){log.push('detail');performance.mark('inside-options',{startTime:1});return {x:1};},get startTime(){log.push('startTime');return 2;}});
    eq('name変換と辞書順序',log.join(','),'name,detail,startTime');eq('アクセサ内mark記録',performance.getEntriesByName('inside-options').length,1);eq('外mark記録',performance.getEntriesByName('accessor-mark')[0],own);
    const sentinel=Error('sentinel');let reads=0;
    eq('アクセサ例外identity',throws('辞書例外伝播',Error,()=>performance.mark('accessor-throws',{get detail(){throw sentinel;},get startTime(){reads++;return 0;}})),sentinel);
    eq('例外以降getter非実行',reads,0);eq('辞書例外非記録',performance.getEntriesByName('accessor-throws').length,0);
    const cloneInput={get x(){performance.clearMarks('inside-options');performance.mark('inside-clone',{startTime:3});return 7;}};
    const cloneOuter=performance.mark('clone-outer',{detail:cloneInput,startTime:4});
    eq('clone getter値',cloneOuter.detail.x,7);eq('clone getter内消去反映',performance.getEntriesByName('inside-options').length,0);
    eq('clone getter内mark保持',performance.getEntriesByName('inside-clone').length,1);
    const measureLog=[];
    const accessorMeasure=performance.measure('accessor-measure',{
        get detail(){measureLog.push('detail');return {good:true};},
        get duration(){measureLog.push('duration');return undefined;},
        get end(){measureLog.push('end');performance.mark('mutating-start',{startTime:2});return 20;},
        get start(){measureLog.push('start');performance.mark('mutating-start',{startTime:9});return 'mutating-start';}
    });
    eq('measure辞書変換順',measureLog.join(','),'detail,duration,end,start');eq('変換完了後最新mark解決',accessorMeasure.startTime,9);eq('変換後期間',accessorMeasure.duration,11);
    const noPartial=new PerformanceObserver(()=>{});noPartial.observe({entryTypes:['mark','measure']});
    let proxyTraps=0;
    const rejected=new Proxy({},{ownKeys(){proxyTraps++;throw Error('trap');},get(){proxyTraps++;throw Error('trap');}});
    throws('proxy detail拒否','DataCloneError',()=>performance.mark('bad-proxy',{detail:rejected}));eq('proxy trap非実行',proxyTraps,0);
    throws('measure clone失敗','DataCloneError',()=>performance.measure('bad-clone',{start:0,end:1,detail:{bad:()=>{}}}));
    eq('clone失敗はmark非記録',performance.getEntriesByName('bad-proxy').length,0);eq('clone失敗はmeasure非記録',performance.getEntriesByName('bad-clone').length,0);
    eq('clone失敗は通知非記録',noPartial.takeRecords().length,0);noPartial.disconnect();
    let detailReads=0;
    throws('Symbol名は辞書変換前に拒否',TypeError,()=>performance.mark(Symbol(),{get detail(){detailReads++;return 1;}}));eq('不正名でgetter非実行',detailReads,0);
    const notSerializable=[performance,new PerformanceMark('nonserializable'),performance.measure('nonserializable',{start:0,end:1}),new PerformanceObserver(()=>{})];
    const withList=new PerformanceObserver(list=>notSerializable.push(list));withList.observe({type:'mark'});performance.mark('list-brand');await tick();withList.disconnect();
    eq('platform class coverage',notSerializable.length,5);
    for(const value of notSerializable){
        const originalPrototype=Object.getPrototypeOf(value);
        let memberReads=0;Object.defineProperty(value,'member',{enumerable:true,get(){memberReads++;throw Error('platform getter');}});
        throws('直接platform複製拒否','DataCloneError',()=>structuredClone(value));
        throws('入れ子platform metadata拒否','DataCloneError',()=>performance.mark('platform-detail',{detail:{nested:new Map([[1,value]])}}));
        throws('measure platform metadata拒否','DataCloneError',()=>performance.measure('platform-measure',{start:0,end:1,detail:value}));
        eq('platform getter非実行',memberReads,0);
        Object.setPrototypeOf(value,{});
        throws('prototype変更後もbrand拒否','DataCloneError',()=>structuredClone({value}));
        Object.setPrototypeOf(value,originalPrototype);
    }
    eq('platform失敗mark非記録',performance.getEntriesByName('platform-detail').length,0);
    eq('platform失敗measure非記録',performance.getEntriesByName('platform-measure').length,0);
    class Ordinary {constructor(){this.x=3;}}
    const plain=structuredClone(new Ordinary());eq('一般クラスclone保持',plain.x,3);eq('一般クラスclone prototype',Object.getPrototypeOf(plain),Object.prototype);
    let visitsClone=0;const once=structuredClone({get child(){visitsClone++;return {v:4};}});
    eq('拒否hookでgetter二重走査しない',visitsClone,1);eq('getter値保持',once.child.v,4);
    clean();return count;
}

/* 観測callback例外を1件報告する負系。QEMUではexpected_errors=1で別実行。 */
async function runPerformanceObserverExceptionCases() {
    let calls=0;
    const observer=new PerformanceObserver(()=>{calls++;if(calls===1)throw Error('performance-observer-expected-error');observer.disconnect();});
    try{
        observer.observe({type:'mark'});performance.mark('throwing-observer');
        await waitForPerformanceCase(()=>calls>=1,'callback例外初回');
        if(calls!==1)throw Error('callback例外初回');
        performance.mark('observer-after-error');await waitForPerformanceCase(()=>calls>=2,'callback例外後の継続');
        if(calls!==2)throw Error('callback例外後の継続');
        return 2;
    }finally{observer.disconnect();performance.clearMarks();performance.clearMeasures();}
}
