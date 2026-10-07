/* Private validation realm only. Native callbacks implement UTF-8; there are
 * no DOM/network/filesystem hooks and author scripts cannot reach this realm.
 * The URL bundle only requests UTF-8 and ignoreBOM:true. */
(() => {
    const encode=globalThis.__formEncode,decode=globalThis.__formDecode;
    delete globalThis.__formEncode;delete globalThis.__formDecode;
    class TextEncoder {
        get encoding(){return 'utf-8';}
        encode(value=''){return new Uint8Array(encode(String(value).toWellFormed()));}
    }
    class TextDecoder {
        constructor(label='utf-8',options={}){
            if(!['utf-8','utf8','unicode-1-1-utf-8'].includes(String(label).trim().toLowerCase()))throw new RangeError('UTF-8 only');
            this.ignoreBOM=!!options.ignoreBOM;
        }
        get encoding(){return 'utf-8';}
        decode(value=new Uint8Array()){
            const bytes=value instanceof ArrayBuffer?new Uint8Array(value):new Uint8Array(value.buffer,value.byteOffset,value.byteLength);
            const copy=new Uint8Array(bytes.length);copy.set(bytes);
            const text=decode(copy.buffer);
            return !this.ignoreBOM&&text.charCodeAt(0)===0xfeff?text.slice(1):text;
        }
    }
    Object.assign(globalThis,{TextEncoder,TextDecoder});
})();
