/* Supplemental checks for real Nocturne QuickJS bindings. */
(() => {
    const cases = [];
    const test = (name, f) => { try { if (!f()) throw new Error('false result'); check('clone-'+name,true); } catch(e) { check('clone-'+name,false); console.error(e); } };
    test('primitives',()=>{for(const x of [undefined,null,true,false,NaN,Infinity,-0,1n,'😀'])if(!Object.is(x,structuredClone(x)))return false;return true;});
    test('cycle',()=>{const a={};a.self=a;const b=structuredClone(a);return b!==a&&b.self===b;});
    test('sparse-array',()=>{const a=[];a.length=5;a[3]={x:2};a.foo='x';const b=structuredClone(a);return b.length===5&&!(0 in b)&&b[3].x===2&&b.foo==='x';});
    test('map-set-alias',()=>{const key={x:1},a=new Map();a.set(key,new Set([a,key]));const b=structuredClone(a),k=b.keys().next().value;return k!==key&&k.x===1&&b.get(k).has(b)&&b.get(k).has(k);});
    test('array-buffer-views',()=>{const b=new ArrayBuffer(32),v=new Uint16Array(b,4,4),d=new DataView(b,2,7);v[0]=345;const x=structuredClone({b,v,d});return x.b!==b&&x.v.buffer===x.b&&x.d.buffer===x.b&&x.v.byteOffset===4&&x.v.length===4&&x.v[0]===345&&x.d.byteLength===7;});
    test('buffer-source-spoof',()=>{const b=new Uint8Array([97]);Object.defineProperty(b,'buffer',{value:new ArrayBuffer(30)});return structuredClone(b)[0]===97;});
    test('dates-regexp',()=>{const a=/a+/g;a.lastIndex=5;const b=structuredClone({a,d:new Date(1234)});return b.a.source==='a+'&&b.a.flags==='g'&&b.a.lastIndex===0&&b.d.getTime()===1234;});
    test('error-cause',()=>{const a=new TypeError('bad');a.cause=a;const b=structuredClone(a);return b instanceof TypeError&&b.message==='bad'&&b.cause===b;});
    test('boxed',()=>{for(const x of [new Boolean(false),new Number(NaN),new String('猫'),Object(3n)])if(!Object.is(structuredClone(x).valueOf(),x.valueOf()))return false;return true;});
    test('own-keys-only',()=>{const a=Object.create({inherited:2});a.visible=1;Object.defineProperty(a,'hidden',{value:2});a[Symbol()]=3;const b=structuredClone(a);return Object.getPrototypeOf(b)===Object.prototype&&Object.keys(b).join()==='visible';});
    test('getter-exception',()=>{const e=new Error('getter');try{structuredClone({get a(){throw e;}});}catch(x){return x===e;}return false;});
    test('prototype-setter',()=>{const a={};Object.defineProperty(a,'__proto__',{value:{safe:true},enumerable:true});const b=structuredClone(a);return Object.getPrototypeOf(b)===Object.prototype&&Object.hasOwn(b,'__proto__')&&b.__proto__.safe;});
    for(const [name,value]of [['function',()=>{}],['symbol',Symbol()],['proxy',new Proxy({},{ownKeys(){throw new Error('trap');}})],['weakmap',new WeakMap()],['promise',Promise.resolve()],['dom',document.body]])
        test('reject-'+name,()=>{try{structuredClone(value);}catch(e){return e.name==='DataCloneError';}return false;});
    test('transfer',()=>{const a=new Uint8Array([1,2,3]),b=structuredClone({a},{transfer:[a.buffer]});return a.buffer.byteLength===0&&b.a.join()==='1,2,3';});
    test('duplicate-transfer',()=>{const a=new ArrayBuffer(4);try{structuredClone(a,{transfer:[a,a]});}catch(e){return e.name==='DataCloneError'&&a.byteLength===4;}return false;});
    test('transfer-atomic-error',()=>{const a=new ArrayBuffer(4);try{structuredClone({a,b:()=>{}},{transfer:[a]});}catch(e){return e.name==='DataCloneError'&&a.byteLength===4;}return false;});
    test('buffer-no-species',()=>{const a=new ArrayBuffer(4);new Uint8Array(a)[0]=19;Object.defineProperty(a,'constructor',{get(){throw Error('species');}});const b=structuredClone(a);return b!==a&&new Uint8Array(b)[0]===19;});
    test('transfer-late-snapshot',()=>{const a=new Uint8Array([1]);const b=structuredClone({a,get change(){a[0]=2;return 0;}},{transfer:[a.buffer]});return a.byteLength===0&&b.a[0]===2;});
    test('transfer-getter-once',()=>{let n=0;structuredClone(null,{get transfer(){n++;return[];}});return n===1;});
    test('transfer-iterable',()=>{const a=new ArrayBuffer(2);const b=structuredClone(a,{transfer:new Set([a])});return a.byteLength===0&&b.byteLength===2;});
    test('transfer-iterator-getter-once',()=>{let n=0;const t={get [Symbol.iterator](){n++;return function*(){};}};structuredClone(null,{transfer:t});return n===1;});
    test('transfer-invalid-sequence',()=>{for(const transfer of [{},null,'']){try{structuredClone(null,{transfer});return false;}catch(e){if(e.name!=='TypeError')return false;}}return true;});
    test('transfer-invalid-options',()=>{for(const options of [true,1,'']){try{structuredClone(null,options);return false;}catch(e){if(e.name!=='TypeError')return false;}}return structuredClone(null,null)===null;});
    test('transfer-invalid-element',()=>{try{structuredClone(null,{transfer:[1]});}catch(e){return e.name==='TypeError';}return false;});
    test('transfer-detached-after-getter',()=>{const a=new ArrayBuffer(2);try{structuredClone({get x(){a.transfer();return 1;}},{transfer:[a]});}catch(e){return e.name==='DataCloneError';}return false;});
    test('enumerable-snapshot',()=>{const a={get first(){Object.defineProperty(a,'second',{enumerable:false});return 1;},second:2};return structuredClone(a).second===2;});
    test('intrinsic-map-capture',()=>{const M=Map,get=M.prototype.get;const a=new M([['x',{v:1}]]);let b;try{M.prototype.get=()=>{throw Error('overridden get');};globalThis.Map=function(){throw Error('overridden constructor');};b=structuredClone(a);}finally{globalThis.Map=M;M.prototype.get=get;}return b.get('x').v===1;});
    test('intrinsic-object-capture',()=>{const keys=Object.keys,define=Object.defineProperty;let b;try{Object.keys=()=>{throw Error('overridden keys');};Object.defineProperty=()=>{throw Error('overridden define');};b=structuredClone({a:3});}finally{Object.keys=keys;Object.defineProperty=define;}return b.a===3;});
    if(Object.getOwnPropertyDescriptor(ArrayBuffer.prototype,'resizable')){
        test('resizable-buffer',()=>{const a=new ArrayBuffer(2,{maxByteLength:8});new Uint8Array(a)[0]=7;const b=structuredClone(a);b.resize(4);return b.resizable&&b.maxByteLength===8&&b.byteLength===4&&new Uint8Array(b)[0]===7&&a.byteLength===2;});
        test('resizable-transfer',()=>{const a=new ArrayBuffer(2,{maxByteLength:8});const b=structuredClone(a,{transfer:[a]});return a.byteLength===0&&b.resizable&&b.maxByteLength===8;});
        // Native metadata does not expose auto-length vs fixed-length view slots.
        test('resizable-view-explicit-limit',()=>{const a=new ArrayBuffer(2,{maxByteLength:8});try{structuredClone(new Uint8Array(a));}catch(e){return e.name==='DataCloneError'&&a.byteLength===2;}return false;});
    }
})();
