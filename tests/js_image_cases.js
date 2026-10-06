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
    return checks;
}

function runFormInterfaceCases() {
    let checks=0;
    const equal=(a,b)=>{checks++;if(!Object.is(a,b))throw new Error(String(a)+' != '+String(b));};
    const form=document.createElement('form');form.id='nocturne-form-owner';document.body.appendChild(form);
    try {
        equal(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'form'),undefined);
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
