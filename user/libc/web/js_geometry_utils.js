/* CSSOM View §11 GeometryUtils, the mixin of Text, Element and Document (CSSPseudoElement is
 * no interface here): the bindings of js_geometry_native.h's boxQuads and convertPoints, which
 * read the layout's boxes, fragments and transforms. Web IDL conversions run first (the
 * dictionaries' getters may change the document); native reads the layout afterwards.
 * Include after js_geometry.js. https://drafts.csswg.org/cssom-view/#the-geometryutils-interface */
{
    const geometryDefine=Object.defineProperty,GeometryTypeError=TypeError,GeometryDOMPoint=geometryBridge.DOMPoint;
    /* CSSBoxType, in js_geometry_native.h's order */
    function geometryBox(value){
        value=elementURL.string(value);
        if(value==='margin')return 0;if(value==='border')return 1;
        if(value==='padding')return 2;if(value==='content')return 3;
        throw new GeometryTypeError('Invalid CSSBoxType');
    }
    function geometryDictionary(value,name){
        if(value!=null && typeof value!=='object' && typeof value!=='function')throw new GeometryTypeError(name+' must be a dictionary');
        return value==null?null:value;
    }
    /* GeometryNode: (Text or Element or CSSPseudoElement or Document) */
    function geometryNode(value,name){
        const type=rawDom.isNode(null,value)?rawDom.get(value,'nodeType'):0;
        if(type!==1 && type!==3 && type!==4 && type!==9)throw new GeometryTypeError(name+' is not a GeometryNode');
        return value;
    }
    function geometryResult(value){
        if(typeof value!=='number')return value;
        if(value===1)throw new DOMException('The node is not rendered','NotFoundError');
        throw new DOMException('The nodes are in documents that are not same origin','SecurityError');
    }
    function geometryQuad(values,at){
        return geometryBridge.quad([[values[at],values[at+1],0,1],[values[at+2],values[at+3],0,1],
            [values[at+4],values[at+5],0,1],[values[at+6],values[at+7],0,1]]);
    }
    /* ConvertCoordinateOptions: fromBox, then toBox, both "border" by default */
    function geometryConvertOptions(options){
        options=geometryDictionary(options,'ConvertCoordinateOptions');
        let fromBox=1,toBox=1;
        if(options){
            const from=options.fromBox;if(from!==undefined)fromBox=geometryBox(from);
            const to=options.toBox;if(to!==undefined)toBox=geometryBox(to);
        }
        return {fromBox,toBox};
    }
    function geometryConvert(self,from,options,points){
        from=geometryNode(from,'from');
        const o=geometryConvertOptions(options);
        return geometryResult(dom('convertPoints',self,from,o.fromBox,o.toBox,points,false));
    }
    function geometryArguments(count,required,name){
        if(count<required)throw new GeometryTypeError(`${name} requires ${required} arguments`);
    }
    function geometryMethods(brand){
        return {
            /* getBoxQuads(): relativeTo defaults to this node's document */
            getBoxQuads(options=undefined){
                brand(this);
                options=geometryDictionary(options,'BoxQuadOptions');
                let box=1,relativeTo;
                if(options){
                    const b=options.box;if(b!==undefined)box=geometryBox(b);
                    const r=options.relativeTo;if(r!==undefined)relativeTo=geometryNode(r,'relativeTo');
                }
                if(relativeTo===undefined)relativeTo=rawDom.get(this,'nodeType')===9?this:rawDom.get(this,'ownerDocument');
                const values=geometryResult(dom('boxQuads',this,box,relativeTo)),quads=[];
                for(let at=0;at+8<=values.length;at+=8)quads[quads.length]=geometryQuad(values,at);
                return quads;
            },
            convertQuadFromNode(quad,from,options=undefined){
                brand(this);geometryArguments(arguments.length,2,'convertQuadFromNode');
                const p=geometryBridge.quadInit(quad);
                const values=geometryConvert(this,from,options,[p[0][0],p[0][1],p[1][0],p[1][1],p[2][0],p[2][1],p[3][0],p[3][1]]);
                return geometryQuad(values,0);
            },
            convertRectFromNode(rect,from,options=undefined){
                brand(this);geometryArguments(arguments.length,2,'convertRectFromNode');
                const r=geometryBridge.rectValues(rect),x=r[0],y=r[1],w=r[2],h=r[3];
                const values=geometryConvert(this,from,options,[x,y,x+w,y,x+w,y+h,x,y+h]);
                return geometryQuad(values,0);
            },
            /* convertPointFromNode(): z and w are ignored; the result is a 2D point */
            convertPointFromNode(point,from,options=undefined){
                brand(this);geometryArguments(arguments.length,2,'convertPointFromNode');
                const p=geometryBridge.pointInit(point);
                const values=geometryConvert(this,from,options,[p[0],p[1]]);
                return new GeometryDOMPoint(values[0],values[1]);
            }
        };
    }
    const geometryBrand=(a,b=a)=>node=>{
        const type=rawDom.isNode(null,node)?rawDom.get(node,'nodeType'):0;
        if(type!==a && type!==b)throw new GeometryTypeError('Illegal GeometryUtils receiver');
    };
    for(const [proto,brand] of [[Text.prototype,geometryBrand(3,4)],[Element.prototype,geometryBrand(1)],[Document.prototype,geometryBrand(9)]]){
        const methods=geometryMethods(brand);
        for(const name of Object.keys(methods))
            geometryDefine(proto,name,{configurable:true,enumerable:true,writable:true,value:methods[name]});
    }
}
