/* Shared HTML/SVG checks; supplemental runner models attributes, not native DOM. */
function runClassTokenListCases(tokenNode,equal,illegal) {
    const tokens=tokenNode.classList;
    const error=(fn,name,label)=>{let thrown;try{fn();}catch(e){thrown=e;}equal(thrown?.name,name,label);};
    tokenNode.setAttribute('class',' one\tone two ');
    Object.defineProperty(tokenNode,'className',{configurable:true,get(){throw new Error('classList used public className');},set(){throw new Error('classList wrote public className');}});
    equal(tokens.value,' one\tone two ');equal(tokens.length,2);equal(tokens.contains('one'),true);equal(tokens.item(1),'two');
    tokens.add('three');equal(tokenNode.getAttribute('class'),'one two three');
    tokens.remove('one');equal(tokenNode.getAttribute('class'),'two three');
    equal(tokens.toggle('four'),true);equal(tokenNode.getAttribute('class'),'two three four');
    equal(tokens.toggle('four'),false);equal(tokenNode.getAttribute('class'),'two three');
    equal(tokens.replace('two','five'),true);equal(tokenNode.getAttribute('class'),'five three');
    tokens.value=' z  z\ty ';equal(tokenNode.getAttribute('class'),' z  z\ty ');equal(tokens.length,2);equal(String(tokens),' z  z\ty ');
    equal([...tokens].join('|'),'z|y');
    tokenNode.setAttribute('class','external');equal(tokens.contains('external'),true);
    tokenNode.removeAttribute('class');equal(tokens.value,'');equal(tokens.length,0);
    tokens.add('after');equal(tokenNode.getAttribute('class'),'after');
    tokens.value='';equal(tokenNode.getAttribute('class'),'');equal(tokens.contains('after'),false);

    // Ordered-set replace retains the first old/new position and removes duplicates.
    for(const [value,expected] of [['a b c','c b'],['c b a','c b'],['a b a c c','c b']]){
        tokens.value=value;equal(tokens.replace('a','c'),true);equal(tokens.value,expected,'replace ordered set');
    }
    tokens.value=' a a b ';equal(tokens.replace('a','a'),true);equal(tokens.value,'a b','replace same normalizes');
    tokens.value=' a a b ';equal(tokens.replace('absent','b'),false);equal(tokens.value,' a a b ','replace absent does not write');
    equal(tokens.contains(''),false);equal(tokens.contains('a b'),false);
    tokens.value='a\u00a0b c\u000bd\td';equal(tokens.length,3,'ASCII whitespace only');
    equal(tokens.contains('a\u00a0b'),true);equal(tokens.contains('c\u000bd'),true);
    tokens.add('e\u00a0f');equal(tokens.contains('e\u00a0f'),true,'non-ASCII space accepted');
    tokens.value='keep';error(()=>tokens.add('good','bad token'),'InvalidCharacterError','add validates all');equal(tokens.value,'keep');
    error(()=>tokens.remove('keep',''),'SyntaxError','remove validates all');equal(tokens.value,'keep');
    error(()=>tokens.replace('bad token',''),'SyntaxError','replace empty precedes whitespace');equal(tokens.value,'keep');
    error(()=>tokens.toggle('bad\ntoken'),'InvalidCharacterError','toggle token validation');equal(tokens.value,'keep');
    tokens.value=' keep keep ';equal(tokens.toggle('keep',true),true);equal(tokens.value,' keep keep ','forced present no-op');
    equal(tokens.toggle('absent',false),false);equal(tokens.value,' keep keep ','forced absent no-op');
    equal(tokens.toggle('keep',undefined),false);equal(tokens.value,'','undefined optional force is omitted');
    tokenNode.removeAttribute('class');tokens.add();tokens.remove();equal(tokenNode.getAttribute('class'),null,'empty updates keep absent attribute');
    equal(tokens.toggle('absent',false),false);equal(tokenNode.getAttribute('class'),null);
    tokens.value=' a a b ';tokens.add();equal(tokens.value,'a b','empty add normalizes existing attribute');
    tokens.value=' a a b ';tokens.remove();equal(tokens.value,'a b','empty remove normalizes existing attribute');
    equal(tokens.item(4294967297),'b','unsigned index conversion');equal(tokens.item(-1),null);equal(tokens.item(undefined),'a');
    illegal(()=>tokens.item(),'required index');illegal(()=>tokens.item(Symbol('index')),'symbol index');
    illegal(()=>tokens.contains(),'required token');illegal(()=>tokens.replace('a'),'required replacement');
    illegal(()=>tokens.add(Symbol('token')),'symbol token');illegal(()=>{tokens.value=Symbol('value');},'symbol value');equal(tokens.value,'a b');
    illegal(()=>tokens.supports('a'),'class has no supported vocabulary');illegal(()=>tokens.supports(),'required supports token');
    const proto=Object.getPrototypeOf(tokens), descriptor=Object.getOwnPropertyDescriptor(proto,'value');
    illegal(()=>proto.add.call({}),'token list brand');illegal(()=>descriptor.get.call({}),'value get brand');illegal(()=>descriptor.set.call({},'x'),'value set brand');
    tokens.node={className:'spoof'};equal(tokens.contains('a'),true,'public node property cannot retarget');
    let conversions=0;
    error(()=>tokens.add({toString(){conversions++;return 'bad token';}},{toString(){conversions++;tokenNode.setAttribute('class','converted');return 'other';}}),'InvalidCharacterError','all conversions before validation');
    equal(conversions,2);equal(tokens.value,'converted','only conversion side effects survive validation');
    tokens.add({toString(){tokenNode.setAttribute('class','during');return 'after';}});equal(tokens.value,'during after','conversion then live snapshot');
    tokens.value=undefined;equal(tokens.value,'undefined','DOMString undefined');
}

/* Native DOM contracts. Caller supplies the real Nocturne document. */
function runSVGDOMCases() {
    let checks=0;
    const equal=(a,b,label='')=>{checks++;if(!Object.is(a,b))throw new Error('SVG DOM '+label+': '+String(a)+' != '+String(b));};
    const TypeErrorImpl=TypeError;
    const illegal=(fn,label)=>{checks++;try{fn();}catch(e){if(e instanceof TypeErrorImpl)return;throw e;}throw new Error('SVG DOM expected TypeError: '+label);};
    const NS='http://www.w3.org/2000/svg', HTML='http://www.w3.org/1999/xhtml', MATH='http://www.w3.org/1998/Math/MathML';
    const SVG=SVGElement, Root=SVGSVGElement, Animated=SVGAnimatedString;
    const getClass=Object.getOwnPropertyDescriptor(SVG.prototype,'className').get;
    const getOwner=Object.getOwnPropertyDescriptor(SVG.prototype,'ownerSVGElement').get;
    const getViewport=Object.getOwnPropertyDescriptor(SVG.prototype,'viewportElement').get;
    const base=Object.getOwnPropertyDescriptor(Animated.prototype,'baseVal');
    const anim=Object.getOwnPropertyDescriptor(Animated.prototype,'animVal').get;
    const getID=Root.prototype.getElementById;
    illegal(()=>new SVG(),'constructor');illegal(()=>new Root(),'root constructor');illegal(()=>new Animated(),'animated constructor');
    const root=document.createElementNS(NS,'svg'), group=document.createElementNS(NS,'g'), rect=document.createElementNS(NS,'rect');
    equal(Object.getPrototypeOf(root),Root.prototype,'native root prototype');equal(root instanceof SVG,true);equal(root instanceof Element,true);equal(root instanceof HTMLElement,false);
    equal(Object.getPrototypeOf(group),SVG.prototype,'native generic prototype');equal(group instanceof Root,false);equal(group.namespaceURI,NS);equal(group.localName,'g');equal(group.tagName,'g');
    equal(root.ownerSVGElement,null);equal(root.viewportElement,null);equal(group.ownerSVGElement,null);
    root.appendChild(group);group.appendChild(rect);equal(group.ownerSVGElement,root);equal(rect.ownerSVGElement,root);equal(rect.viewportElement,root);
    const nested=document.createElementNS(NS,'svg');group.appendChild(nested);nested.appendChild(rect);
    equal(nested.ownerSVGElement,root);equal(rect.ownerSVGElement,nested);equal(rect.viewportElement,nested);
    rect.remove();equal(rect.namespaceURI,NS,'detached namespace');equal(rect.ownerSVGElement,null);equal(rect instanceof SVG,true);
    const html=document.createElement('svg');equal(html.namespaceURI,HTML);equal(html instanceof SVG,false,'HTML svg not SVG');equal(html instanceof HTMLElement,true);
    const upper=document.createElementNS(NS,'SVG');equal(upper.localName,'SVG');equal(upper instanceof SVG,true);equal(upper instanceof Root,false,'createElementNS preserves case');
    for(const wrong of [{},html,document,document.createTextNode('x')]){
        illegal(()=>getClass.call(wrong),'class brand');illegal(()=>getOwner.call(wrong),'owner brand');illegal(()=>getViewport.call(wrong),'viewport brand');illegal(()=>getID.call(wrong,'x'),'root brand');
    }
    illegal(()=>getID.call(group,'x'),'generic SVG not root');illegal(()=>getID.call(root),'missing id');
    const className=root.className;equal(className,root.className,'SameObject');equal(className instanceof Animated,true);equal(Object.prototype.toString.call(className),'[object SVGAnimatedString]');
    equal(className.baseVal,'');equal(className.animVal,'');root.setAttribute('class','one');equal(className.baseVal,'one');equal(className.animVal,'one');
    className.baseVal='two three';equal(root.getAttribute('class'),'two three');equal(root.classList.contains('two'),true);equal(className.animVal,'two three');
    root.classList.add('four');equal(className.baseVal,'two three four');root.removeAttribute('class');equal(className.baseVal,'');
    // classList reflects the native class attribute, not HTML's string className
    // or SVG's SVGAnimatedString, and must ignore a replaced public className.
    for(const tokenNode of [document.createElement('div'),document.createElementNS(NS,'g')]){
        runClassTokenListCases(tokenNode,equal,illegal);
    }
    className.baseVal={toString(){root.setAttribute('class','during');return 'after';}};equal(root.getAttribute('class'),'after','conversion reentry then write');
    illegal(()=>{className.baseVal=Symbol('x');},'symbol conversion');equal(className.baseVal,'after');
    for(const wrong of [{},Object.create(Animated.prototype),root]){
        illegal(()=>base.get.call(wrong),'animated get brand');illegal(()=>base.set.call(wrong,'x'),'animated set brand');illegal(()=>anim.call(wrong),'anim brand');
    }
    const classDescriptor=Object.getOwnPropertyDescriptor(SVG.prototype,'className');equal(classDescriptor.set,undefined,'readonly className');
    root.setAttribute('viewBox','0 0 10 10');root.setAttribute('viewbox','different');equal(root.getAttribute('viewBox'),'0 0 10 10');equal(root.getAttribute('viewbox'),'different');root.removeAttribute('viewbox');equal(root.getAttribute('viewBox'),'0 0 10 10');
    const copy=root.cloneNode(true);equal(copy.namespaceURI,NS);equal(Object.getPrototypeOf(copy),Root.prototype);equal(copy.className===root.className,false);equal(copy.className.baseVal,'after');
    const other=document.implementation.createHTMLDocument();other.adoptNode(copy);equal(copy.ownerDocument,other);equal(copy.namespaceURI,NS);equal(copy instanceof Root,true);
    const imported=document.importNode(copy,true);equal(imported.namespaceURI,NS);equal(imported instanceof Root,true);equal(imported.ownerDocument,document);
    group.id='svg-child-id';root.id='svg-root-id';equal(root.getElementById('svg-child-id'),group);equal(root.getElementById('svg-root-id'),null,'ID descendants only');equal(root.getElementById(''),null);
    const holder=document.createElement('div');
    holder.innerHTML='<SVG id="parsed-svg" VIEWBOX="0 0 12 12"><lineargradient id="gradient"></lineargradient><foreignobject id="foreign"><div id="html-child"></div><svg id="nested-svg"></svg></foreignobject><title><span id="title-html"></span></title></SVG><math><mrow id="math-row"></mrow><mtext><b id="math-html"></b></mtext><annotation-xml encoding="text/html"><div id="annotation-html"></div></annotation-xml></math>';
    const parsed=holder.querySelector('#parsed-svg'), gradient=holder.querySelector('#gradient'), foreign=holder.querySelector('#foreign');
    equal(parsed instanceof Root,true);equal(parsed.localName,'svg');equal(parsed.getAttribute('viewBox'),'0 0 12 12');equal(parsed.getAttribute('viewbox'),null,'parser SVG attr adjustment');
    equal(gradient.localName,'linearGradient');equal(gradient.namespaceURI,NS);equal(foreign.localName,'foreignObject');equal(foreign.namespaceURI,NS);
    equal(holder.querySelector('#html-child').namespaceURI,HTML,'foreignObject HTML integration');equal(holder.querySelector('#title-html').namespaceURI,HTML,'title HTML integration');
    equal(holder.querySelector('#nested-svg').namespaceURI,NS);equal(holder.querySelector('#nested-svg') instanceof Root,true);
    const math=holder.querySelector('#math-row');equal(math.namespaceURI,MATH);equal(math instanceof SVG,false);math.remove();equal(math.namespaceURI,MATH,'MathML detach retained');equal(math.cloneNode(true).namespaceURI,MATH);document.adoptNode(math);equal(math.namespaceURI,MATH);
    equal(holder.querySelector('#math-html').namespaceURI,HTML,'MathML text integration');equal(holder.querySelector('#annotation-html').namespaceURI,HTML,'MathML annotation integration');
    group.innerHTML='<rect id="fragment-rect"/><foreignObject><p id="fragment-html"></p></foreignObject>';
    equal(group.firstChild.namespaceURI,NS,'SVG fragment namespace');equal(group.querySelector('#fragment-html').namespaceURI,HTML,'SVG fragment HTML integration');
    const template=document.createElement('template');template.innerHTML='<svg><g id="inert-svg"></g></svg>';const inert=template.content.firstChild;
    equal(inert.namespaceURI,NS);equal(inert instanceof Root,true);equal(inert.firstChild.ownerSVGElement,inert);document.adoptNode(inert);equal(inert.namespaceURI,NS);equal(inert.firstChild.namespaceURI,NS);
    let clicks=0;root.onclick=function(e){equal(this,root);equal(e.target,root);clicks++;};root.dispatchEvent(new Event('click'));equal(clicks,1,'SVG event handler');root.onclick=null;
    const saved={SVGElement:globalThis.SVGElement,SVGSVGElement:globalThis.SVGSVGElement,SVGAnimatedString:globalThis.SVGAnimatedString};
    const savedGet=root.getAttribute,savedSet=root.setAttribute;
    try{
        globalThis.SVGElement=function Fake(){};globalThis.SVGSVGElement=function FakeRoot(){};globalThis.SVGAnimatedString=function FakeAnimated(){};
        root.getAttribute=()=>{throw new Error('public getAttribute called')};root.setAttribute=()=>{throw new Error('public setAttribute called')};
        equal(getClass.call(root),className,'private cache after public replacement');base.set.call(className,'native');equal(base.get.call(className),'native');equal(anim.call(className),'native');
        const fresh=document.createElementNS(NS,'svg');equal(Object.getPrototypeOf(fresh),Root.prototype,'private prototype after constructor replacement');equal(fresh instanceof SVG,true);
        Object.defineProperty(html,'namespaceURI',{configurable:true,value:NS});illegal(()=>getOwner.call(html),'public namespace spoof');delete html.namespaceURI;
    }finally{Object.assign(globalThis,saved);root.getAttribute=savedGet;root.setAttribute=savedSet;}
    return checks;
}
