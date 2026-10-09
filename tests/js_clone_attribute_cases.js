/* Supplemental native clone/import checks; real Reddit remains acceptance. */
async function runCloneAttributeCases(){
    let count=0;
    const equal=(name,a,b)=>{count++;check('clone-attrs-'+name,Object.is(a,b));};
    const source=document.createElement('section');
    source.id='clone-source';source.className='alpha Beta alpha';
    for(let i=0;i<64;i++)source.setAttribute('data-value-'+i,'value-'+i);
    source.setAttribute('hidden','');
    source.setAttributeNS('urn:One','P:MiXeD','one');
    source.setAttributeNS('urn:Two','P:MiXeD','two');
    source.setAttributeNS('urn:NotCache','id','foreign-id');
    source.setAttributeNS('urn:NotCache','class','foreign-class');
    const originals=Array.from(source.attributes);
    const observer=new MutationObserver(()=>{});observer.observe(source,{attributes:true,attributeOldValue:true,subtree:true});
    const copy=source.cloneNode(false);
    equal('count',copy.attributes.length,source.attributes.length);
    equal('order',copy.getAttributeNames().join('|'),source.getAttributeNames().join('|'));
    for(let i=0;i<originals.length;i++){
        const a=originals[i],b=copy.attributes[i];
        for(const p of ['name','value','namespaceURI','prefix','localName'])equal('metadata-'+i+'-'+p,b[p],a[p]);
        equal('new-identity-'+i,b===a,false);equal('owner-element-'+i,b.ownerElement,copy);equal('owner-document-'+i,b.ownerDocument,document);
    }
    equal('id-cache',copy.id,'clone-source');equal('class-cache',copy.classList.contains('Beta'),true);
    equal('namespace-cache-boundary',copy.classList.contains('foreign-class'),false);
    equal('boolean-empty',copy.getAttribute('hidden'),'');
    copy.setAttribute('data-value-0','copy-only');equal('mutation-isolation',source.getAttribute('data-value-0'),'value-0');
    equal('no-source-mutation',observer.takeRecords().length,0);observer.disconnect();
    const inert=document.implementation.createHTMLDocument('clone-owner');
    const imported=inert.importNode(copy,true);equal('import-owner',imported.ownerDocument,inert);equal('import-ns',imported.getAttributeNS('urn:One','MiXeD'),'one');
    equal('import-attr-owner',imported.getAttributeNodeNS('urn:Two','MiXeD').ownerDocument,inert);
    imported.getAttributeNode('data-value-1').value='import-only';equal('import-isolation',copy.getAttribute('data-value-1'),'value-1');
    const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.setAttribute('viewBox','0 0 10 10');svg.setAttributeNS('http://www.w3.org/1999/xlink','xlink:href','#target');
    const sc=svg.cloneNode(false);equal('svg-case',sc.getAttributeNode('viewBox').name,'viewBox');equal('svg-case-distinct',sc.getAttribute('viewbox'),null);equal('svg-prefix',sc.getAttributeNodeNS('http://www.w3.org/1999/xlink','href').prefix,'xlink');
    const template=document.createElement('template');template.innerHTML='<article class="template-copy"><template><span data-value="nested">text</span></template></article>';
    const tc=template.cloneNode(true);equal('template-host-empty',tc.childNodes.length,0);equal('template-content-copy',tc.content.firstChild!==template.content.firstChild,true);
    equal('template-content-inert-owner',tc.content.ownerDocument===document,false);equal('nested-template',tc.content.querySelector('template').content.firstChild.textContent,'text');
    const input=document.createElement('input');input.type='text';input.setAttribute('value','default');input.value='dirty';
    const ic=input.cloneNode(false);equal('input-value',ic.value,'dirty');equal('input-default',ic.defaultValue,'default');
    const checkbox=document.createElement('input');checkbox.type='checkbox';checkbox.checked=true;checkbox.indeterminate=true;
    const cc=checkbox.cloneNode(false);equal('checkbox-checked',cc.checked,true);equal('checkbox-indeterminate',cc.indeterminate,true);
    const select=document.createElement('select');select.innerHTML='<option>A</option><option selected>B</option>';select.selectedIndex=0;
    equal('select-dirty-clone',select.cloneNode(true).selectedIndex,0);
    const details=document.createElement('details');details.setAttribute('open','');equal('details-open',details.cloneNode(false).open,true);
    const canvas=document.createElement('canvas');canvas.width=7;canvas.height=11;const cv=canvas.cloneNode(false);equal('canvas-width',cv.width,7);equal('canvas-height',cv.height,11);
    let constructed=0,changes=[];
    class CloneAttributesProbe extends HTMLElement{
        constructor(){super();constructed++;}
        static get observedAttributes(){return ['data-a','data-b'];}
        attributeChangedCallback(name,oldValue,newValue){changes.push([name,oldValue,newValue]);}
    }
    customElements.define('clone-attributes-probe',CloneAttributesProbe);
    const ce=document.createElement('clone-attributes-probe');ce.setAttribute('data-a','one');ce.setAttribute('data-b','two');
    const before=constructed;changes=[];const ceCopy=ce.cloneNode(false);
    equal('ce-class',ceCopy instanceof CloneAttributesProbe,true);equal('ce-constructor',constructed,before+1);
    equal('ce-attribute-order',changes.map(x=>x[0]).join('|'),'data-a|data-b');
    equal('ce-attribute-old',changes.every(x=>x[1]===null),true);equal('ce-attribute-values',changes.map(x=>x[2]).join('|'),'one|two');
    await Promise.resolve();return count;
}
