/* Credential Management's empty-manager path. Nocturne has no authenticator
   or password vault yet. Advertise that fact, and reject ceremonies through
   their Promise rather than crashing on undefined navigator.credentials.
   This is NOT passkey/signature/password-storage implementation. */
if (cryptoSecureContext) {
    const P=Promise, Err=TypeError, DomErr=DOMException, apply=Reflect.apply;
    const define=Object.defineProperty, create=Object.create, string=String;
    const abort=Object.getOwnPropertyDescriptor(AbortSignal.prototype,'aborted').get;
    const reason=Object.getOwnPropertyDescriptor(AbortSignal.prototype,'reason').get;
    const dictionary=value=>{
        if(value==null)return {};
        if(typeof value!=='object'&&typeof value!=='function')throw new Err('Expected a dictionary');
        return value;
    };
    const settle=operation=>new P((resolve,reject)=>{try{resolve(operation());}catch(e){reject(e);}});
    function validate(receiver,options) {
        if(receiver!==container)throw new Err('Illegal CredentialsContainer invocation');
        options=dictionary(options);
        const mediation=options.mediation;
        if(mediation!==undefined) {
            if(typeof mediation==='symbol')throw new Err('Invalid credential mediation');
            const value=string(mediation);
            if(value!=='silent'&&value!=='optional'&&value!=='required'&&value!=='conditional')
                throw new Err('Invalid credential mediation');
        }
        const signal=options.signal;
        if(signal!==undefined&&apply(abort,signal,[]))throw apply(reason,signal,[]);
        return options;
    }
    class Credential { constructor(){throw new Err('Illegal constructor');} }
    class CredentialsContainer {
        constructor(){throw new Err('Illegal constructor');}
        get(options={}) {
            return settle(()=>{
                options=validate(this,options);
                if(options.publicKey!==undefined)
                    throw new DomErr('No authenticator is available in Nocturne','NotAllowedError');
                if(options.identity!==undefined||options.otp!==undefined)
                    throw new DomErr('This credential provider is not implemented','NotSupportedError');
                // No stored password/federated credential exists, and none is
                // read from the native search history or ordinary form inputs.
                return null;
            });
        }
        create(options={}) {
            return settle(()=>{
                options=validate(this,options);
                if(options.publicKey!==undefined)
                    throw new DomErr('No authenticator is available in Nocturne','NotAllowedError');
                if(options.password!==undefined||options.federated!==undefined||options.identity!==undefined||options.otp!==undefined)
                    throw new DomErr('Credential creation is not implemented','NotSupportedError');
                return null;
            });
        }
        store(credential) {
            const argc=arguments.length;
            return settle(()=>{
                if(this!==container||!argc)throw new Err('store requires a Credential and the native container');
                // Do not accept forged objects or silently persist secrets.
                throw new Err('Expected a native Credential; no credential vault is implemented');
            });
        }
        preventSilentAccess() {
            return settle(()=>{if(this!==container)throw new Err('Illegal CredentialsContainer invocation');});
        }
    }
    class PublicKeyCredential extends Credential {
        static isUserVerifyingPlatformAuthenticatorAvailable(){return new P(resolve=>resolve(false));}
        static isConditionalMediationAvailable(){return new P(resolve=>resolve(false));}
        static getClientCapabilities(){
            return new P(resolve=>resolve({conditionalCreate:false,conditionalGet:false,hybridTransport:false,
                passkeyPlatformAuthenticator:false,relatedOrigins:false,signalAllAcceptedCredentials:false,
                signalCurrentUserDetails:false,signalUnknownCredential:false,userVerifyingPlatformAuthenticator:false}));
        }
    }
    const container=create(CredentialsContainer.prototype);
    for(const [Type,name]of [[Credential,'Credential'],[CredentialsContainer,'CredentialsContainer'],[PublicKeyCredential,'PublicKeyCredential']]) {
        define(Type.prototype,Symbol.toStringTag,{value:name,configurable:true});
        define(globalThis,name,{value:Type,writable:true,configurable:true});
        for(const key of Object.getOwnPropertyNames(Type.prototype)) {
            if(key==='constructor')continue;
            const descriptor=Object.getOwnPropertyDescriptor(Type.prototype,key);descriptor.enumerable=true;
            define(Type.prototype,key,descriptor);
        }
    }
    define(navigator,'credentials',{get:()=>container,enumerable:true,configurable:true});
}
