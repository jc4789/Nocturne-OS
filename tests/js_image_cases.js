/* Supplementary Image/DOM regressions for the real web_live/native decoder.
 * imagetest.c serves delayed SVG and invalid bytes through the public host API.
 * These tests are not substitutes for the public YouTube/DeepMind UI checks. */
async function runImageCases() {
    let checks=0;
    const equal=(a,b)=>{checks++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const illegal=fn=>{checks++;try{fn();}catch(e){if(e instanceof TypeError)return;throw e;}throw new Error('Expected TypeError');};
    const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
    const load=(image,url,expected='load')=>new Promise((resolve,reject)=>{
        const done=e=>{image.removeEventListener('load',done);image.removeEventListener('error',done);
            if(e.type===expected)resolve(e);else reject(new Error('Expected '+expected+' for '+url+', got '+e.type));};
        image.addEventListener('load',done);image.addEventListener('error',done);image.src=url;
    });
    const base='http://image.fixture/';
    const fresh=new Image(11,12);
    equal(Image.prototype,HTMLImageElement.prototype);equal(Image===HTMLImageElement,false);
    equal(fresh instanceof Image,true);equal(fresh instanceof HTMLImageElement,true);equal(fresh instanceof HTMLElement,true);
    equal(Object.getPrototypeOf(fresh),HTMLImageElement.prototype);equal(fresh.localName,'img');equal(fresh.parentNode,null);
    equal(fresh.ownerDocument,document);equal(fresh.width,11);equal(fresh.height,12);
    equal(fresh.getAttribute('width'),'11');equal(fresh.getAttribute('height'),'12');
    equal(fresh.naturalWidth,0);equal(fresh.naturalHeight,0);equal(fresh.complete,true);equal(fresh.currentSrc,'');equal(fresh.src,'');
    equal(Image(4,5).width,4);equal(new Image(undefined).getAttribute('width'),'0');
    equal(document.createElement('img') instanceof HTMLImageElement,true);
    equal(document.createElement('div') instanceof HTMLImageElement,false);
    equal(document.createElementNS('http://www.w3.org/2000/svg','img') instanceof HTMLImageElement,false);
    illegal(()=>new HTMLImageElement());illegal(()=>new(class extends HTMLImageElement {})());
    illegal(()=>Object.getOwnPropertyDescriptor(HTMLImageElement.prototype,'naturalWidth').get.call(document.createElement('div')));
    equal(await fresh.decode().then(()=>false,e=>e.name==='EncodingError'),true);

    let calls=0;fresh.onload=()=>{calls++;};
    const loaded=load(fresh,base+'one.svg');equal(fresh.complete,false);
    const decoded=fresh.decode();const event=await loaded;await decoded;
    equal(event.target,fresh);equal(event.isTrusted,true);equal(calls,1);equal(fresh.isConnected,false);
    equal(fresh.naturalWidth,2);equal(fresh.naturalHeight,3);equal(fresh.width,11);equal(fresh.height,12);
    equal(fresh.currentSrc,base+'one.svg');equal(fresh.complete,true);
    fresh.removeAttribute('width');fresh.removeAttribute('height');equal(fresh.width,2);equal(fresh.height,3);
    fresh.alt='native alt';equal(fresh.getAttribute('alt'),'native alt');
    const replacing=load(fresh,base+'slow.svg');equal(fresh.complete,false);
    equal(fresh.naturalWidth,2);equal(fresh.currentSrc,base+'one.svg');
    await replacing;equal(fresh.naturalWidth,4);equal(fresh.naturalHeight,6);equal(calls,2);
    await load(fresh,base+'slow.svg');equal(calls,3); // cached same-URL reselect is still an asynchronous load
    fresh.id='nocturne-image-identity';document.body.appendChild(fresh);
    equal(document.getElementById(fresh.id),fresh);fresh.remove();equal(fresh.parentNode,null);
    const clone=fresh.cloneNode(false);equal(clone instanceof HTMLImageElement,true);equal(clone!==fresh,true);
    await clone.decode();equal(clone.naturalWidth,4);

    const broken=new Image();const bad=load(broken,base+'broken.img','error');
    equal(await broken.decode().then(()=>false,e=>e.name==='EncodingError'),true);await bad;
    equal(broken.complete,true);equal(broken.naturalWidth,0);equal(broken.naturalHeight,0);
    await load(broken,base+'two.svg');equal(broken.naturalWidth,5);equal(broken.complete,true);
    const blank=load(broken,'','error');equal(broken.complete,true);await blank;equal(broken.currentSrc,'');
    broken.removeAttribute('src');equal(broken.complete,true);equal(broken.src,'');

    const changing=new Image();let changes=0;changing.onload=()=>{changes++;};
    changing.src=base+'old.svg';
    const invalidated=changing.decode().then(()=>false,e=>e.name==='EncodingError');
    await Promise.resolve();
    const latest=load(changing,base+'two.svg');equal(await invalidated,true);await latest;
    await wait(160);equal(changes,1);equal(changing.currentSrc,base+'two.svg');equal(changing.naturalWidth,5);
    const data=new Image();
    const svg='<svg xmlns="http://www.w3.org/2000/svg" width="7" height="9"><rect width="7" height="9" fill="red"/></svg>';
    await load(data,'data:image/svg+xml,'+encodeURIComponent(svg));await data.decode();
    equal(data.naturalWidth,7);equal(data.naturalHeight,9);equal(data.complete,true);
    const empty=new Image();const rejected=HTMLImageElement.prototype.decode.call({}).then(()=>false,e=>e instanceof TypeError);
    equal(await rejected,true);equal(await empty.decode().then(()=>false,e=>e.name==='EncodingError'),true);

    // Template inert image boundary check:
    const template=document.createElement('template');
    template.innerHTML='<img id="inert-img" src="'+base+'two.svg">';
    document.body.appendChild(template);
    const inertImg=template.content.querySelector('#inert-img');
    equal(inertImg.ownerDocument!==document,true);
    equal(inertImg.naturalWidth,0);
    equal(inertImg.complete,true);
    let inertEventFired=false;
    inertImg.onload=inertImg.onerror=()=>{inertEventFired=true;};
    equal(inertEventFired,false);
    template.remove();

    return checks;
}

function runFormInterfaceCases() {
    let checks=0;
    const equal=(a,b)=>{checks++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const rejects=(fn,name)=>{checks++;try{fn();}catch(e){if(e.name===name && (name!=='NotFoundError'||e instanceof DOMException))return;throw e;}throw new Error('Expected '+name);};
    const form=document.createElement('form');form.id='nocturne-form-owner';document.body.appendChild(form);
    try {
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'form'),undefined);
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'async'),undefined);
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'defer'),undefined);
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'submit'),undefined);
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'requestSubmit'),undefined);
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'elements'),undefined);

        equal(form instanceof HTMLFormElement,true);
        equal(Object.getPrototypeOf(form),HTMLFormElement.prototype);
        equal(Object.getPrototypeOf(HTMLFormElement.prototype),HTMLElement.prototype);
        checks++;try{new HTMLFormElement();throw new Error('Expected illegal constructor');}catch(e){if(!(e instanceof TypeError))throw e;}

        // YouTube Polymer prototype async isolation test:
        const polymerBase=Object.create(HTMLElement.prototype);
        polymerBase.async=function(cb){return typeof cb==='function'?cb():42;};
        equal(polymerBase.async(()=>100),100);

        // HTMLScriptElement tests:
        const script=document.createElement('script');
        equal(script instanceof HTMLScriptElement,true);
        equal(Object.getPrototypeOf(script),HTMLScriptElement.prototype);
        equal(Object.getPrototypeOf(HTMLScriptElement.prototype),HTMLElement.prototype);
        checks++;try{new HTMLScriptElement();throw new Error('Expected illegal constructor');}catch(e){if(!(e instanceof TypeError))throw e;}
        equal(script.async,true);
        script.async=false;
        equal(script.async,false);
        equal(script.hasAttribute('async'),false);
        script.async=true;
        equal(script.async,true);
        equal(script.hasAttribute('async'),true);
        script.defer=true;
        equal(script.defer,true);
        equal(script.hasAttribute('defer'),true);
        script.defer=false;
        equal(script.defer,false);
        equal(script.hasAttribute('defer'),false);
        script.src='module.js';
        equal(script.src,new URL('module.js',document.baseURI).href);
        script.type='text/javascript';
        equal(script.type,'text/javascript');
        script.noModule=true;
        equal(script.noModule,true);
        script.text='var a = 1;';
        equal(script.text,'var a = 1;');
        equal(script.textContent,'var a = 1;');
        script.crossOrigin='anonymous';
        equal(script.crossOrigin,'anonymous');
        script.setAttribute('crossorigin','');
        equal(script.crossOrigin,'anonymous');
        script.crossOrigin=null;
        equal(script.crossOrigin,null);

        // HTMLFormElement properties & methods:
        const formInput=document.createElement('input');
        formInput.name='account';
        formInput.defaultValue='default_val';
        formInput.value='default_val';
        form.appendChild(formInput);
        equal(formInput.defaultValue,'default_val');
        equal(form.elements.length,1);
        equal(form.length,1);
        equal(form.elements[0],formInput);
        equal(form.elements.namedItem('account'),formInput);
        equal(form.elements['account'],formInput);
        const liveControls=form.elements;equal(liveControls,form.elements);
        const imageControl=document.createElement('input');imageControl.type='image';form.appendChild(imageControl);
        equal(liveControls.length,1);imageControl.type='text';equal(liveControls.length,2);imageControl.remove();
        equal(liveControls.length,1);
        equal(form.action,document.URL);form.action='';equal(form.action,document.URL);
        form.action='submit.html';
        equal(form.action,new URL('submit.html',document.baseURI).href);
        equal(form.method,'get');
        form.method='POST';
        equal(form.method,'post');
        form.method='INVALID';
        equal(form.method,'get');
        form.name='mainForm';
        equal(form.name,'mainForm');
        form.target='_blank';
        equal(form.target,'_blank');
        equal(form.enctype,'application/x-www-form-urlencoded');
        form.enctype='multipart/form-data';
        equal(form.enctype,'multipart/form-data');
        equal(form.encoding,'multipart/form-data');
        form.encoding='text/plain';
        equal(form.enctype,'text/plain');
        form.enctype='INVALID';
        equal(form.enctype,'application/x-www-form-urlencoded');
        equal(form.noValidate,false);
        form.noValidate=true;
        equal(form.noValidate,true);
        equal(form.hasAttribute('novalidate'),true);

        // form reset:
        const formSelect=document.createElement('select');
        const opt1=document.createElement('option');opt1.value='1';
        const opt2=document.createElement('option');opt2.value='2';
        formSelect.append(opt1,opt2);
        form.appendChild(formSelect);
        formSelect.selectedIndex=1;
        equal(formSelect.selectedIndex,1);
        formInput.value='changed_val';
        form.reset();
        equal(formInput.value,'default_val');
        equal(formSelect.selectedIndex,0);
        formInput.defaultValue='next_default';equal(formInput.value,'next_default');
        const checkbox=document.createElement('input');checkbox.type='checkbox';checkbox.defaultChecked=true;
        checkbox.checked=false;form.appendChild(checkbox);form.reset();equal(checkbox.checked,true);
        checkbox.defaultChecked=false;equal(checkbox.checked,false);checkbox.remove();
        const radioOne=document.createElement('input'),radioTwo=document.createElement('input');
        for(const radio of [radioOne,radioTwo]){radio.type='radio';radio.name='reset-group';radio.defaultChecked=true;form.appendChild(radio);}
        form.reset();equal(radioOne.checked,false);equal(radioTwo.checked,true);
        equal(radioOne.defaultChecked,true);equal(radioTwo.defaultChecked,true);radioOne.remove();radioTwo.remove();
        let resetCalls=0;
        const reenter=()=>{resetCalls++;if(resetCalls===1)form.reset();};
        form.addEventListener('reset',reenter);form.reset();equal(resetCalls,1);form.removeEventListener('reset',reenter);
        const cancelReset=e=>e.preventDefault();form.addEventListener('reset',cancelReset);
        formInput.value='keep';form.reset();equal(formInput.value,'keep');form.removeEventListener('reset',cancelReset);
        form.reset();equal(formInput.value,'next_default');

        const textArea=document.createElement('textarea');textArea.defaultValue='initial';form.appendChild(textArea);
        equal(textArea.value,'initial');textArea.firstChild.data='child';equal(textArea.value,'child');
        textArea.replaceChildren(document.createTextNode('replaced'));equal(textArea.value,'replaced');
        textArea.value='edited';textArea.firstChild.data='default';equal(textArea.value,'edited');
        form.reset();equal(textArea.value,'default');textArea.defaultValue='after-reset';equal(textArea.value,'after-reset');
        const otherArea=document.createElement('textarea');form.appendChild(otherArea);equal(otherArea.value,'');
        otherArea.appendChild(textArea.firstChild);equal(textArea.value,'');equal(otherArea.value,'after-reset');
        textArea.remove();otherArea.remove();
        const text=document.createTextNode('x\ud83d\ude00'),comment=document.createComment('original');
        equal(text.data,'x\ud83d\ude00');equal(text.length,3);text.data=null;equal(text.data,'');equal(text.length,0);
        text.data='updated';equal(text.nodeValue,'updated');text.nodeValue='nodeValue';equal(text.data,'nodeValue');
        comment.data='c\ud83d\ude00';equal(comment.nodeValue,'c\ud83d\ude00');equal(comment.length,3);
        rejects(()=>{text.data=Symbol();},'TypeError');equal(text.data,'nodeValue');
        rejects(()=>Object.getOwnPropertyDescriptor(CharacterData.prototype,'data').get.call(form),'TypeError');
        rejects(()=>Object.getOwnPropertyDescriptor(CharacterData.prototype,'data').set.call({},'invalid'),'TypeError');
        rejects(()=>Object.getOwnPropertyDescriptor(CharacterData.prototype,'length').get.call(form),'TypeError');

        // disconnected form:
        const disconnForm=document.createElement('form');
        const disconnInput=document.createElement('input');
        disconnInput.name='disconn';
        disconnForm.appendChild(disconnInput);
        equal(disconnForm.elements.length,1);
        equal(disconnForm.elements['disconn'],disconnInput);

        // requestSubmit validation:
        const foreignButton=document.createElement('button');
        rejects(()=>form.requestSubmit(foreignButton),'NotFoundError');
        rejects(()=>form.requestSubmit(formInput),'TypeError');
        rejects(()=>form.requestSubmit({form}),'TypeError');
        rejects(()=>HTMLFormElement.prototype.reset.call(document.createElement('div')),'TypeError');
        const validButton=document.createElement('button');form.appendChild(validButton);
        let submitEvents=0;const cancelSubmit=e=>{submitEvents++;equal(e.submitter,validButton);e.preventDefault();};
        form.addEventListener('submit',cancelSubmit);form.requestSubmit(validButton);equal(submitEvents,1);
        form.removeEventListener('submit',cancelSubmit);validButton.remove();

        const isolated=document.implementation.createHTMLDocument('form-owner');
        const isolatedForm=isolated.createElement('form');isolatedForm.id='same-owner';isolated.body.appendChild(isolatedForm);
        const isolatedInput=isolated.createElement('input');isolatedInput.setAttribute('form','same-owner');isolated.body.appendChild(isolatedInput);
        equal(isolatedInput.form,isolatedForm);equal(isolatedForm.elements[0],isolatedInput);
        equal(isolatedForm.action,isolated.URL);isolatedForm.action='';equal(isolatedForm.action,isolated.URL);
        isolatedInput.defaultValue='owner-default';isolatedInput.value='edited';isolatedForm.reset();equal(isolatedInput.value,'owner-default');
        isolatedInput.defaultValue='owner-next';equal(isolatedInput.value,'owner-next');

        // HTMLAnchorElement & HTMLAreaElement URL reflection:
        const a=document.createElement('a');
        equal(a instanceof HTMLAnchorElement,true);
        equal(Object.getPrototypeOf(a),HTMLAnchorElement.prototype);
        equal(Object.getPrototypeOf(HTMLAnchorElement.prototype),HTMLElement.prototype);
        checks++;try{new HTMLAnchorElement();throw new Error('Expected illegal constructor');}catch(e){if(!(e instanceof TypeError))throw e;}

        const emptyA=document.createElement('a');
        equal(emptyA.href,'');
        equal(emptyA.pathname,'');
        equal(emptyA.search,'');
        equal(emptyA.hash,'');
        equal(emptyA.protocol,':');
        equal(emptyA.host,'');
        equal(emptyA.hostname,'');
        equal(emptyA.port,'');
        equal(emptyA.origin,'');

        a.href='https://baike.baidu.com:443/item/%E4%B8%89%E4%BD%93/5739303?fr=aladdin#target';
        equal(a.href,'https://baike.baidu.com/item/%E4%B8%89%E4%BD%93/5739303?fr=aladdin#target');
        equal(a.origin,'https://baike.baidu.com');
        equal(a.protocol,'https:');
        equal(a.host,'baike.baidu.com');
        equal(a.hostname,'baike.baidu.com');
        equal(a.port,'');
        equal(a.pathname,'/item/%E4%B8%89%E4%BD%93/5739303');
        equal(a.search,'?fr=aladdin');
        equal(a.hash,'#target');
        equal(a.toString(),a.href);

        a.pathname='/item/different';
        equal(a.pathname,'/item/different');
        equal(a.href,'https://baike.baidu.com/item/different?fr=aladdin#target');

        a.search='?fr=new';
        equal(a.search,'?fr=new');
        equal(a.href,'https://baike.baidu.com/item/different?fr=new#target');

        a.hash='#new_section';
        equal(a.hash,'#new_section');
        equal(a.href,'https://baike.baidu.com/item/different?fr=new#new_section');

        a.hostname='example.org';
        equal(a.hostname,'example.org');
        equal(a.host,'example.org');

        a.port='8080';
        equal(a.port,'8080');
        equal(a.host,'example.org:8080');

        a.protocol='http:';
        equal(a.protocol,'http:');

        a.text='Baidu Baike';
        equal(a.text,'Baidu Baike');
        equal(a.textContent,'Baidu Baike');

        // HTMLAreaElement reflection:
        const area=document.createElement('area');
        equal(area instanceof HTMLAreaElement,true);
        equal(Object.getPrototypeOf(area),HTMLAreaElement.prototype);
        equal(Object.getPrototypeOf(HTMLAreaElement.prototype),HTMLElement.prototype);
        checks++;try{new HTMLAreaElement();throw new Error('Expected illegal constructor');}catch(e){if(!(e instanceof TypeError))throw e;}
        area.href='https://example.com/map/area?zoom=1#pin';
        equal(area.pathname,'/map/area');
        equal(area.search,'?zoom=1');
        equal(area.hash,'#pin');
        equal(area.host,'example.com');

        const savedURL=URL,savedPath=Object.getOwnPropertyDescriptor(URL.prototype,'pathname');
        const urlExpectedScript=script.src,urlExpectedAction=form.action;
        try {
            globalThis.URL=function(){throw new Error('public URL must not be called');};
            Object.defineProperty(savedURL.prototype,'pathname',{configurable:true,get(){return 'poison';},set(){throw new Error('public URL.prototype must not be called');}});
            a.pathname='/private';equal(a.pathname,'/private');equal(a.href.startsWith('http://example.org:8080/private'),true);
            equal(script.src,urlExpectedScript);equal(form.action,urlExpectedAction);
        }finally{globalThis.URL=savedURL;Object.defineProperty(savedURL.prototype,'pathname',savedPath);}
        const marker=new Error('conversion');let sameError=false;
        try{a.pathname={toString(){throw marker;}};}catch(e){sameError=e===marker;}equal(sameError,true);
        rejects(()=>{emptyA.pathname={toString(){throw marker;}};},'Error');
        rejects(()=>{a.href=Symbol('href');},'TypeError');
        rejects(()=>Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype,'pathname').get.call(document.createElement('div')),'TypeError');
        rejects(()=>Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype,'href').get.call(area),'TypeError');
        rejects(()=>Object.getOwnPropertyDescriptor(HTMLScriptElement.prototype,'src').get.call(a),'TypeError');
        a.href='x\ud800\udc00y\ud800';equal(a.getAttribute('href'),'x\ud800\udc00y\ufffd');
        a.href='https://first.example/a';
        a.pathname={toString(){a.href='https://second.example/b';return '/reentered';}};
        equal(a.href,'https://second.example/reentered');

        const ordinary=document.createElement('section');ordinary.form=form;equal(ordinary.form,form);
        const definitions=[['input',HTMLInputElement],['button',HTMLButtonElement],['select',HTMLSelectElement],
            ['textarea',HTMLTextAreaElement],['fieldset',HTMLFieldSetElement],['object',HTMLObjectElement],['output',HTMLOutputElement]];
        for(const [tag,C] of definitions){
            const node=document.createElement(tag);equal(Object.getPrototypeOf(node),C.prototype);equal(node instanceof HTMLElement,true);
            equal(node.form,null);form.appendChild(node);equal(node.form,form);
            node.remove();node.setAttribute('form',form.id);document.body.appendChild(node);equal(node.form,form);node.remove();
            checks++;try{new C();throw new Error('Expected illegal constructor');}catch(e){if(!(e instanceof TypeError))throw e;}
        }
        const select=document.createElement('select'),option=document.createElement('option');
        select.setAttribute('form',form.id);select.appendChild(option);document.body.appendChild(select);
        equal(option instanceof HTMLOptionElement,true);equal(option.form,form);select.remove();
        const name='nocturne-form-expando-'+Date.now();
        class Newsletter extends HTMLElement {connectedCallback(){this.form=this.querySelector('form');}}
        customElements.define(name,Newsletter);const custom=document.createElement(name);custom.innerHTML='<form></form>';
        document.body.appendChild(custom);equal(custom.form,custom.firstChild);custom.remove();
        return checks;
    }finally{form.remove();}
}
