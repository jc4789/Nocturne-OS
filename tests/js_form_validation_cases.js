/* Real web_live/native constraint validation; run once with the full forms
 * batch. Native GUI editing/validation also has independent C regressions. */
globalThis.runFormValidationCases=async function(){
    let checks=0;
    const equal=(a,b,label='')=>{checks++;if(!Object.is(a,b))throw new Error('フォーム検証 '+label+': '+String(a)+' != '+String(b));};
    const rejects=(fn,name,label='')=>{checks++;let error;try{fn();}catch(e){error=e;}if(error?.name!==name)throw new Error('フォーム検証 '+label+': '+error?.name+' != '+name);};
    const make=(tag='input',attrs={})=>{const n=document.createElement(tag);for(const [key,value] of Object.entries(attrs))n.setAttribute(key,value);return n;};
    const validity=ValidityState,submitEvent=SubmitEvent,input=make();
    equal(input.validity instanceof validity,true);equal(Object.getPrototypeOf(input.validity),validity.prototype);
    equal(Object.prototype.toString.call(input.validity),'[object ValidityState]');equal(input.validity,input.validity,'SameObject');
    equal(input.willValidate,true);equal(input.validity.valid,true);equal(input.validationMessage,'');equal(input.checkValidity(),true);
    rejects(()=>new validity(),'TypeError');rejects(()=>Object.getOwnPropertyDescriptor(validity.prototype,'valid').get.call({}),'TypeError');
    const held=input.validity;input.required=true;equal(input.getAttribute('required'),'');equal(held.valueMissing,true);equal(held.valid,false);
    equal(input.validationMessage.length>0,true);input.value='filled';equal(held.valueMissing,false);equal(held.valid,true);equal(input.validationMessage,'');
    input.required=false;equal(input.hasAttribute('required'),false);
    for(const [tag,C] of [['input',HTMLInputElement],['button',HTMLButtonElement],['select',HTMLSelectElement],['textarea',HTMLTextAreaElement],
        ['fieldset',HTMLFieldSetElement],['object',HTMLObjectElement],['output',HTMLOutputElement]]){
        const n=make(tag);equal(n.validity instanceof validity,true,tag+' validity');
        n.setCustomValidity('独自のエラー');equal(n.validity.customError,true,tag+' custom');equal(n.validity.valid,false,tag+' validity false');
        const barred=['fieldset','object','output'].includes(tag);
        equal(n.willValidate,!barred,tag+' willValidate');equal(n.checkValidity(),barred,tag+' checkValidity');
        equal(n.validationMessage,barred?'':'独自のエラー',tag+' message');
        n.setCustomValidity('');equal(n.validity.valid,true);equal(n.checkValidity(),true);
        rejects(()=>C.prototype.checkValidity.call(make('div')),'TypeError',tag+' wrong tag');
        rejects(()=>Object.getOwnPropertyDescriptor(C.prototype,'validity').get.call(Object.create(C.prototype)),'TypeError',tag+' forged');
        rejects(()=>n.setCustomValidity(),'TypeError');rejects(()=>n.setCustomValidity(Symbol()),'TypeError');
    }
    input.setCustomValidity('\0エラー\0');equal(input.validity.customError,true,'embedded NUL is not empty');equal(input.checkValidity(),false);
    equal(input.validationMessage,'\0エラー\0','DOMString NUL roundtrip');input.setCustomValidity('\0');equal(input.validity.valid,false);equal(input.validationMessage,'\0');input.setCustomValidity('');
    input.setCustomValidity(null);equal(input.validationMessage,'null');input.setCustomValidity(undefined);equal(input.validationMessage,'undefined');input.setCustomValidity('');
    input.setCustomValidity('native');const clone=input.cloneNode();equal(clone.validity.customError,false,'clone starts without custom error');input.setCustomValidity('');
    const inert=document.implementation.createHTMLDocument('validation'), adopted=make();adopted.setCustomValidity('adopt');inert.adoptNode(adopted);
    equal(adopted.validity.customError,true);equal(adopted.validationMessage,'adopt');document.adoptNode(adopted);equal(adopted.validationMessage,'adopt');

    for(const type of ['hidden','button','reset']){const n=make('input',{type,required:''});equal(n.willValidate,false,type);equal(n.validity.valueMissing,false);}
    for(const type of ['submit','image']){const n=make('input',{type});equal(n.willValidate,true,type);n.setCustomValidity('bad');equal(n.checkValidity(),false);}
    for(const type of ['button','reset']){const n=make('button',{type});equal(n.willValidate,false,type);n.setCustomValidity('bad');equal(n.checkValidity(),true);equal(n.validity.customError,true);}
    for(const type of ['text','search','url','tel','email','password','date','month','week','time','datetime-local','number']){
        const n=make('input',{type,readonly:'',required:''});equal(n.willValidate,false,type+' readonly');equal(n.validity.valueMissing,false);equal(n.checkValidity(),true);
    }
    for(const type of ['checkbox','radio','range','color','file','submit']){const n=make('input',{type,readonly:''});equal(n.willValidate,true,type+' readonly ignored');}
    const readonlyText=make('textarea',{readonly:'',required:''});equal(readonlyText.willValidate,false);equal(readonlyText.validity.valueMissing,false);
    const datalist=make('datalist'),barred=make('input',{required:''});datalist.append(barred);equal(barred.willValidate,false);equal(barred.checkValidity(),true);barred.remove();equal(barred.willValidate,true);

    const fieldset=make('fieldset',{disabled:''}),legend=make('legend'),secondLegend=make('legend'),before=make('span'),free=make('input',{required:''}),second=make('input',{required:''}),blocked=make('input',{required:''});
    legend.append(free);secondLegend.append(second);fieldset.append(before,legend,secondLegend,blocked);
    equal(free.disabled,false);equal(free.willValidate,true,'first legend exception');equal(second.willValidate,false,'second legend is not exempt');equal(blocked.willValidate,false,'fieldset inherited disabled');
    const nested=make('fieldset',{disabled:''}),nestedLegend=make('legend'),nestedFree=make('input',{required:''});nestedLegend.append(nestedFree);nested.append(nestedLegend);legend.append(nested);
    equal(nestedFree.willValidate,true,'nested first legend exemptions');free.appendChild(make('span'));
    legend.remove();equal(second.willValidate,true,'first legend changes live');fieldset.disabled=false;equal(blocked.willValidate,true);

    const checkbox=make('input',{type:'checkbox',required:''});equal(checkbox.validity.valueMissing,true);checkbox.checked=true;equal(checkbox.validity.valueMissing,false);
    checkbox.checked=false;checkbox.disabled=true;equal(checkbox.validity.valueMissing,true,'disabled checkbox still has missing state');equal(checkbox.checkValidity(),true,'disabled checkbox does not validate');
    const radioForm=make('form'),r1=make('input',{type:'radio',name:'group',required:'',disabled:''}),r2=make('input',{type:'radio',name:'group'}),r3=make('input',{type:'radio',name:'other'});
    radioForm.append(r1,r2,r3);equal(r1.validity.valueMissing,true);equal(r2.validity.valueMissing,true,'required disabled member applies to radio group');equal(r2.willValidate,true);equal(r3.validity.valueMissing,false);
    r2.checked=true;equal(r1.validity.valueMissing,false);equal(r2.validity.valueMissing,false);r2.checked=false;equal(r2.validity.valueMissing,true);
    const foreignForm=make('form');foreignForm.append(r2);equal(r2.validity.valueMissing,false,'distinct form radio group');

    const select=make('select',{required:''});select.innerHTML='<option value="">choose</option><option value="x">chosen</option>';
    equal(select.validity.valueMissing,true,'placeholder');select.selectedIndex=1;equal(select.validity.valueMissing,false);select.value='';equal(select.validity.valueMissing,true);
    select.size=2;equal(select.validity.valueMissing,false,'listbox empty option is a value');select.selectedIndex=-1;equal(select.validity.valueMissing,true,'no selection');
    const multi=make('select',{multiple:'',required:''});multi.innerHTML='<option value="">empty</option><option value="x">x</option>';
    equal(multi.type,'select-multiple');equal(multi.selectedIndex,-1,'multiple has no automatic first selection');equal(multi.validity.valueMissing,true);
    multi.options[0].selected=true;equal(multi.validity.valueMissing,false,'multiple empty option is not a placeholder');multi.options[1].selected=true;
    equal(multi.selectedOptions.length,2);multi.options[0].selected=false;equal(multi.validity.valueMissing,false);multi.options[1].selected=false;equal(multi.validity.valueMissing,true);
    multi.value='x';equal(multi.validity.valueMissing,false);multi.selectedIndex=-1;equal(multi.validity.valueMissing,true);
    const grouped=make('select',{required:''});grouped.innerHTML='<optgroup><option value="">empty optgroup value</option></optgroup>';equal(grouped.validity.valueMissing,false,'optgroup is not placeholder');
    const textarea=make('textarea',{required:''});equal(textarea.validity.valueMissing,true);textarea.value='\n';equal(textarea.validity.valueMissing,false);

    const email=make('input',{type:'email'});
    for(const value of ['a@b','a.b+tag@example.test',"!#$%&'*+/=?^_`{|}~-@example.test"]){email.value=value;equal(email.validity.typeMismatch,false,value);}
    for(const value of ['plain','@example.test','a@','a b@example.test','a@-host.test','a@host-.test','a@a..test','a@a.','a@日本.test']){email.value=value;equal(email.validity.typeMismatch,true,value);}
    email.value='';equal(email.validity.typeMismatch,false);email.multiple=true;email.value='a@b, c@d';equal(email.validity.typeMismatch,false);email.value='a@b,';equal(email.validity.typeMismatch,true);email.value='a@b,c';equal(email.validity.typeMismatch,true);
    email.pattern='.+@example\\.test';email.value='a@example.test,b@example.test';equal(email.validity.patternMismatch,false,'pattern matches each email');email.value='a@example.test,b@other.test';equal(email.validity.patternMismatch,true);
    const url=make('input',{type:'url'});
    for(const value of ['https://example.test/a?q=1#f','mailto:a@example.test','urn:example:one','data:text/plain,hello','file:///data/example','https://日本.example/猫','http://[::1]/']){url.value=value;equal(url.validity.typeMismatch,false,value);}
    for(const value of ['example.test','/relative','https://','http://[broken]/','http://host:70000/']){url.value=value;equal(url.validity.typeMismatch,true,value);}
    url.value='';equal(url.validity.typeMismatch,false);

    const pattern=make('input',{pattern:'[A-Z]{2}[0-9]{3}'});pattern.value='AB123';equal(pattern.validity.patternMismatch,false);pattern.value='prefixAB123';equal(pattern.validity.patternMismatch,true,'full match');
    pattern.pattern='[';equal(pattern.validity.patternMismatch,false,'invalid regex ignored');pattern.pattern='';equal(pattern.validity.patternMismatch,true,'empty pattern rejects nonempty');pattern.value='';equal(pattern.validity.patternMismatch,false,'empty value skips pattern');
    pattern.pattern='[\\p{Letter}&&\\p{ASCII}]+';pattern.value='Ascii';equal(pattern.validity.patternMismatch,false,'v set intersection');pattern.value='日本';equal(pattern.validity.patternMismatch,true);
    pattern.pattern='[\\q{cat|dog}]';pattern.value='cat';equal(pattern.validity.patternMismatch,false,'v string disjunction');pattern.value='cats';equal(pattern.validity.patternMismatch,true);
    pattern.pattern='.';pattern.value='😺';equal(pattern.validity.patternMismatch,false,'UTF-16 unicode scalar');
    const limited=make('input',{minlength:'4',maxlength:'6'});limited.value='x';equal(limited.validity.tooShort,false,'script setter does not establish user-edit length failure');limited.value='long text';equal(limited.validity.tooLong,false);
    equal(limited.minLength,4);equal(limited.maxLength,6);limited.minLength=0;equal(limited.getAttribute('minlength'),'0');limited.maxLength=5;equal(limited.getAttribute('maxlength'),'5');
    rejects(()=>{limited.minLength=-1;},'IndexSizeError');rejects(()=>{limited.maxLength=-1;},'IndexSizeError');rejects(()=>{limited.minLength=Symbol();},'TypeError');
    limited.removeAttribute('minlength');equal(limited.minLength,-1);limited.setAttribute('maxlength','bad');equal(limited.maxLength,-1);

    const number=make('input',{type:'number',min:'2',max:'10',step:'2'});
    number.value='1';equal(number.validity.rangeUnderflow,true);equal(number.validity.stepMismatch,true);number.value='4';equal(number.validity.valid,true);number.value='11';equal(number.validity.rangeOverflow,true);
    number.step='any';equal(number.validity.stepMismatch,false);number.value='broken';equal(number.value,'');equal(number.validity.badInput,false,'script sanitization is not bad user input');equal(number.validity.valid,true);
    const cases=[['date','2024-02-29','2024-03-01','2024-02-28','2'],['month','2024-02','2024-06','2024-01','2'],['week','2024-W01','2024-W10','2023-W52','2'],
        ['time','08:00','10:00','07:00','120'],['datetime-local','2024-01-01T08:00','2024-01-01T10:00','2024-01-01T07:00','120']];
    for(const [type,min,max,value,step] of cases){const n=make('input',{type,min,max,step});n.value=value;equal(n.validity.rangeUnderflow,true,type);n.value=min;equal(n.validity.valid,true,type+' base');}
    const time=make('input',{type:'time',min:'22:00',max:'02:00'});time.value='23:00';equal(time.validity.rangeUnderflow,false);equal(time.validity.rangeOverflow,false);
    time.value='01:00';equal(time.validity.valid,true);time.value='12:00';equal(time.validity.rangeUnderflow,true);equal(time.validity.rangeOverflow,true,'periodic reversed range');

    const form=make('form',{id:'validation-form'}),a=make('input',{required:'',name:'a'}),b=make('input',{required:'',name:'b'}),submit=make('button'),outside=make('input',{required:'',form:'validation-form'});
    form.append(a,b,submit);document.body.append(form,outside);
    let invalid=[],bubbles=0,submitted=0,lastSubmitter='unset';
    form.addEventListener('invalid',()=>bubbles++);form.addEventListener('submit',event=>{submitted++;lastSubmitter=event.submitter;equal(event instanceof submitEvent,true,'native SubmitEvent brand');event.preventDefault();});
    const capture=event=>{invalid.push(event.target);equal(event.bubbles,false);equal(event.cancelable,true);event.preventDefault();};
    a.addEventListener('invalid',capture);b.addEventListener('invalid',capture);outside.addEventListener('invalid',capture);
    equal(form.checkValidity(),false);equal(invalid.length,3);equal(invalid[0],a);equal(invalid[1],b);equal(invalid[2],outside);equal(bubbles,0,'invalid does not bubble');
    invalid=[];form.requestSubmit(submit);equal(submitted,0,'invalid form suppresses submit');equal(invalid.length,3);
    form.noValidate=true;form.requestSubmit(submit);equal(submitted,1);equal(lastSubmitter,submit);form.noValidate=false;
    submit.formNoValidate=true;form.requestSubmit(submit);equal(submitted,2);equal(lastSubmitter,submit);submit.formNoValidate=false;
    form.requestSubmit();equal(submitted,2);a.value='a';b.value='b';outside.value='out';equal(form.checkValidity(),true);
    form.requestSubmit();equal(submitted,3);equal(lastSubmitter,null,'requestSubmit without argument');form.requestSubmit(submit);equal(submitted,4);equal(lastSubmitter,submit);
    submit.click();equal(submitted,5,'native click validation and SubmitEvent');equal(lastSubmitter,submit);
    a.value='';submit.click();equal(submitted,5,'native click blocked');
    rejects(()=>form.requestSubmit(a),'TypeError');rejects(()=>form.requestSubmit(make('button')),'NotFoundError');rejects(()=>form.requestSubmit({}),'TypeError');
    const commandButton=make('button',{commandfor:'target'});form.append(commandButton);equal(commandButton.willValidate,false,'Auto command button is not submit');rejects(()=>form.requestSubmit(commandButton),'TypeError');
    const se=new submitEvent('submit',{submitter:submit,cancelable:true});equal(se.submitter,submit);equal(se instanceof Event,true);equal(Object.prototype.toString.call(se),'[object SubmitEvent]');
    rejects(()=>new submitEvent(),'TypeError');rejects(()=>new submitEvent('submit',{submitter:{}}),'TypeError');rejects(()=>Object.getOwnPropertyDescriptor(submitEvent.prototype,'submitter').get.call({}),'TypeError');
    let submitterReads=0;const getterEvent=new submitEvent('submit',{get submitter(){submitterReads++;return submit;}});equal(getterEvent.submitter,submit);equal(submitterReads,1,'dictionary member is read once');
    const beforeMessage=a.validationMessage;a.removeEventListener('invalid',capture);let canceled=0;const cancel=event=>{canceled++;event.preventDefault();};a.addEventListener('invalid',cancel);
    equal(a.reportValidity(),false);equal(canceled,1);equal(a.validationMessage,beforeMessage,'cancel does not make invalid valid');a.removeEventListener('invalid',cancel);
    b.value='';let removed=false;const mutate=()=>{b.remove();outside.value='changed';removed=true;};a.addEventListener('invalid',mutate);invalid=[];
    equal(form.checkValidity(),false);equal(removed,true);equal(invalid.includes(b),true,'precallback invalid snapshot survives removal');
    a.removeEventListener('invalid',mutate);form.append(b);outside.remove();b.removeEventListener('invalid',capture);
    a.value='';b.value='';const fixFirst=()=>{a.value='fixed';};a.addEventListener('invalid',fixFirst);
    let bFocus=0;const focused=()=>bFocus++;b.addEventListener('focus',focused);
    equal(form.reportValidity(),false,'report still returns negative snapshot');equal(document.activeElement,b,'report skips candidate fixed by handler');equal(bFocus,1,'report runs focus events');
    a.removeEventListener('invalid',fixFirst);b.removeEventListener('focus',focused);
    a.value='';b.value='';const moveOnFocus=()=>inert.adoptNode(a);a.addEventListener('focus',moveOnFocus);
    equal(form.reportValidity(),false,'focus callback keeps negative validation snapshot');equal(a.ownerDocument,inert,'focus callback may adopt invalid candidate');equal(document.activeElement,b,'report skips candidate adopted during focus');
    a.removeEventListener('focus',moveOnFocus);document.adoptNode(a);form.insertBefore(a,b);a.value='filled';b.value='filled';
    let reentered=0;const reentry=()=>{reentered++;form.requestSubmit(submit);};form.addEventListener('submit',reentry);form.requestSubmit(submit);equal(reentered,1,'submission reentry guarded');form.removeEventListener('submit',reentry);
    form.remove();
    const originalValidity=globalThis.ValidityState,originalSubmit=globalThis.SubmitEvent;
    try{globalThis.ValidityState=function FakeValidity(){};globalThis.SubmitEvent=function FakeSubmit(){};equal(make().validity instanceof validity,true,'captured native validity brand');}
    finally{globalThis.ValidityState=originalValidity;globalThis.SubmitEvent=originalSubmit;}
    const direct=make('form',{action:'?direct-form-submit'}),directInput=make('input',{required:''});direct.append(directInput);document.body.append(direct);
    let directInvalid=0,directSubmit=0;directInput.addEventListener('invalid',()=>directInvalid++);direct.addEventListener('submit',()=>directSubmit++);
    direct.submit();equal(directInvalid,0,'submit bypasses validation');equal(directSubmit,0,'submit bypasses submit event');direct.remove();
    return checks;
};
