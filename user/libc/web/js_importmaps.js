    /* HTML Standard import-map processing, kept private to the document loader.
       https://html.spec.whatwg.org/multipage/webappapis.html#import-maps
       Only resolution is implemented here. Native fetching still enforces its
       HTTP(S)/local-file policy, CORS, source size, and JavaScript MIME checks. */
    const importMapsBridge = (() => {
        const URLClass=URL, parse=JSON.parse, keys=Object.keys, create=Object.create;
        const array=Array.isArray, own=Object.prototype.hasOwnProperty;
        const starts=String.prototype.startsWith, ends=String.prototype.endsWith;
        const slice=String.prototype.slice, sort=Array.prototype.sort;
        const href=Object.getOwnPropertyDescriptor(URLClass.prototype,'href').get;
        const protocol=Object.getOwnPropertyDescriptor(URLClass.prototype,'protocol').get;
        const TypeErrorClass=TypeError, RangeErrorClass=RangeError;
        let imports=create(null),scopes=create(null),resolved=create(null),recordCount=0;
        const records=[];
        const has=(o,k)=>apply(own,o,[k]);
        const prefix=(s,p)=>apply(starts,s,[p]);
        const slash=s=>apply(ends,s,['/']);
        const object=o=>o!==null && typeof o==='object' && !array(o);
        const ordered=o=>apply(sort,keys(o),[(a,b)=>a>b?-1:a<b?1:0]);
        const warn=message=>host.log(1,'Import map: '+message);
        function url(input,base){return apply(href,new URLClass(input,base),[]);}
        function urlLike(input,base){
            try{return url(input,prefix(input,'/') || prefix(input,'./') || prefix(input,'../')?base:undefined);}
            catch(error){if(!(error instanceof TypeErrorClass))throw error;return null;}
        }
        function special(input){
            const scheme=apply(protocol,new URLClass(input),[]);
            return scheme==='http:' || scheme==='https:' || scheme==='file:' || scheme==='ftp:' || scheme==='ws:' || scheme==='wss:';
        }
        function normalize(map,base){
            const result=create(null),names=keys(map);
            for(let i=0;i<names.length;i++){
                const key=names[i];if(!key){warn('empty specifier key ignored');continue;}
                const normalized=urlLike(key,base)||key,value=map[key];
                const address=typeof value==='string'?urlLike(value,base):null;
                if(address===null || (slash(key) && !slash(address))){result[normalized]=null;warn('blocked invalid address for '+key);}
                else result[normalized]=address;
            }
            return result;
        }
        function scopeMatches(scope,base){return scope===base || (slash(scope) && prefix(base,scope));}
        function keyMatches(key,specifier,prefixable){return key===specifier || (prefixable && slash(key) && prefix(specifier,key));}
        function merge(map,old,scope){
            const names=keys(map);
            for(let i=0;i<names.length;i++){
                const key=names[i];let blocked=has(old,key);
                for(let j=0;!blocked && j<records.length;j++){
                    const record=records[j];
                    if(scope!==null && !scopeMatches(scope,record.base))continue;
                    /* Do not permit a late prefix rule to retarget a previously
                       resolved pair. The global merge also drops extensions of
                       a resolved key, as specified by HTML's merge algorithm. */
                    blocked=keyMatches(key,record.specifier,record.prefixable) || (scope===null && prefix(key,record.specifier));
                }
                if(blocked)warn('ignored conflicting or already-resolved rule '+key);
                else old[key]=map[key];
            }
        }
        function register(source,base){
            base=url(base);
            const parsed=parse(source);
            if(!object(parsed))throw new TypeErrorClass('Import map must be a JSON object');
            const fields=['imports','scopes','integrity'];
            for(let i=0;i<fields.length;i++)
                if(has(parsed,fields[i]) && !object(parsed[fields[i]]))throw new TypeErrorClass('Import map '+fields[i]+' must be a JSON object');
            /* Do not claim or silently skip SRI. Unsupported integrity-bearing
               maps must not cause their modules to load without verification. */
            if(has(parsed,'integrity') && keys(parsed.integrity).length)throw new TypeErrorClass('Import-map integrity verification is not implemented');
            const newImports=has(parsed,'imports')?normalize(parsed.imports,base):create(null);
            const newScopes=create(null);
            if(has(parsed,'scopes')){
                const names=keys(parsed.scopes);
                for(let i=0;i<names.length;i++){
                    const name=names[i],map=parsed.scopes[name];
                    if(!object(map))throw new TypeErrorClass('Import map scope must be a JSON object: '+name);
                    let scope;
                    try{scope=url(name,base);}catch(error){if(!(error instanceof TypeErrorClass))throw error;warn('invalid scope ignored: '+name);continue;}
                    newScopes[scope]=normalize(map,base);
                }
            }
            const names=keys(parsed);
            for(let i=0;i<names.length;i++)if(names[i]!=='imports' && names[i]!=='scopes' && names[i]!=='integrity')warn('unknown top-level key '+names[i]);
            /* Parsing completes before changing the active map. */
            const scopeNames=keys(newScopes);
            for(let i=0;i<scopeNames.length;i++){
                const scope=scopeNames[i];if(!has(scopes,scope))scopes[scope]=create(null);
                merge(newScopes[scope],scopes[scope],scope);
            }
            merge(newImports,imports,null);
        }
        function match(specifier,prefixable,map){
            const names=ordered(map);
            for(let i=0;i<names.length;i++){
                const key=names[i];if(!keyMatches(key,specifier,prefixable))continue;
                const address=map[key];
                if(address===null)throw new TypeErrorClass('Module resolution blocked by import-map entry: '+key);
                if(key===specifier)return address;
                let target;
                try{target=url(apply(slice,specifier,[key.length]),address);}catch(error){if(!(error instanceof TypeErrorClass))throw error;throw new TypeErrorClass('Invalid import-map prefix suffix: '+specifier);}
                if(!prefix(target,address))throw new TypeErrorClass('Import-map prefix backtracking is not permitted: '+specifier);
                return target;
            }
            return null;
        }
        function resolve(specifier,base){
            base=url(base);
            const asURL=urlLike(specifier,base),normalized=asURL||specifier;
            const prefixable=asURL===null || special(asURL);
            /* Success records double as the resolved-pair cache. Failures are
               not frozen: a later map may define a previously unknown key. */
            if(has(resolved,base) && has(resolved[base],normalized))return resolved[base][normalized];
            let result=null;const scopeNames=ordered(scopes);
            for(let i=0;i<scopeNames.length && result===null;i++)
                if(scopeMatches(scopeNames[i],base))result=match(normalized,prefixable,scopes[scopeNames[i]]);
            if(result===null)result=match(normalized,prefixable,imports);
            if(result===null)result=asURL;
            if(result===null)throw new TypeErrorClass('Unmapped bare module specifier: '+specifier);
            if(!has(resolved,base))resolved[base]=create(null);
            resolved[base][normalized]=result;records[recordCount++]={base,specifier:normalized,prefixable};
            return result;
        }
        return {register,resolve,url};
    })();
