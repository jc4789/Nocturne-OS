/* ProcessingInstruction has a native, case-sensitive ordered map. It is not
 * an Element, and exposes no Attr/NamedNodeMap facade. */
const piBridge=(()=>{
    const native=host.pi,define=Object.defineProperty,StringImpl=String;
    function brand(node){native(0,node);}
    function required(count,min){if(count<min)throw new TypeError('ProcessingInstruction argument required');}
    function string(value){if(typeof value==='symbol')throw new TypeError('Cannot convert Symbol to DOMString');return StringImpl(value);}
    function validName(name){if(!name.length||/[\u0000\t\n\f\r /=>]/u.test(name))throw new DOMException('Invalid processing instruction attribute name','InvalidCharacterError');}
    const methods={
        hasAttributes(){brand(this);return native(4,this);},
        getAttributeNames(){brand(this);return native(1,this);},
        getAttribute(name){brand(this);required(arguments.length,1);return native(2,this,string(name));},
        hasAttribute(name){brand(this);required(arguments.length,1);return native(3,this,string(name));},
        setAttribute(name,value){brand(this);required(arguments.length,2);name=string(name);value=string(value);validName(name);native(5,this,name,value);},
        removeAttribute(name){brand(this);required(arguments.length,1);native(6,this,string(name));},
        toggleAttribute(name,force=undefined){brand(this);required(arguments.length,1);name=string(name);const given=arguments.length>1&&force!==undefined;if(given)force=!!force;validName(name);return given?native(7,this,name,force):native(7,this,name);}
    };
    for(const name of Object.keys(methods))define(ProcessingInstruction.prototype,name,{value:methods[name],writable:true,enumerable:true,configurable:true});
    return {};
})();
