/* Shared Window/Worker Web IDL DOMException bindings and the one
   DOMException-derived interface, QuotaExceededError. Internal values never
   depend on author-overridden name/message getters during structured clone.
   https://webidl.spec.whatwg.org/#idl-DOMException
   https://webidl.spec.whatwg.org/#quotaexceedederror */
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
    /* Web IDL double: ToNumber, and a TypeError for NaN and the infinities. */
    const double=(value,what)=>{
        const n=+value;
        if(n!==n||n===Infinity||n===-Infinity)throw new TypeError(`QuotaExceededError ${what} is not a finite number`);
        return n;
    };
    const quotas=new WeakMap();
    const quotaState=value=>{
        const s=apply(get,quotas,[value]);
        if(!s)throw new TypeError('Illegal QuotaExceededError receiver');
        return s;
    };
    class QuotaExceededError extends DOMException {
        constructor(message='',options=undefined) {
            /* The arguments convert first, message then the dictionary (its members in
               lexicographic order); the constructor steps then check the values. */
            const m=text(message);
            if(options!==undefined&&options!==null&&typeof options!=='object'&&typeof options!=='function')
                throw new TypeError('QuotaExceededError options is not an object');
            let quota=null,requested=null;
            if(options!==undefined&&options!==null) {
                const q=options.quota;if(q!==undefined)quota=double(q,'quota');
                const r=options.requested;if(r!==undefined)requested=double(r,'requested');
            }
            if(quota!==null&&quota<0)throw new RangeError('QuotaExceededError quota is negative');
            if(requested!==null&&requested<0)throw new RangeError('QuotaExceededError requested is negative');
            if(quota!==null&&requested!==null&&requested<quota)throw new RangeError('QuotaExceededError requested is less than quota');
            super(m,'QuotaExceededError');
            apply(set,quotas,[this,{quota,requested}]);
        }
        get quota(){return quotaState(this).quota;}
        get requested(){return quotaState(this).requested;}
    }
    define(QuotaExceededError.prototype,Symbol.toStringTag,{value:'QuotaExceededError',configurable:true});
    /* The exception a specification names: QuotaExceededError is its own interface (the
       DOMException name is deprecated), every other name a DOMException. */
    const create=(name,message)=>name==='QuotaExceededError'?new QuotaExceededError(message):new DOMException(message,name);
    return {
        DOMException,QuotaExceededError,create,
        brand:value=>apply(has,slots,[value]),
        /* The DOMException serialization steps, and QuotaExceededError's quota and requested. */
        snapshot:value=>{
            const s=state(value),q=apply(get,quotas,[value]);
            return q?[s.name,s.message,q.quota,q.requested]:[s.name,s.message];
        },
        restore:r=>{
            if(r.length<4)return new DOMException(r[1],r[0]);
            const value=new QuotaExceededError(r[1]);
            apply(set,quotas,[value,{quota:r[2],requested:r[3]}]);
            if(r[0]!=='QuotaExceededError')apply(get,slots,[value]).name=r[0];
            return value;
        }
    };
})();
