/* Shared Window/Worker Web IDL DOMException bindings. Internal values never
   depend on author-overridden name/message getters during structured clone.
   https://webidl.spec.whatwg.org/#idl-DOMException */
const domExceptionBridge = (() => {
    const slots=new WeakMap(),apply=Reflect.apply,construct=Reflect.construct;
    const get=WeakMap.prototype.get,set=WeakMap.prototype.set,has=WeakMap.prototype.has;
    const string=String,own=Object.hasOwn,define=Object.defineProperty;
    const setPrototype=Object.setPrototypeOf,ErrorBase=Error;
    const text=value=>{
        if(typeof value==='symbol')throw new TypeError('Symbol is not a DOMString');
        return string(value);
    };
    const state=value=>{
        const s=apply(get,slots,[value]);
        if(!s)throw new TypeError('Illegal DOMException receiver');
        return s;
    };
    const codes={
        IndexSizeError:1,HierarchyRequestError:3,WrongDocumentError:4,
        InvalidCharacterError:5,NoModificationAllowedError:7,NotFoundError:8,
        NotSupportedError:9,InUseAttributeError:10,InvalidStateError:11,
        SyntaxError:12,InvalidModificationError:13,NamespaceError:14,
        InvalidAccessError:15,TypeMismatchError:17,SecurityError:18,
        NetworkError:19,AbortError:20,URLMismatchError:21,QuotaExceededError:22,
        TimeoutError:23,InvalidNodeTypeError:24,DataCloneError:25
    };
    class DOMException {
        constructor(message='',name='Error') {
            /* Convert in argument order before constructing the native Error.
               This preserves engine Error identity and its own stack without
               installing writable public message/name storage. */
            const m=text(message),n=text(name);
            const value=construct(ErrorBase,[m],new.target);
            delete value.message;
            apply(set,slots,[value,{message:m,name:n}]);
            return value;
        }
        get name(){return state(this).name;}
        get message(){return state(this).message;}
        get code(){const s=state(this);return own(codes,s.name)?codes[s.name]:0;}
    }
    setPrototype(DOMException.prototype,ErrorBase.prototype);
    for(const key of ['name','message','code']) {
        const d=Object.getOwnPropertyDescriptor(DOMException.prototype,key);
        define(DOMException.prototype,key,{...d,enumerable:true});
    }
    const legacy=[
        'INDEX_SIZE_ERR','DOMSTRING_SIZE_ERR','HIERARCHY_REQUEST_ERR',
        'WRONG_DOCUMENT_ERR','INVALID_CHARACTER_ERR','NO_DATA_ALLOWED_ERR',
        'NO_MODIFICATION_ALLOWED_ERR','NOT_FOUND_ERR','NOT_SUPPORTED_ERR',
        'INUSE_ATTRIBUTE_ERR','INVALID_STATE_ERR','SYNTAX_ERR',
        'INVALID_MODIFICATION_ERR','NAMESPACE_ERR','INVALID_ACCESS_ERR',
        'VALIDATION_ERR','TYPE_MISMATCH_ERR','SECURITY_ERR','NETWORK_ERR',
        'ABORT_ERR','URL_MISMATCH_ERR','QUOTA_EXCEEDED_ERR','TIMEOUT_ERR',
        'INVALID_NODE_TYPE_ERR','DATA_CLONE_ERR'
    ];
    for(let i=0;i<legacy.length;i++) {
        const descriptor={value:i+1,enumerable:true};
        define(DOMException,legacy[i],descriptor);
        define(DOMException.prototype,legacy[i],descriptor);
    }
    define(DOMException.prototype,Symbol.toStringTag,{value:'DOMException',configurable:true});
    return {
        DOMException,
        brand:value=>apply(has,slots,[value]),
        snapshot:value=>{const s=state(value);return [s.name,s.message];}
    };
})();
