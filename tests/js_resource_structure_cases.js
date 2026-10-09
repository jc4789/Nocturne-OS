async function runResourceStructureCases(){
    let count=0;
    function ok(name,value){count++;if(!value)throw Error('resource-structure '+name);}
    const target=document.getElementById('target'),head=document.head;
    const sheet=document.createElement('style');sheet.textContent='#resource-normal{display:block;height:17px}';head.appendChild(sheet);
    const span=document.createElement('span');span.id='resource-normal';span.textContent='one';target.appendChild(span);
    ok('ordinary-insert-cascade',span.getBoundingClientRect().height===17);
    span.firstChild.data='two';ok('ordinary-text-cascade',span.getBoundingClientRect().height===17);
    const other=document.createElement('div');document.body.appendChild(other);other.appendChild(span);
    ok('ordinary-move-cascade',span.getBoundingClientRect().height===17);
    sheet.firstChild.data='#resource-normal{display:block;height:29px}';
    ok('style-character-data-rescan',span.getBoundingClientRect().height===29);
    const detached=document.createElement('div');detached.appendChild(sheet);
    ok('style-live-to-detached',span.getBoundingClientRect().height!==29);
    head.appendChild(sheet);ok('style-reinsert',span.getBoundingClientRect().height===29);
    const wrapper=document.createElement('div'),nested=document.createElement('style');
    nested.textContent='#resource-normal{height:31px}';wrapper.appendChild(nested);document.body.appendChild(wrapper);
    ok('nested-sheet-insert',span.getBoundingClientRect().height===31);
    wrapper.remove();ok('nested-sheet-remove',span.getBoundingClientRect().height===29);
    const title=document.createElement('title');title.textContent='Resource before';head.appendChild(title);
    const active=document.querySelector('title');active.textContent='Resource after';ok('title-text',document.title==='Resource after');
    active.firstChild.data='Resource character';ok('title-character-data',document.title==='Resource character');
    const base=document.createElement('base');base.href='http://fixture.test/changed/';head.appendChild(base);
    ok('base-insert',new URL('relative',document.baseURI).href==='http://fixture.test/changed/relative');
    base.remove();ok('base-remove',new URL('relative',document.baseURI).href==='http://fixture.test/dir/relative');
    const ta=document.createElement('textarea');ta.textContent='initial';document.body.appendChild(ta);
    ok('textarea-insert',ta.value==='initial');ta.firstChild.data='changed';ok('textarea-character-data',ta.value==='changed');
    const select=document.createElement('select'),option=document.createElement('option');option.textContent='option';select.appendChild(option);document.body.appendChild(select);
    ok('select-insert',select.options.length===1&&select.selectedIndex===0);
    option.remove();ok('option-remove',select.options.length===0&&select.selectedIndex===-1);
    const template=document.createElement('template');template.innerHTML='<style>#resource-normal{height:99px}</style>';document.body.appendChild(template);
    ok('template-style-inert',span.getBoundingClientRect().height===29);
    template.remove();select.remove();ta.remove();title.remove();sheet.remove();other.remove();
    return count;
}
