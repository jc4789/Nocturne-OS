/* Real-site observation only. The harness supplies trusted pointer/key input. */
(() => {
  'use strict';
  const interfaces = ['HTMLBodyElement','HTMLHtmlElement','HTMLParagraphElement','HTMLSpanElement',
    'HTMLBRElement','HTMLHRElement','HTMLPreElement','HTMLQuoteElement','HTMLModElement',
    'HTMLUListElement','HTMLDListElement','HTMLDirectoryElement','HTMLTableElement',
    'HTMLTableCaptionElement','HTMLTableSectionElement','HTMLTableRowElement','HTMLTableCellElement',
    'HTMLTableColElement','HTMLLegendElement','HTMLOptGroupElement','HTMLMapElement','HTMLEmbedElement',
    'HTMLParamElement','HTMLFontElement','HTMLFrameSetElement','HTMLMarqueeElement'];
  const missing = interfaces.filter(name => typeof globalThis[name] !== 'function');
  console.log('HTML_SITE_DOCUMENT', location.href, document.title, 'globals', interfaces.length,
    'missing', missing.join(',') || 'none');
  setTimeout(() => console.log('HTML_SITE_READY', location.href, document.title),20000);
  const interactive = 'a,button,input,select,textarea,[role="button"],[role="link"],[contenteditable]';
  let selected = null;
  function label(node) { return (node.textContent || '').trim().replace(/\s+/g,' ').slice(0,90); }
  function visibleRect(node) {
    const rect = node.getBoundingClientRect();
    if (rect.width < 8 || rect.height < 8 || rect.right <= 3 || rect.bottom <= 3 ||
        rect.left >= innerWidth-3 || rect.top >= innerHeight-3) return null;
    const style = getComputedStyle(node);
    return style.display === 'none' || style.visibility === 'hidden' ? null : rect;
  }
  function choose(node,id,action=false) {
    const rect = visibleRect(node); if (!rect) return false;
    const left = Math.max(4,rect.left), right = Math.min(innerWidth-4,rect.right);
    const top = Math.max(action?4:150,rect.top), bottom = Math.min(innerHeight-4,rect.bottom);
    if (right-left < 4 || bottom-top < 4) return false;
    const xs = [0.5,0.25,0.75], ys = action?[0.5]:[0.2,0.5,0.8];
    for (const yf of ys) for (const xf of xs) {
      const x = Math.round(left+(right-left)*xf), y = Math.round(top+(bottom-top)*yf);
      const hit = document.elementFromPoint(x,y);
      if (!hit || !node.contains(hit) || !action && hit.closest(interactive)) continue;
      selected = {id,node,hit,x,y,hovered:false,stale:false,clicked:false};
      console.log('HTML_SITE_TARGET', id, node.tagName, label(node),
        action ? node.getAttribute('href') || '' : getComputedStyle(node).fontFamily);
      console.log('HTML_SITE_POINT', id, x, y); return true;
    }
    return false;
  }
  setTimeout(() => {
    for (const selector of ['main p,article p,p','main,article,[role="main"]','main div,article div','body div','body']) {
      for (const node of document.querySelectorAll(selector)) if (choose(node,1)) return;
    }
    console.log('HTML_SITE_POINT_UNAVAILABLE', 1, location.href);
  },25000);
  function stale(point,reason) {
    if (!point.stale) console.log('HTML_SITE_STALE', point.id, reason, label(point.node));
    point.stale = true;
  }
  function current(point) {
    if (!point.node.isConnected || !point.hit.isConnected) { stale(point,'detached');return false; }
    if (document.elementFromPoint(point.x,point.y) !== point.hit) { stale(point,'changed-hit-target');return false; }
    return true;
  }
  document.addEventListener('mousemove',event => {
    const point = selected;
    if (!event.isTrusted || !point || point.hovered || point.stale ||
        Math.abs(event.clientX-point.x)>2 || Math.abs(event.clientY-point.y)>2) return;
    if (current(point)) {
      point.hovered = true;console.log('HTML_SITE_HOVER',point.id,point.x,point.y);
    }
  },true);
  document.addEventListener('scroll',event => {
    const target = event.target;
    console.log('HTML_SITE_SCROLL',window.scrollY,document.documentElement.clientHeight,
      location.href,target && target.tagName || 'document',target && target.scrollTop || 0);
  },true);
  function afterClick(id) {
    const menus = [];
    for (const node of document.querySelectorAll('[role="menu"],[role="dialog"],dialog,[aria-expanded="true"]'))
      if (visibleRect(node)) menus.push(node.tagName+':'+label(node));
    console.log('HTML_SITE_AFTER_CLICK',id,location.href,document.title,'visible-menu',menus.join('|').slice(0,500));
  }
  document.addEventListener('click',event => {
    const point = selected;
    if (!event.isTrusted || !point || point.clicked ||
        Math.abs(event.clientX-point.x)>2 || Math.abs(event.clientY-point.y)>2) return;
    if (!point.hovered || point.stale || !current(point)) {
      console.log('HTML_SITE_CLICK_UNCONFIRMED',point.id,event.target && event.target.tagName);return;
    }
    point.clicked = true;
    const link = event.target && event.target.closest('a');
    console.log('HTML_SITE_CLICK',point.id,event.target && event.target.tagName,
      link && link.getAttribute('href') || '',label(point.node));
    setTimeout(() => afterClick(point.id),0);
    setTimeout(() => afterClick(point.id),1500);
  },true);
  setTimeout(() => {
    const host = location.hostname;
    const github = /(^|\.)github\.com$/.test(host), mdn = host === 'developer.mozilla.org';
    const baidu = /(^|\.)baidu\.com$/.test(host);
    const selectors = github?['button'] : mdn?['main p a,article p a'] :
      baidu?['a,button','main a,article a,.main-content a','a[href^="/item/"]'] : ['button,[role="button"],a[aria-label],main p a,article p a'];
    for (const selector of selectors) for (const node of document.querySelectorAll(selector)) {
      const text = label(node), href = node.getAttribute('href') || '';
      if (github && !/^Code$/i.test(text)) continue;
      if (baidu && selector==='a,button' && !/目录|目錄|更多|导航/.test(text)) continue;
      if (!github && !mdn && !baidu && node.tagName !== 'A' &&
          !/menu|navigation|メニュー|more|更多|すべて|all/i.test(text+' '+(node.getAttribute('aria-label')||''))) continue;
      if (/delete|remove|buy|checkout|purchase|submit|send|削除|購入|注文|送信|退出|登录/i.test(text)) continue;
      if (node.tagName === 'A' && (!href || /^javascript:/i.test(href))) continue;
      if (choose(node,2,true)) return;
    }
    console.log('HTML_SITE_POINT_UNAVAILABLE',2,location.href);
  },110000);
})();
