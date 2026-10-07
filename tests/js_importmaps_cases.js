/* Supplemental regression of the real native loader, not website acceptance.
   The dedicated js_importmaps_native.c fixture provides these module resources. */
globalThis.runImportMapCases=async function(){
    let count=0;
    function eq(a,b,label){count++;if(a!==b)throw Error('import maps: '+label+' '+String(a)+' != '+String(b));}
    eq(HTMLScriptElement.supports('classic'),true,'classic detection');
    eq(HTMLScriptElement.supports('module'),true,'module detection');
    eq(HTMLScriptElement.supports('importmap'),true,'importmap detection');
    eq(HTMLScriptElement.supports('IMPORTMAP'),false,'case-sensitive detection');
    eq(HTMLScriptElement.supports('speculationrules'),false,'unsupported detection');
    async function rejects(specifier,name,label){let error;try{await import(specifier);}catch(e){error=e;}eq(error&&error.name,name,label);}
    function add(map){const script=document.createElement('script');script.type='importmap';script.text=JSON.stringify(map);document.head.appendChild(script);return script;}
    add({imports:{
        shared:'/map/shared.mjs',alias:'/map/shared.mjs','pkg/':'/map/',
        'pkg/long/':'/map/sub/',flavor:'/map/shared.mjs',fallback:'/map/shared.mjs',
        blocked:null,'broken/':'/map/shared.mjs','number':42,
        './url-key.mjs':'/map/shared.mjs',
        '/map/blocked.mjs':null,'opaque:group/':'/map/'
    },scopes:{
        '/map/':{flavor:'/map/scoped.mjs',scopeBlock:null},
        '/map/sub/':{flavor:'/map/specific.mjs'},
        '/map/sub/main.mjs':{exactScope:'/map/scoped.mjs'},
        '/map/sub':{flavor:'/map/wrong.mjs'}
    }});
    const shared=await import('shared'),alias=await import('alias');
    eq(shared,alias,'exact aliases share namespace');eq(shared.value,'shared','exact mapping');eq(globalThis.__mapEvaluations,1,'one evaluation');
    eq((await import('./url-key.mjs')),shared,'relative URL key normalized');
    eq((await import('/dir/./url-key.mjs')),shared,'dot-normalized key');
    eq((await import('HTTP://FIXTURE.TEST:80/map/./shared.mjs')),shared,'canonical absolute URL cache');
    eq((await import('pkg/shared.mjs')),shared,'prefix mapping');
    const cycle=await import('pkg/a.mjs');eq(cycle.cycle(),'AB','bare prefix graph and cycle');
    const nested=await import('pkg/long/main.mjs');eq(nested.flavor,'specific','longest scope wins');
    eq(nested.fallback,'shared','scope fallthrough to imports');eq(nested.exact,'scoped','scope exact URL');
    eq(nested.cycle(),'AB','cycle namespace reused');eq(nested.meta,'http://fixture.test/map/sub/main.mjs','import meta fetched URL');
    eq(nested.current,null,'module currentScript null');
    await rejects('blocked','TypeError','null blocks bare');
    await rejects('/map/blocked.mjs','TypeError','null blocks URL fallback');
    await rejects('broken/shared.mjs','TypeError','invalid slash target blocks');
    await rejects('number','TypeError','invalid value blocks');
    await rejects('pkg/%2e%2e/private.mjs','TypeError','encoded prefix backtracking rejected');
    await rejects('pkg/../private.mjs','TypeError','literal prefix backtracking rejected');
    await rejects('opaque:group/child','TypeError','non-special URL not prefix mapped or fetched');
    await rejects('notMapped','TypeError','unmapped bare rejected');
    const graph=await import('/map/null-scope.mjs');eq(await graph.blocked(),'TypeError','scoped null does not fall through');
    eq(await graph.fallback(),'shared','parent scope then imports fallback');
    add({imports:{shared:'/map/wrong.mjs','pkg/':'/map/wrong/',notMapped:'/map/shared.mjs',fresh:'/map/scoped.mjs',
        '/map/shared.mjs':'/map/wrong.mjs'},scopes:{'/map/sub/':{flavor:'/map/wrong.mjs'}}});
    eq(await import('shared'),shared,'first exact mapping retained');eq(await import('pkg/shared.mjs'),shared,'first prefix mapping retained');
    eq(await import('/map/shared.mjs'),shared,'already-resolved URL protected');
    eq((await import('notMapped')),shared,'failed resolution may later be mapped');eq((await import('fresh')).value,'scoped','new nonconflicting rule');
    eq(await import('pkg/long/main.mjs'),nested,'module cache survives merge');
    const late=await import('/map/late.mjs');eq(late.flavor,'specific','late scope conflict ignored');
    const template=document.createElement('template');template.innerHTML='<script type="importmap">{"imports":{"templateOnly":"/map/shared.mjs"}}<\/script>';
    document.body.appendChild(template);await rejects('templateOnly','TypeError','template map inactive');template.remove();
    const inert=new DOMParser().parseFromString('<script type="importmap">{"imports":{"inertOnly":"/map/shared.mjs"}}<\/script>','text/html');
    document.head.appendChild(inert.querySelector('script'));await rejects('inertOnly','TypeError','parsed inert map stays inactive');
    const detached=document.createElement('script');detached.type='importmap';detached.text='{"imports":{"detachedOnly":"/map/shared.mjs"}}';
    await rejects('detachedOnly','TypeError','detached map inactive');document.head.appendChild(detached);eq(await import('detachedOnly'),shared,'connected map activates');
    detached.text='{"imports":{"mutatedOnly":"/map/shared.mjs"}}';await rejects('mutatedOnly','TypeError','already-started map text does not reactivate');
    await rejects('file:///data/secret.mjs','TypeError','HTTP document local-file policy preserved');
    await rejects('/map/mime.mjs','ReferenceError','non-JavaScript MIME remains rejected');
    await rejects('/map/cors.mjs','ReferenceError','host CORS refusal remains rejected');
    eq(typeof globalThis.importMapsBridge,'undefined','resolver remains private');
    return count;
};
