/* HTMLHyperlinkElementUtils. Private URL operations cannot be replaced by page code. */
(function () {
    'use strict';
    const URLImpl=globalThis.URL, call=Reflect.apply, StringImpl=String, TypeErrorImpl=TypeError;
    const codeUnit=String.prototype.charCodeAt, define=Object.defineProperty;
    const keys=['href','origin','protocol','username','password','host','hostname','port','pathname','search','hash'];
    const urlGet=Object.create(null),urlSet=Object.create(null);
    for(const key of keys){
        const descriptor=Object.getOwnPropertyDescriptor(URLImpl.prototype,key);
        urlGet[key]=descriptor.get;urlSet[key]=descriptor.set;
    }
    function string(value){
        if(typeof value==='symbol')throw new TypeErrorImpl('Cannot convert Symbol to string');
        return StringImpl(value);
    }
    function scalar(value){
        const text=string(value);let result='';
        for(let i=0;i<text.length;i++){
            const c=call(codeUnit,text,[i]);
            if(c>=0xd800 && c<=0xdbff){
                const next=i+1<text.length?call(codeUnit,text,[i+1]):0;
                if(next>=0xdc00 && next<=0xdfff){result+=text[i]+text[++i];continue;}
            }else if(c<0xdc00 || c>0xdfff){result+=text[i];continue;}
            result+='\ufffd';
        }
        return result;
    }
    function parse(el,attribute){
        try { return new URLImpl(attribute,rawDom('get',el,'baseURI')); }
        catch(error){if(error instanceof TypeErrorImpl)return null;throw error;}
    }
    function parsedHref(el){const value=rawDom('attr',el,'href');return value===null?null:parse(el,value);}
    function attribute(el,tag,name,emptyDocument){
        htmlElementBrand(el,tag);
        const value=rawDom('attr',el,name);
        if(emptyDocument && (value===null || value===''))return rawDom('get',rawDom('get',el,'ownerDocument'),'URL');
        if(value===null)return '';
        const url=parse(el,value);return url?call(urlGet.href,url,[]):scalar(value);
    }
    elementURL={string,scalar,attribute};
    const empty={origin:'',protocol:':',username:'',password:'',host:'',hostname:'',port:'',pathname:'',search:'',hash:''};
    for(const [C,tag] of [[HTMLAnchorElement,'a'],[HTMLAreaElement,'area']]){
        define(C.prototype,'href',{configurable:true,enumerable:true,
            get(){return attribute(this,tag,'href',false);},
            set(value){htmlElementBrand(this,tag);dom('attr',this,'href',scalar(value));}});
        for(const key of keys.slice(1)){
            const descriptor={configurable:true,enumerable:true,get(){
                htmlElementBrand(this,tag);const url=parsedHref(this);return url?call(urlGet[key],url,[]):empty[key];
            }};
            if(key!=='origin')descriptor.set=function(value){
                htmlElementBrand(this,tag);
                // IDL conversion may reenter and change href. Parse only after it finishes.
                value=scalar(value);const url=parsedHref(this);if(!url)return;
                call(urlSet[key],url,[value]);dom('attr',this,'href',call(urlGet.href,url,[]));
            };
            define(C.prototype,key,descriptor);
        }
        for(const name of ['rel','target','download'])define(C.prototype,name,{configurable:true,enumerable:true,
            get(){htmlElementBrand(this,tag);return rawDom('attr',this,name)||'';},
            set(value){htmlElementBrand(this,tag);dom('attr',this,name,string(value));}});
        define(C.prototype,'toString',{configurable:true,writable:true,value(){return attribute(this,tag,'href',false);}});
    }
    define(HTMLAnchorElement.prototype,'text',{configurable:true,enumerable:true,
        get(){htmlElementBrand(this,'a');return rawDom('get',this,'textContent');},
        set(value){htmlElementBrand(this,'a');dom('set',this,'textContent',string(value));}});
})();
