/* Read-only real-site observation. No DOM, CSS, font or scroll replacement. */
(() => {
  console.log('FONT_OBSERVER', location.href, document.title);
  let target = null, point = null, sent = false, hovered = false;
  setTimeout(() => {
    const candidates = document.querySelectorAll('main p, article p, p');
    for (const node of candidates) {
      const r = node.getBoundingClientRect();
      if (r.width < 200 || r.height < 10 || r.top < 150 || r.top >= innerHeight - 30) continue;
      const x = Math.round(Math.max(4, Math.min(innerWidth - 4, r.left + r.width / 2)));
      const y = Math.round(Math.max(4, Math.min(innerHeight - 4, r.top + Math.min(r.height / 2, 15))));
      const hit = document.elementFromPoint(x, y);
      if (!hit || !node.contains(hit) || hit.closest('a,button,input,select,textarea')) continue;
      target = node; point = [x,y];
      console.log('FONT_FOCUS_TARGET', hit.tagName, getComputedStyle(node).fontFamily,
        node.textContent.trim().slice(0,70));
      console.log('FONT_FOCUS', 1, x, y); sent = true; break;
    }
    if (!sent) console.log('FONT_FOCUS_UNAVAILABLE');
  }, 20000);
  document.addEventListener('mousemove', event => {
    if (!event.isTrusted || !sent || hovered || !point) return;
    const hit = document.elementFromPoint(point[0], point[1]);
    if (Math.abs(event.clientX-point[0]) <= 2 && Math.abs(event.clientY-point[1]) <= 2 &&
        hit && target.contains(hit)) {
      hovered = true; console.log('FONT_HOVER', 1, point[0], point[1]);
    }
  }, true);
  document.addEventListener('scroll', event => {
    const node = event.target;
    console.log('FONT_SCROLL', window.scrollY, node && node.scrollTop || 0,
      node && node.tagName || 'document');
  }, true);
})();
