/* Read-only diagnostic. No author DOM, styles, functions or requests are replaced. */
(function () {
  var recordedBase=false;
  function publicPath(value) {
    try { var u = new URL(value, document.baseURI);
      return u.protocol==='https:'||u.protocol==='http:'?u.origin+u.pathname:''; }
    catch (_) { return ''; }
  }
  /* 作者処理は差し替えず、完了境界だけ捕捉する。本文・Cookie・任意metaは保存しない。 */
  var partialEventCount=0,publicErrorExcerpted=false;
  function publicBundleErrorStack(error) {
    var descriptor=Object.getOwnPropertyDescriptor(error,'stack');
    if(!descriptor||typeof descriptor.value!=='string'||descriptor.value.length>16000)return false;
    var frames=descriptor.value.split('\n'),publicFrames=0;
    for(var i=0;i<frames.length;i++) {
      var line=frames[i].trim();if(!line)continue;
      var position=/\((https?:\/\/[^\s)]+):\d+:\d+\)$/.exec(line);
      if(position) {
        var asset=new URL(position[1]);
        if(asset.origin!=='https://www.redditstatic.com'||asset.pathname!=='/js/concat'||
           asset.username||asset.password||asset.hash)return false;
        var keys=Array.from(asset.searchParams.keys());
        if(keys.length!==2||keys.indexOf('prefix')<0||keys.indexOf('chunks')<0)return false;
        if(!/^[A-Za-z0-9_./-]{0,200}$/.test(asset.searchParams.get('prefix'))||
           !/^[A-Za-z0-9_.,/-]{1,6000}$/.test(asset.searchParams.get('chunks')))return false;
        publicFrames++;continue;
      }
      if(!/^at (?:[A-Za-z_$][A-Za-z0-9_$.]*|<anonymous>) \((?:native|nocturne:browser-bindings:\d+:\d+)\)$/.test(line))return false;
    }
    return publicFrames>0;
  }
  ['faceplate-load-start','faceplate-response','faceplate-load','faceplate-error','faceplate-alert'].forEach(function(type){
    document.addEventListener(type,function(event){
      var target=event.target,navTarget=false;
      for(var parent=target;parent;parent=parent.parentNode)
        if(parent.localName==='flex-left-nav-container'){navTarget=true;break;}
      if(!navTarget&&target&&target.localName==='faceplate-loader'&&type!=='faceplate-error'&&type!=='faceplate-alert')return;
      if(partialEventCount>=24)return;
      partialEventCount++;
      try {
        var detail=event.detail, response=detail&&detail.response;
        var error=detail instanceof Error?detail:detail&&detail.error instanceof Error?detail.error:null;
        var message=error?String(error.message):'';
        var missing=error&&error.name==='ReferenceError'?/^'?([A-Za-z_$][A-Za-z0-9_$]*)'? is not defined$/.exec(message):null;
        var standard=['DOMParser','XMLSerializer','Document','HTMLDocument','DocumentFragment','HTMLTemplateElement','TreeWalker','NodeFilter','NodeIterator','ShadowRoot','Range','NodeList','Element','HTMLElement','AbortController','IntersectionObserver','TrustedHTML','TrustedTypePolicy','TrustedTypePolicyFactory','Sanitizer','ReadableStream','URL','URLPattern','MutationObserver','customElements','XMLHttpRequest','CSSStyleSheet','CSS','scheduler','Request','Response'];
        var missingPlatform=missing&&standard.indexOf(missing[1])>=0?missing[1]:null;
        var missingNamedNode=!!(missing&&missing[1].length<128&&document.getElementById(missing[1]));
        var mime=response&&response.headers?response.headers.get('content-type'):null;
        /* Error labels and source positions only: no URL queries, request body,
           cookie, dynamic message values or private feed content. */
        var staticMessage=error&&error.name==='TypeError'&&
          /^[A-Za-z_$][A-Za-z0-9_$ .():,-]{0,199}$/.test(message)?message:null;
        var sourcePositions=error?String(error.stack||'').split('\n').slice(0,5).map(function(line){
          var position=/(https?:\/\/[^\s)]+):(\d+):(\d+)\)?$/.exec(line);
          var fn=/\bat\s+([A-Za-z_$][A-Za-z0-9_$.]*)\s*\(/.exec(line);
          var offset=/:([0-9]+):([0-9]+)\)?$/.exec(line);
          return position||fn||offset?{source:position?publicPath(position[1]):null,
            fn:fn?fn[1]:null,line:position?position[2]:offset?offset[1]:null,
            column:position?position[3]:offset?offset[2]:null}:null;
        }).filter(function(position){return !!position;}):[];
        console.log('NAV_LOAD_EVENT',JSON.stringify({type:event.type,
          target:event.target&&event.target.localName,prevented:event.defaultPrevented,
          response:!!response,status:response?response.status:null,
          mime:mime?String(mime).split(';')[0]:null,url:response?publicPath(response.url):'',
          error:!!error,errorName:error?error.name:null,
          staticMessage:staticMessage,sourcePositions:sourcePositions,
          missingPlatform:missingPlatform,missingNamedNode:missingNamedNode,
          timeout:message==='Timed out.',unsupportedType:message.indexOf('Unsupported content type')===0}));
        /* Pass the real caught Error to the existing native debug excerpt
           reader once. Only a fixed message and public hash-only script stack
           are eligible; do not log private message values or private URLs. */
        var messageDescriptor=error&&Object.getOwnPropertyDescriptor(error,'message');
        if(!publicErrorExcerpted&&type==='faceplate-error'&&error instanceof TypeError&&
           messageDescriptor&&messageDescriptor.value==='not a function'&&publicBundleErrorStack(error)) {
          publicErrorExcerpted=true;console.error(error);
        }
      }catch(_){console.log('NAV_LOAD_EVENT_DIAGNOSTIC_FAILURE',type);}
    },true);
  });
  function navPartial(reason) {
    var host = document.querySelector('flex-left-nav-container');
    if (!host) return;
    var nodes = host.querySelectorAll('faceplate-loader,faceplate-partial');
    console.log('NAV_PARTIAL_COUNT', reason, nodes.length);
    if(!recordedBase) {
      var C=customElements.get('faceplate-partial');
      if(C){console.log('NAV_BASE',Function.prototype.toString.call(Object.getPrototypeOf(C)));recordedBase=true;}
    }
    for (var i=0; i<Math.min(nodes.length,4); i++) {
      var n=nodes[i], r=n.getBoundingClientRect(), url=n.getAttribute('src')||n.getAttribute('href');
      console.log('NAV_MIN_STATE',n.localName,n.isConnected,n.loading,
        typeof n.partialRequest,n.constructor===customElements.get(n.localName),typeof n._ctrl);
      if(reason==='1s'&&n.partialRequest)console.log('NAV_REQUEST_CLASS',
        Function.prototype.toString.call(Object.getPrototypeOf(n.partialRequest).constructor));
      console.log('NAV_PARTIAL', reason, JSON.stringify({name:n.localName,
        src:url?publicPath(url):'',loading:n.getAttribute('loading'),disabled:n.getAttribute('disabled'),
        defined:!!customElements.get(n.localName),children:n.childNodes.length,
        shadow:n.shadowRoot?n.shadowRoot.childNodes.length:-1,
        rect:{x:r.x,y:r.y,w:r.width,h:r.height},
        prototype:Object.getOwnPropertyNames(Object.getPrototypeOf(n))}));
    }
    if(reason==='1s') {
      var modules=document.querySelectorAll('script[type=module][src]');
      console.log('NAV_MODULE_COUNT', modules.length);
      var publicAssets=0;
      for(var i=0;i<Math.min(modules.length,8);i++) {
        var src=modules[i].getAttribute('src'), path=publicPath(src);
        if(path)console.log('NAV_MODULE',path,
          Array.from(new URL(src,document.baseURI).searchParams.keys()));
        if(path==='https://www.redditstatic.com/js/concat'&&publicAssets<2){
          var asset=new URL(src,document.baseURI),prefix=asset.searchParams.get('prefix')||'',chunks=asset.searchParams.get('chunks')||'';
          if(/^[A-Za-z0-9_./-]{0,200}$/.test(prefix)&&/^[A-Za-z0-9_.,/-]{1,6000}$/.test(chunks)){
            var params=new URLSearchParams();params.set('prefix',prefix);params.set('chunks',chunks);
            console.log('PUBLIC_STATIC_ASSET',path+'?'+params);publicAssets++;
          }
        }
      }
    }
  }
  function describe(n, label) {
    try {
      var s = getComputedStyle(n), r = n.getBoundingClientRect();
      console.log('COMPAT_BOX', label, n.tagName, n.id, String(n.className).slice(0,160),
        JSON.stringify({x:r.x,y:r.y,w:r.width,h:r.height,display:s.display,visibility:s.visibility,
          position:s.position,width:s.width,height:s.height,minWidth:s.minWidth,maxWidth:s.maxWidth,
          grid:s.gridTemplateColumns,column:s.gridColumn,row:s.gridRow,area:s.gridArea,
          fit:s.objectFit,background:s.backgroundColor,border:s.border,shadow:s.boxShadow,
          fontSize:s.fontSize,fontWeight:s.fontWeight,lineHeight:s.lineHeight,fontFamily:s.fontFamily,
          flex:s.flex,flexDirection:s.flexDirection,overflow:s.overflow,whiteSpace:s.whiteSpace}));
    } catch (e) { console.log('COMPAT_OBSERVER_ERROR', String(e)); }
  }
  function sample(reason) {
    console.log('COMPAT_STATE', reason, location.origin + location.pathname, document.readyState,
      document.querySelectorAll('*').length, document.styleSheets.length);
    var queries = ['input[type=search]','input[name=q]','form','nav','aside','left-nav',
      'reddit-sidebar-nav','shreddit-app','shreddit-post','[class*=Layout]','[class*=sidebar]',
      'flex-left-nav-container','faceplate-loader','faceplate-partial','form div','[data-component="SplitPageLayout.Pane"]',
      '[class*=searchbox]','[class*=search-wrap]','[class*=searchform]',
      'h2[data-component=Heading]','header summary','header svg','table','table th','table td'];
    queries.forEach(function (q) {
      var list = document.querySelectorAll(q);
      console.log('COMPAT_COUNT', q, list.length);
      for (var i=0; i<Math.min(list.length,6); i++) {
        describe(list[i],q);
        if(q==='faceplate-loader'||q==='faceplate-partial')console.log('COMPAT_PARTIAL',q,
          String(list[i].getAttribute('src')||list[i].getAttribute('href')||'').split(/[?#]/)[0],
          list[i].getAttribute('loading'),!!customElements.get(list[i].localName));
        var p=list[i].parentElement;
        for (var j=0; p && j<3; j++,p=p.parentElement) describe(p,q+' parent'+j);
        if (list[i].shadowRoot) {
          console.log('COMPAT_SHADOW',q,list[i].shadowRoot.childNodes.length);
          console.log('COMPAT_LIGHT',q,Array.from(list[i].childNodes).map(function(n){return [n.nodeType,n.nodeName,n.id,n.className,n.slot];}));
          var children=list[i].shadowRoot.querySelectorAll('nav,aside,slot');
          for(var k=0;k<Math.min(children.length,8);k++) {
            describe(children[k],q+' shadow');
            if(children[k].tagName==='SLOT')console.log('COMPAT_ASSIGNED',q,children[k].name,
              Array.from(children[k].assignedNodes()).map(function(n){return [n.nodeType,n.nodeName,n.id,n.className];}));
          }
          var wrappers=list[i].shadowRoot.querySelectorAll('div');
          for(var k=0;k<Math.min(wrappers.length,8);k++) describe(wrappers[k],q+' wrapper');
        }
      }
    });
    var headerIcons=document.querySelectorAll('header button svg,header a svg');
    var visibleIcons=0;
    for(var k=0;k<headerIcons.length&&visibleIcons<12;k++) {
      var icon=headerIcons[k],r=icon.getBoundingClientRect();
      if(r.width<=0||r.height<=0)continue;
      describe(icon,'visible header icon');
      for(var p=icon.parentElement,j=0;p&&j<3;j++,p=p.parentElement)
        describe(p,'visible header icon parent'+j);
      visibleIcons++;
    }
    var images=document.images;
    for(var i=0;i<Math.min(images.length,8);i++) {
      describe(images[i],'img');
      console.log('COMPAT_IMAGE', images[i].complete,images[i].naturalWidth,images[i].naturalHeight);
    }
  }
  addEventListener('load',function(){sample('load');});
  setTimeout(function(){navPartial('1s');},1000);
  setTimeout(function(){sample('30s');navPartial('30s');},30000);
  setTimeout(function(){sample('90s');navPartial('90s');},90000);
  setTimeout(function(){sample('180s');},180000);
})();
